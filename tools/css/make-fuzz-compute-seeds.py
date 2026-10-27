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
"""Write the seed corpus of tests/fuzz/fuzz_css_compute.cpp into tests/fuzz/corpus/css-compute.

    tools/css/make-fuzz-compute-seeds.py [directory]

A seed is four option bytes and a text (see the harness). The corpus is made of

  * every shorthand of the pin (tools/css/shorthand_rules.tsv, in the order of the names, as the harness orders them) with a list of values that
    reach the generic split, the layers, the boxes, the wide keywords, `var()`, and the shapes the shorthands with algorithms of their own read;
  * blocks of declarations that make a cssText and read it back;
  * a parent and a child, and a root, that reach every kind of computation: relative units, math, logical properties, `all`, border widths,
    `display` blockification, the font keywords, light-dark, urls, and the custom properties;
  * the same with an allocation failed (the fourth option byte), at a few places.

The files are named for what they hold and are deterministic. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

GENERIC = [
    "initial", "inherit", "unset", "var(--x)", "1px", "1px 2px", "1px 2px 3px", "1px 2px 3px 4px", "red", "none", "auto", "normal",
    "10px solid red", "red solid 2px", "0", "50%", "1px / 2px", "url(a) repeat-x, red", "calc(1px + 2em)", "left top", "span 2 / 3",
    "1fr / 2fr", "\"a b\" 1fr \"c d\" / auto 1fr", "auto-flow dense 1fr / 100px", "a b c d e f g h", "1 1 1 1 1", "", "(", "\"", "[", "]",
]

SPECIFIC = {
    "background": ["url(a) 1px 2px / 3px 4px no-repeat, red", "border-box content-box", "content-box", "padding-box red", "url(a), url(b), blue",
                   "linear-gradient(red, blue) space round local", "none", "border-area text"],
    "mask": ["no-clip", "border-box content-box", "url(a) luminance add", "none, linear-gradient(red, blue) padding-box"],
    "border-image": ["1 / / 1px", "url(a) 1 2% 3 4% fill / 1px 2 3px 4 / 5 repeat round", "none 100% / 1 / 0 stretch", "repeat round"],
    "font": ["italic bold 12px/1.5 serif", "small-caps 700 condensed 10px/normal \"A B\", sans-serif", "caption", "12px serif", "oblique 10deg 1em x"],
    "flex": ["2", "10px", "auto", "none", "0 0 0", "calc(1) calc(2 + 1) calc(3px)", "1 2 3px"],
    "grid": ["none", "auto / auto", "\"a\" 10px / 10px", "[a] \"a\" [b] \"b\" 1fr / auto", "100px / dense auto-flow 20px", "auto-flow 1fr / none"],
    "grid-template": ["none / none", "[] \"a\" 10px []", "\"a\" [a] [b] \"b\"", "\"a a a\" \"b b b\" 1fr / auto 1fr auto", "10px / repeat(2, 1fr)"],
    "grid-area": ["a / b / c / d", "span 1 i", "-10", "1 / 2"],
    "grid-column": ["a / b", "span 2", "\\31st / \\31 st"],
    "offset": ["100px", "normal none reverse", "left top path(\"M 0 0 H 1\") 5px auto 30deg / right bottom", "auto", "top right / top left"],
    "animation": ["a 1s, b 2s ease-in", "anim paused both reverse, 4 1s -3s cubic-bezier(0, -2, 1, 3)", "none", "1s 2s linear infinite"],
    "animation-range": ["entry, exit", "cover 50%", "entry 0% exit 100%", "normal exit 50%"],
    "white-space": ["pre", "pre-wrap", "pre-line", "nowrap", "normal", "preserve nowrap"],
    "text-spacing": ["none", "auto", "no-autospace space-all", "trim-start"],
    "text-decoration-skip": ["none", "auto"],
    "position-try": ["most-width flip-y flip-x, --foo", "--a, --b", "normal none"],
    "-webkit-line-clamp": ["3", "none"],
    "line-clamp": ["3 ellipsis", "none", "auto"],
    "place-content": ["first baseline", "center end", "baseline"],
    "list-style": ["none", "inside none", "outside outside", "square url(a)"],
    "columns": ["10px / 100px", "3 / 100px", "2 100px"],
    "container": ["a / size", "size", "name / inline-size"],
    "border-radius": ["1px 2px / 3px", "50%", "1px 2px 3px 4px / 5px 6px"],
    "column-rule-inset": ["1px", "1px 2px / 3px"],
    "contain-intrinsic-size": ["auto 1px 1px", "none 1px"],
    "overflow-clip-margin": ["padding-box 10px", "content-box"],
    "text-align": ["justify-all", "center"],
    "font-synthesis": ["weight style", "none", "position"],
    "box-shadow": ["1px 2px 3px red, inset 0 0 1px blue", "none"],
}

# a parent and a child (and a root), by what they reach
PAIRS = [
    ("font-size: 20px; color: red", "width: 2em; height: 50%; margin: 1em; color: inherit"),
    ("font-size: 10px", "font-size: 120%; width: 1.5em; line-height: 1.2"),
    ("font-size: 10px", "font-size: larger; font-weight: bolder"),
    ("font-size: 10px", "font-size: smaller; font-weight: lighter"),
    ("font-size: 16px; writing-mode: vertical-rl", "margin-inline-start: 4px; padding-block: 1px 2px; inline-size: 3em"),
    ("direction: rtl", "margin-inline: 1px 2px; border-inline-start: 3px solid; inset-inline-end: 4px"),
    ("", "border-style: none; border-width: 5px; border-color: red; outline-style: none; outline-width: 3px"),
    ("", "display: inline; float: left; position: absolute"),
    ("", "display: inline flow-root; position: fixed"),
    ("", "display: list-item; float: right"),
    ("color: red; font-size: 12px", "all: initial; color: blue"),
    ("color: red; width: 7px", "color: unset; width: unset"),
    ("color: red; width: 7px", "all: inherit"),
    ("margin-left: 9px", "all: unset; margin-right: 1px"),
    ("", "color: light-dark(red, blue); background-color: light-dark(white, black)"),
    ("", "width: calc(1px + 2em); height: calc(50% - 10px); margin-top: min(1em, 10px); padding-top: clamp(1px, 2em, 3px)"),
    ("", "width: calc(1px * 2); z-index: calc(1.5 + 1); opacity: calc(1 / 3); line-height: calc(1 + 1)"),
    ("", "background-image: url(a), url(\"b c\"); cursor: url(x) 1 2, auto"),
    ("", "transform: rotate(0.5turn) translate(1em, 2px); rotate: 90deg; scale: 2"),
    ("--a: 1; --b: x y", "--c: var(--a); --a: 2"),
    ("", "--a: 1; --b: 2; --A: 3"),
    ("", "margin: 1px 2px 3px 4px; padding: 1px; border: 2px solid red; background: url(a) 1px 2px / 3px 4px no-repeat, blue"),
    ("", "font: italic bold 12px/1.5 serif; border-radius: 1px 2px / 3px; flex: 1 1 10px"),
    ("", "grid-template: [a] \"x y\" 1fr [b] \"z z\" / auto 1fr; offset: 5px; animation: a 1s"),
    ("", "width: 10vw; height: 10svh; min-width: 5dvw; max-height: 1lvh; margin: 1rem; padding: 2vmin"),
    ("", "width: 1in; height: 2cm; margin: 3mm; padding: 4pt; border-width: 5pc; outline-offset: 6Q"),
    ("", "transition-duration: 1500ms, 2s; animation-delay: -250ms; transition-timing-function: steps(4, jump-start)"),
    ("", "image-resolution: 2x; resolution: 192dpi"),
    ("", "text-decoration: underline red wavy; text-decoration-skip: none; text-underline-offset: 2em"),
    ("font-size: 20px", "font-size-adjust: 0.5; font-variation-settings: \"wght\" 700; font-feature-settings: \"liga\" 0"),
    ("", "content: \"a\" counter(x) attr(y); quotes: \"a\" \"b\"; counter-reset: x 2 y"),
    ("", "width: 100px; width: banana; height: calc(1px +); color: #zzz"),
    ("", "width: var(--x); margin: var(--y)"),
    ("", "revert: 1; width: revert; height: revert-layer"),
    ("", "white-space: pre; text-spacing: none; position-try: flip-x flip-y, --a; mask: no-clip"),
    ("", "stroke-width: 0; fill: currentcolor; stroke: url(#a) red; stroke-dasharray: 1 2, 3"),
    ("", "gap: 1px 2px; row-gap: calc(1em + 1px); column-gap: normal; aspect-ratio: 16 / 9"),
    ("font-size: 10px; color: red", "border-top-color: currentcolor; background-color: currentcolor; box-shadow: 1px 1px red"),
]


def shorthand_names():
    names = []
    with open(os.path.join(HERE, "shorthand_rules.tsv"), encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            names.append(line.split("\t")[0])
    return sorted(names)


def seed(o0, o1, o2, o3, text):
    return bytes([o0, o1, o2, o3]) + text.encode("utf-8")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-compute")
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

    names = shorthand_names()
    for index, name in enumerate(names):
        values = GENERIC[:16] + SPECIFIC.get(name, [])
        for k, v in enumerate(values):
            put("short-%03d-%02d" % (index, k), seed(0, index >> 8, index & 255, 0, v))
        # an allocation failed at a few places of one value
        for fault in (1, 3, 8, 21, 55):
            put("short-%03d-oom-%02d" % (index, fault), seed(0, index >> 8, index & 255, fault, (SPECIFIC.get(name) or GENERIC[4:12])[0]))
    for k, (parent, child) in enumerate(PAIRS):
        decls = (parent + "; " + child).strip("; ")
        for mode_bits, label in ((1, "block"), (3, "root"), (3 | 8, "root-plain")):
            put("%s-%02d" % (label, k), seed(mode_bits, 0, 0, 0, decls if mode_bits & 3 == 1 else child))
        put("pair-%02d" % k, seed(2, 0, 0, 0, parent + "\n---\n" + child))
        put("pair-plain-%02d" % k, seed(2 | 8, 0, 0, 0, parent + "\n---\n" + child))
        for fault in (1, 2, 3, 7, 19, 61):
            put("pair-%02d-oom-%02d" % (k, fault), seed(2, 0, 0, fault, parent + "\n---\n" + child))
            put("block-%02d-oom-%02d" % (k, fault), seed(1, 0, 0, fault, decls))
    # the seeds the planted defects of tools/css/plant-fuzz.py are found by (each is a value that shows one of them)
    def short_seed(name, value, fault=0):
        i = names.index(name)
        return seed(0, i >> 8, i & 255, fault, value)

    put("own-white-space-pre", short_seed("white-space", "pre"))
    put("own-grid-template-names", short_seed("grid-template", "\"a\" [a] [b] \"b\""))
    put("own-pending-longhand", seed(1, 0, 0, 0, "width: var(--x); content: \"a\" attr(y)"))
    put("own-context-fails-early", seed(2, 0, 0, 1, "font-size: 10px\n---\nwidth: 2em"))
    put("own-mixed-wide", seed(1, 0, 0, 0, "margin: 1px; margin-top: inherit"))
    put("own-mixed-pending", seed(1, 0, 0, 0, "font-variant: small-caps; font-variant-caps: var(--x)"))
    put("own-mixed-nested", seed(1, 0, 0, 0, "border: 1px solid red; border-top-color: unset"))
    put("own-box-shadow-offset-none", seed(1, 0, 0, 0, "box-shadow-color: red; box-shadow-offset: none; box-shadow-blur: 0px; box-shadow-spread: 0px; box-shadow-position: outset"))
    put("own-line-clamp-legacy", seed(1, 0, 0, 0, "max-lines: auto; block-ellipsis: no-ellipsis; continue: -webkit-legacy"))
    put("own-animation-name-keyword", seed(1, 0, 0, 0, "animation: none; animation-name: auto"))
    put("own-float-under-absolute", seed(2, 0, 0, 0, "position: absolute; float: left; display: inline-block"))
    put("own-display-blockified", seed(2, 0, 0, 0, "float: left; display: table-cell\n---\ndisplay: inline math; position: fixed"))
    put("own-sibling-index", seed(1, 0, 0, 0, "width: calc(sibling-index() * 1px)"))
    put("own-custom-many", seed(3, 0, 0, 0, "; ".join("--c%d: %d" % (i, i) for i in range(40))))
    put("own-background-layers-oom", short_seed("background", "url(a) 1px 2px / 3px 4px no-repeat, red", 1))
    # edge shapes of the harness itself
    put("own-empty-pair", seed(2, 0, 0, 0, "\n---\n"))
    put("own-no-parent", seed(2, 0, 0, 0, "width: 1em"))
    put("own-big-fault", seed(2, 0, 0, 255, "font-size: 10px\n---\nwidth: 2em; height: 3em"))
    put("own-custom-only", seed(3, 0, 0, 0, "--x: 1; --y: var(--x)"))
    put("own-all-then-longhand", seed(3, 0, 0, 0, "all: unset; width: 1px"))
    print("%d seeds in %s" % (n, out))


if __name__ == "__main__":
    main()
