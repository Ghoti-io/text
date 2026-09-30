#!/bin/sh
# Score the UTF-16 decode against the profile API, over the same generated documents.
#
#   tools/oracle/run-ini-win32-encoding-oracle.sh
#
# **This gate exists because the reference turned out to answer the question.** A
# UTF-16LE `.ini` with a byte-order mark is read by `GetPrivateProfileString` and
# `GetPrivateProfileSectionNames` exactly as its UTF-8 form is, so "does our decode
# agree" is measurable rather than a claim in a header. Before this, the decode was
# asserted against hand-built buffers in tests/test-ini.cpp and against nothing else.
#
# **And the negative result is the more useful half.** Without a mark the reference
# reads nothing at all - `NAMES` empty, every `GET` MISSING - so a reader that
# sniffed for a BOM-less UTF-16 document by its shape would be reading a file the
# reference does not read. That is why this module has no content heuristic, and it
# is a stronger reason than the one it replaces ("no population to calibrate it
# against"): the reference does not guess either.
#
# The comparison is ini_win32_diff.py's, taking --encodings. One implementation,
# three populations - generated, corpus, and each generated document re-encoded.
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
runner=$root/build/ini-win32-encoding-ours
cc -O1 -o "$runner" "$root/tools/oracle/ini_win32_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" win32 -- \
	python3 "$root/tools/oracle/ini_win32_diff.py" "$runner" --encodings
