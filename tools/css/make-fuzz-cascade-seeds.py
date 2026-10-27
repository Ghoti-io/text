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
"""Write the seed corpus of tests/fuzz/fuzz_css_cascade.cpp into tests/fuzz/corpus/css-cascade.

    tools/css/make-fuzz-cascade-seeds.py [directory]

A seed is four option bytes and records (see the harness): six key characters, a space and a declaration. The corpus is made of scenarios, each
one for a criterion of the sort order, a rolling keyword, a flow, `all`, a custom property or a shorthand, written so that a library that gets it
wrong disagrees with the harness's reference model: for each pair of neighbouring criteria two records one of which is stronger by the first and
weaker by the second. Every scenario is written with the losers kept and the winners computed, with the parent's flow changed, and with an
allocation failed at a few places.

The files are named for what they hold and are deterministic. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

UA, USER, AUTHOR, ANIM, TRANS = range(5)


def key(origin=AUTHOR, imp=False, hint=False, att=False, layer=0, ctx=0, scoped=False, spec=(0, 0, 0), dist=0, sheet=0, rule=0, decl=0):
    k0 = origin | (8 if imp else 0) | (16 if hint else 0) | (32 if att else 0)
    k1 = layer | (ctx << 3) | (32 if scoped else 0)
    k2 = spec[0] | (spec[1] << 2) | (spec[2] << 4)
    k3 = dist
    k4 = sheet | (rule << 3)
    k5 = decl
    return "".join(chr(0x40 + v) for v in (k0, k1, k2, k3, k4, k5))


class Rec:
    """A record: its keys (as arguments of key()), its property and its value."""
    def __init__(self, name, value, kw):
        self.name, self.value, self.kw = name, value, kw

    def text(self):
        return key(**self.kw) + " " + self.name + ": " + self.value


def rec(name, value, **kw):
    return Rec(name, value, kw)


def seed(records, keep=False, rtl=False, vert=False, compute=False, fault=0):
    o0 = (1 if keep else 0) | (2 if rtl else 0) | (4 if vert else 0) | (8 if compute else 0)
    return bytes([o0, 0, 0, fault]) + "\n".join(r.text() for r in records).encode("latin-1")


def vary(records, how):
    """The same declarations with other keys, so that the model sees other populations of the same shapes."""
    out = []
    for n, r in enumerate(records):
        kw = dict(r.kw)
        if how == "ctx":
            kw["ctx"] = (kw.get("ctx", 0) + 1) % 4
        elif how == "imp":
            if n % 2 and kw.get("origin", AUTHOR) <= AUTHOR and not kw.get("hint"):
                kw["imp"] = not kw.get("imp", False)
        elif how == "layer":
            if kw.get("layer", 0):
                kw["layer"] = kw["layer"] % 7 + 1
        elif how == "origin":
            if n % 3 == 1 and kw.get("origin", AUTHOR) == AUTHOR and not kw.get("hint") and not kw.get("att"):
                kw["origin"] = USER
                kw["layer"] = 0
        elif how == "scope":
            kw["scoped"] = not kw.get("scoped", False)
            kw["dist"] = (kw.get("dist", 0) + n) % 8
        out.append(Rec(r.name, r.value, kw))
    if how == "reversed":
        out.reverse()
    return out


SCENARIOS = {}


def scenario(name, records):
    SCENARIOS[name] = records


# the layers of an origin: 1 a, 2 b, 3 b.x, 4 c, 5 c.y.z, 6 d, 7 a.w  (order: a.w a  b.x b  c.y.z c  d  -> ranks a.w=0 a=1 b.x=2 b=3 c.y.z=4 c=5 d=6)
# ---- the sort order, one criterion at a time
# a custom property holding var() is carried as pending (the substitution layer computes it); the records around it are not affected
scenario("custom-pending-is-carried", [rec("--a", "5", sheet=0), rec("--a", "x var(--m)", sheet=1), rec("--b", "1", sheet=2), rec("--b", "var(--a, 2)", sheet=3)])
scenario("tier-chain", [rec("color", c, origin=o, imp=i, sheet=n) for n, (c, o, i) in enumerate([
    ("red", UA, False), ("orange", USER, False), ("yellow", AUTHOR, False), ("green", ANIM, False), ("blue", AUTHOR, True),
    ("indigo", USER, True), ("violet", UA, True), ("black", TRANS, False)])][::-1])
scenario("tier-important-user-over-author", [rec("color", "red", origin=USER, imp=True, sheet=0), rec("color", "blue", origin=AUTHOR, imp=True, sheet=1)])
scenario("tier-animation-over-normal", [rec("color", "red", origin=ANIM, sheet=0), rec("color", "blue", origin=AUTHOR, spec=(3, 3, 3), sheet=1)])
scenario("tier-important-ua-over-user", [rec("color", "red", origin=UA, imp=True, sheet=0), rec("color", "blue", origin=USER, imp=True, sheet=1)])
scenario("context-normal", [rec("color", "red", ctx=0, spec=(0, 0, 1)), rec("color", "blue", ctx=1, spec=(1, 0, 0), sheet=1), rec("color", "green", ctx=3, sheet=2)])
scenario("context-important", [rec("color", "red", ctx=0, imp=True, spec=(1, 0, 0)), rec("color", "blue", ctx=1, imp=True, sheet=1),
                               rec("color", "green", ctx=3, imp=True, sheet=2)])
scenario("attached-normal", [rec("color", "red", att=True), rec("color", "blue", spec=(3, 3, 3), sheet=3), rec("color", "green", layer=2, sheet=4)])
scenario("attached-important", [rec("color", "red", att=True, imp=True), rec("color", "blue", imp=True, layer=1, spec=(3, 3, 3), sheet=3)])
scenario("attached-mixed", [rec("color", "red", att=True), rec("color", "blue", imp=True, sheet=2), rec("color", "green", att=True, imp=True, sheet=3),
                            rec("color", "black", att=True, imp=True, sheet=4, spec=(3, 0, 0))])
scenario("attached-has-no-specificity", [rec("width", "1px", att=True, spec=(3, 0, 0), sheet=0), rec("width", "2px", att=True, sheet=1)])
scenario("layers-normal", [rec("color", "red", layer=1, spec=(3, 0, 0)), rec("color", "orange", layer=4, sheet=1), rec("color", "yellow", layer=2, sheet=2),
                           rec("color", "green", layer=3, sheet=3), rec("color", "blue", layer=5, sheet=4)])
scenario("layers-normal-unlayered", [rec("color", "red", layer=6, spec=(3, 0, 0)), rec("color", "blue", sheet=1)])
scenario("layers-important", [rec("color", "red", layer=1, imp=True), rec("color", "orange", layer=4, imp=True, sheet=1),
                              rec("color", "blue", imp=True, sheet=2), rec("color", "green", layer=6, imp=True, sheet=3)])
scenario("layers-important-unlayered-last", [rec("color", "red", layer=6, imp=True, sheet=0), rec("color", "blue", imp=True, sheet=1)])
scenario("layers-nested", [rec("color", "red", layer=2), rec("color", "blue", layer=3, sheet=1), rec("color", "green", layer=7, sheet=2)])
scenario("layers-user-origin", [rec("color", "red", origin=USER, layer=4), rec("color", "blue", origin=USER, layer=1, sheet=1),
                                rec("color", "green", origin=USER, sheet=2)])
scenario("specificity", [rec("color", "red", spec=(1, 0, 0)), rec("color", "blue", spec=(0, 3, 3), sheet=1), rec("color", "green", spec=(0, 3, 2), sheet=2)])
scenario("specificity-b-over-c", [rec("color", "red", spec=(0, 1, 0)), rec("color", "blue", spec=(0, 0, 3), sheet=1)])
scenario("specificity-a-over-b", [rec("color", "red", spec=(1, 0, 0)), rec("color", "blue", spec=(0, 3, 3), sheet=1)])
scenario("scope", [rec("color", "red", scoped=True, dist=3), rec("color", "blue", scoped=True, dist=1, sheet=1), rec("color", "green", scoped=True, dist=2, sheet=2),
                   rec("color", "black", sheet=3)])
scenario("scope-none-loses", [rec("color", "red", sheet=5), rec("color", "blue", scoped=True, dist=7, sheet=0)])
scenario("scope-after-specificity", [rec("color", "red", scoped=True, dist=5, spec=(0, 0, 1)), rec("color", "blue", scoped=True, dist=0, sheet=1)])
scenario("order-sheet-rule-declaration", [rec("color", "red", sheet=1), rec("color", "blue", sheet=0, rule=7, decl=7), rec("color", "green", sheet=1, rule=1),
                                          rec("color", "black", sheet=1, rule=0, decl=7)])
scenario("order-rule-over-declaration", [rec("color", "red", rule=2, decl=0), rec("color", "blue", rule=1, decl=7)])
scenario("order-sheet-over-rule", [rec("color", "red", sheet=2, rule=0), rec("color", "blue", sheet=1, rule=7)])
scenario("order-equal-keys", [rec("color", "red"), rec("color", "blue")])
# ---- neighbouring criteria, each stronger by the first and weaker by the second
scenario("pair-tier-context", [rec("color", "red", origin=USER, imp=True, ctx=0, sheet=0), rec("color", "blue", origin=AUTHOR, imp=True, ctx=3, sheet=1)])
scenario("pair-context-attached", [rec("color", "red", ctx=0, sheet=0), rec("color", "blue", ctx=2, att=True, sheet=1)])
scenario("pair-attached-layer", [rec("color", "red", att=True, sheet=0), rec("color", "blue", layer=6, sheet=1), rec("color", "green", sheet=2)])
scenario("pair-layer-specificity", [rec("color", "red", layer=0, sheet=0), rec("color", "blue", layer=1, spec=(3, 3, 3), sheet=1)])
scenario("pair-layer-specificity-important", [rec("color", "red", layer=1, imp=True, sheet=0), rec("color", "blue", layer=6, imp=True, spec=(3, 3, 3), sheet=1)])
scenario("pair-specificity-scope", [rec("color", "red", spec=(0, 0, 1), scoped=True, dist=7, sheet=0), rec("color", "blue", spec=(0, 0, 0), scoped=True, dist=0, sheet=1)])
scenario("pair-scope-order", [rec("color", "red", scoped=True, dist=0, sheet=0), rec("color", "blue", scoped=True, dist=5, sheet=1)])
scenario("pair-sheet-rule", [rec("color", "red", sheet=1, rule=0), rec("color", "blue", sheet=0, rule=5)])
scenario("pair-rule-declaration", [rec("color", "red", rule=1, decl=0), rec("color", "blue", rule=0, decl=5)])
scenario("pair-spec-a-b", [rec("color", "red", spec=(1, 0, 0), sheet=0), rec("color", "blue", spec=(0, 3, 0), sheet=1)])
scenario("pair-spec-b-c", [rec("color", "red", spec=(0, 1, 0), sheet=0), rec("color", "blue", spec=(0, 0, 3), sheet=1)])
# ---- hints
scenario("hint-below-rules", [rec("width", "1px", hint=True, sheet=3), rec("width", "2px", layer=1, sheet=0), rec("width", "3px", sheet=1, origin=UA)])
scenario("hint-above-user", [rec("width", "1px", hint=True), rec("width", "2px", origin=USER, sheet=1), rec("width", "3px", origin=UA, sheet=2)])
scenario("hint-has-no-specificity", [rec("width", "1px", hint=True, spec=(3, 3, 3), sheet=0), rec("width", "2px", hint=True, sheet=1)])
scenario("hint-vs-unlayered-rule", [rec("width", "1px", hint=True, sheet=7), rec("width", "2px", sheet=0)])
scenario("hint-outer-context", [rec("width", "1px", hint=True, ctx=0), rec("width", "2px", ctx=1, sheet=1)])
# ---- rolling back
scenario("revert-author-to-user", [rec("color", "red", origin=USER), rec("color", "blue", sheet=0), rec("color", "revert", sheet=1)])
scenario("revert-chain", [rec("color", "red", origin=UA), rec("color", "revert", origin=USER), rec("color", "blue", sheet=0), rec("color", "revert", sheet=1)])
scenario("revert-ua-is-unset", [rec("color", "red", origin=UA, sheet=0), rec("color", "revert", origin=UA, sheet=1)])
scenario("revert-nothing-left", [rec("color", "revert")])
scenario("revert-animation", [rec("color", "red", origin=USER), rec("color", "blue"), rec("color", "revert", origin=ANIM)])
scenario("revert-transition", [rec("color", "red", origin=USER), rec("color", "blue"), rec("color", "revert", origin=TRANS)])
scenario("revert-important", [rec("color", "red", origin=USER), rec("color", "blue", imp=True, sheet=0), rec("color", "revert", imp=True, sheet=1)])
scenario("revert-user-important-kept", [rec("color", "red", origin=USER, imp=True), rec("color", "revert", imp=True)])
scenario("revert-layer-basic", [rec("width", "1px", layer=1), rec("width", "2px", layer=2, sheet=0), rec("width", "revert-layer", layer=2, sheet=1)])
scenario("revert-layer-chain", [rec("width", "1px", layer=1)] + [rec("width", "revert-layer", layer=l, sheet=l) for l in (2, 4, 6)] + [rec("width", "revert-layer", sheet=7)])
scenario("revert-layer-to-origin", [rec("width", "1px", origin=USER), rec("width", "revert-layer", layer=1)])
scenario("revert-layer-unlayered", [rec("width", "1px", layer=6), rec("width", "2px", sheet=0), rec("width", "revert-layer", sheet=1)])
scenario("revert-layer-important", [rec("width", "1px", layer=1, sheet=0), rec("width", "2px", layer=6, sheet=1), rec("width", "revert-layer", layer=1, imp=True, sheet=2),
                                    rec("width", "3px", layer=6, imp=True, sheet=3)])
scenario("revert-layer-important-later-layer", [rec("width", "1px", origin=USER), rec("width", "2px", layer=1, sheet=0), rec("width", "revert-layer", layer=6, imp=True, sheet=1)])
scenario("revert-layer-attached-important", [rec("width", "1px", layer=1, imp=True), rec("width", "revert-layer", att=True, imp=True, sheet=1),
                                             rec("width", "2px", att=True, sheet=2), rec("width", "3px", origin=ANIM, sheet=3)])
scenario("revert-layer-attached-important-no-rules", [rec("width", "revert-layer", att=True, imp=True), rec("width", "2px", att=True, sheet=2),
                                                      rec("width", "3px", origin=ANIM, sheet=3), rec("width", "4px", layer=1, sheet=4)])
scenario("revert-layer-attached-normal", [rec("width", "revert-layer", att=True), rec("width", "2px", layer=1, sheet=1), rec("width", "3px", att=True, sheet=0)])
scenario("revert-layer-animation", [rec("width", "1px"), rec("width", "2px", origin=ANIM, sheet=0), rec("width", "revert-layer", origin=ANIM, sheet=1)])
scenario("revert-layer-hint", [rec("width", "1px", origin=USER), rec("width", "2px", hint=True, sheet=0), rec("width", "revert-layer", hint=True, sheet=1)])
scenario("revert-layer-contexts", [rec("width", "1px", layer=1, ctx=1), rec("width", "2px", layer=2, ctx=0, sheet=1), rec("width", "revert-layer", layer=2, ctx=0, sheet=2)])
scenario("revert-layer-important-contexts", [rec("width", "1px", layer=1, ctx=0), rec("width", "revert-layer", layer=2, ctx=1, imp=True, sheet=1),
                                             rec("width", "2px", layer=1, ctx=0, imp=True, sheet=2)])
scenario("revert-rule", [rec("width", "1px", layer=1), rec("width", "2px", sheet=0, rule=0), rec("width", "3px", sheet=0, rule=1, decl=0),
                         rec("width", "revert-rule", sheet=0, rule=1, decl=1)])
scenario("revert-rule-animation", [rec("width", "1px"), rec("width", "2px", origin=ANIM, sheet=0), rec("width", "revert-rule", origin=ANIM, sheet=1)])
scenario("revert-rule-chain", [rec("width", "1px", origin=USER), rec("width", "revert-rule", sheet=0, rule=0), rec("width", "revert-rule", sheet=0, rule=1)])
scenario("revert-rule-important", [rec("width", "1px", layer=1, imp=True), rec("width", "revert-rule", sheet=1, imp=True, layer=2)])
scenario("revert-shorthand", [rec("margin", "1px", origin=USER), rec("margin-top", "5px", sheet=0), rec("margin", "revert", sheet=1)])
scenario("revert-custom", [rec("--x", "1", origin=USER), rec("--x", "revert", sheet=1), rec("--y", "revert-layer", layer=2), rec("--y", "2", layer=1, sheet=2)])
scenario("revert-logical", [rec("margin-left", "1px", origin=USER), rec("margin-left", "2px", sheet=0), rec("margin-inline-start", "revert", sheet=1)])
scenario("revert-logical-layer", [rec("height", "1px", layer=1), rec("height", "2px", layer=2, sheet=0), rec("block-size", "revert-layer", layer=2, sheet=1)])
# ---- the fix pass: a rule's declarations of both importances, chained element-attached rolls, a layer registry for each context, hints as an origin
scenario("revert-rule-both-importances", [rec("width", "1px", sheet=0), rec("width", "5px", sheet=1, rule=0, decl=0),
                                          rec("width", "revert-rule", imp=True, sheet=1, rule=0, decl=1)])
scenario("revert-rule-other-rules", [rec("width", "1px", origin=USER, sheet=2), rec("width", "2px", ctx=1, sheet=1), rec("width", "3px", att=True, sheet=1, decl=3),
                                     rec("width", "4px", hint=True, sheet=1, decl=4), rec("width", "revert-rule", imp=True, sheet=1, decl=1),
                                     rec("width", "9px", sheet=1, decl=0)])
scenario("revert-layer-attached-important-chain", [rec("width", "4px", origin=UA), rec("width", "5px", att=True, ctx=2),
                                                   rec("width", "revert-layer", att=True, imp=True, ctx=2, sheet=1),
                                                   rec("width", "revert-layer", att=True, imp=True, ctx=1, sheet=2)])
scenario("revert-layer-attached-important-user", [rec("width", "1px", origin=USER), rec("width", "2px", origin=USER, att=True),
                                                  rec("width", "3px", origin=ANIM, sheet=1),
                                                  rec("width", "revert-layer", origin=USER, att=True, imp=True, sheet=2)])
scenario("layers-per-context", [rec("width", "1px", layer=1, ctx=1), rec("width", "2px", layer=2, ctx=1, sheet=1), rec("height", "1px", layer=3, ctx=2),
                                rec("height", "2px", layer=5, ctx=2, sheet=1), rec("color", "red", layer=6, ctx=3), rec("color", "blue", layer=7, ctx=3, sheet=1),
                                rec("margin-top", "1px", layer=4, ctx=0), rec("margin-top", "2px", layer=1, ctx=0, sheet=1)])
scenario("layers-per-context-important", [rec("width", "1px", layer=1, ctx=1, imp=True), rec("width", "2px", layer=2, ctx=1, imp=True, sheet=1),
                                          rec("height", "1px", layer=3, ctx=3, imp=True), rec("height", "2px", layer=5, ctx=3, imp=True, sheet=1)])
scenario("hint-context-vs-author-context", [rec("width", "1px", hint=True, ctx=0), rec("width", "2px", ctx=1, sheet=1), rec("height", "1px", hint=True, ctx=3),
                                            rec("height", "2px", origin=USER, ctx=0, sheet=1)])
scenario("hint-and-revert", [rec("width", "1px", origin=USER), rec("width", "2px", hint=True, sheet=1), rec("width", "revert", sheet=2),
                             rec("height", "1px", origin=USER), rec("height", "2px", hint=True, sheet=1), rec("height", "revert-layer", sheet=2)])
scenario("scope-on-ruleless", [rec("width", "1px", hint=True, scoped=True, dist=0), rec("width", "2px", hint=True, sheet=1),
                               rec("height", "1px", att=True, scoped=True, dist=0), rec("height", "2px", att=True, sheet=1),
                               rec("color", "red", origin=ANIM, scoped=True, dist=0), rec("color", "blue", origin=ANIM, sheet=1)])
scenario("scope-largest", [rec("width", "1px", scoped=True, dist=7), rec("width", "2px", scoped=True, dist=6, sheet=1), rec("width", "3px", sheet=2)])
# ---- all
scenario("all-initial", [rec("color", "red", sheet=0), rec("direction", "rtl", sheet=1), rec("unicode-bidi", "embed", sheet=2), rec("--x", "1", sheet=3),
                         rec("all", "initial", sheet=4)])
scenario("all-before", [rec("all", "inherit", sheet=0), rec("color", "red", sheet=1), rec("width", "1px", sheet=2)])
scenario("all-important", [rec("all", "unset", imp=True, sheet=0), rec("color", "red", sheet=1), rec("width", "1px", imp=True, sheet=2)])
scenario("all-revert-layer", [rec("color", "red", origin=USER), rec("width", "1px", layer=1), rec("height", "2px", layer=2), rec("all", "revert-layer", layer=2, sheet=1)])
scenario("all-revert", [rec("color", "red", origin=USER), rec("all", "revert", sheet=1), rec("width", "1px", origin=USER, sheet=2)])
scenario("all-many", [rec("all", "initial" if i % 2 else "inherit", sheet=i) for i in range(8)] + [rec("color", "red", sheet=3, rule=4)])
scenario("all-with-logical", [rec("all", "initial", sheet=1), rec("margin-inline-start", "5px", sheet=0)])
scenario("all-with-logical-after", [rec("all", "initial", sheet=0), rec("margin-inline-start", "5px", sheet=1)])
scenario("all-writing-mode", [rec("writing-mode", "vertical-rl", sheet=0), rec("margin-block-start", "1px", sheet=1), rec("all", "initial", sheet=2),
                              rec("margin-block-start", "2px", sheet=3)])
# ---- flows
scenario("flow-rtl", [rec("direction", "rtl", sheet=0), rec("margin-inline-start", "1px", sheet=1), rec("margin-left", "2px", sheet=2), rec("margin-right", "3px", sheet=3)])
scenario("flow-vertical", [rec("writing-mode", "vertical-rl", sheet=0), rec("margin-block-start", "1px", sheet=1), rec("margin-right", "2px", sheet=2),
                           rec("inline-size", "5px", sheet=3), rec("height", "6px", sheet=4)])
scenario("flow-sideways-lr-rtl-upright", [rec("writing-mode", "sideways-lr", sheet=0), rec("direction", "rtl", sheet=0, rule=1), rec("text-orientation", "upright", sheet=0, rule=2),
                                          rec("margin-inline-start", "1px", sheet=1), rec("margin-bottom", "2px", sheet=2)])
scenario("flow-keywords", [rec("direction", "inherit", sheet=0), rec("writing-mode", "unset", sheet=0, rule=1), rec("margin-inline-start", "1px", sheet=1)])
scenario("flow-initial", [rec("direction", "initial", sheet=0), rec("margin-inline-end", "1px", sheet=1)])
scenario("flow-pending", [rec("direction", "var(--d)", sheet=0), rec("margin-inline-start", "1px", sheet=1)])
scenario("flow-revert", [rec("direction", "rtl", origin=USER), rec("direction", "revert", sheet=0), rec("margin-inline-start", "1px", sheet=1)])
scenario("flow-important", [rec("direction", "rtl", imp=True, sheet=0), rec("direction", "ltr", sheet=1), rec("margin-inline-start", "1px", sheet=2)])
scenario("logical-pairs", [rec("margin-inline-start", "1px", sheet=0), rec("margin-left", "2px", sheet=1), rec("width", "3px", sheet=2), rec("inline-size", "4px", sheet=3),
                           rec("border-start-start-radius", "5px", sheet=4), rec("border-top-left-radius", "6px", sheet=5),
                           rec("overflow-block", "hidden", sheet=6), rec("overflow-y", "scroll", sheet=7)])
scenario("logical-layers", [rec("margin-inline-start", "1px", layer=2), rec("margin-left", "2px", layer=1, sheet=1)])
# ---- customs, shorthands, dropped declarations
scenario("custom-names", [rec("--b", "1", sheet=0), rec("--a", "2", sheet=1), rec("--B", "3", sheet=2), rec("--a", "4", sheet=3), rec("--a", "5", origin=USER, sheet=4)])
scenario("custom-case", [rec("--Case", "a", sheet=0), rec("--case", "b", sheet=1)])
scenario("custom-revert-layer", [rec("--x", "1", layer=1), rec("--x", "revert-layer", layer=2)])
scenario("shorthand-margin", [rec("margin", "1px 2px 3px 4px", spec=(0, 1, 0)), rec("margin-left", "9px", spec=(0, 0, 1), sheet=1), rec("margin", "5px", sheet=2, rule=1)])
scenario("shorthand-font", [rec("font", "italic bold 12px/1.5 serif"), rec("font-size", "20px", sheet=1)])
scenario("shorthand-pending", [rec("margin", "var(--m)", sheet=1), rec("margin-top", "3px", sheet=0)])
scenario("shorthand-border", [rec("border", "1px solid red"), rec("border-top-color", "revert", sheet=1), rec("border-color", "blue", sheet=2, origin=USER)])
scenario("shorthand-background", [rec("background", "url(a) 1px 2px / 3px 4px no-repeat, red", sheet=0), rec("background-color", "blue", sheet=1)])
scenario("dropped", [rec("color", "red", sheet=0), rec("color", "not-a-color(", sheet=1), rec("colour", "red", sheet=2), rec("-x-y", "1", sheet=3), rec("width", "10px", sheet=4)])
scenario("mixed", [rec("color", "red", sheet=0, layer=2), rec("color", "revert-layer", sheet=1, layer=3), rec("margin", "1px", sheet=2, att=True), rec("--a", "b", sheet=3),
                   rec("all", "revert", sheet=4, origin=USER), rec("direction", "rtl", sheet=5, imp=True), rec("margin-inline-start", "7px", sheet=6, hint=True),
                   rec("width", "calc(1px + 2px)", sheet=7, origin=ANIM), rec("height", "1em", sheet=1, rule=3, imp=True, ctx=2)])

FAULTS = (1, 2, 3, 5, 9, 17, 33, 65)
VARIATIONS = ("ctx", "imp", "layer", "origin", "scope", "reversed")
FLAGS = {"": dict(), "-kept": dict(keep=True, compute=True), "-rtl": dict(keep=True, rtl=True, vert=False), "-vertical": dict(vert=True, compute=True)}


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-cascade")
    os.makedirs(out, exist_ok=True)
    keep = {f for f in os.listdir(out) if f.startswith("reproducer-")}
    for f in os.listdir(out):
        if f.endswith(".seed") and f not in keep:
            os.remove(os.path.join(out, f))
    n = 0
    for name, records in SCENARIOS.items():
        for suffix, flags in FLAGS.items():
            with open(os.path.join(out, "own-%s%s.seed" % (name, suffix)), "wb") as f:
                f.write(seed(records, **flags))
            n += 1
        for fault in FAULTS:
            with open(os.path.join(out, "own-%s-oom-%02d.seed" % (name, fault)), "wb") as f:
                f.write(seed(records, keep=True, compute=(fault % 2 == 1), fault=fault))
            n += 1
        for how in VARIATIONS:
            with open(os.path.join(out, "own-%s-%s.seed" % (name, how)), "wb") as f:
                f.write(seed(vary(records, how), keep=True, compute=(len(how) % 2 == 0)))
            n += 1
    print("%d seeds in %s" % (n, out))


if __name__ == "__main__":
    main(sys.argv)
