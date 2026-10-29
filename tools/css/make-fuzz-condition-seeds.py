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
"""Write the seed corpus of tests/fuzz/fuzz_css_condition.cpp into tests/fuzz/corpus/css-condition.

    tools/css/make-fuzz-condition-seeds.py [directory]

A seed is two option bytes and a payload (see the harness). The corpus is the probes of tests/css_media_rows.h and a list of hard cases as query text, each plain,
with the small limits and with an allocation failed at a few places, and deterministic payloads for the generator mode. Files are named for a hash of what they hold.
Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""
import hashlib
import os
import re
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-condition")

HARD = [
    "", " ", "all", "(color)", "not (color)", "only screen and (color)", "not screen and (min-width: 5px) and (max-width: 40px)", "(400px <= width <= 700px), print",
    "(400px < width > 700px)", "((color))", "(((((((((((color)))))))))))", "(foo: bar), (--x), not (--y)", "(aspect-ratio: 16/9) or (width > calc(100px + 5em))",
    "(aspect-ratio: calc(16) / calc(9))", "(width: calc(1px + 1rem))", "(min-resolution: 2x)", "(resolution: infinite)", "(min-orientation: portrait)", "(min-width)",
    "foo(bar) and (color)", "(  )", "unknown and (hover)", "(example, all,), speech", "\\{screen and (--\\(FOO: bar)", "(calc(1px) < width)", "(width > 10ex)",
    "(color) and (hover) or (pointer)", "not not (color)", "(not (not (not (color))))", "screen and not (color)", "or and (color)", "layer", "(aspect-ratio: 0/0)",
    "(aspect-ratio: 1/0) and (aspect-ratio > 0/1)", ",,,", "(color", "color)", "(width: -5px)", "(resolution: -300dpi)", "(grid: 2)", "(-webkit-min-device-pixel-ratio: 2)",
]


def add(out, o0, o1, payload):
    data = bytes([o0, o1]) + payload
    out[hashlib.sha256(data).hexdigest()[:12]] = data


def main():
    out = {}
    probes = []
    p = os.path.join(ROOT, "tests", "css_media_rows.h")
    if os.path.exists(p):
        for m in re.finditer(r'\{"[^"]*", "([^"]*)", "([^"]*)"', open(p, encoding="utf-8").read()):
            probes += [m.group(1), m.group(2)]
    for q in HARD + probes:
        b = q.encode("utf-8")
        add(out, 0, 0, b)
        add(out, 2, 0, b)
        for k in (1, 5, 17):
            add(out, 0, k, b)
    rnd = random.Random(1)
    for i in range(400):
        n = rnd.randrange(4, 80)
        add(out, 1 | (2 if i % 5 == 0 else 0), 0 if i % 4 else rnd.randrange(1, 40), bytes(rnd.randrange(256) for _ in range(n)))
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.startswith("own-"):
            os.remove(os.path.join(OUT, f))
    for h, d in sorted(out.items()):
        with open(os.path.join(OUT, "own-%s.seed" % h), "wb") as f:
            f.write(d)
    print("wrote %d seeds to %s" % (len(out), OUT))


main()
