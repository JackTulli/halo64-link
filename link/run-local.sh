#!/bin/bash
# Halo 64 Link joining a system link game on this machine (link/README.md):
# the local-only dedicated server (link/run-local-server.sh) plays from
# 127.0.0.3 and broadcasts to 127.0.0.2; the Link plays from 127.0.0.2 and
# searches 127.0.0.3, so both bind the same ports on one machine. (Not
# 127.0.0.1: to the game that is "this machine", xnet.c's outgoing_address.)
#
#   link/run-local.sh            joins the first game found (HALO_NETWORK_TEST=join)
#
# Environment: HALO_LINK_DATA (the data folder: maps/, debug.txt, saves/),
# HALO_LINK_MAPS (where the Xbox maps are, linked into it), HALO_LINK_INPUT
# (port 1's controller: walk by default; link_input.c), LINK (the program).
here=$(cd "$(dirname "$0")" && pwd)
LINK=${LINK:-$here/../build/link-x86/halo64-link}
DATA=${HALO_LINK_DATA:-/root/n64/online/link-data}
MAPS=${HALO_LINK_MAPS:-/home/jack/halo-64/build/xbox/disc/maps}
mkdir -p "$DATA/maps" "$DATA/saves"
for map in "$MAPS"/*.map; do ln -sf "$map" "$DATA/maps/"; done
cd "$DATA" || exit 1
export HALO_SAVE_ROOT=$DATA/saves
export HALO_NETWORK_TEST=${HALO_NETWORK_TEST-join} HALO_NET_ADDRESS=127.0.0.2 HALO_NET_BROADCAST=127.0.0.3
export HALO_NET_ONLINE=false HALO_NET_PUBLIC_LOBBY=false HALO_NET_BROWSER="" HALO_NET_LIST_GAMES=false
export HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false HALO_CRASH_REPORTS=no HALO_UPDATE_AUTO=false
export HALO_NET_ALLOW_UPNP=false
export HALO_LINK_INPUT=${HALO_LINK_INPUT-walk}
exec "$LINK" "$@"
