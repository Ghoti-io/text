#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
#
# Score the at-rule layer against the at-rule parsing vectors of the pinned web-platform-tests (tools/css/extract-wpt-atrules.py). Every failure is a line of
# CSS_ATRULES_EXCEPTIONS (kind, input, reason); a stale line fails. The floor is the least number of vectors the pages must give.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}
[ -d "$wpt/css/css-animations/parsing" ] || { echo "the pinned pages are not in third_party/css/wpt. Fetch them:  tools/css/fetch.sh" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed (tools/css/extract-wpt-atrules.py)" >&2; exit 1; }
[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-atrules-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_atrules_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"
vectors=$root/build/css-atrules-vectors.tsv
python3 -I "$root/tools/css/extract-wpt-atrules.py" "$wpt" "$vectors"
n=$(grep -vc '^#' "$vectors" || true)
floor=${CSS_ATRULES_FLOOR:-320}
[ "$n" -ge "$floor" ] || { echo "only $n vectors were read; the floor is $floor" >&2; exit 1; }
unscored=$root/tools/conformance/UNSCORED_ATRULES
# the pages that are not read as data are listed, one line a page with its calls and the reason (the extractor writes them as `# unscored` lines)
grep '^# unscored	' "$vectors" | cut -f2- > "$root/build/css-atrules-unscored.tsv"
if [ "$1" = "--regenerate" ]; then
	{ printf '%s\n' "# tools/css/extract-wpt-atrules.py: the pages of the at-rule directories of css/ that are not read as data: page, number of test calls (1 for a reftest), reason."; cat "$root/build/css-atrules-unscored.tsv"; } > "$unscored"
	echo "wrote $(grep -vc '^#' "$unscored") lines to $unscored"
	exit 0
fi
grep -v '^#' "$unscored" | diff - "$root/build/css-atrules-unscored.tsv" > "$root/build/css-atrules-unscored.diff" || {
	echo "UNSCORED VECTORS: tools/conformance/UNSCORED_ATRULES is not what the extractor leaves unread (tools/conformance/run-css-atrules.sh --regenerate, then read the change):" >&2
	head -20 "$root/build/css-atrules-unscored.diff" >&2
	exit 1
}
echo "css-atrules: $(grep -vc '^#' "$unscored") pages unscored (the file says why, a line a page), $(grep -c '^# page' "$vectors") pages read"
"$runner" "$vectors" "$root/tools/conformance/CSS_ATRULES_EXCEPTIONS"
