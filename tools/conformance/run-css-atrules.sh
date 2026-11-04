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
elsewhere=$root/tools/conformance/SCORED_ELSEWHERE
python3 -I "$root/tools/css/extract-wpt-atrules.py" "$wpt" "$vectors" "$elsewhere"
# a page is left out of the unscored list only if the gate named for it really reads it: the CSSOM extractor must have a program for the page
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
python3 -I "$root/tools/css/extract-wpt-props.py" "$wpt" "$scratch" --unextracted "$scratch/UNEXTRACTED" >/dev/null
python3 -I "$root/tools/css/extract-wpt-cssom.py" "$wpt" "$scratch" "$scratch/rule.json" >/dev/null
check_registry() {
	grep -v '^#' "$1" | while IFS="$(printf '\t')" read -r page gate why; do
		[ "$gate" = css-cssom ] || { echo "SCORED ELSEWHERE: $page names the gate '$gate', which this check does not know" >&2; exit 1; }
		grep -q "^P	css/$page	" "$scratch/declaration.tsv" || { echo "SCORED ELSEWHERE: $page is not read by the CSSOM gate (no program for it)" >&2; exit 1; }
	done
}
check_registry "$elsewhere" || exit 1
# the check is seen to fail: a page the CSSOM gate has no program for, and a gate it does not know
printf 'css-page/parsing/page-valid.html\tcss-cssom\tplanted\n' > "$scratch/control-page"
if check_registry "$scratch/control-page" 2>/dev/null; then echo "control scored-elsewhere-unread-page: NOT CAUGHT" >&2; exit 1; fi
printf 'css-page/cssom/margin-001.html\tcss-nowhere\tplanted\n' > "$scratch/control-gate"
if check_registry "$scratch/control-gate" 2>/dev/null; then echo "control scored-elsewhere-unknown-gate: NOT CAUGHT" >&2; exit 1; fi
n=$(grep -vc '^#' "$vectors" || true)
floor=${CSS_ATRULES_FLOOR:-320}
[ "$n" -ge "$floor" ] || { echo "only $n vectors were read; the floor is $floor" >&2; exit 1; }
unscored=$root/tools/conformance/UNSCORED_ATRULES
# the pages that are not read as data are listed, one line a page with its calls and the reason (the extractor writes them as `# unscored` lines)
grep '^# unscored	' "$vectors" | cut -f2- > "$root/build/css-atrules-unscored.tsv"
if [ "$1" = "--regenerate" ]; then
	{ printf '%s\n' "# tools/css/extract-wpt-atrules.py: the pages of the at-rule directories of css/ that are not read as data: page, number of test calls (1 for a reftest), reason (a code of REASON_CODES)."; cat "$root/build/css-atrules-unscored.tsv"; } > "$unscored"
	python3 -I "$root/tools/conformance/css_ledger.py" atrules --unscored "$unscored" --codes "$root/tools/conformance/REASON_CODES" --regenerate
	echo "wrote $(grep -vc '^#' "$unscored") lines to $unscored"
	exit 0
fi
grep -v '^#' "$unscored" | diff - "$root/build/css-atrules-unscored.tsv" > "$root/build/css-atrules-unscored.diff" || {
	echo "UNSCORED VECTORS: tools/conformance/UNSCORED_ATRULES is not what the extractor leaves unread (tools/conformance/run-css-atrules.sh --regenerate, then read the change):" >&2
	head -20 "$root/build/css-atrules-unscored.diff" >&2
	exit 1
}
# the reasons are codes of REASON_CODES and each class has the pages and calls the file pins; the check is seen to fail
ledger=$root/tools/conformance/css_ledger.py
codes=$root/tools/conformance/REASON_CODES
python3 -I "$ledger" atrules --unscored "$unscored" --codes "$codes" || exit 1
grep -v '^unmodeled:script-test	' "$codes" > "$scratch/codes-short"
if python3 -I "$ledger" atrules --unscored "$unscored" --codes "$scratch/codes-short" >/dev/null 2>&1; then echo "control atrules-reason-code-outside-set: NOT CAUGHT" >&2; exit 1; fi
awk -F'\t' 'BEGIN { OFS = "\t" } /^reftest\t/ { $5 = $5 + 1 } { print }' "$codes" > "$scratch/codes-moved"
if python3 -I "$ledger" atrules --unscored "$unscored" --codes "$scratch/codes-moved" >/dev/null 2>&1; then echo "control atrules-class-moved: NOT CAUGHT" >&2; exit 1; fi
{ cat "$unscored"; printf 'css-page/planted.html\t1\ta reftest: the answer is the rendering\n'; } > "$scratch/unscored-free"
if python3 -I "$ledger" atrules --unscored "$scratch/unscored-free" --codes "$codes" >/dev/null 2>&1; then echo "control atrules-free-text-reason: NOT CAUGHT" >&2; exit 1; fi
echo "control atrules ledger (code outside the set, class moved, free text): caught"
echo "css-atrules: $(grep -vc '^#' "$unscored") pages unscored (the file says why, a line a page), $(grep -c '^# page' "$vectors") pages read"
"$runner" "$vectors" "$root/tools/conformance/CSS_ATRULES_EXCEPTIONS"
