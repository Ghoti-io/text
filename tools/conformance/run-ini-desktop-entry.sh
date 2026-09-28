#!/bin/sh
# Score the INI reader over this machine's Desktop Entry files.
#
#   tools/conformance/run-ini-desktop-entry.sh
#
# The corpus is not fetched and not pinned, which makes it unlike every other
# conformance target here, and the reason is worth stating rather than hiding:
# there is no published Desktop Entry test suite. What exists instead is a large
# population of real files on any Linux system, and the freedesktop validator to
# say they are legal. So the denominator is derived at run time and printed, and
# an empty corpus fails rather than scoring 0 of 0.
#
# INI_DESKTOP_DIRS overrides the directories searched.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

dirs=${INI_DESKTOP_DIRS:-/usr/share/applications /usr/local/share/applications $HOME/.local/share/applications}
present=""
for d in $dirs; do
	[ -d "$d" ] && present="$present $d"
done
if [ -z "$present" ]; then
	echo "none of these directories exist: $dirs" >&2
	echo "set INI_DESKTOP_DIRS to a directory of .desktop files" >&2
	exit 1
fi

mkdir -p "$root/build"
runner=$root/build/ini-desktop-runner
cc -O1 -o "$runner" "$root/tools/conformance/desktop_entry_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/desktop_entry_suite.py" "$runner" $present
