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
"""Write the seed corpus of tests/fuzz/fuzz_css_custom.cpp into tests/fuzz/corpus/css-custom.

    tools/css/make-fuzz-custom-seeds.py [directory]

A seed is four option bytes and a text (see the harness). The corpus is made of scenarios, one for each rule of arbitrary substitution, a custom
property, a registration or a limit, each written

  * as it is (mode 0, a root; mode 1, a parent and a child; mode 2, a style sheet and the declarations that use its rules; mode 3, the same
    declarations through a block and the cascade),
  * with the small limits (so that every limit path is reached), with an environment that answers `env()`, `if()` and `random()`,
  * and with an allocation failed at a few places.

The files are named for what they hold and are deterministic. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

SMALL, ENV = 4, 8

# (name, mode, text): mode 0 text is declarations; mode 1 is "parent\n---\nchild"; mode 2 "sheet\n---\ndeclarations"; mode 3 declarations
SCENARIOS = [
    # the matrix of the slice
    ("fallback", 0, "width: var(--w, 10px)"),
    ("guaranteed-invalid", 1, "width: 5px; color: red\n---\nwidth: var(--w); color: var(--w)"),
    ("cycle", 0, "--a: var(--b); --b: var(--a); width: var(--a, 3px)"),
    ("empty", 0, "--x:; margin: var(--x) 1px"),
    ("blowup", 0, "--n0: lol; " + "; ".join("--n%d: var(--n%d) var(--n%d)" % (i, i - 1, i - 1) for i in range(1, 41)) + "; content: var(--n40)"),
    ("registered", 0, "--len: 2em; --col: blue; width: var(--len); color: var(--col)"),
    ("shorthand", 0, "--m: 1px 2px; margin: var(--m)"),
    ("missing-env", 0, "width: env(nosuch, 4px)"),
    # custom properties
    ("original-text", 0, "--c: /* a */ 1.0   2.50 /* b */ x; --d: Red; --e: 12345678-12e3-8d9b-a456-426614174000"),
    ("names-are-case-sensitive", 0, "--foo: 1px; --FOO: 2px; width: var(--foo); height: var(--FOO); margin-top: var(--Foo, 3px)"),
    ("explicit-defaulting", 1, "--a: 1px; --b: 2px\n---\n--a: initial; --b: inherit; --c: unset; --d: revert"),
    ("inherits", 1, "--a: 1px; --len: 4px; --col: green\n---\nwidth: var(--a); height: var(--len); color: var(--col)"),
    ("name-from-var", 0, "--k: --w; --w: 7px; width: var(var(--k))"),
    ("fallback-commas", 0, "--l: 1px, 2px; font-family: var(--none, a, b, c); content: var(--none,)"),
    ("tokens-merge", 0, "--a: 1; --b: var(--a)1; --c: a var(--a) b; --d: var(--a)var(--a); --e: -var(--a); --f: var(--a)%"),
    ("long-chain", 0, "--c0: 1px; " + "; ".join("--c%d: var(--c%d)" % (i, i - 1) for i in range(1, 60)) + "; width: var(--c59)"),
    ("long-cycle", 0, "; ".join("--y%d: var(--y%d)" % (i, (i + 1) % 30) for i in range(30)) + "; width: var(--y0, 1px)"),
    ("cycle-through-the-first", 0, "; ".join("--k%d: var(--k%d) var(--k0)" % (i, i + 1) for i in range(30)) + "; --k30: 1px; width: var(--k0, 1px)"),
    ("wide-keyword-after", 1, "width: 7px; color: red\n---\n--w: inherit; width: var(--w); color: var(--v, unset)"),
    ("invalid-for-the-property", 0, "--x: red; width: var(--x); color: var(--x); --y: 1px 2px 3px 4px 5px; margin: var(--y)"),
    ("invalid-at-computed-value-time", 1, "width: 5px\n---\nwidth: 6px; width: var(--none)"),
    # shorthands
    ("shorthand-longhands", 0, "--s: 1px solid red; border: var(--s); --r: 4px / 8px; border-radius: var(--r)"),
    ("shorthand-wide", 0, "margin: 1px; margin: var(--w, inherit)"),
    ("shorthand-layers", 0, "--b: url(a) 1px 2px / 3px 4px no-repeat, red; background: var(--b)"),
    ("shorthand-all", 0, "--u: unset; all: var(--u)"),
    ("shorthand-grid", 0, "--g: 'a b' 1fr / auto; grid-template: var(--g); --f: italic bold 12px/1.5 serif; font: var(--f)"),
    # the other functions
    ("env", 0, "width: env(e1); height: env(e2, 3px); margin-top: env(nosuch, var(--q, 1px)); --v: env(e1) env(e2)"),
    ("attr", 0, "content: attr(x); width: attr(w px, 5px); --a: attr(data-a type(<length>), 1px)"),
    ("attr-url", 0, "background-image: url(attr(x)); --u: image(attr(src)); background: image('a.png', attr(y))"),
    ("if", 0, "--a: 1; width: if(style(--a: 1): 1px; else: 2px); height: if(media(print): 3px; supports(x: y): 4px; else: 5px)"),
    ("if-nested", 0, "--a: 1; --b: 2; width: if(style(--a: 1) and not style(--b: 3): 1px; style(--a: 2) or style(--b: 2): 2px; else: 3px)"),
    ("inherit-function", 1, "--p: 5px; color: red; width: 4px\n---\n--a: inherit(--p); width: inherit(width); color: inherit(color, blue)"),
    ("ident", 0, "--x: foo; content: ident('a', var(--x), 1); counter-reset: ident(var(--x), '-', 2) 1"),
    ("random", 0, "width: random(100px, 300px); height: calc(random(1px, 3px, 1px) + 1px); opacity: random(--r, 0.2, 0.6); --i: random-item(--k, 1px, 2px, 3px)"),
    ("random-item-spread", 0, "--l: 1px, 2px, 3px; --s: random-item(--k, var(--l)...); width: var(--s)"),
    ("sibling", 0, "width: calc(sibling-index() * 1px); height: calc(sibling-count() * 1px)"),
    ("cycle-function", 1, "margin-top: 1px; font-style: italic\n---\nmargin-top: cycle(1px, 2px, 3px); font-style: cycle(italic, normal)"),
    ("cycle-shorthand", 1, "margin-top: 1px; margin-right: 2px\n---\nmargin: cycle(1px 2px, 4px, 1px 5px 4px)"),
    ("cycle-bad", 0, "background-position: 10px cycle(50px, 100px); margin-top: cycle(calc(1px + 1px), 2px); margin-left: cycle(cycle(1px, 2px), 3px)"),
    ("spread", 0, "--args: 1px, 2px; width: max(var(--args)...); height: max(0px, var(--args)..., 5px); margin: min(var(--none)...)"),
    ("spread-bad", 0, "--args: 1px, 2px; width: max(var(--args).. .); height: max(var(--args) ...)"),
    # limits
    ("many-customs", 0, "; ".join("--c%d: %d" % (i, i) for i in range(60))),
    ("long-value", 0, "--v: " + " ".join("w%d" % i for i in range(400)) + "; --w: var(--v) var(--v); --x: var(--w) var(--w); width: var(--x)"),
    ("deep-nesting", 0, "--s: " + "var(--none, " * 30 + "1px" + ")" * 30 + "; width: var(--s)"),
    ("deep-chain-small", 0, "--c0: 1px; " + "; ".join("--c%d: var(--c%d)" % (i, i - 1) for i in range(1, 23)) + "; width: var(--c22)"),
    ("fan-out", 0, "--b: 1px 2px; " + "; ".join("--u%d: var(--b) var(--b) var(--b)" % i for i in range(20)) + "; --t: " + " ".join("var(--u%d)" % i for i in range(20))),
    # registrations
    ("registered-typed", 1, "--len: 3px; --num: 2\n---\n--len: 2em; --num: auto; --list: 1px, 3em; --id: foo bar; width: var(--len)"),
    ("registered-bad", 0, "--len: red; --col: 3px; --num: x; --list: 1px 2px; --id: 1"),
    ("registered-font-relative", 0, "font-size: 20px; --len: 2em; --a: calc(var(--len) * 2); width: var(--a)"),
    ("registered-font-cycle", 0, "font-size: var(--len); --len: 2em"),
    ("registered-initial", 0, "width: var(--len); color: var(--col); --any: initial; height: var(--any, 9px)"),
    # @property
    ("property-rules", 2, "@property --a { syntax: '<length>'; inherits: true; initial-value: 1px }\n@property --b { syntax: '<color> | auto'; inherits: false; initial-value: red }\n---\n--a: 3em; --b: green; width: var(--a); color: var(--b)"),
    ("property-bad-rules", 2, "@property --x { inherits: true }\n@property --y { syntax: '<length>' }\n@property --z { syntax: '<length>'; inherits: true }\n@property foo { syntax: '*'; inherits: true }\n---\n--x: 1; --y: 2; --z: 3px"),
    ("property-layers", 2, "@layer a { @property --l { syntax: '*'; inherits: true } }\n@media print { @property --m { syntax: '*'; inherits: true } }\n@property --l { syntax: '<number>'; inherits: false; initial-value: 0 }\n---\n--l: 4; --m: x; width: calc(var(--l) * 1px)"),
    ("property-syntax-strings", 2, "@property --a { syntax: '<length> | <percentage>+'; inherits: false; initial-value: 1px }\n@property --b { syntax: '<number>#'; inherits: false; initial-value: 1, 2 }\n@property --c { syntax: 'foo | bar'; inherits: true; initial-value: foo }\n@property --d { syntax: '<transform-list>'; inherits: false; initial-value: none }\n@property --e { syntax: '<image> | <url>'; inherits: true; initial-value: none }\n@property --f { syntax: '<'; inherits: false }\n---\n--a: 5%; --b: 1, 2, 3; --c: bar; --d: scale(2); --e: none"),
    ("property-initial-independent", 2, "@property --a { syntax: '<length>'; inherits: true; initial-value: 3em }\n@property --b { syntax: '<color>'; inherits: false; initial-value: currentcolor }\n@property --c { syntax: '*'; inherits: false; initial-value: var(--x) }\n---\nwidth: var(--a, 1px)"),
    ("property-duplicate", 2, "@property --a { syntax: '*'; inherits: true }\n@property --a { syntax: '<length>'; inherits: false; initial-value: 2px }\n@property --len { syntax: '<number>'; inherits: false; initial-value: 5 }\n---\n--a: 3px; --len: 4"),
    # the block API and the cascade
    ("block-and-cascade", 3, "--a: var(--b); --b: 3px; --c: var(--a) 2px; width: var(--a); height: var(--none, 1px); color: var(--c)"),
    ("block-and-cascade-later-wins", 3, "--a: 1px; width: var(--a); --a: 2px; width: var(--a); margin: var(--a); margin-top: 7px"),
    ("block-and-cascade-wide", 3, "--a: 1px; --a: inherit; --b: initial; width: var(--a); height: var(--b, 2px)"),
    ("block-and-cascade-registered", 3, "--len: 2em; --col: red; width: var(--len); color: var(--col); font-size: 10px"),
]

FAULTS = (1, 2, 3, 5, 9, 17, 33, 65)


def seed(o0, o3, text):
    return bytes([o0, 0, 0, o3]) + text.encode("utf-8")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-custom")
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith(".seed") and not f.startswith("reproducer-"):
            os.remove(os.path.join(out, f))
    n = 0

    def put(name, data):
        nonlocal n
        with open(os.path.join(out, name + ".seed"), "wb") as fh:
            fh.write(data)
        n += 1

    for name, mode, text in SCENARIOS:
        put(name, seed(mode, 0, text))
        put(name + "-small", seed(mode | SMALL, 0, text))
        put(name + "-env", seed(mode | ENV, 0, text))
        put(name + "-env-small", seed(mode | ENV | SMALL, 0, text))
        for fault in FAULTS:
            put("%s-oom-%02d" % (name, fault), seed(mode | (ENV if fault % 2 else 0), fault, text))
    # the seeds the planted defects of tools/css/plant-fuzz.py are found by (each is an input that shows one of them)
    put("own-chain", seed(0, 250, "--a: 1px; --b: var(--a) 2px; --c: var(--b) var(--b); width: var(--a)"))
    put("own-doubling-small", seed(SMALL, 0, "--n0: lol; " + "; ".join("--n%d: var(--n%d) var(--n%d)" % (i, i - 1, i - 1) for i in range(1, 16)) + "; content: var(--n15)"))
    put("own-block-and-cascade", seed(3, 0, "--a: var(--b); --b: 1px; width: var(--a)"))
    for fault in (53, 81, 93):  # the allocations of the joins of this value (the first is the plant's seed; a change of the code moves them, and any of the three shows the defect)
        put("own-join-oom" if fault == 53 else "own-join-oom-%d" % fault, seed(0, fault, "--a: 1px; --b: var(--a) 2px; --c: var(--b) var(--b)"))
    put("own-rules-two", seed(2, 0, "@property --abcd { syntax: '<length>'; inherits: true; initial-value: 1px }\n@property --abce { syntax: '<number>'; inherits: false; initial-value: 1 }\n---\n--abcd: 2px"))
    # edge shapes of the harness itself
    put("own-empty-pair", seed(1, 0, "\n---\n"))
    put("own-no-parent", seed(1, 0, "width: var(--x, 1em)"))
    put("own-empty-sheet", seed(2, 0, "---\n"))
    put("own-big-fault", seed(1, 255, "--a: 10px\n---\nwidth: var(--a)"))
    print("%d seeds in %s" % (n, out))


if __name__ == "__main__":
    main()
