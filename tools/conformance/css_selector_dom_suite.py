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
"""Score the Markup queries against the selector tables of web-platform-tests' dom/nodes.

    css_selector_dom_suite.py <tables.json> <runner> <exceptions> [--regenerate]

`tables.json` is made by tools/css/extract-wpt-selectors.py from `selectors.js`. The runner (tools/conformance/css_selector_dom_suite.c) holds the fixture those tests run
in -- `ParentNode-querySelector-All-content.html` and the special elements `ParentNode-querySelector-All.js` adds -- in the four contexts of the DOM tests, and answers
`querySelectorAll()`, `querySelector()` and `matches()`. What is scored, as the pages `ParentNode-querySelector-All.html` and `Element-matches.html` run it:

* **invalid** (`invalidSelectors`): every selector must be a syntax error.
* **valid, queries** (`validSelectors` with the bit TEST_QSA): in the contexts `document`, `element` (the in-document element, with a copy of itself added to the body that
  nothing may match), `detached` and `fragment`, minus those the entry excludes (and the entries that exclude `html`, the document type of the fixture), the ids returned by
  `querySelectorAll()` are `expect`, in tree order, and `querySelector()` returns the first.
* **valid and scoped, matches** (`validSelectors` and `scopedSelectors` with the bit TEST_MATCH): in the document, each element of `expect` matches the selector.

Every failure is a line of CSS_SELECTOR_DOM_EXCEPTIONS: the key, the selector, the answer the library gives, a class (draft-newer, wpt-tentative, needs-environment, wpt-wrong)
and a reason that quotes the sentence of the served draft that decides it. The gate fails on an unlisted failure ("UNEXPLAINED FAILURES"), a listed case that passes, is another
selector or has another answer ("STALE EXCEPTIONS"), a reason that quotes no sentence or a class that is not one of the four ("SUITE PROBLEMS"), and fewer cases than the floor
(CSS_SELECTOR_DOM_MIN_SCORED, "below the floor").
"""

import json
import os
import re
import subprocess
import sys

CLASSES = ('draft-newer', 'wpt-tentative', 'needs-environment', 'wpt-wrong')
CONTEXTS = ('document', 'element', 'detached', 'fragment')


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def quotes_a_sentence(reason):
    return any(len(q) >= 20 and len(q.split()) >= 4 for q in re.findall(r'"([^"]*)"', reason))


def run_runner(runner, fixture, requests):
    data = ''.join('\t'.join(r[:-1] + (escape(r[-1]),)) + '\n' for r in requests)
    proc = subprocess.run([runner, fixture], input=data.encode('utf-8', 'surrogateescape'), stdout=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        print('SUITE PROBLEMS: the runner exited %d' % proc.returncode)
        sys.exit(1)
    lines = proc.stdout.decode('utf-8', 'surrogateescape').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    if len(lines) != len(requests):
        print('SUITE PROBLEMS: the runner answered %d lines to %d requests' % (len(lines), len(requests)))
        sys.exit(1)
    return lines


def excluded(entry, ctx):
    ex = entry.get('exclude') or []
    return ctx in ex or 'html' in ex


def cases(tables):
    """[(key, selector, kind, request, expected)] for everything the pages run."""
    out = []
    for n, e in enumerate(tables['invalid']):
        out.append(('invalid:%d' % n, e['selector'], 'syntax', ('Q', 'document', e['selector']), 'SYNTAX'))
    for n, e in enumerate(tables['valid']):
        if e['testType'] & 1:
            for ctx in CONTEXTS:
                if not excluded(e, ctx):
                    out.append(('valid:%d@%s' % (n, ctx), e['selector'], 'query', ('Q', ctx, e['selector']), 'OK\t' + ' '.join(e['expect'])))
    for kind in ('valid', 'scoped'):
        for n, e in enumerate(tables[kind]):
            if e['testType'] & 16 and not excluded(e, 'document'):
                for i in e['expect']:
                    out.append(('%s:%d@matches:%s' % (kind, n, i), e['selector'], 'match', ('M', i, e['selector']), 'TRUE'))
                for i in e.get('unexpected') or []:
                    out.append(('%s:%d@matches-not:%s' % (kind, n, i), e['selector'], 'match', ('M', i, e['selector']), 'FALSE'))
    return out


def load_exceptions(path):
    entries, problems = {}, []
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) != 5:
            problems.append('%s:%d: %d columns, not five' % (path, n, len(f)))
            continue
        if f[0] in entries:
            problems.append('%s:%d: %s twice' % (path, n, f[0]))
        entries[f[0]] = f
    return entries, problems


def main():
    args = sys.argv[1:]
    regenerate = '--regenerate' in args
    args = [a for a in args if a != '--regenerate']
    tables_path, runner, exceptions_path, fixture = args[:4]
    tables = json.load(open(tables_path, encoding='utf-8'))
    all_cases = cases(tables)
    answers = run_runner(runner, fixture, [c[3] for c in all_cases])
    failures = []
    for (key, selector, kind, req, expected), got in zip(all_cases, answers):
        if got != expected:
            failures.append((key, selector, got))
    if regenerate:
        with open(exceptions_path, 'w', encoding='utf-8') as f:
            f.write('# tools/conformance/css_selector_dom_suite.py: the cases of web-platform-tests\' dom/nodes selector tables that fail and whose expectation does not bind this library, one line a case.\n')
            f.write('# Columns (tab separated): key (table:index@context), selector, the answer the library gives, class, reason.\n')
            for key, selector, got in failures:
                f.write('\t'.join([key, escape(selector), escape(got), 'UNCLASSIFIED', 'TODO']) + '\n')
        print('wrote %d' % len(failures))
        return 0
    exceptions, problems = load_exceptions(exceptions_path)
    for key, f in exceptions.items():
        if f[3] not in CLASSES:
            problems.append('%s: class %r is not one of %s' % (key, f[3], ', '.join(CLASSES)))
        elif not quotes_a_sentence(f[4]):
            problems.append('%s: the reason quotes no sentence of the served draft' % key)
    unexplained, stale, seen = [], [], set()
    for key, selector, got in failures:
        e = exceptions.get(key)
        if not e:
            unexplained.append((key, selector, got))
            continue
        seen.add(key)
        if unescape(e[1]) != selector or unescape(e[2]) != got:
            stale.append('%s: listed as %r -> %r, the library gives %r -> %r' % (key, unescape(e[1]), unescape(e[2]), selector, got))
    for key in exceptions:
        if key not in seen:
            stale.append('%s: listed, and it passes or does not exist' % key)
    floor = int(os.environ.get('CSS_SELECTOR_DOM_MIN_SCORED', '1000'))
    out = False
    if problems:
        print('SUITE PROBLEMS:')
        print('\n'.join('  ' + p for p in problems))
        out = True
    if unexplained:
        print('UNEXPLAINED FAILURES: %d' % len(unexplained))
        for key, selector, got in unexplained[:80]:
            print('  %s %r -> %r' % (key, selector, got))
        out = True
    if stale:
        print('STALE EXCEPTIONS: %d' % len(stale))
        print('\n'.join('  ' + s for s in stale[:60]))
        out = True
    if len(all_cases) < floor:
        print('below the floor: %d scored, the floor is %d' % (len(all_cases), floor))
        out = True
    if out:
        return 1
    print('css selector queries: %d scored, %d pass, %d exceptions (invalid %d, valid %d, scoped %d in the table)'
          % (len(all_cases), len(all_cases) - len(failures), len(exceptions), len(tables['invalid']), len(tables['valid']), len(tables['scoped'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
