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
"""Read the computed-color vectors of web-platform-tests that compare numbers approximately, as data.

    tools/css/extract-wpt-color-fuzzy.py WPT_ROOT OUT.tsv

`fuzzy_test_computed_color(specified, expected[, epsilon])` (css/support/color-testcommon.js) asks for the computed `color` of `specified` and accepts any answer
whose numbers are within `epsilon` (0.01 by default) of `expected`'s and whose text without the numbers is equal. The conformance gate scores only the exact
`test_computed_value` vectors; these are the mixes and the relative colors, which cannot be exact, and tests/test-css-color-vectors.cpp reads the file this writes.
A `for (const colorSpace of [...])` block is expanded (`${colorSpace}`, `${resultColorSpace}`). Columns: page, specified, expected, epsilon.
"""
import os
import re
import sys

PAGES = "css/css-color/parsing"


def expand(body):
    out = []
    for m in re.finditer(r"for \(const colorSpace of \[([^\]]*)\]\) \{", body):
        pass
    return out


def calls(text):
    """The calls of fuzzy_test_computed_color in a page, the loops expanded."""
    res = []
    pos = 0
    loop = re.compile(r"for \(const colorSpace of \[([^\]]*)\]\)\s*\{")
    call = re.compile(r"fuzzy_test_computed_color\(\s*`([^`]*)`\s*,\s*`([^`]*)`\s*(?:,\s*([0-9.]+))?\s*\)")
    pieces = []
    while pos < len(text):
        m = loop.search(text, pos)
        if not m:
            pieces.append((None, text[pos:]))
            break
        pieces.append((None, text[pos:m.start()]))
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        names = re.findall(r'"([^"]*)"', m.group(1))
        pieces.append((names, text[m.end():i - 1]))
        pos = i
    for names, body in pieces:
        for line in body.split("\n"):
            line = re.sub(r"//.*$", "", line) if "`" not in line.split("//")[0][-1:] else line
            for c in call.finditer(line):
                spec, exp, eps = c.group(1), c.group(2), c.group(3) or "0.01"
                if "var(" in spec:
                    continue   # needs the page's own style rule for the custom property
                if names is None:
                    if "${" not in spec + exp:
                        res.append((spec, exp, eps))
                else:
                    for n in names:
                        r = "xyz-d65" if n == "xyz" else n
                        s = spec.replace("${colorSpace}", n).replace("${resultColorSpace}", r)
                        e = exp.replace("${colorSpace}", n).replace("${resultColorSpace}", r)
                        if "${" not in s + e:
                            res.append((s, e, eps))
    return res


def main():
    root, out = sys.argv[1], sys.argv[2]
    rows = []
    d = os.path.join(root, PAGES)
    for name in sorted(os.listdir(d)):
        if not name.endswith(".html") or "computed" not in name and "alpha" not in name:
            continue
        with open(os.path.join(d, name), encoding="utf-8") as h:
            text = h.read()
        if "fuzzy_test_computed_color(" not in text:
            continue
        for spec, exp, eps in calls(text):
            rows.append((name, spec, exp, eps))
    with open(out, "w", encoding="utf-8") as h:
        h.write("# Written by tools/css/extract-wpt-color-fuzzy.py from the pinned web-platform-tests; do not edit.\n# page, specified, expected, epsilon\n")
        for r in rows:
            h.write("\t".join(r) + "\n")
    print("%d vectors from %d pages" % (len(rows), len({r[0] for r in rows})))


if __name__ == "__main__":
    main()
