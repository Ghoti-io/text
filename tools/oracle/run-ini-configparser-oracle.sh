#!/bin/sh
# Compare this library's configparser reader against CPython's `configparser`.
#
#   tools/oracle/run-ini-configparser-oracle.sh
#
# **One reference, and it is also the specification** - the weakest position any
# dialect in this module is in. git config has a lone reference too, but there
# `git-config(1)` exists to disagree with git; here the Python documentation describes
# the implementation and says so. Two things carry the weight instead: the generator's
# own `REFUSES` table, scored as `intent`, and the local corpus gate
# `make conformance-ini-configparser`, whose 703 real files include 224 the reference
# refuses.
#
# The reference reads a **file**, because Python's universal-newline translation
# applies to `read(path)` and not to `read_string()` and the two disagree about a lone
# CR. See tools/oracle/containers/configparser/driver.py and containers/IMAGES.
#
# INI_CP_ORACLE_COUNT and INI_CP_ORACLE_SEED size and seed the population.
# Needs the pinned image, which is a stock pull: make oracle-images.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/ini-configparser-oracle-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_cp_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" configparser -- \
	python3 "$root/tools/oracle/ini_cp_diff.py" "$runner"
