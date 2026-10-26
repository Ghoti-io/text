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
"""Write the seed corpus of tests/fuzz/fuzz_css_registry.cpp: tests/fuzz/corpus/css-registry/.

    tools/css/make-fuzz-registry-seeds.py

A seed is four option bytes (see the harness) and text. Mode 0 parses a value for every property of the pin by its index (a rotating
list of values: the CSS-wide keywords, substitutions, lengths, colors, urls, functions, strings, nonsense), mode 1 for descriptors, mode 2
for names taken from the text (unknown, upper case, custom, empty, long), mode 3 registers a property (valid, refused for each
reason, an override, a name that exists). A selection of each also has a tiny work limit, the quirks option, or an allocation to fail.
Seeds are indexes, so they stay valid when the pin is raised.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-registry")

VALUES = [
    "inherit", "initial", "unset", "revert-layer", "REVERT", "auto", "none", "normal", "0", "1", "-1", "10px", "-5px", "50%", "1px 2px 3px 4px",
    "1px 2px 3px 4px 5px", "red", "#f00", "rgb(1 2 3 / 50%)", "currentcolor", "calc(1px + 2%)", "min(1px, 2em)", "var(--x)", "var(--x, 1px) 2px",
    "env(safe-area-inset-top)", "attr(title)", "url(a.png)", "url('a b.png') no-repeat", "'a string'", "\"x\" 1", "translate(1px, 2px) rotate(3deg)",
    "linear-gradient(red, blue)", "circle(5px at center)", "1fr 2fr", "repeat(2, 1fr)", "a b c", "1 / 2", "10px / 20px", "bold 12px/1.5 serif",
    "1s ease-in 2s", "span 2", "left top", "inset(1px) border-box", "U+0-7F", "", "   ", ")", "{", "1px }", "calc(", "1e999px", "0.0000001px",
    "var(", "VAR(--X)", "!important", "1px !important", "a, b", ", ,", "#", "@x", "é", "\u0000", "x" * 300,
]
NAMES = ["", "nosuch", "-moz-x", "-webkit-nosuch", "WIDTH", "Margin-Top", "--x", "--X", "--", "-", "width ", " width", "a" * 5000, "all", "ALL",
         "-webkit-transform", "word-wrap", "font", "é", "--é"]
GRAMMARS = [
    ("a | b", "a"), ("<length> | auto", "auto"), ("<length [0,∞]>", "0"), ("<'width'>", "auto"), ("<color>", "red"), ("<image> | none", "none"),
    ("[ a | b ]{1,3}", "a b"), ("[ a b? ]# | none", "none"), ("<x>", "a"), ("<'nosuch'>", "a"), ("a |", "a"), ("", "a"), ("a | b", "c"),
    ("a | b", "inherit"), ("a | b | inherit", "inherit"), ("<length>", "var(--x)"), ("<length>", ""), ("<length>#", "1px, 2px"), ("a { b }", "a"),
    ("<number>", "1e999"), ("<any-value>", "x"), ("<declaration-value>?", ""), ("translate( <length> , <length>? )", "translate(1px)"),
    ("<gradient>", "linear-gradient(red, blue)"), ("<basic-shape>", "circle(5px)"), ("<transform-list>", "none"), ("a&&b&&c", "c b a"),
    ("[[[[[[a]]]]]]", "a"), ("a" * 70, "a" * 70), ("<length [0px, 10px]>", "5px"), ("<foo()>", "foo()"), ("<alpha-value>", "1"),
]

def pin_indexes():
    """The at-rules of the pin in the order the library has them, and the descriptors of each (sorted by name)."""
    import sys
    sys.path.insert(0, HERE)
    import webref_model as wm
    model = wm.load()
    at_rules = sorted(model["atrules"])
    descriptors = {a: sorted({k[1] for k in model["descs"] if k[0] == a}) for a in at_rules}
    return at_rules, descriptors


def write(name, data):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name), "wb") as handle:
        handle.write(data)

def seed(o0, o1, o2, o3, text):
    return bytes([o0 & 255, o1 & 255, o2 & 255, o3 & 255]) + text.encode("utf-8", "surrogatepass")

def main():
    count = 0
    # mode 0: every property by its index
    for i in range(823):
        v = VALUES[i % len(VALUES)]
        o0 = 0
        if i % 7 == 0:
            o0 |= 4              # quirks
        if i % 11 == 0:
            o0 |= (1 + i % 5) << 4   # a tiny work limit
        write("prop-%03d.seed" % i, seed(o0, i >> 8, i & 255, 0, v))
        count += 1
    # every property again with a second value, for the grammars the first value does not reach
    for i in range(0, 823, 3):
        v = VALUES[(i * 5 + 3) % len(VALUES)]
        write("prop2-%03d.seed" % i, seed(0, i >> 8, i & 255, 0, v))
        count += 1
    # mode 1: descriptors, by at-rule and index
    for a in range(48):
        for k in range(4):
            v = VALUES[(a * 4 + k) % len(VALUES)]
            write("desc-%02d-%d.seed" % (a, k), seed(1, a, k, 0, v))
            count += 1
    # mode 2: names from the text
    for n, name in enumerate(NAMES):
        for k, v in enumerate(VALUES[:6] + ["10px", "1px 2px"]):
            write("name-%02d-%d.seed" % (n, k), seed(2, 0, 0, 0, name + "\n" + v))
            count += 1
    # mode 3: registrations
    for g, (grammar, initial) in enumerate(GRAMMARS):
        for variant in range(4):
            o1 = variant                       # the name: fuzz-prop-N
            o2 = 1 if variant == 1 else (2 if variant == 2 else 0)   # override, inherits
            write("reg-%02d-%d.seed" % (g, variant), seed(3, o1, o2, 0, grammar + "\n" + initial))
            count += 1
        write("reg-%02d-width.seed" % g, seed(3, 8, 0, 0, grammar + "\n" + initial))           # a name that exists
        write("reg-%02d-width-override.seed" % g, seed(3, 8, 1, 0, grammar + "\n" + initial))
        count += 2
    # allocation failures: a spread of counts over a few of each
    for n in range(1, 61):
        i = (n * 37) % 823
        write("fault-prop-%02d.seed" % n, seed(0, i >> 8, i & 255, n, VALUES[(n * 3) % len(VALUES)]))
        g = GRAMMARS[n % len(GRAMMARS)]
        write("fault-reg-%02d.seed" % n, seed(3, n, 0, n, g[0] + "\n" + g[1]))
        count += 2
    # allocation failures over a registration that names a property of the pin (a failed definition must not be answered from another
    # source: the result would depend on where the allocation failed)
    for n in range(1, 161):
        write("fault-width-%03d.seed" % n, seed(3, 5, 0, n, "<'width'> <'height'>?\nauto"))
        write("fault-width-override-%03d.seed" % n, seed(3, 8, 1, n, "<'width'> | zzwide\nauto"))
        count += 2
    # seeds the planted defects (tools/css/plant-fuzz.py REGISTRY_PLANTS) are found by
    at_rules, descriptors = pin_indexes()
    face = at_rules.index("@font-face")
    display = descriptors["@font-face"].index("font-display")
    own = {
        "own-valid-margin.seed": seed(2, 0, 0, 0, "margin\n1px 2px"),
        "own-valid-no-fault.seed": seed(2, 0, 0, 0, "color\nred"),
        "own-pending.seed": seed(2, 0, 0, 0, "margin\nvar(--m)"),
        "own-wide.seed": seed(2, 0, 0, 0, "width\nREVERT-layer"),
        "own-all-var.seed": seed(2, 0, 0, 0, "all\nvar(--x)"),
        "own-all-length.seed": seed(2, 0, 0, 0, "all\n1px"),
        "own-alias.seed": seed(2, 0, 0, 0, "-webkit-transform\nrotate(3deg)"),
        "own-unknown.seed": seed(2, 0, 0, 0, "-moz-nosuch\ninherit"),
        "own-quirks.seed": seed(2 | 4, 0, 0, 0, "width\n10"),
        "own-descriptor-var.seed": seed(1, face, display, 0, "var(--x)"),
        "own-descriptor-wide.seed": seed(1, face, display, 0, "inherit"),
        "own-descriptor-valid.seed": seed(1, face, display, 0, "swap"),
        "own-register-valid.seed": seed(3, 0, 0, 0, "a | b\nb"),
        "own-register-bad-initial.seed": seed(3, 0, 0, 0, "a | b\nc"),
        "own-register-exists.seed": seed(3, 8, 0, 0, "a | b\na"),
        "own-register-override-self.seed": seed(3, 8, 1, 0, "<'width'> | zzwide\nauto"),
        "own-register-no-fault.seed": seed(3, 0, 0, 0, "a | b\na"),
    }
    for name, data in own.items():
        write(name, data)
        count += 1
    print("wrote %d seeds to %s" % (count, OUT))

if __name__ == "__main__":
    main()
