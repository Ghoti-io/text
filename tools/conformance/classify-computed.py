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
"""Write CSS_COMPUTED_EXCEPTIONS and CSS_COMPUTED_DEFECTS from the failures of the computed-value gate and the table that says how each kind is listed.

    CSS_COMPUTED_DUMP=failures.json tools/conformance/run-css-computed.sh        (writes the failures)
    tools/conformance/classify-computed.py failures.json computed_classes.tsv CSS_COMPUTED_EXCEPTIONS CSS_COMPUTED_DEFECTS

A failure that no row of the table matches is an error, and so is a row that matches no failure (a stale row). The output is a function of the failures and the table.
"""

import json
import re
import sys

HEADER_EXC = """# The cases of the computed-value gate (tools/conformance/css_computed_suite.py) where the page's expectation does not bind: the vector, the property, the value, the stage,
# the answer this library gives, the class and the sentence of the served text that says so. Written by tools/conformance/classify-computed.py from
# tools/conformance/computed_classes.tsv; do not edit. In this file the library must give exactly the answer listed.
"""
HEADER_DEF = """# The cases of the computed-value gate where the page is right and this library is not: the vector, the property, the value, the stage, the answer this library gives,
# the owner (the piece of work that closes it) and what the library lacks. Written by tools/conformance/classify-computed.py from tools/conformance/computed_classes.tsv;
# do not edit. The number of lines only goes down (CSS_COMPUTED_MAX_DEFECTS in the Makefile).
"""


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def main(argv):
    failures_path, table_path, exc_path, def_path = argv[1:5]
    failures = json.load(open(failures_path, encoding='utf-8'))
    rows = []
    for n, line in enumerate(open(table_path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) != 5:
            sys.exit('%s:%d: not property, answer, kind, class, reason' % (table_path, n))
        rows.append((re.compile(f[0]), re.compile(f[1]), f[2], f[3], f[4], n))
    used = set()
    exc, dfc = [], []
    unmatched = []
    for ident in sorted(failures):
        stage, answer, prop, value, _expected = failures[ident][:5]
        for rx_p, rx_a, kind, cls, reason, n in rows:
            if rx_p.fullmatch(prop) and rx_a.search(answer):
                used.add(n)
                line = '\t'.join([ident, prop, escape(value), stage, escape(answer), cls, reason + '; the page expects ' + escape(str(_expected)) + ' of `' + prop + ': ' + escape(value) + '`'])
                (exc if kind == 'exception' else dfc).append(line)
                break
        else:
            unmatched.append('%s: %s %r -> %r' % (ident, prop, value, answer))
    if unmatched:
        sys.stderr.write('no row of the table matches %d failures:\n  %s\n' % (len(unmatched), '\n  '.join(unmatched[:30])))
        return 1
    stale = [n for _a, _b, _c, _d, _e, n in rows if n not in used]
    if stale:
        sys.stderr.write('rows of the table that match no failure: %s\n' % ', '.join(map(str, stale)))
        return 1
    with open(exc_path, 'w', encoding='utf-8') as h:
        h.write(HEADER_EXC + '\n'.join(exc) + ('\n' if exc else ''))
    with open(def_path, 'w', encoding='utf-8') as h:
        h.write(HEADER_DEF + '\n'.join(dfc) + ('\n' if dfc else ''))
    print('classify-computed: %d exceptions, %d defects' % (len(exc), len(dfc)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
