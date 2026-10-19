#!/usr/bin/env python3
"""
test_sc64_bridge.py - offline tests for sc64_bridge.py (no cart, no Link).

A fake SummerCart64 (the firmware's side of the USB: the DTR/DSR reset, 'v'
and 'V' answered, 'U' data kept, PKT 'U' packets sent in chosen pieces) and
a fake Halo 64 Link (a TCP server on 127.0.0.1:6470, or a free port if that
one is taken; never 6466) are put on each side of the bridge's real relay.
The overlapped Win32 I/O the COM port uses is tried on a named pipe.

  py -3 test_sc64_bridge.py        (add -v for each test's name)
"""

import collections
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sc64_bridge as bridge  # noqa: E402

TEST_PORT = 6470


def pkt(kind, ident, data):
    return kind + ident + struct.pack(">I", len(data)) + data


def pkt_u(datatype, data):
    """the SC64's packet for N64 USB data: PKT 'U', size, header, data"""
    return pkt(b"PKT", b"U", struct.pack(">I", (datatype << 24) | len(data)) + data)


def dma_frame(datatype, data, pad=True):
    """the Link's frame: data padded to an even length (pad=False: "CMPH"
    right after the data, the EverDrive N64's way)"""
    body = data + (b"\0" if pad and len(data) & 1 else b"")
    return b"DMA@" + struct.pack(">I", (datatype << 24) | len(data)) + body + b"CMPH"


def h6(kind, payload, sequence=0):
    return b"H6" + bytes([2, kind]) + struct.pack(">HH", len(payload), sequence) + payload


def wait_until(predicate, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


class FakeSc64Device:
    """The cart's end of the USB, as the SC64 firmware behaves."""

    kind = "FAKE"

    def __init__(self, name="FAKE"):
        self.name = name
        self.cond = threading.Condition()
        self.chunks = collections.deque()
        self.received = bytearray()
        self.pending = bytearray()
        self.commands = []
        self.debug_writes = []
        self.dtr = False
        self.dsr_state = False
        self.dsr_lag = 0
        self.fail = None
        self.closed = False
        self.before_reply = []
        self.after_reply = b""

    # (DSR follows DTR a couple of polls later, as the cart's MCU answers)
    def set_dtr(self, on):
        self.dtr = on
        self.dsr_lag = 2

    def dsr(self):
        if self.dsr_lag:
            self.dsr_lag -= 1
        else:
            self.dsr_state = self.dtr
        return self.dsr_state

    def purge(self):
        with self.cond:
            self.chunks.clear()

    def push(self, *chunks):
        with self.cond:
            self.chunks.extend(bytes(chunk) for chunk in chunks)
            self.cond.notify_all()

    def read(self, size, timeout):
        with self.cond:
            if self.fail:
                raise bridge.DeviceError(self.fail)
            if not self.chunks:
                self.cond.wait(timeout)
            if self.fail:
                raise bridge.DeviceError(self.fail)
            if not self.chunks:
                return b""
            chunk = self.chunks.popleft()
            if len(chunk) > size:
                self.chunks.appendleft(chunk[size:])
                chunk = chunk[:size]
            return chunk

    def write(self, data, stop=None, on_stall=None):
        if self.fail:
            raise bridge.DeviceError(self.fail)
        with self.cond:
            self.received += data
            self.pending += data
            self._commands()
            self.cond.notify_all()
        return True

    def _commands(self):
        buf = self.pending
        while len(buf) >= 12:
            assert buf[:3] == b"CMD", "the bridge sent something that is not a command: %r" % bytes(buf[:16])
            cmd = bytes(buf[3:4])
            arg1, arg2 = struct.unpack(">II", bytes(buf[4:12]))
            if cmd == b"U":
                if len(buf) < 12 + arg2:
                    return
                self.debug_writes.append((arg1, bytes(buf[12:12 + arg2])))
                del buf[:12 + arg2]
                continue
            del buf[:12]
            self.commands.append((cmd, arg1, arg2))
            if cmd == b"v":
                reply = pkt(b"CMP", b"v", b"SCv2")
            elif cmd == b"V":
                reply = pkt(b"CMP", b"V", struct.pack(">HHI", 2, 20, 2))
            else:
                reply = pkt(b"ERR", cmd, b"")
            self.chunks.extend(self.before_reply)
            self.chunks.append(reply + self.after_reply)
            self.before_reply, self.after_reply = [], b""

    def close(self):
        self.closed = True


class FakeLink:
    """Halo 64 Link's end of the cable: a TCP server keeping what comes."""

    def __init__(self, port):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.server.bind(("127.0.0.1", port))
        except OSError:
            self.server.bind(("127.0.0.1", 0))
        assert self.server.getsockname()[1] != 6466
        self.port = self.server.getsockname()[1]
        self.server.listen(4)
        self.server.settimeout(0.2)
        self.lock = threading.Lock()
        self.received = bytearray()
        self.conn = None
        self.connections = 0
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while self.running:
            try:
                conn, _ = self.server.accept()
            except (socket.timeout, OSError):
                continue
            conn.settimeout(0.2)
            with self.lock:
                self.conn = conn
                self.connections += 1
            while self.running:
                try:
                    data = conn.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not data:
                    break
                with self.lock:
                    self.received += data
            with self.lock:
                if self.conn is conn:
                    self.conn = None
            conn.close()

    def take(self):
        with self.lock:
            data = bytes(self.received)
            self.received.clear()
            return data

    def send(self, data):
        with self.lock:
            conn = self.conn
        conn.sendall(data)

    def drop(self):
        with self.lock:
            conn, self.conn = self.conn, None
        if conn:
            conn.shutdown(socket.SHUT_RDWR)
            conn.close()

    def close(self):
        self.running = False
        self.drop()
        self.server.close()
        self.thread.join(2.0)


# ------------------------------------------------------------------ parsers

class ParserTests(unittest.TestCase):
    def test_sc64_messages_whole_and_byte_by_byte(self):
        stream = (pkt_u(0x48, b"abc") + pkt(b"CMP", b"v", b"SCv2") + pkt(b"ERR", b"x", b"") +
                  pkt_u(0x48, b"") + pkt(b"PKT", b"B", b"\x01"))
        expected = [(b"PKT", ord("U"), struct.pack(">I", 0x48000003) + b"abc"), (b"CMP", ord("v"), b"SCv2"),
                    (b"ERR", ord("x"), b""), (b"PKT", ord("U"), struct.pack(">I", 0x48000000)),
                    (b"PKT", ord("B"), b"\x01")]
        self.assertEqual(bridge.Sc64Parser().feed(stream), expected)
        parser, got = bridge.Sc64Parser(), []
        for i in range(len(stream)):
            got += parser.feed(stream[i:i + 1])
        self.assertEqual(got, expected)
        self.assertEqual(parser.skipped, 0)
        self.assertEqual(len(parser.buffer), 0)

    def test_sc64_random_splits(self):
        rng = random.Random(64)
        packets = [pkt_u(0x48, bytes(rng.randrange(256) for _ in range(n))) for n in (1, 2, 3, 7, 4104, 0, 511)]
        stream = b"".join(packets)
        for _ in range(50):
            parser, got, at = bridge.Sc64Parser(), [], 0
            while at < len(stream):
                step = rng.randrange(1, 900)
                got += parser.feed(stream[at:at + step])
                at += step
            self.assertEqual([bytes(p[2]) for p in got], [p[8:] for p in packets])

    def test_sc64_out_of_step(self):
        parser = bridge.Sc64Parser()
        got = parser.feed(b"\x00\xffgarbageCM" + pkt_u(0x48, b"hi") + b"PK")
        self.assertEqual(got, [(b"PKT", ord("U"), struct.pack(">I", 0x48000002) + b"hi")])
        self.assertEqual(parser.skipped, 11)
        got = parser.feed(b"T" + b"U" + struct.pack(">I", 5) + struct.pack(">I", 0x48000001) + b"Z")
        self.assertEqual(got, [(b"PKT", ord("U"), struct.pack(">I", 0x48000001) + b"Z")])
        # (an absurd size is not waited for)
        parser = bridge.Sc64Parser()
        got = parser.feed(b"PKTU\xff\xff\xff\xff" + pkt_u(0x48, b"ok"))
        self.assertEqual(len(got), 1)

    def test_dma_frames(self):
        frames = [dma_frame(0x48, b"odd"), dma_frame(0x48, b"even"), dma_frame(0x48, b""),
                  dma_frame(0x01, b"x", pad=False), dma_frame(0x48, b"q" * 4105)]
        expected = [(0x48, b"odd"), (0x48, b"even"), (0x48, b""), (0x01, b"x"), (0x48, b"q" * 4105)]
        stream = b"".join(frames)
        self.assertEqual(bridge.DmaParser().feed(stream), expected)
        parser, got = bridge.DmaParser(), []
        for i in range(len(stream)):
            got += parser.feed(stream[i:i + 1])
        self.assertEqual(got, expected)
        self.assertEqual(parser.skipped, 0)

    def test_dma_out_of_step(self):
        parser = bridge.DmaParser()
        broken = b"DMA@" + struct.pack(">I", 0x48000002) + b"ab" + b"XXXX"
        got = parser.feed(b"noiseDM" + broken + dma_frame(0x48, b"good"))
        self.assertEqual(got, [(0x48, b"good")])
        self.assertGreater(parser.skipped, 0)

    def test_encoders(self):
        self.assertEqual(bridge.encode_link_packet(0x48, b"abc"),
                         b"DMA@\x48\x00\x00\x03abcCMPH")
        self.assertEqual(bridge.encode_debug_write(0x48, b"abc"),
                         b"CMDU\x00\x00\x00\x48\x00\x00\x00\x03abc")
        self.assertEqual(bridge.encode_command(b"v"), b"CMDv" + b"\0" * 8)
        self.assertEqual(bridge.split_debug_packet(struct.pack(">I", 0x48000002) + b"hi")[:2], (0x48, b"hi"))
        self.assertIsNone(bridge.split_debug_packet(struct.pack(">I", 0x48000009) + b"hi")[1])
        datatype, data, note = bridge.split_debug_packet(struct.pack(">I", 0x48000001) + b"hi")
        self.assertEqual((datatype, data), (0x48, b"h"))
        self.assertTrue(note)

    def test_queue_is_bounded(self):
        queue = bridge.PacketQueue(max_bytes=100)
        for i in range(10):
            queue.put(0x48, bytes([i]) * 30)
        self.assertEqual(queue.dropped, 7)
        batch = queue.get_batch(0.01, max_bytes=60)
        self.assertEqual([data[0] for _, data in batch], [7, 8])
        self.assertEqual(len(queue.get_batch(0.01)), 1)
        self.assertEqual(queue.get_batch(0.01), [])


# ------------------------------------------------------------------ the cart

class CartTests(unittest.TestCase):
    def test_reset_identify_version(self):
        device = FakeSc64Device()
        cart = bridge.Sc64Cart(device, "FAKE")
        cart.reset_and_sync()
        self.assertFalse(device.dtr)
        # (the N64's packets can come before a reply, and after it in one read)
        device.before_reply = [pkt_u(0x48, b"early1")]
        device.after_reply = pkt_u(0x48, b"early2")[:6]
        self.assertTrue(cart.identify())
        device.push(pkt_u(0x48, b"early2")[6:])
        self.assertEqual(cart.firmware_version(), (2, 20, 2))
        early = [bridge.split_debug_packet(p)[1] for kind, ident, p in cart.early if kind == b"PKT"]
        self.assertEqual(early, [b"early1", b"early2"])
        self.assertEqual([c[0] for c in device.commands], [b"v", b"V"])
        self.assertEqual(bytes(device.received), b"CMDv" + b"\0" * 8 + b"CMDV" + b"\0" * 8)

    def test_not_an_sc64(self):
        class Silent(FakeSc64Device):
            def dsr(self):
                return False
        with self.assertRaises(bridge.DeviceError):
            bridge.Sc64Cart(Silent(), "X").reset_and_sync(timeout=0.2)

        class Other(FakeSc64Device):
            def _commands(self):
                self.pending.clear()
                self.chunks.append(pkt(b"CMP", b"v", b"XYZ1"))
        device = Other()
        with self.assertRaises(bridge.DeviceError):
            bridge.open_and_identify(lambda: device, "OTHER")
        self.assertTrue(device.closed)


# ------------------------------------------------------------------ the relay

class RelayTests(unittest.TestCase):
    def setUp(self):
        self.link = FakeLink(TEST_PORT)
        self.devices = []
        self.opened = threading.Event()
        self.logs = []
        self.stop = threading.Event()

        def opener():
            device = FakeSc64Device("FAKE%d" % len(self.devices))
            self.devices.append(device)
            cart = bridge.open_and_identify(lambda: device, device.name)
            self.opened.set()
            return cart, []

        def log(message):
            self.logs.append(message)
            if os.environ.get("SC64_TEST_LOG"):
                print("    bridge: " + message)

        self.thread = threading.Thread(target=bridge.run_bridge, args=("127.0.0.1", self.link.port, opener),
                                       kwargs=dict(stop=self.stop, log=log, report_seconds=0.5,
                                                   cart_retry_seconds=0.3, link_retry_seconds=0.2), daemon=True)
        self.thread.start()
        self.assertTrue(self.opened.wait(5.0), "the bridge never opened the fake cart")
        self.assertTrue(wait_until(lambda: self.link.conn is not None), "the bridge never reached the fake Link")
        self.assertTrue(wait_until(lambda: any("Connected to Halo 64 Link" in m for m in self.logs)))

    def tearDown(self):
        self.stop.set()
        self.thread.join(5.0)
        self.assertFalse(self.thread.is_alive(), "the bridge did not stop")
        self.link.close()

    @property
    def cart(self):
        return self.devices[-1]

    def expect_link(self, expected, timeout=5.0):
        got = bytearray()
        deadline = time.time() + timeout
        while len(got) < len(expected) and time.time() < deadline:
            got += self.link.take()
            time.sleep(0.01)
        time.sleep(0.05)
        got += self.link.take()
        self.assertEqual(bytes(got), expected)

    def expect_cart(self, expected, timeout=5.0):
        self.assertTrue(wait_until(lambda: len(self.cart.debug_writes) >= len(expected), timeout),
                        "the cart got %r" % self.cart.debug_writes)
        time.sleep(0.05)
        self.assertEqual(self.cart.debug_writes, expected)

    def test_n64_packet_odd_length(self):
        hello = h6(bridge.H6_HELLO, b"\x02\x09halo-test")  # 19 bytes
        self.assertEqual(len(hello) & 1, 1)
        self.cart.push(pkt_u(0x48, hello))
        self.expect_link(b"DMA@" + struct.pack(">I", 0x48000000 | len(hello)) + hello + b"CMPH")
        self.assertTrue(wait_until(lambda: any("The N64 says hello (halo-test)" in m for m in self.logs)))

    def test_n64_packets_together_and_split(self):
        packets = [b"\x01", h6(7, b"c" * 32), b"x" * 4104, b"", b"tail!"]
        # (three in one read, then one split in three reads, then one byte by byte)
        self.cart.push(b"".join(pkt_u(0x48, p) for p in packets[:3]))
        split = pkt_u(0x48, packets[3])
        self.cart.push(split[:5], split[5:9], split[9:])
        last = pkt_u(0x48, packets[4])
        self.cart.push(*[last[i:i + 1] for i in range(len(last))])
        self.expect_link(b"".join(dma_frame(0x48, p, pad=False) for p in packets))

    def test_link_packets_to_cart(self):
        odd = h6(bridge.H6_STATUS, b"\x0aSEARCHING!")
        even = h6(bridge.H6_WELCOME, b"\x02\x05Link!")[:-1]
        self.assertEqual((len(odd) & 1, len(even) & 1), (1, 0))
        self.link.send(dma_frame(0x48, odd))
        self.expect_cart([(0x48, odd)])
        # (the 'U' command carries the unpadded data, size = the data's)
        tail = bytes(self.cart.received[-(12 + len(odd)):])
        self.assertEqual(tail, b"CMDU" + struct.pack(">II", 0x48, len(odd)) + odd)
        # (several in one send, one split over three sends, "CMPH" after an odd byte count)
        frames = [dma_frame(0x48, even), dma_frame(0x48, b"abc"), dma_frame(0x48, b"q" * 4105),
                  dma_frame(0x48, b"z", pad=False)]
        self.link.send(b"".join(frames[:2]))
        third = frames[2]
        self.link.send(third[:3])
        time.sleep(0.05)
        self.link.send(third[3:2000])
        time.sleep(0.05)
        self.link.send(third[2000:] + frames[3])
        self.expect_cart([(0x48, odd), (0x48, even), (0x48, b"abc"), (0x48, b"q" * 4105), (0x48, b"z")])
        self.assertTrue(wait_until(lambda: any('Link status: "SEARCHING!"' in m for m in self.logs)))

    def test_hello_welcome_notes(self):
        self.cart.push(pkt_u(0x48, h6(bridge.H6_HELLO, b"\x02\x04demo")))
        self.assertTrue(wait_until(lambda: any("says hello" in m for m in self.logs)))
        self.link.send(dma_frame(0x48, h6(bridge.H6_WELCOME, b"\x02\x04Link")))
        self.assertTrue(wait_until(lambda: any("the N64 is online" in m for m in self.logs)))

    def test_link_drops_and_comes_back(self):
        self.link.drop()
        # (the bridge's own line: it is sending to the new connection by then)
        self.assertTrue(wait_until(lambda: self.link.connections >= 2 and self.link.conn is not None and
                                   sum("Connected to Halo 64 Link" in m for m in self.logs) >= 2),
                        "the bridge did not connect again")
        self.link.take()
        self.cart.push(pkt_u(0x48, b"after"))
        self.expect_link(dma_frame(0x48, b"after", pad=False))
        self.link.send(dma_frame(0x48, b"back"))
        self.expect_cart([(0x48, b"back")])

    def test_packets_without_link_are_dropped(self):
        self.link.close()
        self.assertTrue(wait_until(lambda: any("connecting again" in m for m in self.logs)))
        self.cart.push(*(pkt_u(0x48, b"lost%d" % i) for i in range(5)))
        self.assertTrue(wait_until(lambda: any("no Link to take them" in m for m in self.logs), 3.0))
        # (the cart's stream is still read: a reply after them is seen)
        self.cart.push(pkt(b"ERR", b"U", b""))
        self.assertTrue(wait_until(lambda: any("with ERR" in m for m in self.logs)))
        self.link = FakeLink(TEST_PORT)  # (for tearDown)

    def test_cart_lost_and_found(self):
        first = self.cart
        self.opened.clear()
        first.fail = "the cart was unplugged"
        with first.cond:
            first.cond.notify_all()
        self.assertTrue(self.opened.wait(5.0), "the bridge did not look for the cart again")
        self.assertIsNot(self.cart, first)
        self.assertTrue(first.closed)
        self.assertTrue(any("Lost the cart: the cart was unplugged" in m for m in self.logs))
        self.cart.push(pkt_u(0x48, b"again"))
        self.expect_link(dma_frame(0x48, b"again", pad=False))
        self.link.send(dma_frame(0x48, b"to the new cart"))
        self.expect_cart([(0x48, b"to the new cart")])

    def test_volume_both_ways(self):
        rng = random.Random(6466)
        n64 = [bytes(rng.randrange(256) for _ in range(rng.choice((0, 1, 7, 19, 32, 255, 4104))))
               for _ in range(1500)]
        link = [bytes(rng.randrange(256) for _ in range(rng.choice((1, 2, 9, 64, 513, 4104))))
                for _ in range(1500)]
        cart_stream = b"".join(pkt_u(0x48, p) for p in n64)
        link_stream = b"".join(dma_frame(0x48, p) for p in link)

        def send_link():
            at = 0
            while at < len(link_stream):
                step = rng.randrange(1, 20000)
                self.link.send(link_stream[at:at + step])
                at += step
        sender = threading.Thread(target=send_link, daemon=True)
        sender.start()
        at, chunks = 0, []
        while at < len(cart_stream):
            step = rng.randrange(1, 9000)
            chunks.append(cart_stream[at:at + step])
            at += step
        self.cart.push(*chunks)
        self.expect_link(b"".join(dma_frame(0x48, p, pad=False) for p in n64), timeout=30.0)
        sender.join(30.0)
        self.expect_cart([(0x48, p) for p in link], timeout=30.0)

    def test_stats_lines(self):
        for i in range(3):
            self.cart.push(pkt_u(0x48, b"n%d" % i))
            self.link.send(dma_frame(0x48, b"l%d" % i))
        self.assertTrue(wait_until(lambda: any(m.startswith("Last ") and "N64 -> Link 3 packets" in m and
                                               "Link -> N64 3 packets" in m for m in self.logs), 3.0),
                        "\n".join(self.logs))


# ------------------------------------------------------------------ Windows

@unittest.skipUnless(os.name == "nt", "Windows only")
class Win32StreamTests(unittest.TestCase):
    """The COM port's overlapped I/O (Win32Stream), tried on a named pipe."""

    def setUp(self):
        import ctypes
        from ctypes import wintypes
        self.ctypes, self.wintypes = ctypes, wintypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k32 = k32
        k32.CreateNamedPipeW.restype = wintypes.HANDLE
        k32.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        k32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
        k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                 ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                  ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.path = r"\\.\pipe\sc64-bridge-test-%d-%d" % (os.getpid(), random.randrange(1 << 30))
        # (the server's end: synchronous, byte mode, 4 KB buffers)
        self.server = k32.CreateNamedPipeW(self.path, 3, 0, 1, 4096, 4096, 0, None)
        self.assertNotEqual(self.server, bridge.INVALID_HANDLE_VALUE)
        self.stream = bridge.Win32Stream(self.path, "PIPE")
        k32.ConnectNamedPipe(self.server, None)

    def tearDown(self):
        self.stream.close()
        self.k32.CloseHandle(self.server)

    def server_write(self, data):
        done = self.wintypes.DWORD()
        self.assertTrue(self.k32.WriteFile(self.server, data, len(data), self.ctypes.byref(done), None))
        self.assertEqual(done.value, len(data))

    def server_read(self, size):
        buffer = self.ctypes.create_string_buffer(size)
        done = self.wintypes.DWORD()
        self.assertTrue(self.k32.ReadFile(self.server, buffer, size, self.ctypes.byref(done), None))
        return buffer.raw[:done.value]

    def test_read_returns_what_came(self):
        self.server_write(b"0123456789")
        self.assertEqual(self.stream.read(4, 1.0), b"0123")
        self.assertEqual(self.stream.read(100, 1.0), b"456789")

    def test_read_times_out_empty(self):
        started = time.time()
        self.assertEqual(self.stream.read(100, 0.3), b"")
        self.assertLess(time.time() - started, 2.0)
        # (and the next read still works after the cancelled one)
        self.server_write(b"after")
        self.assertEqual(self.stream.read(100, 1.0), b"after")

    def test_reading_while_writing(self):
        got = bytearray()
        data = bytes(range(256)) * 400

        def server():
            while len(got) < len(data):
                got.extend(self.server_read(65536))
        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        # (a read waiting in one thread does not hold up a write in another)
        reader_result = []
        reader = threading.Thread(target=lambda: reader_result.append(self.stream.read(100, 3.0)), daemon=True)
        reader.start()
        time.sleep(0.1)
        self.assertTrue(self.stream.write(data))
        thread.join(5.0)
        self.assertEqual(bytes(got), data)
        self.server_write(b"late")
        reader.join(5.0)
        self.assertEqual(reader_result, [b"late"])

    def test_blocked_write_stops(self):
        old = bridge.STALL_SECONDS
        bridge.STALL_SECONDS = 0.5
        try:
            stop, stalls = threading.Event(), []
            threading.Timer(1.2, stop.set).start()
            started = time.time()
            self.assertFalse(self.stream.write(b"x" * (1 << 20), stop, stalls.append))
            self.assertLess(time.time() - started, 3.0)
            self.assertEqual(stalls, [True])
        finally:
            bridge.STALL_SECONDS = old


@unittest.skipUnless(os.name == "nt", "Windows only")
class WindowsDiscoveryTests(unittest.TestCase):
    def test_list_ports_runs(self):
        ports, problem = bridge.list_com_ports()
        self.assertIsInstance(ports, list)
        for port in ports:
            self.assertTrue(port["port"].startswith("COM"))

    def test_setupapi_walk(self):
        # (a PC without a cart may have no COM ports: the same walk over the
        # USB class (instance ids) and the disk drives (friendly names))
        byte8 = bridge.ctypes.c_ubyte * 8
        usb = bridge.GUID(0x36FC9E60, 0xC465, 0x11CF, byte8(0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
        devices = bridge._setupapi_ports(usb, com_only=False)
        self.assertTrue(any(d["hwid"].upper().startswith("USB\\VID_") for d in devices), devices[:3])
        disks = bridge.GUID(0x4D36E967, 0xE325, 0x11CE, byte8(0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18))
        devices = bridge._setupapi_ports(disks, com_only=False)
        self.assertTrue(any(d["name"] and d["hwid"] for d in devices), devices)

    def test_missing_port(self):
        with self.assertRaises(bridge.DeviceError) as caught:
            bridge.ComPortDevice("COM250")
        self.assertIn("COM250 is not there", str(caught.exception))
        cart, problems = bridge.find_cart(port="250")
        self.assertIsNone(cart)
        self.assertEqual(problems, ["COM250 is not there (unplugged?)"])

    def test_d2xx_optional(self):
        devices = bridge.list_d2xx_devices()
        self.assertTrue(devices is None or isinstance(devices, list))

    def test_candidates(self):
        def port(name, hwid, friendly="USB Serial Port"):
            return {"port": name, "hwid": hwid, "name": friendly, "latency": 16}
        sc64 = port("COM7", r"FTDIBUS\VID_0403+PID_6014+SC64A1B2A\0000")
        other_ftdi = port("COM8", r"FTDIBUS\VID_0403+PID_6014+FT9XYZA\0000")
        arduino = port("COM3", r"USB\VID_2341&PID_0043\85735313")
        bluetooth = port("COM4", r"BTHENUM\{00001101}_LOCALMFG&0000\7&1&0&000000000000_00000000")
        self.assertEqual(bridge.sc64_port_candidates([arduino, other_ftdi, sc64, bluetooth]), [sc64])
        self.assertEqual(bridge.sc64_port_candidates([arduino, other_ftdi, bluetooth]), [other_ftdi])
        self.assertEqual(bridge.sc64_port_candidates([arduino, bluetooth]), [])
        vcp = port("COM9", "", r"\Device\VCP0")
        self.assertEqual(bridge.sc64_port_candidates([vcp, port("COM1", "", r"\Device\Serial0")]), [vcp])
        self.assertEqual(bridge.ftdi_serial(sc64["hwid"]), "SC64A1B2")
        self.assertEqual(bridge.normalize_port(r"\\.\com12"), "COM12")
        self.assertEqual(bridge.normalize_port("5"), "COM5")


class CommandLineTests(unittest.TestCase):
    def run_bridge(self, *args):
        return subprocess.run([sys.executable, os.path.join(HERE, "sc64_bridge.py")] + list(args),
                              capture_output=True, text=True, timeout=30)

    def test_version_and_list(self):
        self.assertIn("sc64_bridge", self.run_bridge("--version").stdout)
        if os.name == "nt":
            result = self.run_bridge("--list")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("COM ports:", result.stdout)

    @unittest.skipUnless(os.name == "nt", "Ctrl+Break is Windows'")
    def test_waits_then_stops_cleanly(self):
        link = FakeLink(TEST_PORT)
        try:
            process = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "sc64_bridge.py"),
                                        "--port", "COM250", "--link", "127.0.0.1:%d" % link.port],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                       creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            lines = []
            reader = threading.Thread(target=lambda: lines.extend(process.stdout), daemon=True)
            reader.start()
            self.assertTrue(wait_until(lambda: link.conn is not None, 10.0), "".join(lines))
            self.assertTrue(wait_until(lambda: any("Looking for the SummerCart64" in l for l in lines), 10.0),
                            "".join(lines))
            process.send_signal(signal.CTRL_BREAK_EVENT)
            self.assertEqual(process.wait(10.0), 0)
            reader.join(5.0)
            text = "".join(lines)
            self.assertIn("COM250 is not there", text)
            self.assertIn("Connected to Halo 64 Link", text)
            self.assertIn("Stopping...", text)
            self.assertNotIn("Traceback", text)
        finally:
            if process.poll() is None:
                process.kill()
            process.stdout.close()
            link.close()


if __name__ == "__main__":
    unittest.main()
