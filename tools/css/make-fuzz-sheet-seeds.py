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
"""Write the seed corpus of tests/fuzz/fuzz_css_sheet.cpp into tests/fuzz/corpus/css-sheet.

    tools/css/make-fuzz-sheet-seeds.py [directory]

A seed is three option bytes, four bytes for each element of a tree, and the text of one or two style sheets (a 0x01 byte ends the first; see the harness). The corpus is made of
sheets that use every form the compiler reads (flat rules, nesting, layers, `@scope`, namespaces, conditional rules, imports, the shadow selectors and the pseudo-elements, and
invalid versions of each), each put to trees of several shapes with the options varied. The files are named for a hash of what they hold and are deterministic. Reproducers
the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""

import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-sheet")

SHEETS = [
    "a { color: red } .x { color: blue } #i1 { color: green } a.x { margin-top: 1px } li { padding-top: 2px } DIV { padding-left: 3px }",
    "p, a { color: red } .x.y, #i2 { color: blue !important } [t] { color: green } * { margin-left: 1px } :not(.x) { margin-right: 2px }",
    "a b { color: red } a > b { color: blue } a + b { color: green } a ~ b { color: orange } :is(a, b) .x { margin-top: 1px } :where(li) p { margin-top: 2px }",
    "li:first-child { color: red } li:nth-child(2n+1) { color: blue } li:last-child { color: green } :empty { margin-top: 1px } :root { margin-left: 2px }",
    "a:has(> b) { color: red } a:has(+ b) { color: blue } :not(:has(.x)) { color: green }",
    "a { color: red; & b { color: blue } .x & { color: green } > c { color: orange } &.x { margin-top: 1px } color: black }",
    "a, #i1 { & .x { color: red } .y { color: blue } & + & { color: green } @media print { color: orange; .z { margin-top: 1px } } }",
    "a { b { c { color: red } } } a { & { color: blue } }",
    "@layer base, theme; @layer theme { a { color: red } } @layer base { a { color: blue } } a { margin-top: 1px }",
    "@layer { a { color: red } } @layer { a { color: blue } } @layer x { @layer y { b { color: green } } b { color: orange } }",
    "@layer a { p { color: red !important } } @layer b { p { color: blue !important } } @layer a, b;",
    "@import 'x.css' layer(l); @layer m; a { color: red }",
    "@import url(x.css) supports(display: grid) screen; a { color: red } @import 'late.css';",
    "@namespace svg url(http://www.w3.org/2000/svg); @namespace url(http://www.w3.org/1999/xhtml); svg|a { color: red } *|b { color: blue } |c { color: green }",
    "@media print { a { color: red } @layer q { b { color: blue } } } @media screen { a { color: green } } @supports (display: grid) { a { margin-top: 1px } }",
    "@container (min-width: 1px) { a { color: red } } @starting-style { a { color: blue } }",
    "@scope (.x) { a { color: red } } @scope (.y) { a { color: blue } }",
    "@scope (.x) to (.y) { a { color: red } :scope { color: blue } & b { color: green } }",
    "@scope (.x) { @scope (.y) { a { color: red } } color: blue }",
    "@scope { a { color: red } color: blue }",
    "@scope (a) to (b) { * { color: red } .x { color: blue } } a { & @scope (.x) { c { color: green } } }",
    ":host { color: red } :host(.x) { color: blue } :host-context(a) { color: green } ::slotted(a) { margin-top: 1px } a::part(label) { margin-left: 1px }",
    "a::before { color: red } a::after { color: blue } li::marker { color: green } p::first-line { margin-top: 1px } a::before::marker { color: orange }",
    "a:hover { color: red } a:link { color: blue } :checked { color: green } a:focus-visible { margin-top: 1px } :lang(en) { margin-left: 1px }",
    "a { color: nonsense; colour: red; margin-top: 3px; } b { --custom: 1; color: var(--custom) } c { margin-top: calc(1px + 2px) } d { all: unset }",
    "a:nonesuch { color: red } b { color: blue } :is( { color: green } c, : { color: orange } d { margin-top: 1px }",
    "@layer; @layer 1x { a {} } @layer inherit { a { color: red } } @import; @namespace; @scope ( { a {} } @scope (a) to { b {} } a { color: red }",
    "a { color: red } \x01 :host { color: blue } p { color: green } ::slotted(a) { margin-top: 1px } @layer s { :host(.x) { margin-left: 1px } }",
    "a { color: red !important } \x01 b { color: blue !important } a { color: green }",
    "@layer a, b; @layer a { a { color: red } } \x01 @layer b, a; @layer b { a { color: blue } }",
]


def tree(rng, shape, n):
    out = []
    for i in range(n):
        if shape == 0:
            parent = i
        elif shape == 1:
            parent = 1 if i else 0
        elif shape == 2:
            parent = (i - 1) // 3 + 1 if i else 0
        else:
            parent = rng.randrange(0, i + 1) if i else 0
        out.append(bytes([parent, rng.randrange(6), rng.randrange(8), rng.randrange(4)]))
    return out


def main():
    rng = random.Random(20261029)
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".seed") and not f.startswith("reproducer-"):
            os.remove(os.path.join(OUT, f))
    names = set()
    for si, sheet in enumerate(SHEETS):
        for k in range(9):
            shape = (si + k) % 4
            n = [1, 3, 5, 8, 12, 16][(si * 7 + k) % 6]
            o0 = [0, 1, 1 | 4, 2, 1 | 2, 8 | 1, 16 | 1, 16, 32 | 1][k]
            data = bytes([o0, n - 1, 0]) + b"".join(tree(rng, shape, n)) + sheet.encode("latin-1")
            name = "own-" + hashlib.sha1(data).hexdigest()[:12] + ".seed"
            if name not in names:
                names.add(name)
                with open(os.path.join(OUT, name), "wb") as f:
                    f.write(data)
    print("wrote %d seeds to %s" % (len(names), OUT))


if __name__ == "__main__":
    main()
