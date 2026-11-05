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
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
# The page record against the browsers: Chromium and Firefox run every page the scoring interpreter reads under its own testharness.js (tools/oracle/css_page_record.py), and the answers
# have to be the committed ones (tools/conformance/css_cssom_pagerecord.jsonl), which the CSSOM gate classifies the failures from. The same is asked of the replay: the flat program of
# each page, translated back to JavaScript on the real DOM (tools/css/replay_gen.py), run in the place of the page's scripts, has to answer what the committed replay record
# (tools/conformance/css_cssom_replayrecord.jsonl) says, which the CSSOM gate holds equal to the page's own run. The default gate needs no browser: it refuses a stale record (the pin, the
# harness, the driver, the generator, a page or a program changed); this is the gate that asks again. --controls changes a recorded answer and requires the refusal, then mistranslates pages
# on purpose (a dropped assertion, a changed expectation, a wrong id, a negated comparison), replays them in the browsers and requires the comparison with the page to refuse each, and
# runs the retry policy against scripted answers (a page that differs once and is as recorded on the second try is listed as unstable; one that differs twice is a change).
# Both engines run at the same time, each in its own container: on a quiet 12-thread host the whole gate (1,022 pages and 614 replays, both engines) takes minutes, not hours; a page that
# differs from the record is asked again, alone, and a host so loaded that more than a few pages differ once is refused (css_page_record.py, "Stability").
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}
[ -f "$wpt/resources/testharness.js" ] || { echo "the harness is not in $wpt; fetch it: tools/css/fetch.sh" >&2; exit 1; }
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
"$root/tools/conformance/run-css-cssom.sh" --script-pages > "$scratch/pages.txt"
committed=$root/tools/conformance/css_cssom_pagerecord.jsonl
if [ "$1" = "--controls" ]; then
	python3 - "$committed" "$scratch/tampered.jsonl" <<'PY'
import json, sys
lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
for i, l in enumerate(lines):
    if i and l:
        e = json.loads(l)
        if e['chromium'].get('tests'):
            e['chromium']['tests'][0][1] = 1 - e['chromium']['tests'][0][1] if e['chromium']['tests'][0][1] in (0, 1) else 0
            lines[i] = json.dumps(e, sort_keys=True)
            break
open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(lines))
PY
	if python3 "$root/tools/oracle/css_page_record.py" --compare "$scratch/tampered.jsonl" "$committed" >/dev/null 2>&1; then
		echo "control: a changed answer in the record was not noticed" >&2
		exit 1
	fi
	echo "control record-answer-changed: caught"
	python3 "$root/tools/oracle/css_page_record.py" --policy-controls
	"$root/tools/conformance/run-css-cssom.sh" --script-programs > "$scratch/programs.tsv"
	python3 "$root/tools/oracle/css_page_record.py" --replay-controls --programs "$scratch/programs.tsv"
	exit $?
fi
python3 "$root/tools/oracle/css_page_record.py" --record --pages "$scratch/pages.txt" --out "$scratch/record.jsonl" --against "$committed"
python3 "$root/tools/oracle/css_page_record.py" --compare "$committed" "$scratch/record.jsonl"
"$root/tools/conformance/run-css-cssom.sh" --script-programs > "$scratch/programs.tsv"
python3 "$root/tools/oracle/css_page_record.py" --replay --programs "$scratch/programs.tsv" --out "$scratch/replay.jsonl" --against "$root/tools/conformance/css_cssom_replayrecord.jsonl"
python3 "$root/tools/oracle/css_page_record.py" --replay-compare "$root/tools/conformance/css_cssom_replayrecord.jsonl" "$scratch/replay.jsonl"
