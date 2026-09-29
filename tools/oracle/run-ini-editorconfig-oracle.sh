#!/bin/sh
# Compare this library's EditorConfig reader against both reference cores.
#
#   tools/oracle/run-ini-editorconfig-oracle.sh
#
# Two references, editorconfig-core-c and editorconfig-core-py, and they disagree
# - with each other and with their own specification. Neither is an authority:
# **both score 33 of the normative suite's 34 grammar assertions**, which
# `make conformance-ini-editorconfig` measures separately. So this gate scores our
# verdict against the specification, scores values only where no core is known to
# be wrong, and asserts that each known departure is still there.
#
# INI_EC_ORACLE_COUNT and INI_EC_ORACLE_SEED size and seed the population.
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
runner=$root/build/ini-editorconfig-oracle-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_ec_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" editorconfig -- \
	python3 "$root/tools/oracle/ini_ec_diff.py" "$runner"
