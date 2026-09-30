#!/bin/sh
# Compare this library's Win32 dialect against wine's profile API.
#
#   tools/oracle/run-ini-win32-oracle.sh
#
# **The reference is wine, not Windows**, and the gate prints that with every run.
# Two of the dialect's thirty rules have a second source - Microsoft's own
# documentation for `GetPrivateProfileString` states quote stripping and case
# insensitivity - and the rest rest on this implementation alone. That is a weaker
# position than any other INI dialect here is in, configparser included.
#
# What offsets it is that the reference has **three entry points and two of them
# disagree**: `GetPrivateProfileString` retrieves a `;disabled=1` that
# `GetPrivateProfileSection` does not list. So this is a two-reference differential
# built from one implementation, scored both ways, with the disagreement asserted
# rather than resolved.
#
# There is no `REFUSES` table, because this dialect refuses nothing. `intent`
# checks exactly that: a byte sequence this reader will not parse is a defect.
#
# INI_W32_ORACLE_COUNT caps the population, which is the axis list rather than a
# random sample - tools/oracle/ini_win32_gen.py says why.
# Needs the pinned image, which is built here: make oracle-images.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/ini-win32-oracle-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_win32_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" win32 -- \
	python3 "$root/tools/oracle/ini_win32_diff.py" "$runner"
