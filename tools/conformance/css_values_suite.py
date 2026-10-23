#!/usr/bin/env python3
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
"""Score the CSS value layer (math functions, colors, images, transforms, easing, shapes) against the value
tests of web-platform-tests' css/css-values and css/css-color/parsing.

    css_values_suite.py <wpt vectors json> <runner> <exceptions>

The vectors are made by tools/css/extract-wpt-values.py from the pinned checkout (the pages' own scripts, run
with recording helpers). A vector is a value a page puts into a property and what it expects (kind "valid": the
specified serialization; "invalid": no match; "equiv": the two values are written alike, test_math_specified).
tools/css/ has no registry of properties yet (C1c), so PROP_GRAMMAR gives each property a hand-written grammar; a
property without one is not scored and is counted, and the gate has a floor on how many were scored.

A colour is compared as the pages compare it (color-testcommon.js, fuzzy_test_valid_color): the numbers within
`epsilon` (0.01 unless the page gives one), and what is not a number equal.

Every failure has to be listed in tools/conformance/CSS_VALUES_EXCEPTIONS with the answer the library gives and the sentence of the
served draft that makes the page's expectation not apply; the library must then give exactly that answer. The gate fails
on a failure that is not listed ("UNEXPLAINED FAILURES"), on a listed case that passes or does not exist or gives
another answer ("STALE EXCEPTIONS"), on a reason that quotes no sentence ("SUITE PROBLEMS"), when fewer cases were
scored than the floor (CSS_VALUES_MIN_SCORED, "below the floor") and on a runner that does not answer.
"""

import json
import math
import os
import re
import subprocess
import sys

PROP_GRAMMAR = {
    'color': '<color>', 'background-color': '<color>',
    'opacity': '<number> | <percentage>', 'scale': '<number> | <percentage>',
    'transform': 'none | <transform-list>',
    'width': 'auto | <length-percentage>', 'height': 'auto | <length-percentage>', 'left': 'auto | <length-percentage>',
    'margin-top': 'auto | <length-percentage>', 'margin-left': 'auto | <length-percentage>',
    'text-indent': '<length-percentage>', 'flex-basis': 'auto | <length-percentage>',
    'letter-spacing': 'normal | <length>', 'outline-offset': '<length>', 'border-left-width': '<length>',
    'transition-delay': '<time>', 'animationDuration': '<time>', 'rotate': '<angle>',
    'z-index': 'auto | <integer>', 'image-resolution': '<resolution>', 'grid-template-rows': '<flex>',
    'font-size': '<length-percentage>',
}


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: '\n' if m.group(1) == 'n' else m.group(1), s)


def numbers(s):
    out = []
    for part in re.split(r'( |\(|,)', s):
        m = re.match(r'[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?', part)
        if m:
            out.append(float(m.group(0)))
    return out


def fuzzy(got, want, eps):
    if re.sub(r'[0-9.]', '', got) != re.sub(r'[0-9.]', '', want):
        return False
    a, b = numbers(got), numbers(want)
    return len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


def approx_number(s):
    s = s[5:-1] if s.startswith('calc(') else s
    m = re.match(r'[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?', s)
    return float(m.group(0)) if m else None


def run_runner(runner, requests):
    data = ''.join(escape(g) + '\t' + escape(i) + '\n' for g, i in requests)
    proc = subprocess.run([runner], input=data.encode('utf-8'), stdout=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        print('SUITE PROBLEMS: the runner exited %d' % proc.returncode)
        sys.exit(1)
    lines = proc.stdout.decode('utf-8').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    if len(lines) != len(requests):
        print('SUITE PROBLEMS: the runner answered %d lines to %d requests' % (len(lines), len(requests)))
        sys.exit(1)
    out = []
    for line in lines:
        if line == 'NO':
            out.append(None)
        elif line.startswith('OK\t'):
            out.append(unescape(line[3:]))
        else:
            out.append('!' + line)
    return out


def load_exceptions(path):
    entries = {}
    problems = []
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) < 3:
            problems.append('%s:%d: not <id> TAB <answer> TAB <reason>' % (path, n))
            continue
        if '"' not in f[2]:
            problems.append('%s:%d: the reason quotes no sentence of the draft' % (path, n))
        entries[f[0]] = (unescape(f[1]), f[2])
    return entries, problems


def main():
    vectors_path, runner, exceptions_path = sys.argv[1:4]
    vectors = json.load(open(vectors_path, encoding='utf-8'))
    exceptions, problems = load_exceptions(exceptions_path)
    scored, skipped = [], 0
    requests = []
    for v in vectors:
        g = PROP_GRAMMAR.get(v['prop'])
        if not g:
            skipped += 1
            continue
        scored.append(v)
        requests.append((g, v['value']))
        if v['kind'] == 'equiv':
            requests.append((g, v['expected']))
    answers = run_runner(runner, requests)
    failures = {}
    k = 0
    for v in scored:
        ident = '%s#%d' % (v['file'], v['n'])
        a = answers[k]
        k += 1
        if v['kind'] == 'equiv':
            b = answers[k]
            k += 1
        if (isinstance(a, str) and a.startswith('!')) or (v['kind'] == 'equiv' and isinstance(b, str) and b.startswith('!')):
            problems.append('%s: the runner could not run it (%s)' % (ident, a))
            continue
        if v['kind'] == 'invalid':
            ok = a is None
            answer = 'INVALID' if a is None else a
        elif v['kind'] == 'valid':
            alts = [x for x in v.get('alts', '').split('\x01') if x]
            ok = a is not None and (a == v['expected'] or a in alts or ('epsilon' in v and fuzzy(a, v['expected'], float(v['epsilon']))))
            answer = 'INVALID' if a is None else a
        else:
            if a is None or b is None:
                ok = False
            elif a == b:
                ok = True
            elif v.get('approx'):
                x, y = approx_number(a), approx_number(b)
                ok = x is not None and y is not None and abs(x - y) <= v['approx']
            else:
                ok = False
            answer = '%s <=> %s' % ('INVALID' if a is None else a, 'INVALID' if b is None else b)
        if not ok:
            failures[ident] = (answer, v)
    if os.environ.get('CSS_VALUES_DUMP'):
        json.dump({i: [f[0], f[1]['kind'], f[1]['prop'], f[1]['value']] for i, f in failures.items()}, open(os.environ['CSS_VALUES_DUMP'], 'w'))
    floor = int(os.environ.get('CSS_VALUES_MIN_SCORED', '4000'))
    unexplained = {i: f for i, f in failures.items() if i not in exceptions}
    stale = []
    for ident, (answer, reason) in exceptions.items():
        if ident not in failures:
            stale.append('%s: the case passes or does not exist' % ident)
        elif failures[ident][0] != answer:
            stale.append('%s: the library answers %r, the list says %r' % (ident, failures[ident][0], answer))
    bad = False
    if problems:
        print('SUITE PROBLEMS:')
        for p in problems:
            print('  ' + p)
        bad = True
    if unexplained:
        print('UNEXPLAINED FAILURES: %d' % len(unexplained))
        for ident, (answer, v) in list(unexplained.items())[:(10**9 if os.environ.get('CSS_VALUES_LIST_ALL') else 40)]:
            print('  %s  %s %s %s -> %s' % (ident, v['kind'], v['prop'], v['value'], answer))
        bad = True
    if stale:
        print('STALE EXCEPTIONS: %d' % len(stale))
        for s in stale[:40]:
            print('  ' + s)
        bad = True
    if len(scored) < floor:
        print('scored %d cases, below the floor of %d' % (len(scored), floor))
        bad = True
    print('css values: %d scored, %d passed, %d listed exceptions, %d not scored (no grammar for the property)' %
          (len(scored), len(scored) - len(failures), len(failures) - len(unexplained), skipped))
    sys.exit(1 if bad else 0)


main()
