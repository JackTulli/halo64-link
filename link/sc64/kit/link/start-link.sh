#!/bin/bash
# Halo 64 Link, from this kit, for the N64 on the SummerCart64: it waits for
# the bridge (sc64_bridge) on 127.0.0.1:6466 and plays ChupathingyCE's games
# for the N64.
#
#   bash link/start-link.sh          the internet's games (halo.milenko.org's
#                                    list) and the local network's
#   bash link/start-link.sh local    a test game on this PC only: a local
#                                    server ("Halo 64 test") and a bot
#
# It hosts nothing on the internet, reports nothing, downloads no maps and
# opens no router ports. Everything runs with the 32-bit libraries in
# link/lib32, so Ubuntu needs nothing installed. Ctrl+C, or closing this
# window, stops it (only the processes started here).
KIT=$(cd "$(dirname "$0")/.." && pwd)
DATA=$KIT/data
# (the programs' own folders -- debug.txt, saves -- are in Linux's file system;
# their debug.txt files are copied to data\ when they stop)
WORK=${XDG_DATA_HOME:-$HOME/.local/share}/halo64-online-kit
MODE=${1:-internet}
mkdir -p "$DATA" "$WORK/bin/lib32" || exit 1
PIDS=()
NAMES=()
keep_logs() {
	for name in "${NAMES[@]}"; do
		[ -f "$WORK/$name/debug.txt" ] && cp -f "$WORK/$name/debug.txt" "$DATA/$name-debug.txt" 2>/dev/null
	done
}
stop() {
	trap - INT TERM HUP EXIT
	rm -f "$DATA/link.pid"
	echo
	echo "Stopping Halo 64 Link..."
	for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
	sleep 1
	for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
	keep_logs
	exit 0
}
fail() {
	echo
	echo "$1"
	read -r -p "Press Enter to close." _
	exit 1
}
trap stop INT TERM HUP EXIT
# (link/stop-link.sh stops this script, and so everything it started, by this PID)
echo $$ > "$DATA/link.pid"

if grep -q Microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
	fail "This Ubuntu runs under WSL 1, which cannot run 32-bit programs. In a command
prompt, run:  wsl --set-version Ubuntu 2   (or the name 'wsl -l -v' shows)"
fi
for map in ui bloodgulch; do
	[ -f "$KIT/maps/$map.map" ] || fail "The maps folder has no $map.map ($KIT/maps): unpack the whole zip."
done

# the programs and their libraries, copied into Linux's file system (they
# run from there, whatever the Windows folder allows)
files=(halo64-link lib32/ld-linux.so.2 lib32/libc.so.6 lib32/libm.so.6)
[ "$MODE" = local ] && files+=(chupathingyce-server)
[ -f "$KIT/link/lib32/libgcc_s.so.1" ] && files+=(lib32/libgcc_s.so.1)
for f in "${files[@]}"; do
	[ -f "$KIT/link/$f" ] || fail "link/$f is missing from the kit (unpack the whole zip; a kit made without the server has no test game)."
	cmp -s "$KIT/link/$f" "$WORK/bin/$f" || cp -f "$KIT/link/$f" "$WORK/bin/$f" || fail "Could not copy link/$f to $WORK/bin."
	chmod +x "$WORK/bin/$f"
done

# start NAME PROGRAM [VARIABLE=value...]: one of the programs, in its own
# folder $WORK/NAME (its maps/ = the kit's), its output in data\NAME.log;
# its PID in STARTED
start() {
	local name=$1 program=$2
	shift 2
	mkdir -p "$WORK/$name/saves"
	ln -sfn "$KIT/maps" "$WORK/$name/maps"
	if [ "$program" = chupathingyce-server ]; then
		rm -rf "$WORK/$name/playlists"
		cp -r "$KIT/link/playlists" "$WORK/$name/"
	fi
	(
		cd "$WORK/$name" || exit 1
		unset HALO_NET_ADDRESS HALO_NET_BROADCAST HALO_NET_BROWSER HALO_NETWORK_TEST HALO_DEDICATED HALO_LINK_CABLE HALO_LINK_INPUT
		export HALO_DATA_ROOT=$WORK/$name HALO_SAVE_ROOT=$WORK/$name/saves
		export HALO_NET_PUBLIC_LOBBY=false HALO_NET_HOST_PUBLIC=false HALO_NET_LIST_GAMES=false
		export HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false HALO_NET_MAP_DOWNLOADS=never
		export HALO_NET_ALLOW_UPNP=false HALO_CRASH_REPORTS=no HALO_UPDATE_AUTO=false
		export "$@"
		exec "$WORK/bin/lib32/ld-linux.so.2" --library-path "$WORK/bin/lib32" "$WORK/bin/$program"
	) > "$DATA/$name.log" 2>&1 &
	STARTED=$!
	PIDS+=($STARTED)
	NAMES+=("$name")
}

: > "$DATA/link.log"
if [ "$MODE" = local ]; then
	echo "Halo 64 Online test game, on this PC only"
	echo "  1. the server (\"Halo 64 test\": slayer on every map)"
	start server chupathingyce-server HALO_NET_ONLINE=false HALO_NET_BROWSER= \
		HALO_NET_ADDRESS=127.0.0.3 HALO_NET_BROADCAST=127.0.0.2,127.0.0.4 \
		HALO_DEDICATED=playlists/free_for_all.txt "HALO_DEDICATED_NAME=Halo 64 test" \
		HALO_DEDICATED_PUBLIC=false HALO_DEDICATED_LINK=false
	sleep 6
	echo "  2. the dummy player (a bot)"
	start bot halo64-link HALO_NET_ONLINE=false HALO_NET_BROWSER= HALO_NETWORK_TEST=join \
		HALO_NET_ADDRESS=127.0.0.4 HALO_NET_BROADCAST=127.0.0.3 HALO_LINK_INPUT=bot:7
	sleep 4
	echo "  3. Halo 64 Link, waiting for the N64 (the bridge) on 127.0.0.1:6466"
	start link halo64-link HALO_NET_ONLINE=false HALO_NET_BROWSER= HALO_NET_ADDRESS=127.0.0.2 \
		HALO_NET_BROADCAST=127.0.0.3 HALO_LINK_INPUT=idle HALO_LINK_CABLE=listen:6466
	LINK_PID=$STARTED
	echo
	echo "On the N64: ONLINE, then A on \"Halo 64 test\". Start + Z in a game leaves."
else
	echo "Halo 64 Link, waiting for the N64 (the bridge) on 127.0.0.1:6466"
	start link halo64-link HALO_NET_ONLINE=true HALO_LINK_INPUT=idle HALO_LINK_CABLE=listen:6466
	LINK_PID=$STARTED
	echo
	echo "On the N64: ONLINE lists the games, R sets your name, A joins one."
	echo "Start + Z in a game leaves."
fi
echo "Close this window (or press Ctrl+C) to stop. Logs: $DATA"
echo
# (the Link's own lines; tail stops with the Link)
tail -n +1 -F --pid="$LINK_PID" "$DATA/link.log" 2>/dev/null > >(grep --line-buffered -a -E "link:|rror") &
PIDS+=($!)
wait "$LINK_PID"
keep_logs
echo
echo "Halo 64 Link has stopped (see data\\link.log)."
read -r -t 30 -p "This window closes in 30 seconds (Enter: now)." _
