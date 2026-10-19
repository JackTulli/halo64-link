#!/usr/bin/env python3
"""
sc64_bridge.py - Halo 64 Online on a real SummerCart64 (SC64) flashcart.

Relays the N64's USB packets between the SC64's USB port and Halo 64 Link's
TCP "cable" (127.0.0.1:6466), the same byte stream ares' emulated EverDrive
USB port speaks, so the Link cannot tell a real cartridge from ares:

  N64 -> Link   "DMA@", u32 BE header ((datatype << 24) | size), data, "CMPH"
  Link -> N64   "DMA@", u32 BE header, data padded to an even length, "CMPH"

The SC64 side (UNFLoader's device_sc64.cpp, the SummerCart64 firmware):

  PC -> SC64    "CMD", id, arg1 (u32 BE), arg2 (u32 BE), then data if any.
                Data for the N64: 'U' (arg1 = datatype, arg2 = size), no reply.
  SC64 -> PC    "CMP" / "ERR" (a command's reply) or "PKT" (unasked), id,
                u32 BE size, data. The N64's data: PKT 'U' = u32 BE header
                ((datatype << 24) | size), then the data.
  Reset/sync    DTR on until DSR comes on, purge, DTR off until DSR goes off.
  Identify      'v' answers CMP 'v' "SCv2".

The cart is an FTDI FT232H (USB 0403:6014, description/serial "SC64..."): it
is used as a COM port (FTDI's VCP driver), or through FTDI's D2XX driver
(ftd2xx.dll) when there is no COM port. Python 3.8+, standard library only
(ctypes for the Win32 and D2XX calls).

  py -3 sc64_bridge.py                 find the cart, connect to the Link
  py -3 sc64_bridge.py --port COM5     use this COM port
  py -3 sc64_bridge.py --d2xx          use FTDI's D2XX driver
  py -3 sc64_bridge.py --list          list the ports and FTDI devices seen
"""

import argparse
import collections
import ctypes
import os
import signal
import socket
import struct
import sys
import threading
import time

BRIDGE_VERSION = "1.0"
DEFAULT_LINK = "127.0.0.1:6466"

# ------------------------------------------------------------------ protocol

LINK_DATATYPE = 0x48            # Halo 64 Online's packets
TEXT_DATATYPE = 0x01            # UNFLoader's DATATYPE_TEXT
SC64_VID, SC64_PID = 0x0403, 0x6014
SC64_IDENTIFIER = b"SCv2"
CMD_IDENTIFIER_GET = b"v"
CMD_VERSION_GET = b"V"
CMD_DEBUG_WRITE = b"U"
PACKET_DEBUG = ord("U")         # USB_PACKET_DEBUG: the N64's USB data
PACKET_ISV = ord("I")           # IS-Viewer text (when enabled on the cart)
SC64_TOKENS = (b"CMP", b"ERR", b"PKT")
# (nothing the cart sends is this long: a size this big means out of step)
SC64_MAX_SIZE = 16 << 20
# (the Link's packets are a few KB; the N64 takes 4 KB + 8 at most)
LINK_MAX_SIZE = 1 << 20
SIZE_MASK = 0xFFFFFF

# Halo 64 Online's own packets ('H' '6' version type length(2) sequence(2))
H6_HEADER_SIZE = 8
H6_HELLO = 1
H6_WELCOME = 64
H6_STATUS = 65

READ_CHUNK = 65536
WRITE_BATCH = 65536
STALL_SECONDS = 5.0


class DeviceError(Exception):
    """the cart's USB failed (unplugged, busy, not an SC64...)"""


_log_lock = threading.Lock()


def log(message):
    with _log_lock:
        sys.stdout.write(time.strftime("[%H:%M:%S] ") + message + "\n")
        sys.stdout.flush()


def size_text(count):
    if count < 1024:
        return "%d B" % count
    if count < 1024 * 1024:
        return "%.1f KB" % (count / 1024.0)
    return "%.1f MB" % (count / (1024.0 * 1024.0))


class Sc64Parser:
    """The SC64 -> PC byte stream: "CMP"/"ERR"/"PKT", id, u32 BE size, data.
    feed() returns the whole messages that came, as (kind, id, data); bytes
    that start no message are skipped (counted in .skipped)."""

    def __init__(self):
        self.buffer = bytearray()
        self.skipped = 0

    def _next_token(self, start):
        found = -1
        for token in SC64_TOKENS:
            at = self.buffer.find(token, start)
            if at >= 0 and (found < 0 or at < found):
                found = at
        return found

    def feed(self, data):
        buf = self.buffer
        buf += data
        messages = []
        at = 0
        while True:
            left = len(buf) - at
            if left < 3:
                break
            kind = bytes(buf[at:at + 3])
            if kind not in SC64_TOKENS:
                # (out of step: on to the next token, keeping a token's start)
                found = self._next_token(at + 1)
                if found < 0:
                    found = max(at, len(buf) - 2)
                self.skipped += found - at
                at = found
                continue
            if left < 8:
                break
            size = struct.unpack_from(">I", buf, at + 4)[0]
            if size > SC64_MAX_SIZE:
                self.skipped += 1
                at += 1
                continue
            if left < 8 + size:
                break
            messages.append((kind, buf[at + 3], bytes(buf[at + 8:at + 8 + size])))
            at += 8 + size
        del buf[:at]
        return messages


class DmaParser:
    """The Link -> N64 byte stream: "DMA@", u32 BE header, data (padded to
    an even length), "CMPH" (also accepted right after the data, as the
    Link's own parser does). feed() returns (datatype, data) for each
    whole packet; data is unpadded."""

    def __init__(self):
        self.buffer = bytearray()
        self.skipped = 0

    def feed(self, data):
        buf = self.buffer
        buf += data
        packets = []
        at = 0
        while True:
            found = buf.find(b"DMA@", at)
            if found < 0:
                # (keep what could be the start of the next "DMA@")
                found = max(at, len(buf) - 3)
                self.skipped += found - at
                at = found
                break
            self.skipped += found - at
            at = found
            left = len(buf) - at
            if left < 8:
                break
            header = struct.unpack_from(">I", buf, at + 4)[0]
            size = header & SIZE_MASK
            padded = size + (size & 1)
            if size > LINK_MAX_SIZE:
                self.skipped += 4
                at += 4
                continue
            if left < 8 + size + 4:
                break
            if buf[at + 8 + size:at + 12 + size] == b"CMPH":
                end = at + 12 + size
            elif left < 8 + padded + 4:
                break
            elif buf[at + 8 + padded:at + 12 + padded] == b"CMPH":
                end = at + 12 + padded
            else:
                # (no "CMPH" where it belongs: out of step)
                self.skipped += 4
                at += 4
                continue
            packets.append((header >> 24, bytes(buf[at + 8:at + 8 + size])))
            at = end
        del buf[:at]
        return packets


def encode_link_packet(datatype, data):
    """an N64 packet for the Link, as the EverDrive frames it"""
    header = ((datatype & 0xFF) << 24) | (len(data) & SIZE_MASK)
    return b"DMA@" + struct.pack(">I", header) + bytes(data) + b"CMPH"


def encode_command(cmd, arg1=0, arg2=0, data=b""):
    """a command for the SC64"""
    return b"CMD" + cmd + struct.pack(">II", arg1 & 0xFFFFFFFF, arg2 & 0xFFFFFFFF) + bytes(data)


def encode_debug_write(datatype, data):
    """data for the N64: the SC64's 'U' command (no reply)"""
    return encode_command(CMD_DEBUG_WRITE, datatype & 0xFF, len(data), data)


def split_debug_packet(payload):
    """a PKT 'U''s data -> (datatype, data, note); data None if unusable"""
    if len(payload) < 4:
        return 0, None, "a USB packet of %d bytes, too short for its header" % len(payload)
    header = struct.unpack_from(">I", payload, 0)[0]
    datatype, size = header >> 24, header & SIZE_MASK
    if size > len(payload) - 4:
        return datatype, None, "a USB packet says %d bytes but brought %d" % (size, len(payload) - 4)
    note = None
    if size < len(payload) - 4:
        note = "a USB packet says %d bytes and brought %d (the rest ignored)" % (size, len(payload) - 4)
    return datatype, payload[4:4 + size], note


def h6_string(data, at):
    """a Halo 64 Online string (length byte, bytes) at data[at], printable"""
    if at >= len(data):
        return ""
    raw = data[at + 1:at + 1 + data[at]]
    return "".join(chr(c) if 32 <= c < 127 else "?" for c in raw)


# ------------------------------------------------------------------ helpers

class PacketQueue:
    """The Link's packets waiting for the cart (bounded: when the N64 stops
    reading for long, the oldest are dropped so the Link never stalls)."""

    def __init__(self, max_bytes=4 << 20):
        self._items = collections.deque()
        self._bytes = 0
        self._cond = threading.Condition()
        self.max_bytes = max_bytes
        self.dropped = 0

    def put(self, datatype, data):
        with self._cond:
            self._items.append((datatype, data))
            self._bytes += len(data)
            while self._bytes > self.max_bytes and len(self._items) > 1:
                _, old = self._items.popleft()
                self._bytes -= len(old)
                self.dropped += 1
            self._cond.notify()

    def get_batch(self, timeout, max_bytes=WRITE_BATCH):
        """the oldest packets (at least one, up to max_bytes), or []"""
        with self._cond:
            if not self._items:
                self._cond.wait(timeout)
            batch, total = [], 0
            while self._items and (not batch or total + len(self._items[0][1]) <= max_bytes):
                item = self._items.popleft()
                self._bytes -= len(item[1])
                total += len(item[1])
                batch.append(item)
            return batch

    def clear(self):
        with self._cond:
            count = len(self._items)
            self._items.clear()
            self._bytes = 0
            return count

    def __len__(self):
        with self._cond:
            return len(self._items)


class Stats:
    def __init__(self):
        self._lock = threading.Lock()
        self._counts = collections.Counter()

    def add(self, **counts):
        with self._lock:
            for key, value in counts.items():
                self._counts[key] += value

    def snapshot(self):
        with self._lock:
            return dict(self._counts)


class TrafficNotes:
    """Friendly lines from what passes: the N64 saying hello, the Link
    answering, the Link's status line changing."""

    def __init__(self, log, verbose):
        self.log = log
        self.verbose = verbose
        self._lock = threading.Lock()
        self.hello = False
        self.status = None

    def from_n64(self, datatype, data, delivered):
        if datatype == TEXT_DATATYPE:
            if self.verbose:
                self.log("N64 text: " + bytes(data).decode("ascii", "replace").rstrip())
            return
        if datatype != LINK_DATATYPE or len(data) < H6_HEADER_SIZE or data[:2] != b"H6":
            return
        if data[3] == H6_HELLO:
            with self._lock:
                if self.hello:
                    return
                self.hello = True
            build = h6_string(data, H6_HEADER_SIZE + 1)
            if delivered:
                self.log("The N64 says hello (%s): passed to Halo 64 Link" % (build or "Halo 64 Online"))
            else:
                self.log("The N64 says hello (%s), but Halo 64 Link is not connected yet" %
                         (build or "Halo 64 Online"))

    def to_n64(self, datatype, data):
        if datatype != LINK_DATATYPE or len(data) < H6_HEADER_SIZE or data[:2] != b"H6":
            return
        if data[3] == H6_WELCOME:
            with self._lock:
                was = self.hello
                self.hello = False
            if was:
                self.log("Halo 64 Link answered: the N64 is online")
        elif data[3] == H6_STATUS:
            text = h6_string(data, H6_HEADER_SIZE)
            with self._lock:
                if text == self.status:
                    return
                self.status = text
            self.log('Link status: "%s"' % text)


# ------------------------------------------------------------------ Win32

if os.name == "nt":
    import winreg
    from ctypes import wintypes

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    FILE_FLAG_OVERLAPPED = 0x40000000
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    ERROR_FILE_NOT_FOUND = 2
    ERROR_PATH_NOT_FOUND = 3
    ERROR_ACCESS_DENIED = 5
    ERROR_SHARING_VIOLATION = 32
    ERROR_OPERATION_ABORTED = 995
    ERROR_IO_PENDING = 997
    WAIT_OBJECT_0 = 0
    SETDTR = 5
    CLRDTR = 6
    MS_DSR_ON = 0x20
    PURGE_TXCLEAR = 0x0004
    PURGE_RXCLEAR = 0x0008
    MAXDWORD = 0xFFFFFFFF
    # (a read returns as soon as a byte is there, or after this with none)
    COM_READ_TIMEOUT_MS = 100

    class OVERLAPPED(ctypes.Structure):
        _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                    ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                    ("hEvent", wintypes.HANDLE)]

    class DCB(ctypes.Structure):
        _fields_ = [("DCBlength", wintypes.DWORD), ("BaudRate", wintypes.DWORD),
                    ("flags", wintypes.DWORD), ("wReserved", wintypes.WORD),
                    ("XonLim", wintypes.WORD), ("XoffLim", wintypes.WORD),
                    ("ByteSize", wintypes.BYTE), ("Parity", wintypes.BYTE),
                    ("StopBits", wintypes.BYTE), ("XonChar", ctypes.c_char),
                    ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                    ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char),
                    ("wReserved1", wintypes.WORD)]

    class COMMTIMEOUTS(ctypes.Structure):
        _fields_ = [("ReadIntervalTimeout", wintypes.DWORD),
                    ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                    ("ReadTotalTimeoutConstant", wintypes.DWORD),
                    ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                    ("WriteTotalTimeoutConstant", wintypes.DWORD)]

    def _proto(dll, name, restype, *argtypes):
        function = getattr(dll, name)
        function.restype = restype
        function.argtypes = argtypes
        return function

    _LPOV = ctypes.POINTER(OVERLAPPED)
    _LPDWORD = ctypes.POINTER(wintypes.DWORD)
    _CreateFileW = _proto(_k32, "CreateFileW", wintypes.HANDLE, wintypes.LPCWSTR, wintypes.DWORD,
                          wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    _CreateEventW = _proto(_k32, "CreateEventW", wintypes.HANDLE, ctypes.c_void_p, wintypes.BOOL,
                           wintypes.BOOL, wintypes.LPCWSTR)
    _ReadFile = _proto(_k32, "ReadFile", wintypes.BOOL, wintypes.HANDLE, ctypes.c_void_p,
                       wintypes.DWORD, _LPDWORD, _LPOV)
    _WriteFile = _proto(_k32, "WriteFile", wintypes.BOOL, wintypes.HANDLE, ctypes.c_void_p,
                        wintypes.DWORD, _LPDWORD, _LPOV)
    _GetOverlappedResult = _proto(_k32, "GetOverlappedResult", wintypes.BOOL, wintypes.HANDLE, _LPOV,
                                  _LPDWORD, wintypes.BOOL)
    _WaitForSingleObject = _proto(_k32, "WaitForSingleObject", wintypes.DWORD, wintypes.HANDLE,
                                  wintypes.DWORD)
    _CancelIoEx = _proto(_k32, "CancelIoEx", wintypes.BOOL, wintypes.HANDLE, _LPOV)
    _CloseHandle = _proto(_k32, "CloseHandle", wintypes.BOOL, wintypes.HANDLE)
    _SetupComm = _proto(_k32, "SetupComm", wintypes.BOOL, wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD)
    _GetCommState = _proto(_k32, "GetCommState", wintypes.BOOL, wintypes.HANDLE, ctypes.POINTER(DCB))
    _SetCommState = _proto(_k32, "SetCommState", wintypes.BOOL, wintypes.HANDLE, ctypes.POINTER(DCB))
    _SetCommTimeouts = _proto(_k32, "SetCommTimeouts", wintypes.BOOL, wintypes.HANDLE,
                              ctypes.POINTER(COMMTIMEOUTS))
    _EscapeCommFunction = _proto(_k32, "EscapeCommFunction", wintypes.BOOL, wintypes.HANDLE, wintypes.DWORD)
    _GetCommModemStatus = _proto(_k32, "GetCommModemStatus", wintypes.BOOL, wintypes.HANDLE, _LPDWORD)
    _PurgeComm = _proto(_k32, "PurgeComm", wintypes.BOOL, wintypes.HANDLE, wintypes.DWORD)
    _ClearCommError = _proto(_k32, "ClearCommError", wintypes.BOOL, wintypes.HANDLE, _LPDWORD,
                             ctypes.c_void_p)

    def _win_error(name, what, code=None):
        if code is None:
            code = ctypes.get_last_error()
        return DeviceError("%s: %s failed: %s (Windows error %d)" %
                           (name, what, ctypes.FormatError(code).strip(), code))

    def _new_event():
        event = _CreateEventW(None, True, False, None)
        if not event:
            raise _win_error("bridge", "CreateEvent")
        return event

    def _ms(seconds):
        return max(0, min(int(seconds * 1000), 0x7FFFFFFF))

    class Win32Stream:
        """Overlapped ReadFile/WriteFile on a handle (the COM port; a named
        pipe in the tests), so the reading and writing threads never wait on
        each other."""

        def __init__(self, path, name=None):
            self.name = name or path
            self.handle = None
            handle = _CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, None)
            if not handle or handle == INVALID_HANDLE_VALUE:
                code = ctypes.get_last_error()
                if code in (ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND):
                    raise DeviceError("%s is not there (unplugged?)" % self.name)
                if code in (ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION):
                    raise DeviceError("%s is in use by another program (sc64deployer, UNFLoader, a "
                                      "serial terminal?): close it" % self.name)
                raise _win_error(self.name, "opening", code)
            self.handle = handle
            self._read_event = _new_event()
            self._write_event = _new_event()
            self._read_buffer = ctypes.create_string_buffer(READ_CHUNK)

        def read(self, size, timeout):
            """what has come (up to size bytes), b"" if nothing in timeout"""
            size = max(1, min(size, READ_CHUNK))
            overlapped = OVERLAPPED()
            overlapped.hEvent = self._read_event
            done = wintypes.DWORD(0)
            if not _ReadFile(self.handle, self._read_buffer, size, None, ctypes.byref(overlapped)):
                code = ctypes.get_last_error()
                if code != ERROR_IO_PENDING:
                    raise _win_error(self.name, "reading", code)
                if _WaitForSingleObject(self._read_event, _ms(timeout)) != WAIT_OBJECT_0:
                    _CancelIoEx(self.handle, ctypes.byref(overlapped))
            if not _GetOverlappedResult(self.handle, ctypes.byref(overlapped), ctypes.byref(done), True):
                code = ctypes.get_last_error()
                if code != ERROR_OPERATION_ABORTED:
                    raise _win_error(self.name, "reading", code)
            return ctypes.string_at(self._read_buffer, done.value) if done.value else b""

        def write(self, data, stop=None, on_stall=None):
            """all of data written (True), or False if stop was set first"""
            data = bytes(data)
            if not data:
                return True
            buffer = ctypes.create_string_buffer(data, len(data))
            base = ctypes.addressof(buffer)
            offset = 0
            while offset < len(data):
                overlapped = OVERLAPPED()
                overlapped.hEvent = self._write_event
                done = wintypes.DWORD(0)
                stalled = False
                if not _WriteFile(self.handle, ctypes.c_void_p(base + offset), len(data) - offset, None,
                                  ctypes.byref(overlapped)):
                    code = ctypes.get_last_error()
                    if code != ERROR_IO_PENDING:
                        raise _win_error(self.name, "writing", code)
                    waited = 0.0
                    while _WaitForSingleObject(self._write_event, 250) != WAIT_OBJECT_0:
                        waited += 0.25
                        if stop is not None and stop.is_set():
                            _CancelIoEx(self.handle, ctypes.byref(overlapped))
                            _GetOverlappedResult(self.handle, ctypes.byref(overlapped), ctypes.byref(done), True)
                            return False
                        if on_stall is not None and not stalled and waited >= STALL_SECONDS:
                            stalled = True
                            on_stall(True)
                if not _GetOverlappedResult(self.handle, ctypes.byref(overlapped), ctypes.byref(done), True):
                    raise _win_error(self.name, "writing")
                if stalled:
                    on_stall(False)
                offset += done.value
                if not done.value and stop is not None and stop.is_set():
                    return False
            return True

        def close(self):
            if self.handle:
                _CancelIoEx(self.handle, None)
                _CloseHandle(self.handle)
                _CloseHandle(self._read_event)
                _CloseHandle(self._write_event)
                self.handle = None

    def normalize_port(port):
        port = port.strip().upper()
        if port.startswith("\\\\.\\"):
            port = port[4:]
        if port.isdigit():
            port = "COM" + port
        return port

    class ComPortDevice(Win32Stream):
        """The SC64 as a COM port (FTDI's VCP driver)."""

        kind = "COM"

        def __init__(self, port):
            port = normalize_port(port)
            Win32Stream.__init__(self, "\\\\.\\" + port, port)
            try:
                self._configure()
            except Exception:
                self.close()
                raise

        def _configure(self):
            _SetupComm(self.handle, 65536, 65536)
            dcb = DCB()
            dcb.DCBlength = ctypes.sizeof(DCB)
            if not _GetCommState(self.handle, ctypes.byref(dcb)):
                raise _win_error(self.name, "GetCommState")
            # (the cart's FIFO ignores the baud rate; binary, 8N1, no flow
            # control, DTR and RTS ours, no DSR sensitivity, no NUL stripping)
            dcb.BaudRate = 115200
            dcb.ByteSize = 8
            dcb.Parity = 0
            dcb.StopBits = 0
            dcb.flags = 0x0001
            if dcb.XonChar == dcb.XoffChar:
                dcb.XonChar, dcb.XoffChar = b"\x11", b"\x13"
            if not _SetCommState(self.handle, ctypes.byref(dcb)):
                raise _win_error(self.name, "SetCommState")
            timeouts = COMMTIMEOUTS(MAXDWORD, MAXDWORD, COM_READ_TIMEOUT_MS, 0, 0)
            if not _SetCommTimeouts(self.handle, ctypes.byref(timeouts)):
                raise _win_error(self.name, "SetCommTimeouts")
            errors = wintypes.DWORD(0)
            _ClearCommError(self.handle, ctypes.byref(errors), None)
            self.purge()

        def set_dtr(self, on):
            if not _EscapeCommFunction(self.handle, SETDTR if on else CLRDTR):
                raise _win_error(self.name, "setting DTR")

        def dsr(self):
            status = wintypes.DWORD(0)
            if not _GetCommModemStatus(self.handle, ctypes.byref(status)):
                raise _win_error(self.name, "reading DSR")
            return bool(status.value & MS_DSR_ON)

        def purge(self):
            if not _PurgeComm(self.handle, PURGE_RXCLEAR | PURGE_TXCLEAR):
                raise _win_error(self.name, "PurgeComm")

    # ---------- the COM ports present (SetupAPI; the SERIALCOMM map if not)

    class GUID(ctypes.Structure):
        _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                    ("Data4", ctypes.c_ubyte * 8)]

    class SP_DEVINFO_DATA(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("ClassGuid", GUID), ("DevInst", wintypes.DWORD),
                    ("Reserved", ctypes.c_size_t)]

    # GUID_DEVCLASS_PORTS {4D36E978-E325-11CE-BFC1-08002BE10318}
    _PORTS_CLASS = GUID(0x4D36E978, 0xE325, 0x11CE, (ctypes.c_ubyte * 8)(
        0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18))
    DIGCF_PRESENT = 0x2
    DICS_FLAG_GLOBAL = 1
    DIREG_DEV = 1
    KEY_READ = 0x20019
    SPDRP_FRIENDLYNAME = 0x0C

    def _setupapi_ports(device_class=None, com_only=True):
        """(device_class/com_only: the tests list another class with it)"""
        setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
        advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
        p_devinfo = ctypes.POINTER(SP_DEVINFO_DATA)
        get_class_devs = _proto(setupapi, "SetupDiGetClassDevsW", ctypes.c_void_p, ctypes.POINTER(GUID),
                                wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD)
        enum_info = _proto(setupapi, "SetupDiEnumDeviceInfo", wintypes.BOOL, ctypes.c_void_p,
                           wintypes.DWORD, p_devinfo)
        instance_id = _proto(setupapi, "SetupDiGetDeviceInstanceIdW", wintypes.BOOL, ctypes.c_void_p,
                             p_devinfo, wintypes.LPWSTR, wintypes.DWORD, _LPDWORD)
        registry_property = _proto(setupapi, "SetupDiGetDeviceRegistryPropertyW", wintypes.BOOL,
                                   ctypes.c_void_p, p_devinfo, wintypes.DWORD, _LPDWORD, ctypes.c_void_p,
                                   wintypes.DWORD, _LPDWORD)
        open_key = _proto(setupapi, "SetupDiOpenDevRegKey", ctypes.c_void_p, ctypes.c_void_p, p_devinfo,
                          wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD)
        destroy = _proto(setupapi, "SetupDiDestroyDeviceInfoList", wintypes.BOOL, ctypes.c_void_p)
        close_key = _proto(advapi32, "RegCloseKey", wintypes.LONG, ctypes.c_void_p)

        devices = get_class_devs(ctypes.byref(device_class or _PORTS_CLASS), None, None, DIGCF_PRESENT)
        if not devices or devices == INVALID_HANDLE_VALUE:
            raise _win_error("SetupAPI", "SetupDiGetClassDevs")
        ports = []
        try:
            index = 0
            while True:
                info = SP_DEVINFO_DATA()
                info.cbSize = ctypes.sizeof(SP_DEVINFO_DATA)
                if not enum_info(devices, index, ctypes.byref(info)):
                    break
                index += 1
                text = ctypes.create_unicode_buffer(512)
                hwid = text.value if instance_id(devices, ctypes.byref(info), text, 512, None) else ""
                name = ""
                if registry_property(devices, ctypes.byref(info), SPDRP_FRIENDLYNAME, None, text,
                                     ctypes.sizeof(text), None):
                    name = text.value
                port, latency = "", None
                key = open_key(devices, ctypes.byref(info), DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ)
                if key and key != INVALID_HANDLE_VALUE:
                    try:
                        try:
                            port = str(winreg.QueryValueEx(key, "PortName")[0])
                        except OSError:
                            pass
                        try:
                            latency = int(winreg.QueryValueEx(key, "LatencyTimer")[0])
                        except (OSError, ValueError, TypeError):
                            pass
                    finally:
                        close_key(key)
                if port.upper().startswith("COM") or not com_only:
                    ports.append({"port": port.upper(), "hwid": hwid, "name": name, "latency": latency})
        finally:
            destroy(devices)
        return ports

    def _serialcomm_ports():
        ports = []
        try:
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
        except OSError:
            return ports
        with key:
            index = 0
            while True:
                try:
                    device, port, _ = winreg.EnumValue(key, index)
                except OSError:
                    break
                index += 1
                ports.append({"port": str(port).upper(), "hwid": "", "name": device, "latency": None})
        return ports

    def list_com_ports():
        """the COM ports present: port, hwid (device instance), name, latency"""
        try:
            return _setupapi_ports(), None
        except Exception as error:  # (SetupAPI unusable: the plain map)
            return _serialcomm_ports(), str(error)

    # ---------- FTDI's D2XX driver (ftd2xx.dll), when installed

    FT_OK = 0
    FT_FLAGS_OPENED = 1
    FT_PURGE_RX = 1
    FT_PURGE_TX = 2
    FT_EVENT_RXCHAR = 1
    FT_STATUS_NAMES = {1: "invalid handle", 2: "device not found", 3: "device not opened", 4: "I/O error",
                       5: "insufficient resources", 6: "invalid parameter", 7: "invalid baud rate",
                       18: "other error"}

    _d2xx = None

    def load_d2xx():
        global _d2xx
        if _d2xx is not None:
            return _d2xx or None
        try:
            dll = ctypes.WinDLL("ftd2xx.dll")
        except OSError:
            _d2xx = False
            return None
        ulong, handle, p_ulong = ctypes.c_ulong, ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)
        _proto(dll, "FT_CreateDeviceInfoList", ulong, p_ulong)
        _proto(dll, "FT_GetDeviceInfoDetail", ulong, ulong, p_ulong, p_ulong, p_ulong, p_ulong,
               ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(handle))
        _proto(dll, "FT_Open", ulong, ctypes.c_int, ctypes.POINTER(handle))
        _proto(dll, "FT_Close", ulong, handle)
        _proto(dll, "FT_ResetDevice", ulong, handle)
        _proto(dll, "FT_SetTimeouts", ulong, handle, ulong, ulong)
        _proto(dll, "FT_SetLatencyTimer", ulong, handle, ctypes.c_ubyte)
        _proto(dll, "FT_SetDtr", ulong, handle)
        _proto(dll, "FT_ClrDtr", ulong, handle)
        _proto(dll, "FT_GetModemStatus", ulong, handle, p_ulong)
        _proto(dll, "FT_Purge", ulong, handle, ulong)
        _proto(dll, "FT_GetQueueStatus", ulong, handle, p_ulong)
        _proto(dll, "FT_Read", ulong, handle, ctypes.c_void_p, ulong, p_ulong)
        _proto(dll, "FT_Write", ulong, handle, ctypes.c_void_p, ulong, p_ulong)
        _proto(dll, "FT_SetEventNotification", ulong, handle, ulong, ctypes.c_void_p)
        _d2xx = dll
        return dll

    def list_d2xx_devices():
        """FTDI devices D2XX sees: index, id, serial, description, opened"""
        dll = load_d2xx()
        if dll is None:
            return None
        count = ctypes.c_ulong(0)
        if dll.FT_CreateDeviceInfoList(ctypes.byref(count)) != FT_OK:
            return []
        devices = []
        for index in range(count.value):
            flags, kind, ident, location = (ctypes.c_ulong() for _ in range(4))
            serial = ctypes.create_string_buffer(64)
            description = ctypes.create_string_buffer(128)
            handle = ctypes.c_void_p()
            if dll.FT_GetDeviceInfoDetail(index, ctypes.byref(flags), ctypes.byref(kind), ctypes.byref(ident),
                                          ctypes.byref(location), serial, description,
                                          ctypes.byref(handle)) != FT_OK:
                continue
            devices.append({"index": index, "id": ident.value,
                            "serial": serial.value.decode("ascii", "replace"),
                            "description": description.value.decode("ascii", "replace"),
                            "opened": bool(flags.value & FT_FLAGS_OPENED)})
        return devices

    class D2xxDevice:
        """The SC64 through FTDI's D2XX driver."""

        kind = "D2XX"

        def __init__(self, index, name):
            self.dll = load_d2xx()
            self.name = name
            self.handle = ctypes.c_void_p()
            self.event = None
            status = self.dll.FT_Open(index, ctypes.byref(self.handle))
            if status != FT_OK:
                self.handle = None
                raise DeviceError("%s: FT_Open failed (%s): another program (sc64deployer, UNFLoader) may have "
                                  "it open" % (name, FT_STATUS_NAMES.get(status, status)))
            try:
                self._check(self.dll.FT_ResetDevice(self.handle), "FT_ResetDevice")
                self._check(self.dll.FT_SetTimeouts(self.handle, 500, 500), "FT_SetTimeouts")
                # (2 ms instead of 16: the N64's small packets come sooner; not kept)
                self.dll.FT_SetLatencyTimer(self.handle, 2)
                self.event = _CreateEventW(None, False, False, None)
                if self.event:
                    self.dll.FT_SetEventNotification(self.handle, FT_EVENT_RXCHAR, self.event)
                self._buffer = ctypes.create_string_buffer(READ_CHUNK)
                self.purge()
            except Exception:
                self.close()
                raise

        def _check(self, status, what):
            if status != FT_OK:
                raise DeviceError("%s: %s failed (%s)" % (self.name, what, FT_STATUS_NAMES.get(status, status)))

        def _queued(self):
            count = ctypes.c_ulong(0)
            self._check(self.dll.FT_GetQueueStatus(self.handle, ctypes.byref(count)), "FT_GetQueueStatus")
            return count.value

        def read(self, size, timeout):
            count = self._queued()
            if not count:
                if self.event:
                    _WaitForSingleObject(self.event, _ms(min(timeout, 0.1)))
                else:
                    time.sleep(0.002)
                count = self._queued()
                if not count:
                    return b""
            count = min(count, size, READ_CHUNK)
            done = ctypes.c_ulong(0)
            self._check(self.dll.FT_Read(self.handle, self._buffer, count, ctypes.byref(done)), "FT_Read")
            return ctypes.string_at(self._buffer, done.value) if done.value else b""

        def write(self, data, stop=None, on_stall=None):
            data = bytes(data)
            buffer = ctypes.create_string_buffer(data, len(data)) if data else None
            base = ctypes.addressof(buffer) if data else 0
            offset, stalled_since, stalled = 0, None, False
            while offset < len(data):
                if stop is not None and stop.is_set():
                    return False
                done = ctypes.c_ulong(0)
                self._check(self.dll.FT_Write(self.handle, ctypes.c_void_p(base + offset), len(data) - offset,
                                              ctypes.byref(done)), "FT_Write")
                offset += done.value
                if done.value:
                    stalled_since = None
                    if stalled:
                        stalled = False
                        on_stall(False)
                elif stalled_since is None:
                    stalled_since = time.time()
                elif on_stall is not None and not stalled and time.time() - stalled_since >= STALL_SECONDS:
                    stalled = True
                    on_stall(True)
            if stalled:
                on_stall(False)
            return True

        def set_dtr(self, on):
            if on:
                self._check(self.dll.FT_SetDtr(self.handle), "FT_SetDtr")
            else:
                self._check(self.dll.FT_ClrDtr(self.handle), "FT_ClrDtr")

        def dsr(self):
            status = ctypes.c_ulong(0)
            self._check(self.dll.FT_GetModemStatus(self.handle, ctypes.byref(status)), "FT_GetModemStatus")
            return bool(status.value & 0x20)

        def purge(self):
            self._check(self.dll.FT_Purge(self.handle, FT_PURGE_RX | FT_PURGE_TX), "FT_Purge")

        def close(self):
            if self.handle:
                self.dll.FT_Close(self.handle)
                self.handle = None
            if self.event:
                _CloseHandle(self.event)
                self.event = None


def ftdi_serial(hwid):
    """the serial number in an FTDI port's instance id
    (FTDIBUS\\VID_0403+PID_6014+SC64xxxxA\\0000 -> SC64xxxx), or "" """
    parts = hwid.split("\\")
    if len(parts) >= 2 and "+" in parts[1]:
        serial = parts[1].split("+")[-1]
        return serial[:-1] if len(serial) > 1 and serial.endswith("A") else serial
    if len(parts) >= 3 and parts[0].upper() == "USB":
        return parts[2]
    return ""


def is_sc64_hwid(hwid):
    upper = hwid.upper()
    return "VID_%04X" % SC64_VID in upper and "PID_%04X" % SC64_PID in upper


def sc64_port_candidates(ports):
    """the COM ports worth asking: FTDI 0403:6014 with an "SC64" serial
    first; else other 0403:6014 ports; else (no device ids known) FTDI's
    VCP ports"""
    named, others, unknown = [], [], []
    for port in ports:
        if is_sc64_hwid(port["hwid"]):
            if ftdi_serial(port["hwid"]).upper().startswith("SC64") or "SC64" in port["name"].upper():
                named.append(port)
            else:
                others.append(port)
        elif not port["hwid"] and "VCP" in port["name"].upper():
            unknown.append(port)
    return named or others or unknown


# ------------------------------------------------------------------ the cart

class Sc64Cart:
    """An SC64 on a byte device (COM port, D2XX, or a test's fake), with
    the reply/packet parser its session carries on with."""

    def __init__(self, device, label=None):
        self.device = device
        self.label = label or getattr(device, "name", "SC64")
        self.parser = Sc64Parser()
        self.early = []
        self.firmware = None

    def close(self):
        self.device.close()

    def reset_and_sync(self, timeout=2.0):
        """UNFLoader's device_reset_and_sync_sc64 (sc64deployer's reset)"""
        device = self.device
        device.set_dtr(True)
        deadline = time.time() + timeout
        while True:
            device.purge()
            time.sleep(0.01)
            if device.dsr():
                break
            if time.time() > deadline:
                device.set_dtr(False)
                raise DeviceError("%s: no answer to the reset (DSR stayed off): not an SC64, or its USB is "
                                  "stuck (unplug it and plug it in again)" % self.label)
        device.purge()
        device.set_dtr(False)
        deadline = time.time() + timeout
        while True:
            time.sleep(0.01)
            if not device.dsr():
                break
            if time.time() > deadline:
                raise DeviceError("%s: the reset did not end (DSR stayed on)" % self.label)
        self.parser = Sc64Parser()
        self.early = []

    def command(self, cmd, arg1=0, arg2=0, data=b"", timeout=1.5):
        """(ok, reply data) for a command that answers; packets that come
        first are kept in .early for the session"""
        self.device.write(encode_command(cmd, arg1, arg2, data))
        deadline = time.time() + timeout
        reply = None
        while reply is None:
            left = deadline - time.time()
            if left <= 0:
                raise DeviceError("%s: no reply to command '%s'" % (self.label, cmd.decode("ascii", "replace")))
            for kind, ident, payload in self.parser.feed(self.device.read(READ_CHUNK, min(left, 0.25))):
                if reply is None and kind in (b"CMP", b"ERR") and ident == cmd[0]:
                    reply = (kind == b"CMP", payload)
                else:
                    self.early.append((kind, ident, payload))
        return reply

    def identify(self):
        ok, payload = self.command(CMD_IDENTIFIER_GET)
        return ok and len(payload) == 4 and payload == SC64_IDENTIFIER

    def firmware_version(self):
        try:
            ok, payload = self.command(CMD_VERSION_GET)
        except DeviceError:
            return None
        if not ok or len(payload) < 8:
            return None
        self.firmware = struct.unpack(">HHI", payload[:8])
        return self.firmware


def open_and_identify(device_opener, label):
    """a cart ready for the bridge, or a DeviceError saying why not"""
    device = device_opener()
    cart = Sc64Cart(device, label)
    try:
        cart.reset_and_sync()
        if not cart.identify():
            raise DeviceError("%s answers, but not as a SummerCart64 (no \"SCv2\")" % label)
        cart.firmware_version()
        return cart
    except BaseException:
        cart.close()
        raise


def find_cart(port=None, use_d2xx=False):
    """(cart, problems): the first SC64 that answers, or None and why not"""
    if os.name != "nt":
        return None, ["this bridge talks to the cart with Windows' COM ports or FTDI's D2XX: run it on Windows"]
    candidates, problems = [], []
    if port:
        name = normalize_port(port)
        candidates.append((name, lambda name=name: ComPortDevice(name)))
    else:
        if not use_d2xx:
            ports, _ = list_com_ports()
            for found in sc64_port_candidates(ports):
                candidates.append((found["port"], lambda name=found["port"]: ComPortDevice(name)))
        if not candidates:
            devices = list_d2xx_devices()
            if devices is None:
                if use_d2xx:
                    problems.append("FTDI's D2XX driver (ftd2xx.dll) is not installed")
            else:
                for device in devices:
                    if device["id"] == ((SC64_VID << 16) | SC64_PID) and \
                            device["description"].upper().startswith("SC64"):
                        label = "D2XX device %d (%s %s)" % (device["index"], device["description"], device["serial"])
                        if device["opened"]:
                            problems.append("%s is in use by another program (sc64deployer, UNFLoader?)" % label)
                            continue
                        candidates.append((label, lambda index=device["index"], label=label: D2xxDevice(index, label)))
    if not candidates and not problems:
        problems.append("no SummerCart64 on USB")
    for label, opener in candidates:
        try:
            return open_and_identify(opener, label), problems
        except DeviceError as error:
            problems.append(str(error))
    return None, problems


def latency_tip(port_name):
    """a line about the COM port's FTDI latency timer, if it is slow"""
    if os.name != "nt" or not port_name.upper().startswith("COM"):
        return None
    ports, _ = list_com_ports()
    for port in ports:
        if port["port"] == port_name.upper() and port["latency"] and port["latency"] > 4:
            return ("tip: %s's FTDI latency timer is %d ms; 1 ms (Device Manager > Ports > %s > Port Settings > "
                    "Advanced > Latency Timer) sends the N64's packets sooner" %
                    (port_name, port["latency"], port_name))
    return None


# ------------------------------------------------------------------ the Link

class LinkClient:
    """The TCP connection to Halo 64 Link: made, and made again when it
    drops, every second; what comes is cut into packets for on_packet."""

    def __init__(self, host, port, on_packet, stop, log=log, stats=None, retry_seconds=1.0):
        self.host, self.port = host, port
        self.on_packet = on_packet
        self.stop = stop
        self.log = log
        self.stats = stats or Stats()
        self.retry_seconds = retry_seconds
        self._lock = threading.Lock()
        self._send_lock = threading.Lock()
        self._sock = None
        self.thread = threading.Thread(target=self._run, name="link", daemon=True)

    @property
    def connected(self):
        return self._sock is not None

    def start(self):
        self.thread.start()

    def _drop(self, sock):
        with self._lock:
            if self._sock is sock:
                self._sock = None
        try:
            sock.close()
        except OSError:
            pass

    def _run(self):
        waiting_said = False
        address = "%s:%d" % (self.host, self.port)
        while not self.stop.is_set():
            try:
                sock = socket.create_connection((self.host, self.port), timeout=2.0)
            except OSError:
                if not waiting_said:
                    self.log("Waiting for Halo 64 Link on %s..." % address)
                    waiting_said = True
                self.stop.wait(self.retry_seconds)
                continue
            waiting_said = False
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.settimeout(0.5)
            parser = DmaParser()
            with self._lock:
                self._sock = sock
            self.log("Connected to Halo 64 Link on %s" % address)
            reason = "closed by the Link"
            while not self.stop.is_set():
                try:
                    data = sock.recv(65536)
                except socket.timeout:
                    continue
                except OSError as error:
                    reason = "lost (%s)" % error
                    break
                if not data:
                    break
                for datatype, payload in parser.feed(data):
                    self.stats.add(link_in=1, link_in_bytes=len(payload))
                    self.on_packet(datatype, payload)
            dropped = self._sock is sock
            self._drop(sock)
            if not self.stop.is_set():
                if dropped:
                    self.log("Halo 64 Link connection %s; connecting again..." % reason)
                self.stop.wait(self.retry_seconds)

    def send(self, data):
        """data sent to the Link (True), or not connected (False)"""
        with self._lock:
            sock = self._sock
        if sock is None:
            return False
        view = memoryview(data)
        with self._send_lock:
            while view:
                try:
                    sent = sock.send(view)
                except socket.timeout:
                    if self.stop.is_set():
                        return False
                    continue
                except OSError as error:
                    self.log("Halo 64 Link connection lost while sending (%s)" % error)
                    self._drop(sock)
                    return False
                view = view[sent:]
        return True

    def close(self):
        self.stop.set()
        with self._lock:
            sock, self._sock = self._sock, None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        if self.thread.is_alive():
            self.thread.join(3.0)


# ------------------------------------------------------------------ the relay

class CartSession:
    """One opened cart's two threads: the cart's packets to the Link, the
    Link's packets (queued) to the cart. A device error ends the session
    (failure says why) so the bridge can look for the cart again."""

    def __init__(self, cart, link, queue, stats, notes, log=log, verbose=False):
        self.cart = cart
        self.device = cart.device
        self.link = link
        self.queue = queue
        self.stats = stats
        self.notes = notes
        self.log = log
        self.verbose = verbose
        self.stop = threading.Event()
        self.failure = None
        self._said = set()
        self.threads = [threading.Thread(target=self._guard, args=(self._read_cart,), name="cart-read", daemon=True),
                        threading.Thread(target=self._guard, args=(self._write_cart,), name="cart-write", daemon=True)]

    def start(self):
        for thread in self.threads:
            thread.start()

    @property
    def alive(self):
        return not self.stop.is_set()

    def _guard(self, body):
        try:
            body()
        except DeviceError as error:
            self._fail(str(error))
        except Exception as error:  # (a bug: say it, and start the cart again)
            self._fail("%s: %r" % (threading.current_thread().name, error))

    def _fail(self, why):
        if self.failure is None:
            self.failure = why
        self.stop.set()

    def _say_once(self, key, message):
        if key not in self._said:
            self._said.add(key)
            self.log(message)

    # ---------- cart -> Link

    def _read_cart(self):
        for message in self.cart.early:
            self._handle(*message)
        self.cart.early = []
        parser = self.cart.parser
        while not self.stop.is_set():
            data = self.device.read(READ_CHUNK, 0.5)
            if not data:
                continue
            for message in parser.feed(data):
                self._handle(*message)
            if parser.skipped:
                self.log("The cart's USB stream was out of step: %d bytes skipped" % parser.skipped)
                self.stats.add(cart_skipped=parser.skipped)
                parser.skipped = 0

    def _handle(self, kind, ident, payload):
        if kind == b"PKT" and ident == PACKET_DEBUG:
            datatype, data, note = split_debug_packet(payload)
            if note:
                self._say_once(("note", note), "The cart sent " + note)
            if data is None:
                self.stats.add(n64_bad=1)
                return
            delivered = self.link.send(encode_link_packet(datatype, data))
            if delivered:
                self.stats.add(n64_out=1, n64_out_bytes=len(data))
            else:
                self.stats.add(n64_dropped=1)
            self.notes.from_n64(datatype, data, delivered)
        elif kind == b"PKT" and ident == PACKET_ISV:
            if self.verbose:
                self.log("N64 IS-Viewer: " + payload.decode("ascii", "replace").rstrip())
        elif kind == b"PKT":
            self._say_once(("pkt", ident), "The cart sent a packet of type '%s' (%d bytes): ignored" %
                           (chr(ident) if 32 <= ident < 127 else "?", len(payload)))
        else:
            self._say_once((kind, ident), "The cart answered command '%s' with %s (%d bytes; not shown again)" % (
                chr(ident) if 32 <= ident < 127 else "?", kind.decode("ascii"), len(payload)))

    # ---------- Link -> cart

    def _on_stall(self, stalled):
        if stalled:
            self.log("The cart has not taken the Link's data for %d s: is Halo 64 Online running on the N64?" %
                     STALL_SECONDS)
        else:
            self.log("The cart is taking data again")

    def _write_cart(self):
        while not self.stop.is_set():
            batch = self.queue.get_batch(0.25)
            if not batch:
                continue
            data = b"".join(encode_debug_write(datatype, payload) for datatype, payload in batch)
            if not self.device.write(data, self.stop, self._on_stall):
                return
            self.stats.add(n64_in=len(batch), n64_in_bytes=sum(len(payload) for _, payload in batch))
            for datatype, payload in batch:
                self.notes.to_n64(datatype, payload)

    def close(self):
        self.stop.set()
        for thread in self.threads:
            if thread.is_alive():
                thread.join(3.0)


def stats_line(now, before, seconds):
    def delta(key):
        return now.get(key, 0) - before.get(key, 0)

    line = "Last %d s: N64 -> Link %d packets (%s), Link -> N64 %d packets (%s)" % (
        round(seconds), delta("n64_out"), size_text(delta("n64_out_bytes")), delta("n64_in"),
        size_text(delta("n64_in_bytes")))
    extra = []
    if delta("n64_dropped"):
        extra.append("%d N64 packets with no Link to take them" % delta("n64_dropped"))
    if delta("link_dropped"):
        extra.append("%d Link packets dropped (the cart was not taking them)" % delta("link_dropped"))
    if delta("n64_bad"):
        extra.append("%d bad cart packets" % delta("n64_bad"))
    return line + ("   [" + "; ".join(extra) + "]" if extra else "")


def run_bridge(link_host, link_port, open_cart, stop=None, log=log, verbose=False, report_seconds=5.0,
               cart_retry_seconds=2.0, link_retry_seconds=1.0):
    """The bridge: the Link's connection kept up, the cart found (and found
    again when it goes), packets relayed both ways until stop is set (or
    Ctrl+C). open_cart() -> (Sc64Cart or None, [problems])."""
    stop = stop or threading.Event()
    stats = Stats()
    notes = TrafficNotes(log, verbose)
    queue = PacketQueue()

    def from_link(datatype, payload):
        before = queue.dropped
        queue.put(datatype, payload)
        if queue.dropped != before:
            stats.add(link_dropped=queue.dropped - before)

    link = LinkClient(link_host, link_port, from_link, stop, log, stats, link_retry_seconds)
    session = None
    last_problems = None
    try:
        link.start()
        while not stop.is_set():
            try:
                cart, problems = open_cart()
            except DeviceError as error:
                cart, problems = None, [str(error)]
            except Exception as error:  # (a bug in the search: say it, keep trying)
                cart, problems = None, ["looking for the cart failed: %r" % error]
            if cart is None:
                if problems != last_problems:
                    log("Looking for the SummerCart64: " + "; ".join(problems))
                    log("  (trying again every %d s; Ctrl+C to quit)" % cart_retry_seconds)
                    last_problems = problems
                deadline = time.time() + cart_retry_seconds
                while not stop.is_set() and time.time() < deadline:
                    time.sleep(0.1)
                continue
            last_problems = None
            firmware = ""
            if cart.firmware:
                firmware = " (firmware %d.%d.%d)" % cart.firmware
                if cart.firmware[0] != 2:
                    firmware += ": not firmware 2.x, untested"
                elif cart.firmware[1] < 14:
                    firmware += ": older than 2.14, which UNFLoader asks for; if nothing comes through, " \
                                "update it (sc64deployer firmware update)"
            log("SummerCart64 found on %s%s" % (cart.label, firmware))
            tip = latency_tip(cart.label) if getattr(cart.device, "kind", "") == "COM" else None
            if tip:
                log(tip)
            stale = queue.clear()
            if stale and verbose:
                log("(%d old Link packets dropped)" % stale)
            session = CartSession(cart, link, queue, stats, notes, log, verbose)
            reported, last_report = stats.snapshot(), time.time()
            session.start()
            while session.alive and not stop.is_set():
                time.sleep(0.1)
                if time.time() - last_report >= report_seconds:
                    now = stats.snapshot()
                    if now != reported:
                        log(stats_line(now, reported, time.time() - last_report))
                    reported, last_report = now, time.time()
            session.close()
            cart.close()
            if session.failure and not stop.is_set():
                log("Lost the cart: %s" % session.failure)
                log("Looking for it again...")
                deadline = time.time() + cart_retry_seconds
                while not stop.is_set() and time.time() < deadline:
                    time.sleep(0.1)
            session = None
    except KeyboardInterrupt:
        log("Stopping...")
    finally:
        stop.set()
        if session is not None:
            session.close()
            session.cart.close()
        link.close()
    return 0


# ------------------------------------------------------------------ main

def parse_link(text):
    host, _, port = text.rpartition(":")
    if not host or not port.isdigit() or not 0 < int(port) < 65536:
        raise argparse.ArgumentTypeError("expected host:port, such as 127.0.0.1:6466")
    return host, int(port)


def list_devices():
    if os.name != "nt":
        print("Listing needs Windows.")
        return 1
    ports, problem = list_com_ports()
    if problem:
        print("(SetupAPI failed: %s; the plain COM port map is shown)" % problem)
    print("COM ports:")
    if not ports:
        print("  none")
    candidates = sc64_port_candidates(ports)
    for port in ports:
        mark = "  <- SummerCart64?" if port in candidates else ""
        latency = ", latency %d ms" % port["latency"] if port["latency"] else ""
        print("  %-6s %s [%s%s]%s" % (port["port"], port["name"], port["hwid"] or "?", latency, mark))
    devices = list_d2xx_devices()
    if devices is None:
        print("FTDI D2XX driver: not installed")
    else:
        print("FTDI D2XX devices:")
        if not devices:
            print("  none")
        for device in devices:
            print("  %d: %04X:%04X \"%s\" serial \"%s\"%s" % (
                device["index"], device["id"] >> 16, device["id"] & 0xFFFF, device["description"],
                device["serial"], " (in use)" if device["opened"] else ""))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Halo 64 Online on a SummerCart64: relays the cart's USB to Halo 64 Link.")
    parser.add_argument("--port", help="the cart's COM port (COM5); found by itself if left out")
    parser.add_argument("--d2xx", action="store_true", help="use FTDI's D2XX driver (ftd2xx.dll), not a COM port")
    parser.add_argument("--link", type=parse_link, default=parse_link(DEFAULT_LINK),
                        help="Halo 64 Link's address (default %s)" % DEFAULT_LINK)
    parser.add_argument("--list", action="store_true", help="list the COM ports and FTDI devices, then quit")
    parser.add_argument("--verbose", action="store_true", help="more detail (the N64's text output too)")
    parser.add_argument("--version", action="version", version="sc64_bridge " + BRIDGE_VERSION)
    args = parser.parse_args(argv)
    if args.list:
        return list_devices()
    if hasattr(signal, "SIGBREAK"):
        # (Ctrl+Break as Ctrl+C: a clean stop)
        def on_break(signum, frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGBREAK, on_break)
    log("Halo 64 Online SC64 bridge %s: SummerCart64 USB <-> Halo 64 Link (%s:%d). Ctrl+C stops it." %
        (BRIDGE_VERSION, args.link[0], args.link[1]))
    return run_bridge(args.link[0], args.link[1], lambda: find_cart(args.port, args.d2xx), verbose=args.verbose)


if __name__ == "__main__":
    sys.exit(main())
