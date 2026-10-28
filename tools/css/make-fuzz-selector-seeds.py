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
"""Write the seed corpus of tests/fuzz/fuzz_css_selector.cpp into tests/fuzz/corpus/css-selector.

    tools/css/make-fuzz-selector-seeds.py [directory]

A seed is three option bytes and the selector text (see the harness). The corpus is made of the selectors of the web-platform-tests' parsing vectors (the pinned
checkout's selector.json, when it is there) and the probes of tests/css_selector_rows.h and a list of hard cases, each written in its own mode, in every mode, with the
namespaces declared, with the small limits and with an allocation failed at a few places. The files are named for a hash of what they hold and are deterministic.
Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""

import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-selector")

HARD = [
    "a", "*", "a b c d e", "a > b + c ~ d", "a || b", ":is()", ":is(,)", ":is(:is(:is(a)))", ":not(:is(.a, ::b))", ":has(> a, + b, ~ c)", ":has(:is(:has(*)))",
    "::part(a b):hover", "::part(a):is(:first-child)", "::slotted(*):is(#id)", "::slotted(:has(a))", ":host(.a):host-context(b)", "::before::marker",
    "a::before:hover", "a:before", ":nth-child(2n+1 of .a, .b)", ":nth-child(-n+3)", ":nth-last-of-type(odd)", ":nth-child(99999999999999999999n+1)",
    "[a]", "[a=b i]", "[a|=b s]", "[svg|a]", "[*|a]", "[|a]", "svg|rect", "*|*", "|a", "h|*:hover", "& > a, .b &", "a:is(&)", ":lang(de, \"fr-*\", '')",
    ":dir(rtl)", ":heading(1, 2, -3)", ":state(--x)", ":current(.a, .b)", "::highlight(*)", "::view-transition-group(x.y.z)", "::picker(select)",
    "::scroll-button(up):disabled", ":matches(a)", ":-webkit-foo", "::-moz-bar", "#1", ".1", "a b", "a  >  b", "a>b", "\\31 23", ".\\@", "a\\\\b",
    ":is(" + ":is(" * 20 + "a" + ")" * 21, "a" + ".c" * 40, ",".join("a%d" % i for i in range(20)), "", " ", ",", "a,", ",a", ">", "a >", "[", "[a", "(", ")",
    ":", "::", ":::a", ":a:", ":is(", "\x00", "a\x00b", "\xff\xfe", "\ud800",
]


def seed(o0, o1, o2, text):
    return bytes([o0, o1, o2]) + (text.encode("utf-8", "surrogatepass") if isinstance(text, str) else text)


def main():
    texts = list(HARD)
    vec = os.path.join(ROOT, "build", "css-property-vectors", "selector.json")
    if os.path.exists(vec):
        texts += [v["value"] for v in json.load(open(vec, encoding="utf-8"))]
    rows = open(os.path.join(ROOT, "tests", "css_selector_rows.h"), encoding="utf-8").read()
    texts += [m.group(1).replace('\\"', '"').replace("\\\\", "\\") for m in re.finditer(r'^  \{"(?:[^"\\]|\\.)*", "((?:[^"\\]|\\.)*)",', rows, re.M)]
    seen = []
    for t in texts:
        if t not in seen:
            seen.append(t)
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".seed") and not f.startswith("reproducer-"):
            os.remove(os.path.join(OUT, f))
    names = set()

    def write(data):
        name = "own-" + hashlib.sha1(data).hexdigest()[:12] + ".seed"
        if name in names:
            return
        names.add(name)
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(data)

    for i, t in enumerate(seen):
        write(seed(0, 0, 0, t))
        if i % 2 == 0:
            write(seed(1 + i % 4, 0, 0, t))
        if "|" in t or i % 13 == 0:
            write(seed(8 | (i % 5), 0, 0, t))
            write(seed(8 | 16, 0, 0, t))
        if len(t) > 24 or i % 17 == 0:
            write(seed(32 | (i % 5), 0, 0, t))
        if i % 6 == 0:
            for n in (2, 7):
                write(seed(i % 5, 0, n, t))
    print("wrote %d seeds to %s" % (len(names), OUT))


if __name__ == "__main__":
    main()
