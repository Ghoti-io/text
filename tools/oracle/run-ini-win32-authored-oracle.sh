#!/bin/sh
# Score our reader against the profile API over files the profile API wrote.
#
#   tools/oracle/run-ini-win32-authored-oracle.sh
#
# **The one population here whose provenance is right by construction.** The corpus
# gate reads this machine's real `.ini` files and states with every run that they are
# real bytes of the right shape from the wrong provenance - two were written by a
# Windows application, the other 699 belong to freedesktop and Python - and nothing on
# this host changes that. What can be changed is who wrote the population:
# `WritePrivateProfileString` is the other half of the same reference, so asking it to
# author a file produces a `.ini` file of exactly the right provenance.
#
# It is also the only gate here that exercises the reference as a **writer** rather
# than as a reader. What its first run turned up was already known - a one-off probe
# measured that `WritePrivateProfileString` strips a value's leading and trailing
# whitespace, and documentation/formats/ini.md records it - so this confirms rather
# than discovers. The value is the confirmation being automatic: the rule now sits
# under a gate instead of in a sentence that nothing would contradict.
#
# The comparison is ini_win32_diff.py's, taking --authored. One implementation, four
# populations - generated, corpus, re-encoded, and authored.
#
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
runner=$root/build/ini-win32-authored-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_win32_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" win32 -- \
	python3 "$root/tools/oracle/ini_win32_diff.py" "$runner" --authored
