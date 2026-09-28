#!/bin/sh
# Compare this library's Desktop Entry reader against both of its references.
#
#   tools/oracle/run-ini-oracle.sh
#
# Two references in one pinned image, because they answer different halves of one
# question and disagree about the answer: GKeyFile says what a value is, and
# desktop-file-validate says whether the document is legal. See
# containers/IMAGES for why neither alone would do.
#
# INI_ORACLE_COUNT and INI_ORACLE_SEED size and seed the population.
# Needs the built-here image: make oracle-images.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/ini-oracle-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" inidesktop -- \
	python3 "$root/tools/oracle/ini_diff.py" "$runner"
