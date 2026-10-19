# Halo 64 Online on a SummerCart64

`sc64_bridge.py` puts a real N64 with a SummerCart64 where ares' emulated
cart would be: it talks to the cart over its USB port and to Halo 64 Link
over TCP, passing the game's packets both ways. **Untested on a real cart
so far**: its protocol follows UNFLoader's SC64 code (`device_sc64.cpp`),
and `test_sc64_bridge.py` checks it against a simulated cart.

Windows, Python 3 with only its standard library (the COM port through
ctypes; FTDI's `ftd2xx.dll` is used if there is no COM port, `--d2xx`).

1. Put `halo-online.z64` on the SummerCart64 (its SD card, or
   `sc64deployer upload --save-type eeprom4k halo-online.z64`). The save
   type is EEPROM 4 kbit.
2. Start Halo 64 Link waiting for the cable: `HALO_LINK_CABLE=listen:6466`
   (README.md, "Playing on the N64").
3. Wire the cart's USB port to the computer, close anything else using it
   (sc64deployer, UNFLoader), and run

   ```
   py -3 sc64_bridge.py --link 127.0.0.1:6466
   ```

   It finds the cart (FTDI 0403:6014 "SC64", answering `v` with `SCv2`;
   `--port COMn` picks one, `--list` lists them), resets it, connects to the
   Link (again each second until it answers) and prints what goes each way
   every few seconds.

How it translates: the cart's `PKT` `U` packets (a `(type << 24 | size)`
header and the data) go to the Link as `"DMA@"`, header, data, `"CMPH"`;
the Link's `"DMA@"` frames go to the cart as `CMD` `U` (type, size, the
data unpadded).

FTDI's latency timer (16 ms by default) adds that much to each read: set it
to 1-2 ms in the port's Device Manager properties (Advanced) for less input
lag.

`python test_sc64_bridge.py` runs the offline tests (no cart needed).
