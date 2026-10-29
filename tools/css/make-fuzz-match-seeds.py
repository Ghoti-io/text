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
"""Write the seed corpus of tests/fuzz/fuzz_css_match.cpp into tests/fuzz/corpus/css-match.

    tools/css/make-fuzz-match-seeds.py [directory]

A seed is three option bytes, six bytes for each element of a tree, and the selector text (see the harness). The corpus is made of a list of selectors that use every form the
reference matcher knows (and some it does not), each put to trees of several shapes (a chain, a star, wide parents, mixed trees from a fixed generator) with the limits and
options varied. The files are named for a hash of what they hold and are deterministic. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""

import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-match")

SELECTORS = [
    "a", "*", ".x", ".x.y", "#i1", "[t]", "[t=x]", "[t=X i]", "[t~=x]", "[t|=x]", "[t^=x]", "[t$=y]", "[t*=-]", "a b", "a > b", "a + b", "a ~ b", "a b c", "a > b > c",
    "div p", "DIV", ":is(a, b) .x", ":not(.x)", ":where(.y) li", "li:first-child", "li:last-child", "li:only-child", "li:nth-child(2n+1)", "li:nth-last-child(2)",
    "li:nth-child(odd of .x)", "li:nth-last-child(2n of .y, .z)", "p:nth-of-type(2)", "li:first-of-type", "li:last-of-type", "li:only-of-type", "a:nth-last-of-type(1)", ":empty",
    ":root", ":scope > *", ":scope", "&", "a:has(> b)", "a:has(b)", "a:has(+ b)", "a:has(~ b)", "a:has(> b > c)", "a:has(b c)", ":not(:has(.x))", ".x:has(~ .y ~ .z)",
    "li:is(:first-child, :last-child)", ":hover", ":checked, :disabled", "a:not(:link)", ":any-link", ":is(a, b):not(.x):has(> c)", "a:has(:scope b)", "* *", "* > *", "* + *",
    "* ~ *", "a ~ b ~ c", "a + b + c", "a b + c", "a > b ~ c", ":not(a b)", ":is(a b, c > d)", ":nth-child(1 of :not(.x))", ":is()", ":not(:not(a))", "a:has(:has(b))",
    "a:lang(en)", "::before", "a::before", "|a", "*|a", "a:host", ":has(> .x)", "p:has(+ p)", "li:has(~ li:last-child)",
    ":nth-child(1 of :not(:scope))", "li:nth-child(odd of :scope > *)", "li:nth-last-child(2n of :not(:scope), .x)", ":scope :nth-of-type(2)", ":has(> :scope) > *",
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
        out.append(bytes([parent, rng.randrange(6), rng.randrange(8), rng.randrange(4), rng.randrange(5), rng.randrange(8)]))
    return out


def main():
    rng = random.Random(20261028)
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".seed") and not f.startswith("reproducer-"):
            os.remove(os.path.join(OUT, f))
    names = set()
    for si, sel in enumerate(SELECTORS):
        for k in range(9):
            shape = (si + k) % 4
            n = [1, 3, 7, 12, 18, 24][(si * 7 + k) % 6]
            if shape == 0 and n > 20:
                n = 20
            o0 = [0, 0, 4, 1, 2, 3, 8, 16 | 4, 32][k]
            data = bytes([o0, n - 1, 0]) + b"".join(tree(rng, shape, n)) + sel.encode()
            name = "own-" + hashlib.sha1(data).hexdigest()[:12] + ".seed"
            if name not in names:
                names.add(name)
                with open(os.path.join(OUT, name), "wb") as f:
                    f.write(data)
    print("wrote %d seeds to %s" % (len(names), OUT))


if __name__ == "__main__":
    main()
