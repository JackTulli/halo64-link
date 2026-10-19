#!/usr/bin/env python3
"""PC side test client for ares's emulated EverDrive USB port (ARES_N64_USB).

Talks to the usb-echo ROM: waits for its hello, then sends packets and checks
that each comes back with the same datatype, the ROM's counter in front (4
bytes big-endian, or "%08x:" for text) and the same payload. Measures
round-trip time and throughput.

  --framing raw        ARES_N64_USB=tcp:HOST:PORT (default port 6464). The socket
                       is the cart's USB FIFO: packets are UNFLoader's EverDrive
                       framing, "DMA@" <type:8 size:24> data "CMPH".
  --framing unfloader  ARES_N64_USB=unfloader (default port 48646). Packets are
                       <type:32> <size:32> data, as UNFLoader's gopher64 driver
                       sends them; also checks the TCPTEST handshake.
  --listen             ARES_N64_USB=connect:HOST:PORT (raw framing): listen on
                       HOST:PORT and let ares connect to us.
"""
import argparse
import os
import select
import socket
import statistics
import struct
import sys
import time

DATATYPE_TEXT, DATATYPE_RAWBINARY, DATATYPE_HEARTBEAT, DATATYPE_TCPTEST = 1, 2, 5, 7
DATATYPE_NETPACKET = 0x27  # N64-NetLib's packets
DATATYPE_TICKS = 0x30      # usb-echo: answers with its 64-bit tick count
N64_TICKS_PER_SECOND = 46_875_000


class RawFraming:
    """The ED64 FT245 byte stream, as UNFLoader's device_everdrive.cpp sees it."""
    name = "raw"

    @staticmethod
    def encode(dtype, data):
        # PC -> N64: data padded to even BEFORE "CMPH" (usb_everdrive_poll reads ALIGN(size, 2))
        pad = b"\0" if len(data) & 1 else b""
        return b"DMA@" + struct.pack(">I", dtype << 24 | len(data)) + data + pad + b"CMPH"

    @staticmethod
    def decode(buf):
        # N64 -> PC: "DMA@" hdr data "CMPH", then one pad byte if the size is odd
        while True:
            start = buf.find(b"DMA@")
            if start < 0:
                del buf[:max(0, len(buf) - 3)]
                return None
            if start:
                print(f"  [raw] skipped {start} bytes before DMA@", file=sys.stderr)
                del buf[:start]
            if len(buf) < 8:
                return None
            header = struct.unpack(">I", buf[4:8])[0]
            dtype, size = header >> 24, header & 0xFFFFFF
            total = 8 + size + 4 + (size & 1)
            if len(buf) < total:
                return None
            data, tail = bytes(buf[8:8 + size]), bytes(buf[8 + size:12 + size])
            del buf[:total]
            if tail != b"CMPH":
                print(f"  [raw] bad tail {tail!r}, dropping packet", file=sys.stderr)
                continue
            return dtype, data


class UnfFraming:
    """UNFLoader's gopher64 TCP packets: <type:32 BE> <size:32 BE> <data>."""
    name = "unfloader"

    @staticmethod
    def encode(dtype, data):
        return struct.pack(">II", dtype, len(data)) + data

    @staticmethod
    def decode(buf):
        if len(buf) < 8:
            return None
        dtype, size = struct.unpack(">II", buf[:8])
        if len(buf) < 8 + size:
            return None
        data = bytes(buf[8:8 + size])
        del buf[:8 + size]
        return dtype, data


class Link:
    def __init__(self, host, port, framing, connect_timeout=30.0, listen=False):
        self.framing = framing
        self.buf = bytearray()
        if listen:
            # ARES_N64_USB=connect:HOST:PORT: ares connects to us (it retries once a second)
            server = socket.create_server((host, port))
            server.settimeout(connect_timeout)
            self.sock, self.peer = server.accept()
            server.close()
        else:
            deadline = time.monotonic() + connect_timeout
            while True:
                try:
                    self.sock = socket.create_connection((host, port), timeout=2)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.1)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.setblocking(False)
        self.bytes_in = self.bytes_out = 0

    def close(self):
        self.sock.close()

    def send(self, dtype, data):
        msg = memoryview(self.framing.encode(dtype, data))
        while msg:
            _, w, _ = select.select([], [self.sock], [], 5)
            if not w:
                raise TimeoutError("send stalled")
            n = self.sock.send(msg)
            self.bytes_out += n
            msg = msg[n:]

    def pump(self, timeout):
        r, _, _ = select.select([self.sock], [], [], timeout)
        if r:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise ConnectionError("ares closed the connection")
            self.bytes_in += len(chunk)
            self.buf += chunk

    def recv(self, timeout=5.0, skip_heartbeats=True):
        deadline = time.monotonic() + timeout
        while True:
            pkt = self.framing.decode(self.buf)
            if pkt:
                if skip_heartbeats and pkt[0] == DATATYPE_HEARTBEAT:
                    continue
                return pkt
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("no packet")
            self.pump(min(left, 0.5))


def split_echo(dtype, data):
    """The ROM's counter prefix: "%08x:" for text, 4 bytes big-endian otherwise."""
    try:
        if dtype == DATATYPE_TEXT:
            return int(data[:8], 16), data[9:]
        return struct.unpack(">I", data[:4])[0], data[4:]
    except ValueError:
        return -1, data


def payload(size, seed):
    return bytes((seed * 31 + i * 7 + (i >> 8)) & 0xFF for i in range(size))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int)
    ap.add_argument("--framing", choices=["raw", "unfloader"], default="raw")
    ap.add_argument("--rtt-count", type=int, default=300)
    ap.add_argument("--burst-count", type=int, default=300)
    ap.add_argument("--burst-size", type=int, default=4096)
    ap.add_argument("--window", type=int, default=8, help="packets in flight in the burst test")
    ap.add_argument("--hello-timeout", type=float, default=60,
                    help="seconds to wait for the ROM's hello (0: don't wait, e.g. on a reconnect)")
    ap.add_argument("--listen", action="store_true",
                    help="listen on HOST:PORT and wait for ares to connect (ARES_N64_USB=connect:HOST:PORT, raw)")
    ap.add_argument("--accept-timeout", type=float, default=60,
                    help="with --listen: seconds to wait for ares to connect")
    args = ap.parse_args()
    if args.listen and args.framing != "raw":
        ap.error("--listen needs --framing raw (connect: mode carries the raw stream)")
    framing = RawFraming if args.framing == "raw" else UnfFraming
    port = args.port or (6464 if args.framing == "raw" else 48646)
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)
            print("  FAIL:", what)

    if framing is UnfFraming:
        # UNFLoader's device_test_gopher64: connect, TCPTEST "N64", expect it back, close.
        probe = Link(args.host, port, framing)
        t0 = time.monotonic()
        probe.send(DATATYPE_TCPTEST, b"N64")
        dtype, data = probe.recv(timeout=5)
        print(f"TCPTEST handshake: type {dtype} {data!r} in {(time.monotonic() - t0) * 1e3:.1f} ms")
        check(dtype == DATATYPE_TCPTEST and data == b"N64", "TCPTEST answer")
        probe.close()

    if args.listen:
        print(f"listening on {args.host}:{port}, waiting for ares to connect")
        t0 = time.monotonic()
        link = Link(args.host, port, framing, connect_timeout=args.accept_timeout, listen=True)
        print(f"ares connected from {link.peer[0]}:{link.peer[1]} after {time.monotonic() - t0:.2f} s "
              f"({framing.name} framing)")
    else:
        link = Link(args.host, port, framing)
        print(f"connected to {args.host}:{port} ({framing.name} framing)")

    if args.hello_timeout > 0:
        t0 = time.monotonic()
        dtype, hello = link.recv(timeout=args.hello_timeout)
        print(f"hello after {time.monotonic() - t0:.2f} s: type {dtype} {hello!r}")
        check(dtype == DATATYPE_TEXT and hello.startswith(b"usb-echo hello"), "hello packet")
    # the ROM's counter: 1 on a fresh boot; taken from the first echo after a reconnect
    expected = 1 if args.hello_timeout > 0 else None

    def echo(dtype, data, timeout=10):
        nonlocal expected
        link.send(dtype, data)
        while True:
            rtype, rdata = link.recv(timeout=timeout)
            if rtype == DATATYPE_TEXT and rdata.startswith(b"usb-echo hello"):
                continue
            break
        counter, body = split_echo(rtype, rdata)
        if expected is None:
            print(f"ROM counter continues at {counter}")
            expected = counter
        ok = rtype == dtype and counter == expected and body == data
        if not ok:
            check(False, f"echo #{expected}: sent type {dtype} {len(data)} bytes, got type {rtype} "
                         f"{len(body)} bytes counter {counter}")
        expected += 1
        return ok

    # 1. sizes, odd and even, across the 512-byte block boundaries, several datatypes
    sizes = [1, 2, 3, 4, 5, 7, 8, 15, 16, 63, 64, 255, 256, 499, 500, 501, 503, 504, 505,
             511, 512, 513, 1000, 1001, 1023, 1024, 1025, 2047, 2048, 3000, 4095, 4096,
             4097, 8191, 8192, 16384, 32768, 65000]
    print(f"size sweep: {len(sizes)} packets, 1 to {max(sizes)} bytes")
    t0 = time.monotonic()
    good = 0
    for i, size in enumerate(sizes):
        dtype = (DATATYPE_RAWBINARY, DATATYPE_TEXT, DATATYPE_NETPACKET)[i % 3]
        good += echo(dtype, payload(size, i))
    print(f"  {good}/{len(sizes)} echoes correct in {time.monotonic() - t0:.2f} s")

    # 2. round-trip time, one packet in flight
    for size in (16, 1024):
        rtts = []
        for i in range(args.rtt_count):
            data = payload(size, i)
            t = time.perf_counter()
            echo(DATATYPE_RAWBINARY, data)
            rtts.append((time.perf_counter() - t) * 1e3)
        rtts.sort()
        print(f"RTT {size} B x{len(rtts)}: min {rtts[0]:.2f} ms, median {statistics.median(rtts):.2f} ms, "
              f"p95 {rtts[int(len(rtts) * 0.95)]:.2f} ms, max {rtts[-1]:.2f} ms")

    def n64_seconds():
        nonlocal expected
        link.send(DATATYPE_TICKS, b"")
        rtype, rdata = link.recv(timeout=10)
        counter, body = split_echo(rtype, rdata)
        check(rtype == DATATYPE_TICKS and counter == expected and len(body) == 8, "ticks query")
        expected += 1
        return struct.unpack(">Q", body)[0] / N64_TICKS_PER_SECOND

    # 3. throughput: many packets in a row, up to --window in flight
    size, count = args.burst_size, args.burst_count
    n64_t0, host_t0 = n64_seconds(), time.perf_counter()
    sent = received = 0
    start_counter = expected
    t0 = time.perf_counter()
    while received < count:
        while sent < count and sent - received < args.window:
            link.send(DATATYPE_RAWBINARY, payload(size, sent))
            sent += 1
        rtype, rdata = link.recv(timeout=10)
        if rtype == DATATYPE_TEXT and rdata.startswith(b"usb-echo hello"):
            continue
        counter, body = split_echo(rtype, rdata)
        if counter != start_counter + received or body != payload(size, received) or rtype != DATATYPE_RAWBINARY:
            check(False, f"burst echo {received}: counter {counter}, {len(body)} bytes")
        received += 1
    elapsed = time.perf_counter() - t0
    expected = start_counter + count
    n64_elapsed, host_elapsed = n64_seconds() - n64_t0, time.perf_counter() - host_t0
    total = 2 * size * count
    print(f"burst {count} x {size} B, window {args.window}: {elapsed:.2f} s, "
          f"{count / elapsed:.0f} packets/s, {size * count / elapsed / 1024:.0f} KiB/s each way "
          f"({total / elapsed / 1024:.0f} KiB/s total payload)")
    print(f"  N64 time for the burst: {n64_elapsed:.2f} s (emulation ran at {n64_elapsed / host_elapsed:.2f}x "
          f"real time); in N64 time: {count / n64_elapsed:.0f} packets/s, "
          f"{size * count / n64_elapsed / 1024:.0f} KiB/s each way")

    link.close()
    print(f"socket totals: {link.bytes_out} bytes out, {link.bytes_in} bytes in, {expected - 1} echoes")
    print("RESULT:", "PASS" if not failures else f"FAIL ({len(failures)} problems)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
