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
"""The population of the rule-operations differential (tools/oracle/css_ruleops_diff.py): sequences of `insertRule()`, `deleteRule()`, `appendRule()`, `findRule()` and the
keyframes `deleteRule(select)` on a style sheet and on the rules in it.

    Gen(seed, index).document() -> {"ops": [["S", css] | ["C"] | ["I", path, index, text] | ["D", path, index] | ["A", path, text] | ["X", path, select] | ["F", path, select], ...]}

A document starts from one sheet (a <style> element's, or a constructed one) drawn from a pool of sheets that hold the shapes the operations are about (imports, namespaces,
layer statements, style rules with nested rules, grouping rules, keyframes, a page), and applies a few operations drawn from pools of the texts that the specification's error
table and its orders of checks are about. The first documents are fixed: for every list of 'The order of checks' (the specification of CSSOM) a document for each two adjacent
conditions, so that each pair is asked of both browsers in every run. A population is a function of the seed and the document's index only.
"""

import random

SHEETS = [
    "",
    "a{}",
    "a{color:red} b{}",
    "@import url(a.css);",
    "@import url(a.css); @namespace x url(u);",
    "@namespace x url(u); x|a{} b{}",
    "@layer l; a{}",
    "@import url(b.css); @layer l; a{}",
    "@media print{ a{} b{} }",
    "a{ b{} color:red; & c{} }",
    "a{ @media print{ b{} } }",
    "@keyframes k{ from{opacity:0} 50%{opacity:.5} to{opacity:1} }",
    "@page{ margin:1px }",
    "@supports (a:b){ a{} }",
    "@layer x{ a{} } @scope (.s){ b{} }",
    "@font-face{font-family:x;src:url(a)} @counter-style c{system:cyclic;symbols:a}",
]

RULES = [
    "a{}", "b{color:red}", "c > d{margin:1px}", "x|e{}", "*|f{}", "|g{}", "> h{}", "& i{}", "j & {}", "a{} b{}", "", "  ", "x >{}", "b{color:red", "} ", ";",
    "@import url(c.css);", "@import url(c.css) print;", "@import \"c.css\" layer(q) supports(display:grid);", "@import;", "@import 3;", "@import url(c.css) {}",
    "@namespace y url(v);", "@namespace url(w);", "@namespace;", "@charset \"utf-8\";",
    "@layer m;", "@layer m, n.o;", "@layer p{}", "@layer q{ a{} }", "@layer;", "@layer a b;", "@layer a, b {}",
    "@media print{}", "@media (min-width:1px){ a{} }", "@media print;", "@media print", "@supports (display:grid){ a{} }", "@supports{}",
    "@container n (min-width:1px){ a{} }", "@scope (.a) to (.b){ c{} }", "@scope (a) to {}", "@starting-style{ a{} }",
    "@font-face{font-family:x;src:url(a)}", "@font-face{}", "@keyframes k2{ from{opacity:0} }", "@keyframes k3{}", "@keyframes {}", "@counter-style c2{system:cyclic;symbols:a b}",
    "@counter-style c3{system:fixed}", "@page :first{ margin:1px }", "@page foo bar{}", "@top-left{ content:\"x\" }", "@property --p{syntax:'*';inherits:false}", "@property --q{syntax:'<length>'}",
    "@position-try --t{ top:1px }", "@bogus x;",
    "width:1px", "width:1px; color:red", "xwidth:1px", "color:nonsense", "--x:1", "color:red !important", "c{} d:e", "color:red; c{}",
]

NESTED_ONLY = ["> k{}", "& l{}", "m{}", "width:2px", "@media print{ n{} }", "@font-face{font-family:x;src:url(a)}", "@import url(a.css);"]

SELECTS = ["from", "to", "0%", "50%", "100%", "25%,75%", " 50% ", "FROM", "To", "bogus", "", "10%, 20%", "75%, 25%"]
KEYFRAMES = ["60% {opacity:1}", "from{}", "70% {animation-name:x; width:1px}", "bogus", "", "from{} to{}", "10%, 20% {opacity:.5}", "80% {color:bogus}", "FROM {opacity:1}", "200% {opacity:1}"]
INDEXES = [None, 0, 0, 1, 1, 2, 3, 9]


def order_documents():
    """One document for each two adjacent conditions of each list of 'The order of checks' that the browsers can be asked."""
    docs = []
    # (a) sheet.insertRule: the parse before the index, the constructed import before the index, the index before the constraints, the constraints before the namespace state
    docs.append({"ops": [["S", "a{}"], ["I", "", 9, "a{} b{}"]]})
    docs.append({"ops": [["S", "a{}"], ["I", "", 9, ""]]})
    docs.append({"ops": [["C"], ["I", "", 9, "@import url(a.css);"]]})
    docs.append({"ops": [["S", "a{}"], ["I", "", 9, "@import url(a.css);"]]})
    docs.append({"ops": [["S", "a{}"], ["I", "", 1, "@namespace x url(u);"]]})
    docs.append({"ops": [["S", "a{}"], ["I", "", 0, "@namespace x url(u);"]]})
    # (b) a rule's insertRule: the index before the parse, the parse before the constraints
    docs.append({"ops": [["S", "a{ b{} }"], ["I", "0", 9, "x y z{"]]})
    docs.append({"ops": [["S", "a{ b{} }"], ["I", "0", 9, ""]]})
    docs.append({"ops": [["S", "@media print{ a{} }"], ["I", "0", 0, "@import url(a.css); b{}"]]})
    docs.append({"ops": [["S", "@media print{ a{} }"], ["I", "0", 0, "@import url(a.css);"]]})
    docs.append({"ops": [["S", "@media print{ a{} }"], ["I", "0", 0, "width:1px"]]})
    docs.append({"ops": [["S", "a{ b{} }"], ["I", "0", 0, "width:1px"]]})
    # (c) sheet.deleteRule: the index before the namespace state
    docs.append({"ops": [["S", "@namespace x url(u); a{}"], ["D", "", 9]]})
    docs.append({"ops": [["S", "@namespace x url(u); a{}"], ["D", "", 0]]})
    # (d) a rule's deleteRule
    docs.append({"ops": [["S", "@media print{ a{} }"], ["D", "0", 9]]})
    return docs


class Gen:
    def __init__(self, seed, index):
        self.index = index
        self.rng = random.Random(seed * 1000003 + index)

    def document(self):
        fixed = order_documents()
        if self.index < len(fixed):
            return fixed[self.index]
        r = self.rng
        ops = [["C"] if r.random() < 0.15 else ["S", r.choice(SHEETS)]]
        for _ in range(r.randint(1, 6)):
            ops.append(self.op(ops))
        return {"ops": ops}

    def op(self, ops):
        r = self.rng
        path = r.choice(["", "", "", "0", "0", "1", "0.0", "0.1", "2"])
        x = r.random()
        if x < 0.55:
            text = r.choice(RULES if path == "" or r.random() < 0.6 else RULES + NESTED_ONLY)
            return ["I", path, r.choice(INDEXES), text]
        if x < 0.75:
            return ["D", path, r.choice([0, 0, 1, 2, 9])]
        if x < 0.85:
            return ["A", path, r.choice(KEYFRAMES)]
        if x < 0.92:
            return ["X", path, r.choice(SELECTS)]
        return ["F", path, r.choice(SELECTS)]
