#!/bin/bash
# the local-only dedicated server for the Link's tests (link/README.md):
# nothing listed, no invites, no reports (every outside service off), playing
# from 127.0.0.3: to the
# game 127.0.0.1 is "this machine" (xnet.c's outgoing_address sends a
# destination of 127.0.0.1 to the machine's own network.address), so a host
# at 127.0.0.1 is one no other game on the machine can reach. The server
# broadcasts to 127.0.0.2, where the Link plays (link/run-local.sh).
#
#   link/run-local-server.sh [playlist]     (free_for_all.txt by default)
#
# Environment: HALO_SERVER_DATA (its data folder: maps/ and playlists/; this
# repository's server/README.md says what goes in it), SERVER (the program:
# chupathingyce-server, ninja server-x86).
here=$(cd "$(dirname "$0")" && pwd)
SERVER=${SERVER:-$here/../build/server-x86/chupathingyce-server}
cd "${HALO_SERVER_DATA:?set HALO_SERVER_DATA to the data folder of the server (maps/ and playlists/)}" || exit 1
# (the Link plays from 127.0.0.2; a second machine, as the dummy player, from 127.0.0.4)
export HALO_NET_ADDRESS=127.0.0.3 HALO_NET_BROADCAST=${HALO_SERVER_BROADCAST:-127.0.0.2,127.0.0.4}
export HALO_DEDICATED=playlists/${1:-free_for_all.txt} HALO_DEDICATED_NAME="Halo 64 test"
export HALO_DEDICATED_PUBLIC=false HALO_NET_ONLINE=false HALO_NET_PUBLIC_LOBBY=false HALO_NET_BROWSER=""
export HALO_NET_LIST_GAMES=false HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false
export HALO_CRASH_REPORTS=no HALO_UPDATE_AUTO=false HALO_NET_ALLOW_UPNP=false HALO_DEDICATED_LINK=false
exec "$SERVER"
