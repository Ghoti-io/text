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
"""Account for the pages of web-platform-tests' css/selectors that ask the DOM.

    css_selector_pages.py <wpt checkout> <UNSCORED_SELECTOR_PAGES>

The scored pages of css/selectors/parsing are tables of selectors (css_selector_suite.py), and dom/nodes' selector tables are scored by css_selector_dom_suite.py. The other
pages of css/selectors call `querySelector()`, `querySelectorAll()`, `matches()` or `closest()` from a script of their own, with the fixture and the expectation written beside
the call. Nothing here reads that script, so a page is only ever missing from the count: this checker lists them all, so that a page which is not read is a line with a
reason and not an absence nobody sees (the `:scope` inside `:has()` was one of them). A page of that kind that is not listed fails ("UNLISTED PAGES"), a listed page that
does not exist or no longer calls the DOM fails ("STALE PAGES"), and a reason that is a few words fails ("SUITE PROBLEMS"). The columns, tab separated: page (under css/selectors),
class (state, mutation or script), reason.
"""

import os
import re
import sys

CLASSES = ('state', 'mutation', 'script')
CALLS = re.compile(r'querySelector|querySelectorAll|\.matches\(|\.closest\(')


def asking_pages(base):
    out = []
    for d, _, files in os.walk(base):
        for f in files:
            if not f.endswith(('.html', '.js', '.xht')):
                continue
            rel = os.path.relpath(os.path.join(d, f), base)
            if rel.startswith('parsing' + os.sep):
                continue
            with open(os.path.join(d, f), encoding='utf-8', errors='replace') as h:
                if CALLS.search(h.read()):
                    out.append(rel)
    return sorted(out)


def main():
    wpt, listing = sys.argv[1:3]
    base = os.path.join(wpt, 'css', 'selectors')
    listed = {}
    problems = []
    with open(listing, encoding='utf-8') as h:
        for line in h:
            if line.startswith('#') or not line.strip():
                continue
            f = line.rstrip('\n').split('\t')
            if len(f) != 3 or f[1] not in CLASSES or len(f[2].split()) < 5:
                problems.append(line.rstrip('\n')[:120])
                continue
            listed[f[0]] = f
    pages = asking_pages(base)
    unlisted = [p for p in pages if p not in listed]
    stale = [p for p in listed if p not in pages]
    if problems:
        print('SUITE PROBLEMS: %d' % len(problems))
        for p in problems:
            print('  ' + p)
    if unlisted:
        print('UNLISTED PAGES: %d' % len(unlisted))
        for p in unlisted:
            print('  ' + p)
    if stale:
        print('STALE PAGES: %d' % len(stale))
        for p in stale:
            print('  ' + p)
    if problems or unlisted or stale:
        sys.exit(1)
    print('css/selectors pages that ask the DOM: %d, all listed (%s)' % (len(pages), ', '.join('%d %s' % (sum(1 for v in listed.values() if v[1] == c), c) for c in CLASSES)))


main()
