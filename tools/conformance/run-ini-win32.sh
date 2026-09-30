#!/bin/sh
# Score the Win32 reader against wine's profile API over this machine's .ini files.
#
#   tools/conformance/run-ini-win32.sh
#
# **The corpus is real bytes of the right shape from the wrong provenance**, and the
# gate prints that with every run. This machine has exactly two `.ini` files a
# Windows application wrote, both inside a wine prefix. What it has hundreds of are
# `.ini` and `.cfg` files belonging to freedesktop, Python and other tools - and the
# profile API will read any of them, so they are a valid population for "do we agree
# with the reference about real bytes" and no population at all for "is this format
# used this way".
#
# That matters because for configparser the corpus gate found the cheapest of the
# three defects - a value beginning with `;`, in 331 of 479 real files and in none of
# the probes. This gate is the same instrument aimed at a weaker population, which is
# the honest description of what the Win32 work could get.
#
# **The comparison is the differential's**, not a second copy of it:
# tools/oracle/ini_win32_diff.py takes `--corpus` and reads files instead of
# generating documents. One implementation, two populations - a second copy is where
# the two would drift.
#
# INI_WIN32_DIRS overrides the directories searched; INI_W32_CORPUS_MAX caps the count.
# Needs the pinned image, which is built here: make oracle-images.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

dirs=${INI_WIN32_DIRS:-/etc /usr/share /usr/lib}
present=""
for d in $dirs; do
	[ -d "$d" ] && present="$present $d"
done
if [ -z "$present" ]; then
	echo "none of these directories exist: $dirs" >&2
	echo "set INI_WIN32_DIRS to a directory of .ini or .cfg files" >&2
	exit 1
fi

mkdir -p "$root/build"
runner=$root/build/ini-win32-corpus-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_win32_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" win32 -- \
	python3 "$root/tools/oracle/ini_win32_diff.py" "$runner" --corpus $present
