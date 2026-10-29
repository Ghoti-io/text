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
"""Write the seed corpus of tests/fuzz/fuzz_css_atrule.cpp into tests/fuzz/corpus/css-atrule.

    tools/css/make-fuzz-atrule-seeds.py [directory]

A seed is two bytes (which allocation to fail in the second pass) followed by style sheet text. The text is built from fragments of every at-rule the model knows, each alone,
in pairs, wrapped in `@layer` blocks and in `@media`, and cut at several points, so the parser meets the broken forms. Files are named for a hash of what they hold.
Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""
import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-atrule")

FRAGMENTS = [
    '@font-face { font-family: A; src: url(a.woff2) format(woff2) tech(variations), local("A b"); font-weight: 400 700; unicode-range: U+25-FF, U+4??; font-display: swap }',
    '@font-face { font-family: "B c"; src: local(B); font-style: oblique 10deg 20deg; font-feature-settings: "liga" 1, "kern"; font-variation-settings: "wght" 300 }',
    '@font-face { font-family: C; ascent-override: 90% 80%; size-adjust: 110%; font-width: 75% 100%; font-size: 10 20; src: url(c) }',
    '@font-face { font-family: D }',
    '@font-face { src: url(x) }',
    '@font-feature-values A, "B c" { font-display: swap; @styleset { nice: 12 3 } @swash { s: 1 } @styleset { nice: 9 } }',
    '@font-feature-values X { @bogus { a: 1 } @ornaments { o: 1 2 } }',
    '@font-palette-values --x { font-family: Bixa, "Foo Bar"; base-palette: 1; override-colors: 0 red, 3 #00f }',
    '@font-palette-values --y { base-palette: dark }',
    '@color-profile --p { src: url(a.icc); rendering-intent: perceptual; components: a, b }',
    '@color-profile device-cmyk { src: url(b.icc) }',
    '@counter-style x { system: fixed 5; symbols: a b; range: 1 3, infinite 0; pad: 3 "0"; negative: "-" "!" }',
    '@counter-style y { system: additive; additive-symbols: 3 c, 2 b, 1 a }',
    '@counter-style z { system: extends x; suffix: ")" }',
    '@counter-style p { system: extends q } @counter-style q { system: extends p }',
    '@counter-style f1 { system: cyclic; symbols: a; fallback: f2 } @counter-style f2 { system: cyclic; symbols: a; fallback: f1 }',
    '@counter-style bad { system: alphabetic; symbols: a }',
    '@keyframes k { from { color: red } 50%, 60% { color: blue; animation-name: x } to { color: green !important } }',
    '@keyframes "q" { to { width: 1px } 50% { width: 2px } from { width: 3px } 50% { height: 1px } }',
    '@keyframes none { from { color: red } }',
    '@-webkit-keyframes w { 0%, 100% { opacity: 0 } 50% { opacity: 1 } }',
    '@page :first { size: A4 landscape; margin: 1in; @top-left { content: "x" } @bottom-center { content: "y" } }',
    '@page named:left:first { margin: 2px } @page :blank { margin: 3px } @page { margin: 4px }',
    '@page :bogus { margin: 1px }',
    '@view-transition { navigation: auto; types: a b }',
    '@position-try --a { top: anchor(bottom); color: red; left: 1px }',
    '@property --a { syntax: "<number>"; inherits: false; initial-value: 0 }',
    '@layer a, b; @layer b { @keyframes k { from { color: red } } } @layer a { @keyframes k { from { color: blue } } }',
    '@layer { @font-face { font-family: L; src: url(l) } }',
    '@layer a.b { @counter-style n { system: cyclic; symbols: x } } @layer a { @counter-style n { system: cyclic; symbols: y } }',
    '@media (min-width: 100px) { @keyframes m { from { color: red } } }',
    '@supports (display: grid) { @font-face { font-family: S; src: url(s) } }',
    'a { color: red; @font-face { font-family: N; src: url(n) } }',
    '@top-left { content: "x" }',
    '@unknown-rule foo { bar }',
    '@namespace svg url(http://www.w3.org/2000/svg); @charset "utf-8"; @import url(a.css) layer(x);',
]


def put(out, data):
    out[hashlib.sha256(data).hexdigest()[:12]] = bytes(data)


def main():
    rng = random.Random(0xA7B01E)
    out = {}
    for i, f in enumerate(FRAGMENTS):
        for k in range(3):
            put(out, bytes([rng.randrange(256), rng.randrange(256)]) + f.encode())
        for cut in range(8, len(f), max(7, len(f) // 6)):
            put(out, bytes([rng.randrange(256), rng.randrange(256)]) + f[:cut].encode())
    for _ in range(260):
        n = 2 + rng.randrange(3)
        text = " ".join(rng.choice(FRAGMENTS) for _ in range(n))
        if rng.random() < 0.3:
            text = "@layer %s { %s }" % (rng.choice(["a", "b", "a.b", ""]), text)
        if rng.random() < 0.3:
            text = "@media all { %s }" % text
        if rng.random() < 0.25:
            text = text[: rng.randrange(1, len(text))]
        put(out, bytes([rng.randrange(256), rng.randrange(256)]) + text.encode())
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith(".seed") and not name.startswith("reproducer-"):
            os.remove(os.path.join(OUT, name))
    for h, data in out.items():
        with open(os.path.join(OUT, h + ".seed"), "wb") as fh:
            fh.write(data)
    print("%d seeds in %s" % (len(out), OUT))


if __name__ == "__main__":
    main()
