# An emulated flashcart USB port for ares

For playing and testing Halo 64 Online without a flashcart. The N64 ROM
talks to the computer the way N64-NetLib does: libdragon's `usb.c`
(UNFLoader's USB library). `ares-usb.patch` makes ares emulate an
**EverDrive-64 X7**'s USB port and connects it to a TCP socket on the
computer, so a program there (Halo 64 Link) exchanges USB packets with the
running ROM as it would over the cable.

The patch applies to ares commit `9408cb43d4948fc3ea6e152a307a34348df3fe04`
(2026-10-06) with `git apply` (or `patch -p1`), and builds on Linux (GCC)
and Windows (MSVC) as ares does: see ares' own build instructions. With
`ARES_N64_USB` unset ares behaves exactly as before.

## Using it

`ARES_N64_USB` says where the port's bytes go:

| Value | What |
| --- | --- |
| `tcp:<host>:<port>` | ares listens; one program at a time connects (the Link's `HALO_LINK_CABLE=tcp:<host>:<port>`) |
| `connect:<host>:<port>` | ares connects to a program that listens, and tries again each second until it answers or after it goes (the Link's `HALO_LINK_CABLE=listen:<port>`); use a numeric address |
| `unfloader` | UNFLoader's own packets on localhost:48646 (its "Gopher64" TCP device), for UNFLoader in debug mode |

The bytes are the EverDrive's USB stream as `usb.c` makes it (protocol 2):
from the N64 `"DMA@"`, a big-endian header (type << 24 | size), the data,
`"CMPH"` (and a pad byte after it when the size is odd); to the N64
`"DMA@"`, the header, the data padded to an even size, `"CMPH"`. Received
packets go where the real cart puts them, cartridge space past the ROM
(`0x13800000`); a ROM over 56 MiB is overwritten there, as on the cart.

## What the patch changes

- `ares/n64/cartridge/everdrive.cpp` (new): the EverDrive-64 X7 as a PI
  device beside the IS-Viewer: its key, version, system and USB
  configuration registers (commands, busy, RXF#, TXE#, power), the 512-byte
  USB buffer, the writable SDRAM past the ROM, the two FIFOs, the socket
  (listen, or connect out with retries), the optional UNFLoader packets.
  Whole `usb.c` packets go out in one send (WSL's localhost relay delays
  small ones).
- `ares/n64/cartridge/everdrive-socket.cpp` (new): non-blocking TCP for
  Linux, macOS and Windows, in its own translation unit (Winsock's headers
  clash with nall's names).
- `cartridge.hpp/.cpp`: the device attached only when the port is on.
- `system.cpp`: the socket served once per frame.
- `ares/n64/CMakeLists.txt`: the new files.

## Testing the port: usb-echo

`usb-echo/usb-echo.c` (libdragon: `make` with `N64_INST` set) is a ROM that
finds the cart and echoes every packet back; `usb-echo/usb_client.py` is the
computer's side: it connects (or listens, `--listen`), checks packets of
every size from 1 to 65000 bytes, measures round trips and a burst.

```sh
ARES_N64_USB=tcp:127.0.0.1:6464 ares --system "Nintendo 64" usb-echo.z64 &
python3 usb-echo/usb_client.py --framing raw --port 6464
```
