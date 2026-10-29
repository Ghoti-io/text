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
"""The population of the rule differential (tools/oracle/css_rule_diff.py): at-rules for `CSSStyleSheet.insertRule()`.

    Gen(seed, index).document() -> {"env": "desktop", "w": 1280, "h": 800, "dpr": 1.0, "q": [["A", rule text] | ["R", supports condition], ...]}

Each rule is built from a name or prelude and a list of descriptors, drawn from pools that hold the valid forms and the mistakes the review of the at-rule layer found (a
reserved counter style name, ascending additive weights, a backwards range, a generic family in @font-face, a feature tag of five letters, `currentcolor` in a palette,
`!important` in a page block and in a keyframe, an unknown at-rule in a page block, an @property missing a descriptor). A population is a function of the seed and the
document's index only.
"""

import random

_r = random.Random()
rng = _r.random
R = _r.choice


def some(pool, lo=0, hi=4):
    return [R(pool) for _ in range(lo + int(rng() * (hi - lo + 1)))]


def block(decls):
    return "{ " + "; ".join(decls) + " }" if rng() < 0.7 else "{" + ";".join(decls) + "}"


COUNTER_NAMES = ["foo", "my-counter", "Custom", "decimal", "disc", "circle", "square", "disclosure-open", "disclosure-closed", "none", "initial", "inherit", "DECIMAL",
                 "upper-roman", "decimal-leading-zero", "a", "x-y", "\"string\"", "1x", "-x"]
COUNTER_DECLS = ["system: cyclic", "system: fixed", "system: fixed 5", "system: symbolic", "system: alphabetic", "system: numeric", "system: additive", "system: extends decimal",
                 "system: extends foo", "system: foo", "symbols: a b c", "symbols: \"x\" \"y\"", "symbols: a", "symbols:", "additive-symbols: 3 c, 2 b, 1 a", "additive-symbols: 1 a, 2 b",
                 "additive-symbols: 0 a, 1 b", "additive-symbols: 1 a, 1 b", "additive-symbols: 10 x", "additive-symbols: -1 a", "range: 1 10", "range: 10 1", "range: infinite 5",
                 "range: 5 infinite", "range: infinite infinite", "range: auto", "range: 1 10, 20 30", "range: 1 10, 30 20", "fallback: decimal", "fallback: none", "fallback: foo",
                 "pad: 3 \"0\"", "pad: -1 \"0\"", "pad: 3", "negative: \"-\"", "negative: \"(\" \")\"", "prefix: \"[\"", "suffix: \". \"", "suffix: foo", "speak-as: auto", "speak-as: bullets",
                 "speak-as: words", "speak-as: decimal", "foo: bar", "range: 1.5 3", "pad: 1 \"ab\""]
FACE_DECLS = ["font-family: x", "font-family: \"x y\"", "font-family: serif", "font-family: sans-serif", "font-family: monospace", "font-family: x y", "font-family: serif foo",
              "src: url(a.woff2)", "src: url(a) format(\"woff2\")", "src: local(x)", "src: local(\"x y\")", "src: url(a) tech(variations)", "src: url(a), url(b)", "src: url(a) format(woff2) tech(color-COLRv1)",
              "src: bogus", "src: url(a), bogus", "src:", "font-style: italic", "font-style: oblique 10deg 20deg", "font-style: foo", "font-weight: 400", "font-weight: 100 900", "font-weight: 900 100",
              "font-weight: bold", "font-weight: 0", "font-weight: 1001", "font-stretch: 50% 200%", "font-stretch: condensed", "font-width: 75%", "unicode-range: U+25-FF", "unicode-range: U+4??",
              "unicode-range: U+0-10FFFF", "unicode-range: U+110000", "unicode-range: U+FF-25", "unicode-range: foo", "font-feature-settings: \"liga\" 1", "font-feature-settings: \"abc\" 1",
              "font-feature-settings: \"abcde\"", "font-feature-settings: normal", "font-variation-settings: \"wght\" 300", "font-variation-settings: \"ab\" 1", "font-display: swap",
              "font-display: foo", "ascent-override: 90%", "ascent-override: -1%", "descent-override: normal", "line-gap-override: 10%", "size-adjust: 110%", "size-adjust: 0%", "size-adjust: -5%",
              "font-named-instance: foo", "font-language-override: \"TRK\"", "foo: bar", "font-family: x !important", "src: url(a) !important", "font-variant: small-caps", "font-feature-settings: \"liga\" 1 !important"]
PALETTE_DECLS = ["font-family: x", "font-family: x, y", "font-family: serif", "base-palette: 1", "base-palette: light", "base-palette: dark", "base-palette: -1", "base-palette: foo",
                 "override-colors: 0 red", "override-colors: 0 red, 1 #00f", "override-colors: 0 currentcolor", "override-colors: 0 rgb(0 0 0 / .5)", "override-colors: -1 red", "override-colors: red",
                 "override-colors: 1.5 red", "override-colors: 0 color-mix(in srgb, red, currentcolor)", "foo: bar"]
FEATURE_DECLS = ["@stylistic { fancy: 1 }", "@stylistic { fancy: 1 2 }", "@styleset { a: 1 2 3 }", "@character-variant { cv: 1 2 }", "@character-variant { cv: 1 2 3 }", "@swash { s: 1 }",
                 "@ornaments { o: 2 }", "@annotation { n: 1 }", "@historical-forms { h: 1 }", "@foo { a: 1 }", "@stylistic { fancy: -1 }", "@stylistic { fancy: a }", "@stylistic { fancy: 1 !important }",
                 "@styleset { a: 0 }", "@stylistic { 1: 1 }", "@stylistic { }", "@styleset { a: 1; a: 2 }"]
PAGE_SELECTORS = ["", ":first", ":left", ":right", ":blank", "foo", "foo:first", ":first:left", ":bogus", "foo bar", "\"x\"", ":nth(2)", "foo:nth(2)", ":nth(2n+1)"]
PAGE_DECLS = ["size: A4", "size: A4 !important", "size: letter landscape", "size: 10cm 20cm", "size: auto", "size: foo", "size: 10cm 20cm 30cm", "margin: 1in", "margin-top: 1in !important",
              "page-orientation: rotate-left", "page-orientation: foo", "marks: crop cross", "marks: none", "bleed: 3mm", "bleed: auto", "color: red", "width: 10px", "foo: bar", "background: red"]
PAGE_MARGINS = ["@top-left-corner", "@top-left", "@top-center", "@top-right", "@top-right-corner", "@bottom-left", "@bottom-center", "@left-top", "@right-middle", "@bottom", "@foo", "@top",
                "@left-bottom", "@right-bottom"]
MARGIN_DECLS = ["content: \"x\"", "content: counter(page)", "color: red", "width: 10px", "foo: bar", "size: A4", "content: \"x\" !important"]
KEYFRAME_NAMES = ["foo", "\"foo\"", "none", "\"none\"", "initial", "", "\"\"", "-x", "1", "inherit", "Foo", "x y", "a b c"]
KEYFRAME_SELECTORS = ["from", "to", "0%", "50%", "100%", "150%", "-10%", "FROM", "To", "from, to", "25%, 75%", "foo", "10", "10px", "cover 50%", "entry 0%", "50% 50%", "calc(50%)", "50.5%", "0% 100%"]
KEYFRAME_DECLS = ["color: red", "color: red !important", "animation-duration: 1s", "animation-name: foo", "animation-timing-function: ease", "opacity: .5", "width: 1px", "foo: bar", "--x: 1",
                  "transition: all 1s", "animation: foo 1s", "display: none", "color: inherit", "color: var(--x)"]
PROPERTY_DECLS = ["syntax: \"*\"", "syntax: \"<length>\"", "syntax: \"<length> | <percentage>\"", "syntax: \"<foo>\"", "syntax: \"<color>+\"", "syntax: \"<length>#\"", "syntax: foo", "syntax: \"\"",
                  "syntax: \"<length>\" !important", "inherits: true", "inherits: false", "inherits: maybe", "inherits: TRUE", "initial-value: 1px", "initial-value: 1em", "initial-value: red",
                  "initial-value: auto", "initial-value: 0", "initial-value: calc(1px + 2px)", "initial-value: var(--x)", "initial-value:", "initial-value: none", "foo: bar"]
PROPERTY_NAMES = ["--a", "--a-b", "a", "--", "-a", "--1", "\"--a\"", "--a b", "--X"]
POSITION_DECLS = ["top: 0", "left: anchor(right)", "color: red", "position-area: top", "align-self: end", "width: 10px", "inset: 0", "position-anchor: --a", "margin: 0", "foo: bar", "top: 0 !important"]
VIEW_DECLS = ["navigation: auto", "navigation: none", "navigation: foo", "types: a b", "types: none", "types: none a", "foo: bar", "navigation: auto !important"]
COLOR_PROFILE_DECLS = ["src: url(a.icc)", "components: red green blue", "rendering-intent: perceptual", "rendering-intent: foo", "foo: bar", "src: foo"]
SUPPORTS_CONDS = ["(display: grid)", "not (display: grid)", "(display: grid) and (color: red)", "(display: grid) or (foo: bar)", "(foo: red})", "garbage", "", "selector(a > b)",
                  "selector(:is(a,))", "selector(:nth-col(1))", "at-rule(@media)", "at-rule(@charset)", "font-tech(variations)", "font-format(woff2)", "(--x: a;b)", "(--x: )", "(--x: {a})",
                  "(foo)", "foo", "(display: grid) and", "not (not (display: grid))", "(display: grid) and (color: red) or (width: 1px)", "(color: red !important)", "display: grid",
                  "(foo: url(bad url))", "(( ))", "()", "env(x)", "named-feature(x)", "(width: 1px) and not (height: 2px)"]
CONTAINER_PRELUDES = ["(width > 10px)", "card (min-width: 1px)", "style(--x: 1)", "(foo)", "(", "not not (width)", "scroll-state(stuck: top)", "(width) and (height)", "(width) or (height) and (color)",
                      "card", "none (width)", "initial (width)", "(width: 10px) and style(--x)", "(inline-size > 1px)", "(aspect-ratio > 1)", "(orientation: portrait)", "anchored(fallback: none)", "style(color)"]
MEDIA_PRELUDES = ["screen", "(min-width: 1px)", "(color: foo)", "not all", "only screen and (color)", "(width > 10px)", "foo", "", ",", "(", "screen and", "not (color)"]
LAYER_RULES = ["@layer a;", "@layer a, b;", "@layer a.b { }", "@layer { }", "@layer a, b { }", "@layer 1;", "@layer a.;", "@layer a..b;", "@layer a b;"]
SCOPE_PRELUDES = ["(.a)", "(.a) to (.b)", "to (.b)", "", "(.a, .b)", "(&)", "(:is(.a))", "(::before)", "(.a) (.b)"]


def decls(pool, lo=0, hi=5):
    return [R(pool) for _ in range(lo + int(rng() * (hi - lo + 1)))]


def rule():
    k = rng()
    if k < 0.18:
        return "@counter-style %s %s" % (R(COUNTER_NAMES), block(decls(COUNTER_DECLS, 1, 5)))
    if k < 0.36:
        return "@font-face %s" % block(decls(FACE_DECLS, 1, 6))
    if k < 0.42:
        return "@font-palette-values %s %s" % (R(["--x", "--Y", "x", "--", "--a b"]), block(decls(PALETTE_DECLS, 0, 4)))
    if k < 0.47:
        return "@font-feature-values %s { %s }" % (R(["Foo", "\"Foo\"", "foo bar", "foo, bar", "", "serif", "x"]), " ".join(decls(FEATURE_DECLS, 0, 3)))
    if k < 0.58:
        inner = decls(PAGE_DECLS, 0, 3)
        margins = ["%s %s" % (R(PAGE_MARGINS), block(decls(MARGIN_DECLS, 0, 2))) for _ in range(int(rng() * 3))]
        body = "; ".join(inner) + (";" if inner and margins else "") + " " + " ".join(margins)
        return "@page %s { %s }" % (R(PAGE_SELECTORS), body)
    if k < 0.70:
        frames = ["%s %s" % (R(KEYFRAME_SELECTORS), block(decls(KEYFRAME_DECLS, 0, 3))) for _ in range(int(rng() * 4))]
        return "@keyframes %s { %s }" % (R(KEYFRAME_NAMES), " ".join(frames))
    if k < 0.78:
        return "@property %s %s" % (R(PROPERTY_NAMES), block(decls(PROPERTY_DECLS, 0, 4)))
    if k < 0.82:
        return "@position-try %s %s" % (R(["--a", "a", "--B"]), block(decls(POSITION_DECLS, 0, 3)))
    if k < 0.85:
        return "@view-transition %s" % block(decls(VIEW_DECLS, 0, 3))
    if k < 0.87:
        return "@color-profile %s %s" % (R(["--p", "device-cmyk", "p", "--"]), block(decls(COLOR_PROFILE_DECLS, 0, 3)))
    if k < 0.91:
        return "@supports %s { }" % R(SUPPORTS_CONDS)
    if k < 0.94:
        return "@container %s { }" % R(CONTAINER_PRELUDES)
    if k < 0.96:
        return "@media %s { }" % R(MEDIA_PRELUDES)
    if k < 0.98:
        return R(LAYER_RULES)
    return "@scope %s { }" % R(SCOPE_PRELUDES)


class Gen:
    PER_DOCUMENT = 40

    def __init__(self, seed, index):
        self.seed, self.index = seed, index

    def document(self):
        _r.seed((self.seed << 32) ^ (self.index * 2246822519) ^ 0x27d4eb2f)
        return {"env": "desktop", "w": 1280, "h": 800, "dpr": 1.0, "q": [["A", rule()] for _ in range(self.PER_DOCUMENT)]}
