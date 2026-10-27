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
"""Write the seed corpus of tests/fuzz/fuzz_css_style.cpp into tests/fuzz/corpus/css-style.

    tools/css/make-fuzz-style-seeds.py [directory]

A seed is four option bytes and sections of text (see the harness): the parent's `style` attribute, the parent's presentation attributes, the child's
`style` attribute, the child's presentation attributes, the entries. The corpus is made of scenarios, one for each rule of the call (each attribute-only
spelling, the cascade between the sources, the sections that make a diagnostic, the limits), each written

  * as it is, with every attribute-only spelling and both attribute sources on,
  * with the small limits, as an HTML element (no presentation attributes), as a `circle`, with an environment that answers `env()`,
  * and with an allocation failed at a few places.

The files are named for what they hold and are deterministic. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

SMALL, HTML, CIRCLE, ENV = 4, 8, 16, 32
ALL = 0x3F          # every spelling, both sources
ENTRIES_ONLY = 0

# (name, shape, parent style, parent attributes, child style, child attributes, entries)
S = []


def scenario(name, shape=0, ps="", pa="", cs="", ca="", en=""):
    S.append((name, shape, ps, pa, cs, ca, en))


scenario("attribute", ca="stroke-width=2")
scenario("style-wins", cs="stroke-width: 5", ca="stroke-width=2")
scenario("important", cs="fill: red !important", ca="fill=blue")
scenario("rule-beats-hint", ca="stroke-width=2\nfill=red", en="0|stroke-width: 7px")
scenario("hint-beats-nothing", ca="opacity=0.5\nfont-size=12")
scenario("bad-attribute", 1, pa="fill=orange", ca="fill=12px\nstroke=nonsense\nstroke-width=wide")
scenario("unitless", ca="stroke-width=2\nfont-size=12\nletter-spacing=-1\nstroke-dasharray=4,5 6\nx=7\nwidth=8\nr=3\ncx=4")
scenario("geometry-elements", ca="cx=1\ncy=2\nr=3\nrx=4\nry=5\nwidth=6\nheight=7\nx=8\ny=9")
scenario("geometry-circle", ca="cx=1\ncy=2\nr=3\nrx=4")
scenario("transform", ca="transform=rotate(30 5 5) translate(1,2)")
scenario("transform-bad", ca="transform=rotate(1 2)\npatternTransform=scale(2)")
scenario("transform-css", ca="transform=translate(10px, 20px) rotate(30deg)")
scenario("transform-empty", ca="transform=   ")
scenario("path", ca="d=M0 0 L10 10 z")
scenario("path-quotes", ca="d=M0 0 \"q\" \\ x")
scenario("trim", ca="fill=  red  \nstroke=\tblue\t")
scenario("inherit", 1, pa="fill=green\nstroke-width=3", ca="fill=inherit\nstroke=initial\nstroke-width=unset")
scenario("current-color", 1, pa="color=purple", ca="fill=currentColor\nstroke=currentcolor")
scenario("em-rem", 1, pa="font-size=10", ca="font-size=2em\nstroke-width=1.5em\nletter-spacing=1rem")
scenario("percent", 1, pa="font-size=10", ca="font-size=150%\nword-spacing=10%")
scenario("calc", 1, ps="font-size: 10px", cs="stroke-width: calc(1em + 2px); word-spacing: calc(10% + 1em)")
scenario("custom-properties", cs="--a: 3px; --b: var(--a) 2px; stroke-width: var(--a); fill: var(--none, red)")
scenario("custom-in-attribute", 1, ps="--accent: teal", ca="fill=var(--accent)\nstroke-width=var(--w, 4)")
scenario("var-cycle", cs="--a: var(--b); --b: var(--a); stroke-width: var(--a, 4px)")
scenario("blowup", cs="--n0: lol; " + "; ".join("--n%d: var(--n%d) var(--n%d)" % (i, i - 1, i - 1) for i in range(1, 25)) + "; content: var(--n24)")
scenario("shorthand", cs="margin: 1px 2px; border: 1px solid red; font: italic bold 12px/1.5 serif")
scenario("logical", cs="margin-inline-start: 1px; margin-left: 2px; direction: rtl")
scenario("all", cs="all: initial; color: red")
scenario("revert", cs="color: red; stroke: revert", en="0|color: green")
scenario("unknown", cs="nonsense: 1; fill: red; -vendor-thing: 2")
scenario("invalid", cs="fill: 12; stroke-width: wide; opacity: 0.5")
scenario("bad-declaration-list", cs="fill red; ; : x; opacity: 0.5; }{ ; stroke: blue")
scenario("unsupported", cs="corner-top-left-shape: round")
scenario("entries", cs="fill: red", ca="fill=blue\nopacity=0.1", en="0|fill: green; stroke-width: 9px !important\n1|fill: blue\n2|fill: teal")
scenario("entries-specificity", en="0|fill: red\n1|fill: blue\n2|fill: green")
scenario("entries-invalid", en="0|fill: nonsense; bogus: 1\n1|opacity: 0.5")
scenario("entries-many", en="\n".join("%d|margin-left: %dpx" % (i % 10, i) for i in range(16)))
scenario("same-text-chain", 2, cs="font-size: 1.1em; fill: red", ca="stroke-width=2")
scenario("long-value", cs="--v: " + " ".join("w%d" % i for i in range(400)) + "; --w: var(--v) var(--v); content: var(--w)")
scenario("many-declarations", cs="; ".join("--c%d: %d" % (i, i) for i in range(60)))
scenario("many-same-property", cs="; ".join("margin-left: %dpx" % i for i in range(60)))
scenario("comments", cs="/**/fill/**/:/**/red/**/;/* x */ stroke: blue", ca="fill=/**/red")
scenario("newlines", cs="fill: red;\nstroke: blue;\n\nopacity: 0.5")
scenario("strings", cs="font-family: 'a\\a b'; content: 'x\\d y'; --s: 'q\\c r'")
scenario("urls", cs="fill: url(#a) red; cursor: url(a.png) 1 2, auto; clip-path: url(#c)")
scenario("paint-keywords", cs="fill: context-fill; stroke: context-stroke")
scenario("colors", cs="color: color-mix(in srgb, red, blue); fill: rgb(1 2 3 / 50%); stroke: #abc")
scenario("font", ca="font-family=serif, 'Times New Roman'\nfont-weight=bold\nfont-style=italic\nfont-variant=small-caps\nfont-stretch=condensed\nfont-size-adjust=0.5")
scenario("text", ca="text-decoration=underline\ntext-anchor=middle\nwriting-mode=vertical-rl\ndirection=rtl\nunicode-bidi=embed\nwhite-space=pre\ntext-overflow=ellipsis")
scenario("markers", ca="marker-start=url(#m)\nmarker-mid=none\nmarker-end=url(#m)\npaint-order=stroke fill\nvector-effect=non-scaling-stroke")
scenario("dash", ca="stroke-dasharray=1 2 3\nstroke-dashoffset=-2\nstroke-linecap=round\nstroke-linejoin=bevel\nstroke-miterlimit=2")
scenario("filter-mask", ca="filter=blur(2px)\nmask=url(#m)\nmask-type=alpha\nclip-rule=evenodd\nfill-rule=evenodd\nopacity=2")
scenario("display-overflow", ca="display=none\noverflow=hidden\nvisibility=hidden\npointer-events=none")
scenario("transform-origin", ca="transform-origin=10 20\nbaseline-shift=sub\nalignment-baseline=middle\ndominant-baseline=hanging")
scenario("lighting", ca="flood-color=red\nflood-opacity=0.5\nlighting-color=blue\nstop-color=green\nstop-opacity=0.2\ncolor-interpolation=linearRGB")

FAULTS = (1, 2, 3, 5, 9, 17, 33, 65)


def seed(o0, o1, o3, parts):
    text = "\n---\n".join(parts)
    return bytes([o0, o1, 0, o3]) + text.encode("utf-8")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-style")
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

    for name, shape, ps, pa, cs, ca, en in S:
        parts = [ps, pa, cs, ca, en]
        put(name, seed(shape, ALL, 0, parts))
        put(name + "-small", seed(shape | SMALL, ALL, 0, parts))
        put(name + "-html", seed(shape | HTML, ALL, 0, parts))
        put(name + "-circle", seed(shape | CIRCLE, ALL, 0, parts))
        put(name + "-env", seed(shape | ENV, ALL, 0, parts))
        put(name + "-plain", seed(shape, 0x38, 0, parts))   # the spellings off: attributes are declarations
        put(name + "-style-only", seed(shape, 0x28, 0, parts))
        put(name + "-attributes-only", seed(shape, 0x1F, 0, parts))
        put(name + "-entries-only", seed(shape, 0, 0, parts))
        for fault in FAULTS:
            put("%s-oom-%02d" % (name, fault), seed(shape | (ENV if fault % 2 else 0), ALL, fault, parts))
    # the seeds the planted defects of tools/css/plant-fuzz.py (STYLE_PLANTS) are found by
    put("own-attribute-red", seed(0, ALL, 0, ["", "", "", "fill=red", ""]))
    put("own-entries-equal-specificity", seed(0, ALL, 0, ["", "", "", "", "0|fill: red\n3|fill: blue"]))
    put("own-diagnostic-leak", seed(0, ALL, 255, ["", "", "bogus: 1; fill: red", "stroke=nonsense", ""]))
    put("own-empty", seed(0, ALL, 0, ["", "", "", "", ""]))
    put("own-no-sections", bytes([0, ALL, 0, 0]))
    put("own-big-fault", seed(1, ALL, 255, ["fill: red", "", "stroke-width: 2px", "", ""]))
    print("%d seeds in %s" % (n, out))


if __name__ == "__main__":
    main()
