# Halo 64 Link

The computer side of the Nintendo 64 port's online play (halo-64,
`port/n64/docs/online/README.md`). The N64 can't be a machine of a
ChupathingyCE network game, so the computer beside it is: the Link is
ChupathingyCE v0.7.5d itself, the same code, network protocol and version,
built as a program with no window and no sound, as the dedicated server is,
but as a **client**: a player's machine that joins (and later hosts) games
as a player's copy of the game does. Its players will be the N64's
controllers, and after each tick it will send the N64 what to draw.

It works now (October 2026): the N64 cartridge (halo-64's
`halo-online.z64`) lists the internet's and the local network's games
through the Link, joins one, and plays it -- in ares with an emulated
flashcart USB port (`link/ares/`), or on a real SummerCart64 through a small
bridge (`link/sc64/`). "Playing on the N64" below says how the two share the
work.

Everything needed to play online, and in what order (the cartridge's side
is halo-64's `port/n64/docs/online/README.md`):

1. **The cartridge**, `halo-online.z64`: built in
   [halo-64](https://github.com/JackTulli/halo-64) (`ninja -f
   build/n64/build.ninja online`), from your own Xbox disc's maps.
2. **The Link**, from this repository (below), with the **same Xbox maps**
   (`ui.map` and the 13 multiplayer maps: the cartridge's are converted from
   them, and the Link names everything by their tag indices).
3. **The cable**: a SummerCart64 and the bridge (`link/sc64/`; its
   `make-kit.sh` packs all of it into one folder for a Windows PC), or, with
   no flashcart, ares with an emulated cart USB port (`link/ares/`).

## Building

On Linux (an x86 machine, or x86-64 with 32-bit glibc, as `ninja linux` and
`ninja server-x86` need). On Windows, in WSL 2: the same steps in Ubuntu,
with the repository in WSL's own file system (not under `/mnt/c`).

```sh
sudo apt install git python3 ninja-build clang lld gcc-multilib libc6-dev-i386
git clone https://github.com/JackTulli/halo64-link ~/halo64-link
cd ~/halo64-link
python3 configure.py --release --pgo=off --lto=off
ninja link-x86      # build/link-x86/halo64-link
ninja server-x86    # build/server-x86/chupathingyce-server (for local test games)
```

It is the 32-bit build, the one that plays the Xbox maps the cartridge's
maps are converted from. Like the server it needs SDL's headers, never its
library (tools/server_build.py fetches them once into
`build/server/third_party`, so the first configure needs the internet).

What the build adds (`tools/link_build.py`, called from `configure.py`):

- `HALO_LINK` is defined for every unit; `HALO_SERVER` is not.
- `port/linux/src/sdl_platform.c`, `xinput_sdl.c` and `updater.c` are left
  out, as the server leaves them out.
- In their place: `link/platform/link_platform.c` (the platform layer:
  start-up checks, `--help`, no window, frame pacing, stopping) and
  `link/platform/link_input.c` (the controllers), with the Xbox's ABI, and
  the server's `server/platform/sdl_headless.c` and `backtrace.c` (SDL's
  few utility functions, glibc's backtrace), with the host's.
- None of the server's control API, web admin page, Delta Control or roles
  units: a Link is not a server.

### `HALO_SERVER` in the Link

Each place the sources test `HALO_SERVER`, and what the Link does there:

| Where | What it does in the server | In the Link |
| --- | --- | --- |
| `port/linux/src/build_identity.c` | names the build "ChupathingyCE Dedicated Server" | **"Halo 64 Link"** (`HALO_LINK`; the HTTP user agent stays ChupathingyCE's) |
| `source/cseries/errors.c`, `port/linux/src/xbox_kernel.c` | the log also goes to the control API | no (no control API) |
| `port/linux/src/delta_peer_game.c` | Delta's platform key says "dedicated" | no: a player's machine |
| `port/linux/src/hud_hires.c` | decodes no high-res HUD bitmaps | the same, through `debug.null_renderer`, which the Link sets |
| `port/linux/src/p2p.c` | registers no `halo://` link handler | the same, through `debug.null_renderer`: the player's own copy keeps the links |
| `source/objects/object_lights.c` | lights join no clusters (only for drawing) | as a client: kept, so the renderer's visibility works as the game's |
| `port/linux/game/browser_screen.c`, `port/linux/src/map_download.c` | no map offers, no map downloads | as a client (the menus and downloads stay; the N64 has only its own maps, see below) |
| `server/src/server_commands.c`, `server_moderation.c` | the server program's control | the game's own dedicated mode, as in a player's copy (never used: `HALO_DEDICATED` is refused) |

## Running

The data folder (`HALO_DATA_ROOT`, else the current folder) holds `maps/`
(`ui.map` and the multiplayer maps: the Xbox's) and gets the game's log,
`debug.txt`. Settings are `config.toml`'s, beside the program (written with
the defaults on the first run), and the environment's; `HALO_SAVE_ROOT`
says where saves go (the profile, the player key).

`halo64-link --help` lists the settings. The Link's own:

- `HALO_LINK_INPUT`: port 1's controller (`link_input.c`), until the N64
  sends its own. Default: `debug.test_input`'s, else idle.
  - `idle`: a controller that never moves;
  - `walk`: walks forward at three quarters, turning right (a circle);
  - `bot[:<seed>]`, `look[:<seed>]`, `menu:<buttons>`: `debug.test_input`'s
    scripted players (`port/linux/NETCODE.md`, "Testing");
  - `pipe:<path>`: controller states read from a file or a named pipe, a
    line each, as they arrive; a line's state holds until the next. A line
    is the whole state as words: `lx=` `ly=` `rx=` `ry=` (sticks, -32768 to
    32767), `a=` `b=` `x=` `y=` `black=` `white=` `lt=` `rt=` (0 to 255; the
    word alone is 255), `up` `down` `left` `right` `start` `back` `lthumb`
    `rthumb` (held when there); `#` starts a comment:

    ```sh
    mkfifo /tmp/pad; HALO_LINK_INPUT=pipe:/tmp/pad link/run-local.sh &
    echo "ly=32000 rx=14000 rt" > /tmp/pad   # forward, turning, firing
    echo "" > /tmp/pad                       # let go
    ```

- `HALO_DEDICATED` is refused: hosting a playlist is the server's.

With nothing drawn no display paces the main loop, so the Link holds it to
60 frames a second (twice the game's ticks), as the server's director does.
SIGINT or SIGTERM stops it at the next frame, as closing the game's window
does; if the main loop is held up (a blocking read) SIGALRM ends it 5
seconds later, and a second signal ends it at once.

### Against a local dedicated server

`link/run-local-server.sh` runs the dedicated server (`ninja server-x86`,
from the same version) local-only: nothing listed, no invites, no reports.
`link/run-local.sh` runs the Link with `HALO_NETWORK_TEST=join`
(`port/linux/game/network_test.c`: it searches for system link games, joins
the first, adds its player and picks a team) and port 1 walking:

```sh
HALO_SERVER_DATA=~/server-data link/run-local-server.sh > server.log 2>&1 &
HALO_LINK_MAPS=~/halo-64/build/xbox/disc/maps link/run-local.sh > link.log 2>&1 &
```

Both run on one machine by playing from different loopback addresses: the
server from 127.0.0.3, broadcasting to 127.0.0.2, the Link from 127.0.0.2,
searching 127.0.0.3 (`HALO_NET_ADDRESS`, `HALO_NET_BROADCAST`), so they bind
the same ports without colliding. Not 127.0.0.1: to the game that address is
"this machine" (`xnet.c`, `outgoing_address` sends a destination of
127.0.0.1 to the machine's own `network.address`), so a host at 127.0.0.1
answers the search but its game can't be reached from the other program
(the join's connection is refused, and the client's main loop then waits
in a blocking read).

Once in the game `network_test.c` logs every second, in the Link's output:
the tick, the player's position, health and shields, weapon, aim and
facing, items, the netcode's sent/received/corrected counts and the camera.

Tested (2026-10-19, bloodgulch slayer from `free_for_all.txt`): the Link
found the server's game 2 seconds after the main menu, joined as machine
#1, added its player and loaded the map at the countdown's end, then played
2 minutes 42 seconds walking in circles (10621 updates sent, 11078
received, its position corrected by the host 9 times in all), and on
SIGINT left the game (the server: "client machine 1 removed from game").
No errors in its `debug.txt` past the start-up's (no intro movie, no last
profile). It used about 8% of one core and 110 MB. With
`HALO_LINK_INPUT=pipe:` the player walked while a line held `ly=32000`,
stopped on an empty line and turned with `rx=20000`.

## Playing on the N64

The N64 is wired to the computer by its flashcart's USB port (or ares'
emulated one): `link/platform/link_cable.c` speaks UNFLoader's USB framing
over TCP (`HALO_LINK_CABLE=listen:<port>` waits for ares' `connect:` mode or
the SummerCart64 bridge; `tcp:<host>:<port>` connects to ares' listening
mode). The protocol (version 2) is halo-64's `port/n64/src/n64_link.h`.

- **The menus** (`port/linux/game/link_director.c`): the N64's ONLINE screen
  asks for the games (LIST): the local network's, then halo.milenko.org's
  (`browser_get_games`), sent again whenever they change; JOIN joins one
  (an internet game through its invite); the lobby's players and the
  game's scores go to the N64 once a second (SCORES); when the game's map
  is loaded here the N64 is told to load it (LOAD); ENDED when it is over.
  The player's name is the one the N64 sends (NAME), put in the player's
  profile (`player_ui_link_set_player_name`).
- **The player's own unit is the N64's to play**: the cartridge plays a
  game of its own on the map, its player's unit moving, aiming and firing at
  once, and sends each tick's control (CONTROL: facing, throttle, trigger,
  buttons, weapon, grenade, zoom). `port/linux/game/link_control.c` plays
  exactly that control as the Link's player's (in `player_control.c`'s
  `handle_one_player_input`), a tick at a time, so both games move the unit
  alike; each frame says which N64 tick the Link last played and the
  unit's state (position, velocity, facing, health, weapons, rounds), and
  the N64 corrects its own by it.
- **Everything else is drawn from the Link's frames**
  (`port/linux/game/link_export.c`), one each time the N64 has drawn one:
  the views, the other objects (model, regions, nodes), and what happens --
  the effects begun with their sounds, the other sounds and the announcer,
  the hits the player takes, the game's HUD messages, and each frame the
  HUD's view of the game (the crosshair's target, the state message, the
  goals, the motion sensor's blips). The N64's own player's effects and
  sounds are left out: its own game makes them.

Running it all on one Linux (or WSL) computer, for an emulated N64: build
the Link (above) and the server (`ninja server-x86`), build ares with
`link/ares/ares-usb.patch`, then

```sh
HALO_LINK_CABLE=listen:6466 HALO_LINK_INPUT=idle HALO_NETWORK_TEST= \
	HALO_LINK_MAPS=~/halo-64/build/xbox/disc/maps link/run-local.sh &
ARES_N64_USB=connect:127.0.0.1:6466 ares --system "Nintendo 64" halo-online.z64
```

For internet games run the Link with `HALO_NET_ONLINE=true` and without
`HALO_NET_ADDRESS`/`HALO_NET_BROADCAST` (the game must play from the
machine's own address); turn off what a player's machine would publish or
report. A data folder of its own, with the maps:

```sh
mkdir -p ~/link-data/saves && ln -sfn ~/halo-64/build/xbox/disc/maps ~/link-data/maps
cd ~/link-data
HALO_SAVE_ROOT=~/link-data/saves HALO_NET_ONLINE=true \
HALO_LINK_CABLE=listen:6466 HALO_LINK_INPUT=idle \
HALO_NET_LIST_GAMES=false HALO_NET_PUBLIC_LOBBY=false HALO_NET_HOST_PUBLIC=false \
HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false HALO_NET_MAP_DOWNLOADS=never \
HALO_NET_ALLOW_UPNP=false HALO_CRASH_REPORTS=no HALO_UPDATE_AUTO=false \
    ~/halo64-link/build/link-x86/halo64-link
```

then connect the N64: the SummerCart64 bridge (`link/sc64/README.md`), or
ares as above. The N64's ONLINE screen lists the local network's games and
halo.milenko.org's; R sets the player's name, A joins a game; in a game
START shows the players and scores, START and Z together leave.

On a Windows PC with a SummerCart64, `link/sc64/make-kit.sh` packs all of
this -- the Link, the test server, their 32-bit libraries, the maps, the
cartridge, the bridge -- into one folder whose `play-sc64.bat` starts it
(in WSL 2, with nothing installed in Ubuntu).

## Where the Link hooks into the game

Where `link_export.c`, `link_control.c`, `link_director.c` and
`link_cable.c` found what they needed, located in v0.7.5d:

- **Once per tick, after the simulation.** `source/game/game_time.c:475`
  runs `game_tick()` for each tick due, then `render_interpolation_tick()`
  (476, the per-tick snapshot of every object's nodes) and
  `network_distributed_tick()` (480): after 480 the world is the tick's.
  The cameras are updated per frame after that, in `main_loop`
  (`source/main/main.c:3536` `director_update`, 3537 `observer_update`), so
  a frame packet with the camera goes after `main.c:3539`
  (`game_engine_update_non_deterministic`), when `game_time_get()` has
  moved on since the last packet. `network_test_update` (`main.c:3442`) is
  the existing per-frame test hook.
- **The objects.** `object_iterator_new(&iterator, _object_mask_all, 0)` /
  `object_iterator_next` (`source/objects/objects.c:300`, 315) walk every
  object, as `render_interpolation_tick` does
  (`port/linux/game/render_interpolation.c:386`). An `object_datum`
  (`source/objects/objects.h:318`) has `definition_index` (the tag index,
  the same in the N64's converted map), `object.position`, `forward`, `up`
  (259-262; relative to `parent_object_index`, 296, when attached),
  `location.cluster_index` (264), the bounding sphere (265),
  `region_permutations` (310) and `node_matrices` (315);
  `object_get_node_matrices` (`objects.c:1293`) returns the world-space
  node matrices, `object_get_origin` / `object_get_orientation`
  (`objects.c:2071`, 2090) the world placement. What the player could see:
  the PVS of the camera's cluster, `structure_bsp_get_cluster_pvs`
  (`source/structures/structure_bsp_definitions.c:45`; tested as in
  `source/ai/actor_perception.c:5735`), against each object's cluster; or
  the renderer's own list: the Link's renderer runs all its CPU side
  (only the drawing calls go nowhere: checked with breakpoints in a game),
  and each frame `render_window` (`source/render/render.c:302`, per local
  player) calls `render_objects` (338), whose `find_rendered_objects`
  (`source/render/render_objects.c:414`) fills
  `render_object_globals.rendered_object_indices` (static to that file)
  with the objects in the camera's visible clusters.
- **The camera.** `observer_get_camera(local_player_index)`
  (`source/camera/observer.c:397`) returns the `observer_result`
  (`source/camera/observer.h:48`: position, location, velocity, forward,
  up, field of view), updated each frame by `observer_update`
  (`observer.c:1306`, called at `main.c:3537`); first or third person is
  `director_get_perspective` (`source/camera/director.c:300`). The
  first-person weapon is `source/interface/first_person_weapons.c`'s
  (`first_person_weapons_update`, 804, run each tick at
  `source/game/game.c:359`; its state `first_person_weapon_get`, 379).
- **Sounds.** Every impulse sound goes through `sound_new_impulse`
  (`source/sound/sound_manager.c:2424`), reached from
  `object_impulse_sound_new`, `unattached_impulse_sound_new` and
  `unspatialized_impulse_sound_new` (`source/sound/game_sound.c:419`, 461,
  491) and the scripts' `sound_impulse_start` (`scripted_sound_new`, 525).
  Looping sounds start with `game_looping_sound_new` (`game_sound.c:357`,
  an object's, in `game_looping_sound_data`) and
  `unattached_looping_sound_start` (796) / `scripted_looping_sound_start`
  (758), and stop with `game_looping_sound_delete` (398) /
  `unattached_looping_sound_stop` (708); `game_sound_update` (820) runs them
  each frame. (The mixer still runs with `HALO_NO_AUDIO`, on a silent
  clock: `dsound_sdl.c:1207`.)
- **Effects.** Every effect is made by `effect_allocate`
  (`source/effects/effects.c:1504`), called from
  `effect_new_attached_from_markers` (710),
  `effect_new_unattached_from_markers` (805), `effect_new_from_object`
  (887) and `effect_new_looping` (964); `effects_update` (1031, each tick
  at `game.c:350`) plays their parts. Decals: `decal_new`
  (`source/effects/decals.c:2273`); particles `particle_new`
  (`particles.c:810`), particle systems `particle_system_new_unattached` /
  `_attached` (`particle_systems.c:407`, 441), contrails `contrail_new`
  (`contrails.c:308`).
- **The HUD.** Health and shields: the unit's `object.body_vitality` and
  `shield_vitality` (`objects.h:280`, 281), as
  `hud_update_unit_local_player` reads them
  (`source/interface/hud_unit.c:528`). Ammunition: the weapon's
  `weapon.magazines[2]` (`source/items/weapons.h:207`; `rounds_total`,
  `rounds_loaded`: 183, 184), `weapon.heat` and `weapon.age` (196, 197;
  the unit's weapons are `unit.weapon_object_indices[]`), as
  `hud_update_weapon_local_player` reads them (`hud_weapon.c:690`).
  Grenades: the unit's `unit.grenade_counts[]` and
  `current_grenade_index`, as `render_grenade_hud` reads them
  (`hud_weapon.c:553`). The motion
  tracker: `motion_sensor_update` (`source/interface/motion_sensor.c:292`)
  fills `motion_sensor_globals->players[i].sensor_data[]` (171-184) once a
  tick, through `motion_sensor_tick` (786), which only the HUD's drawing
  calls (`source/interface/hud.c:1380`, in `hud_draw_screen`, 1350): the
  Link's renderer draws the HUD (to nothing) each frame, so it is kept up
  to date; a Link that stopped rendering would call it itself.
  `hud_update` (`game.c:371`) runs each tick.
- **Joining.** System link: a network client
  (`create_global_network_game_client`,
  `source/networking/network_game_globals.c:481`) searches by itself; its
  finds are `network_game_client_get_available_games`
  (`source/networking/network_client_manager.c:1120`), joined with
  `network_game_client_initiate_join_game` (2075), as
  `network_game_client_join_first_available_game` (3268, the automated
  tests') does for the first. Internet games: `browser_get_games`
  (`port/linux/src/browser.c:1805`) lists halo.milenko.org's games (each
  with its invite); `browser_join(invite)` (1835, through `p2p_join_invite`,
  `p2p.c:2495`) reaches the host, whose game then shows to the searching
  client as a system link game, joined by
  `network_game_client_join_invite_host(invite)`
  (`network_client_manager.c:3377`, after
  `network_game_client_invite_host_advertisement`, 3428, has its map), as
  the Online Games screen does (`port/linux/game/browser_screen.c:621`
  `join_selected`, 678 `wait_for_host`). The local player is added with
  `network_game_client_add_player` (`network_client_manager.c:1518`).
  Hosting: `player_ui_fast_setup_network_server`
  (`source/interface/player_ui.c:690`) and the server calls in
  `network_test.c`'s host mode (`port/linux/game/network_test.c`, the
  `_network_test_host` case of `network_test_update`).

Maps: the N64 has the multiplayer cartridge's maps only; a game on another
map (a Custom Edition map, a download) is one the Link must not join.
