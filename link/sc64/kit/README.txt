Halo 64 Online on a real N64 with a SummerCart64
================================================

Everything needed is in this folder: the cartridge (halo-online.z64), Halo
64 Link (the program that plays ChupathingyCE's games for the N64, a Linux
program that runs in WSL) with the Xbox maps, and the bridge between the
SummerCart64's USB port and the Link. Nothing to install apart from
Windows' own WSL.

This folder holds Halo game data (the maps, and the cartridge made from
them) from your own copy of the game: keep it to yourself.

NOTE: the bridge has so far been tested only against a simulated cart. If
anything goes wrong, keep the text in the bridge's window and the files in
the data folder.


What you need
-------------
- Windows 10 or 11 with WSL 2 and Ubuntu (any Ubuntu; nothing needs to be
  installed inside it). If you don't have it: open a command prompt as
  administrator, run   wsl --install   restart, and open Ubuntu once from
  the Start menu to create its user.
- An N64 with the Expansion Pak.
- A SummerCart64 (firmware 2.x) with its SD card and N64FlashcartMenu.
- A USB data cable from the SummerCart64 to this PC.


What's in here
--------------
  halo-online.z64        the cartridge: put this on the SummerCart64
  play-sc64.bat          start here: plays the internet's games
  play-sc64-local.bat    a test game on this PC only (a local server called
                         "Halo 64 test" plus a bot), for when nobody is
                         hosting on the internet
  sc64\sc64_bridge.exe   the bridge (sc64_bridge.py is its source: without
                         the .exe, play-sc64.bat runs the .py with Python 3)
  link\                  Halo 64 Link, the test server, the scripts the .bat
                         files run, and the licences (link\licenses)
  maps\                  the Xbox maps the Link and the server load
  data\                  logs (made on the first run)


Steps
-----
1. Unpack the whole zip into a normal folder, for example
   C:\Halo64Online (not inside OneDrive, and not run from inside the zip).

2. Put halo-online.z64 on the SummerCart64's SD card and start it from
   N64FlashcartMenu. The save type is EEPROM 4 kbit: the ROM's header asks
   for it, so the menu should pick it by itself; set it by hand if it asks.
   (Or over USB with sc64deployer, from the official SummerCart64 releases:
        sc64deployer upload --save-type eeprom4k halo-online.z64
    then close sc64deployer before step 4.)

3. Connect the SummerCart64's USB port to the PC. The first time, Windows
   installs FTDI's driver by itself; the cart then shows up in Device
   Manager under Ports as "USB Serial Port (COMn)".

4. Close every other program that uses the cart: sc64deployer (including
   "sc64deployer debug"), UNFLoader, any serial terminal.

5. Double-click play-sc64.bat (or play-sc64-local.bat for the test game).
   If Windows SmartScreen warns about it: More info > Run anyway (the files
   are not signed). It starts Ubuntu, then Halo 64 Link in its own window,
   then the bridge in another window, which says "SummerCart64 found on
   COMn". The first start takes a little longer (it copies the Link into
   Ubuntu).

6. On the N64, in Halo 64 Online, choose ONLINE. The bridge's window says
   "The N64 says hello", then "Halo 64 Link answered: the N64 is online".
   R sets your player name. A joins a game. In a game START shows the
   players and scores, and START + Z leaves. In the test game
   (play-sc64-local.bat) the game is called "Halo 64 test".

7. To stop, close the bridge's window (or press Ctrl+C in it): the Link
   stops too.


If something goes wrong
-----------------------
- "Ubuntu (WSL) would not start": run   wsl --shutdown   in a command
  prompt and try again. Not installed: see "What you need".
- "This Ubuntu runs under WSL 1": run   wsl --set-version Ubuntu 2
  (the name from   wsl -l -v   if yours is called something else).
- "Halo 64 Link did not start": look in the "Halo 64 Link" window and in
  data\link.log.
- "no SummerCart64 on USB": check the cable. Find the cart's port in Device
  Manager > Ports, then run from a command prompt in this folder:
        play-sc64.bat --port COM5        (with your port's number)
- "... is in use by another program": close sc64deployer or UNFLoader.
- "The cart has not taken the Link's data for 5 s": Halo 64 Online is not
  running on the N64 (still in the flashcart menu?).
- Input lag: Device Manager > Ports > USB Serial Port (COMn) > Properties >
  Port Settings > Advanced: set Latency Timer to 1 ms. The bridge mentions
  it when the timer is higher.
- sc64\sc64_bridge.exe --list   (in a command prompt) shows the COM ports
  and FTDI devices the bridge sees. With no COM port it uses FTDI's D2XX
  driver if that is installed (play-sc64.bat --d2xx forces it).
- play-sc64.bat --verbose shows more detail in the bridge window, including
  the N64's own text output.

Logs (in data\): link.log and link-debug.txt (the Link); server.log,
server-debug.txt, bot.log, bot-debug.txt (the test game). The *-debug.txt
copies are made when the Link stops.
