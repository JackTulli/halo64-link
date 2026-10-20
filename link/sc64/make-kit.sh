#!/bin/bash
# Makes the folder (and, with --zip, the zip) that plays Halo 64 Online on a
# Windows PC with a SummerCart64 and nothing else installed but WSL: the
# cartridge, the Link and the test server (stripped) with the 32-bit glibc
# they were linked against, the Xbox maps, the bridge and the launchers
# (link/sc64/kit/; its README.txt is the player's guide).
#
#   link/sc64/make-kit.sh --rom halo-online.z64 --maps <the Xbox maps folder> \
#       --out <folder> [--bridge-exe sc64_bridge.exe] [--zip]
#
# The Link must be built first (ninja link-x86); the server (ninja
# server-x86) too for the kit's test game, which is left out without it.
# Run it on the x86-64 Linux (or WSL) the Link was built on: the libraries
# packed are that system's 32-bit ones. --bridge-exe is sc64_bridge.py made
# a program on Windows (link/sc64/README.md); without it the kit runs the
# .py, which needs Python 3 on the PC.
#
# The kit holds Halo's maps and a cartridge made from them: it is for your
# own use, not to hand out.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
ROM=
MAPS=
OUT=
EXE=
ZIP=0
LINK=$repo/build/link-x86/halo64-link
SERVER=$repo/build/server-x86/chupathingyce-server
MAP_NAMES=(ui beavercreek bloodgulch boardingaction carousel chillout damnation hangemhigh
	longest prisoner putput ratrace sidewinder wizard)

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }
fail() { echo "make-kit: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
	case $1 in
	--rom) ROM=$2; shift 2 ;;
	--maps) MAPS=$2; shift 2 ;;
	--out) OUT=$2; shift 2 ;;
	--bridge-exe) EXE=$2; shift 2 ;;
	--link) LINK=$2; shift 2 ;;
	--server) SERVER=$2; shift 2 ;;
	--zip) ZIP=1; shift ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
if [ -z "$ROM" ] || [ -z "$MAPS" ] || [ -z "$OUT" ]; then usage >&2; exit 2; fi
[ -f "$ROM" ] || fail "no ROM at $ROM (halo-64: ninja -f build/n64/build.ninja online)"
[ -f "$LINK" ] || fail "no Link at $LINK (ninja link-x86)"
for map in "${MAP_NAMES[@]}"; do
	[ -f "$MAPS/$map.map" ] || fail "$MAPS has no $map.map (ui.map and the 13 multiplayer maps are needed)"
done
if [ -n "$EXE" ] && [ ! -f "$EXE" ]; then fail "no bridge program at $EXE"; fi
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT")" ]; then fail "$OUT is not empty"; fi

# the 32-bit libraries the Link loads: its loader, libc and libm
loader=$(readelf -l "$LINK" | sed -n 's/.*program interpreter: \(.*\)]/\1/p')
[ -n "$loader" ] && [ -f "$loader" ] || fail "the Link's loader ($loader) is not on this system (32-bit glibc: libc6-i386)"
libc=
libm=
while read -r name arrow path _; do
	[ "$arrow" = "=>" ] || continue
	case $name in
	libc.so.6) libc=$path ;;
	libm.so.6) libm=$path ;;
	esac
done < <(ldd "$LINK")
[ -f "$libc" ] && [ -f "$libm" ] || fail "could not find the Link's libc.so.6 and libm.so.6 (ldd $LINK)"
# (glibc loads libgcc_s for a crash's backtrace)
gcc_s=$(gcc -m32 -print-file-name=libgcc_s.so.1 2>/dev/null || true)
if [ ! -f "$gcc_s" ]; then gcc_s=$(ls /usr/lib32/libgcc_s.so.1 /lib32/libgcc_s.so.1 2>/dev/null | head -1 || true); fi

echo "Making the kit in $OUT"
mkdir -p "$OUT"
cp -r "$here/kit/." "$OUT/"
rm -f "$OUT/.gitattributes"
mkdir -p "$OUT/maps" "$OUT/link/lib32" "$OUT/link/playlists" "$OUT/sc64"
cp "$ROM" "$OUT/halo-online.z64"
for map in "${MAP_NAMES[@]}"; do cp -L "$MAPS/$map.map" "$OUT/maps/"; done
strip -o "$OUT/link/halo64-link" "$LINK"
cp -L "$loader" "$OUT/link/lib32/ld-linux.so.2"
cp -L "$libc" "$OUT/link/lib32/libc.so.6"
cp -L "$libm" "$OUT/link/lib32/libm.so.6"
if [ -n "$gcc_s" ] && [ -f "$gcc_s" ]; then
	cp -L "$gcc_s" "$OUT/link/lib32/libgcc_s.so.1"
else
	echo "  (no 32-bit libgcc_s.so.1 here: crash backtraces won't work; lib32gcc-s1 has it)"
fi
if [ -f "$SERVER" ]; then
	strip -o "$OUT/link/chupathingyce-server" "$SERVER"
	cp "$repo/server/playlists/free_for_all.txt" "$OUT/link/playlists/"
else
	echo "  (no server at $SERVER: the kit has no test game; ninja server-x86 makes it)"
	rm -f "$OUT/play-sc64-local.bat"
fi
cp "$here/sc64_bridge.py" "$OUT/sc64/"
if [ -n "$EXE" ]; then cp "$EXE" "$OUT/sc64/sc64_bridge.exe"; fi
# (Windows' line endings for the .bat and .txt files, Linux's for the
# scripts, whatever this checkout has)
for f in "$OUT"/*.bat "$OUT/README.txt"; do
	if [ -f "$f" ]; then sed -i 's/\r*$/\r/' "$f"; fi
done
for f in "$OUT"/link/*.sh; do
	sed -i 's/\r$//' "$f"
	bash -n "$f"
done
"$OUT/link/lib32/ld-linux.so.2" --library-path "$OUT/link/lib32" --list "$OUT/link/halo64-link" > /dev/null \
	|| fail "the packed loader can't load the Link"

if [ "$ZIP" = 1 ]; then
	zip=${OUT%/}.zip
	python3 - "$OUT" "$zip" <<'EOF'
import os, sys, zipfile
folder, out = sys.argv[1], sys.argv[2]
top = os.path.basename(os.path.normpath(folder))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if d != "data")
        for name in sorted(files):
            path = os.path.join(root, name)
            info = zipfile.ZipInfo.from_file(path, top + "/" + os.path.relpath(path, folder).replace(os.sep, "/"))
            with open(path, "rb") as f:
                z.writestr(info, f.read(), zipfile.ZIP_DEFLATED, 6)
EOF
	echo "Zip: $zip ($(du -h "$zip" | cut -f1))"
fi
echo "Done: $(du -sh "$OUT" | cut -f1) in $OUT"
