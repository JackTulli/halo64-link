# Halo 64 Online on a SummerCart64

`sc64_bridge.py` puts a real N64 with a SummerCart64 where ares' emulated
cart would be: it talks to the cart over its USB port and to Halo 64 Link
over TCP, passing the game's packets both ways. **Untested on a real cart
so far**: its protocol follows UNFLoader's SC64 code (`device_sc64.cpp`),
and `test_sc64_bridge.py` checks it against a simulated cart.

The bridge runs on Windows, with Python 3 and only its standard library
(the COM port through ctypes; FTDI's `ftd2xx.dll` if there is no COM port,
`--d2xx`). The Link runs in WSL 2 on the same PC (or on any Linux machine
the bridge can reach).

## The easy way: a kit

`make-kit.sh` makes one folder with everything a Windows PC needs (only WSL
2 with any Ubuntu, nothing installed inside it): the cartridge, the Link
and the test server with the 32-bit glibc they were linked against, the
Xbox maps, the bridge, and launchers (`kit/`). Its `README.txt`
(`kit/README.txt`) is the player's guide; `play-sc64.bat` starts the Link
in WSL, waits for it, then starts the bridge, and closing the bridge stops
both; `play-sc64-local.bat` plays a test game on the PC alone (a local
server and a bot) instead of the internet's.

On the Linux (or WSL) machine that built the Link, the server and the
cartridge:

```sh
ninja link-x86 server-x86
link/sc64/make-kit.sh --rom ~/halo-64/build/n64/halo-online.z64 \
    --maps ~/halo-64/build/xbox/disc/maps --out ~/Halo64Online --zip
```

Halo 64's ROM Builder (halo-64's releases) writes this folder itself, with
the player's own maps and cartridge, when its Online cartridge is ticked:
it carries a kit made with `--template` (no cartridge, no maps), built for
any PC: the Link and the server static, on musl, with `configure.py
--portable` (as ChupathingyCE's releases build their servers: an Alpine
Linux x86 system, `tools/ci_build.py --alpine`, or a chroot of one), with
`--notice` adding musl's COPYRIGHT and Python's LICENSE.txt to its
`link/licenses/`. A Link built with the system's glibc (as above) brings
that system's 32-bit loader and libraries instead, and is built for the
building machine's processor unless configured `--portable`.

`--bridge-exe` adds the bridge as a Windows program, so the PC doesn't need
Python. Make it on Windows with PyInstaller:

```
py -3 -m pip install pyinstaller
py -3 -m PyInstaller --onefile --console --name sc64_bridge link\sc64\sc64_bridge.py
```

(`dist\sc64_bridge.exe`; from WSL that is `/mnt/c/<the folder>/dist/sc64_bridge.exe`.)

The kit contains Halo's maps and a cartridge made from them: it is for your
own use, not to hand out.

## By hand

1. Put `halo-online.z64` on the SummerCart64 (its SD card, or
   `sc64deployer upload --save-type eeprom4k halo-online.z64`). The save
   type is EEPROM 4 kbit.
2. Start Halo 64 Link waiting for the cable: `HALO_LINK_CABLE=listen:6466`
   (`link/README.md`, "Playing on the N64").
3. Wire the cart's USB port to the computer, close anything else using it
   (sc64deployer, UNFLoader), and run

   ```
   py -3 sc64_bridge.py --link 127.0.0.1:6466
   ```

   It finds the cart (FTDI 0403:6014 "SC64", answering `v` with `SCv2`;
   `--port COMn` picks one, `--list` lists them), resets it, connects to the
   Link (again each second until it answers) and prints what goes each way
   every few seconds. A Link in WSL 2 is reached at 127.0.0.1 (WSL forwards
   localhost).

How it translates: the cart's `PKT` `U` packets (a `(type << 24 | size)`
header and the data) go to the Link as `"DMA@"`, header, data, `"CMPH"`;
the Link's `"DMA@"` frames go to the cart as `CMD` `U` (type, size, the
data unpadded).

FTDI's latency timer (16 ms by default) adds that much to each read: set it
to 1-2 ms in the port's Device Manager properties (Advanced) for less input
lag.

`python test_sc64_bridge.py` runs the offline tests (no cart needed).
