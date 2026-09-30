#!/bin/sh
# Compare this library's systemd reader against systemd itself.
#
#   tools/oracle/run-ini-systemd-oracle.sh
#
# One reference, and an instrument that answers a different question than it looks
# like it answers: `systemd-analyze verify` **exits 0 on a syntax error**. It warns,
# skips the line, and keeps the file, reserving its exit status for semantic failure.
# So the gate reads the diagnostics and classifies them, and values come back through
# `Environment=`, which is the only channel that reports a parsed setting. See
# tools/oracle/ini_sd_diff.py and containers/IMAGES.
#
# INI_SD_ORACLE_COUNT and INI_SD_ORACLE_SEED size and seed the population.
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
runner=$root/build/ini-systemd-oracle-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_sd_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" systemd -- \
	python3 "$root/tools/oracle/ini_sd_diff.py" "$runner"
