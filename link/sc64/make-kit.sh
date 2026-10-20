#!/bin/bash
# Makes the folder (and, with --zip, the zip) that plays Halo 64 Online on a
# Windows PC with a SummerCart64 and nothing else installed but WSL: the
# cartridge, the Link and the test server (stripped), the Xbox maps, the
# bridge, the launchers (link/sc64/kit/; its README.txt is the player's
# guide) and the notices their parts' licences ask for (link/licenses/).
#
#   link/sc64/make-kit.sh --rom halo-online.z64 --maps <the Xbox maps folder> \
#       --out <folder> [--bridge-exe sc64_bridge.exe] [--notice <file>]... [--zip]
#   link/sc64/make-kit.sh --template --out <folder> --bridge-exe sc64_bridge.exe ...
#
# --template leaves the cartridge and the maps out: the kit Halo 64's ROM
# Builder carries (package_rombuilder.py --online-kit), which it fills with
# the player's own.
#
# The Link must be built first (ninja link-x86); the server (ninja
# server-x86) too for the kit's test game, which is left out without it.
# A static Link (built on musl, as tools/ci_build.py --alpine builds the
# servers) runs as it is; a Link linked with glibc brings that system's
# 32-bit loader and libraries (run it on the system it was built on).
# --bridge-exe is sc64_bridge.py made a program on Windows
# (link/sc64/README.md); without it the kit runs the .py, which needs Python
# 3 on the PC. --notice adds a file to link/licenses/ (musl's COPYRIGHT for
# a static Link, Python's LICENSE.txt for the bridge program);
# --source-commit names the commit the programs were built from (this
# checkout's otherwise).
#
# A kit with the maps holds Halo's data and a cartridge made from it: it is
# for your own use, not to hand out.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
ROM=
MAPS=
OUT=
EXE=
ZIP=0
TEMPLATE=0
NOTICES=()
# (the commit the programs were built from, for the notices: this checkout's
# unless --source-commit says)
COMMIT=
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
	--notice) NOTICES+=("$2"); shift 2 ;;
	--source-commit) COMMIT=$2; shift 2 ;;
	--template) TEMPLATE=1; shift ;;
	--zip) ZIP=1; shift ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
if [ -z "$OUT" ] || { [ "$TEMPLATE" = 0 ] && { [ -z "$ROM" ] || [ -z "$MAPS" ]; }; }; then usage >&2; exit 2; fi
if [ "$TEMPLATE" = 0 ]; then
	[ -f "$ROM" ] || fail "no ROM at $ROM (halo-64: ninja -f build/n64/build.ninja online)"
	for map in "${MAP_NAMES[@]}"; do
		[ -f "$MAPS/$map.map" ] || fail "$MAPS has no $map.map (ui.map and the 13 multiplayer maps are needed)"
	done
fi
[ -f "$LINK" ] || fail "no Link at $LINK (ninja link-x86)"
if [ -n "$EXE" ] && [ ! -f "$EXE" ]; then fail "no bridge program at $EXE"; fi
for notice in ${NOTICES[@]+"${NOTICES[@]}"}; do [ -f "$notice" ] || fail "no notice at $notice"; done
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT")" ]; then fail "$OUT is not empty"; fi

# a Link linked with glibc: the 32-bit loader, libc and libm it loads
loader=$(readelf -l "$LINK" | sed -n 's/.*program interpreter: \(.*\)]/\1/p')
if [ -n "$loader" ]; then
	[ -f "$loader" ] || fail "the Link's loader ($loader) is not on this system (32-bit glibc: libc6-i386)"
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
fi
if [ -f "$SERVER" ] && [ "$(readelf -l "$SERVER" | sed -n 's/.*program interpreter: \(.*\)]/\1/p')" != "$loader" ]; then
	fail "the server and the Link are not linked alike (one static, or another loader)"
fi

echo "Making the kit in $OUT"
mkdir -p "$OUT"
cp -r "$here/kit/." "$OUT/"
rm -f "$OUT/.gitattributes"
mkdir -p "$OUT/link/playlists" "$OUT/link/licenses" "$OUT/sc64"
if [ "$TEMPLATE" = 0 ]; then
	mkdir -p "$OUT/maps"
	cp "$ROM" "$OUT/halo-online.z64"
	for map in "${MAP_NAMES[@]}"; do cp -L "$MAPS/$map.map" "$OUT/maps/"; done
fi
strip -o "$OUT/link/halo64-link" "$LINK"
licenses=$OUT/link/licenses
cp "$repo/LICENSE.md" "$licenses/halo64-link-LICENSE.md"
for pair in "port/third_party/mbedtls/LICENSE:mbedtls-LICENSE.txt" \
	"port/third_party/miniupnpc/LICENSE:miniupnpc-LICENSE.txt" \
	"port/third_party/expat/COPYING:expat-COPYING.txt" \
	"port/third_party/extract-xiso/LICENSE.TXT:extract-xiso-LICENSE.txt" \
	"port/assets/fonts/Overpass-OFL.txt:Overpass-OFL.txt" \
	"port/linux/ui/fonts/OFL.txt:NotoSans-OFL.txt" \
	"port/linux/ui/fonts/KENNEY-CC0.txt:Kenney-Input-Prompts-CC0.txt"; do
	cp "$repo/${pair%%:*}" "$licenses/${pair#*:}"
done
for notice in ${NOTICES[@]+"${NOTICES[@]}"}; do cp "$notice" "$licenses/"; done
if [ -n "$loader" ]; then
	mkdir -p "$OUT/link/lib32"
	cp -L "$loader" "$OUT/link/lib32/ld-linux.so.2"
	cp -L "$libc" "$OUT/link/lib32/libc.so.6"
	cp -L "$libm" "$OUT/link/lib32/libm.so.6"
	if [ -n "$gcc_s" ] && [ -f "$gcc_s" ]; then
		cp -L "$gcc_s" "$OUT/link/lib32/libgcc_s.so.1"
	else
		echo "  (no 32-bit libgcc_s.so.1 here: crash backtraces won't work; lib32gcc-s1 has it)"
	fi
	for doc in /usr/share/doc/libc6-i386/copyright /usr/share/doc/libc6/copyright; do
		if [ -f "$doc" ]; then cp "$doc" "$licenses/glibc-copyright.txt"; break; fi
	done
fi
if [ -f "$SERVER" ]; then
	strip -o "$OUT/link/chupathingyce-server" "$SERVER"
	cp "$repo/server/playlists/free_for_all.txt" "$OUT/link/playlists/"
	cp "$repo/server/blocked_names.txt" "$OUT/link/"
else
	echo "  (no server at $SERVER: the kit has no test game; ninja server-x86 makes it)"
	rm -f "$OUT/play-sc64-local.bat"
fi
cp "$here/sc64_bridge.py" "$OUT/sc64/"
if [ -n "$EXE" ]; then cp "$EXE" "$OUT/sc64/sc64_bridge.exe"; fi
{
	echo "What this kit carries, and the licences"
	echo "======================================="
	echo
	echo "link/halo64-link, link/chupathingyce-server: Halo 64 Link and ChupathingyCE's"
	echo "dedicated server, built from https://github.com/JackTulli/halo64-link"
	echo "(commit ${COMMIT:-$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)}): CC0 (halo64-link-LICENSE.md), with"
	echo "Mbed TLS (Apache-2.0), miniupnpc (BSD), Expat (MIT), extract-xiso, the"
	echo "Overpass and Noto Sans fonts (OFL) and Kenney's input prompts (CC0): the"
	echo "files here."
	if [ -n "$loader" ]; then
		echo
		echo "lib32/: the GNU C Library's 32-bit loader and libraries (LGPL-2.1+;"
		echo "glibc-copyright.txt), as the building system's distribution ships them;"
		echo "its source is that distribution's glibc source package."
	else
		echo
		echo "The programs are statically linked with musl (MIT: its COPYRIGHT, when"
		echo "given, is here)."
	fi
	echo
	echo "sc64/sc64_bridge.py: the SummerCart64 bridge (CC0, the same repository)."
	if [ -n "$EXE" ]; then
		echo "sc64/sc64_bridge.exe: the bridge packaged with PyInstaller: it carries"
		echo "Python (PSF License; its LICENSE.txt, when given, is here)."
	fi
} > "$licenses/README.txt"
# (Windows' line endings for the .bat and .txt files, Linux's for the
# scripts, whatever this checkout has)
for f in "$OUT"/*.bat "$OUT/README.txt" "$licenses"/README.txt; do
	if [ -f "$f" ]; then sed -i 's/\r*$/\r/' "$f"; fi
done
for f in "$OUT"/link/*.sh; do
	sed -i 's/\r$//' "$f"
	bash -n "$f"
done
if [ -n "$loader" ]; then
	"$OUT/link/lib32/ld-linux.so.2" --library-path "$OUT/link/lib32" --list "$OUT/link/halo64-link" > /dev/null \
		|| fail "the packed loader can't load the Link"
fi

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
