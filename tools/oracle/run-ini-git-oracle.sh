#!/bin/sh
# Compare this library's git config reader against git itself.
#
#   tools/oracle/run-ini-git-oracle.sh
#
# One reference, because git config has one implementation - the same program
# decides legality and values. What stands in for a second is the generator's own
# intent; see tools/oracle/ini_git_diff.py and containers/IMAGES.
#
# INI_GIT_ORACLE_COUNT and INI_GIT_ORACLE_SEED size and seed the population.
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
runner=$root/build/ini-git-oracle-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_git_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" gitconfig -- \
	python3 "$root/tools/oracle/ini_git_diff.py" "$runner"
