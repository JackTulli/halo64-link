#!/bin/bash
# stops link/start-link.sh (and so everything it started: the Link, and in a
# test game the server and the bot) by the PID it recorded
KIT=$(cd "$(dirname "$0")/.." && pwd)
pidfile=$KIT/data/link.pid
[ -f "$pidfile" ] || exit 0
pid=$(cat "$pidfile")
if [ -n "$pid" ] && grep -q start-link "/proc/$pid/cmdline" 2>/dev/null; then
	kill -TERM "$pid"
	for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
fi
rm -f "$pidfile"
