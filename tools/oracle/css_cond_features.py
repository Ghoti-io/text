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
"""The features a divergence of the condition and rule differentials is listed under (tools/oracle/css_cond_diff.py).

A divergence is abstracted to the smallest thing in the question that explains it, so that a list of a few dozen reasons covers a population of thousands: the first rule whose
pattern is found in the question's text names the feature, and a question no rule names is its own feature (`q:` and its text), which the list then has to name one by one.
Each rule is (feature, pattern, kinds); the order is the order they are tried in.
"""

import re

# Families named by hand, tried before the leaf of a question is looked for: (feature, pattern, the kinds of question it names). A pattern is found in the question's text.
COND = [
    ("S:declaration-value-if", r"\bif\(", ("S", "P")),
    ("S:sel::-webkit-autofill", r":-webkit-autofill", ("S",)),
    ("S:sel::-moz-any", r":-moz-any", ("S",)),
    ("S:sel::-webkit-any", r":-webkit-any", ("S",)),
]
RULE = []

FUNCS_SUPPORTS = ("selector", "font-tech", "font-format", "at-rule", "env", "named-feature")
VALUE_FUNCTIONS = ("calc", "min", "max", "clamp", "var", "url", "attr", "rgb", "rgba", "hsl", "if", "round", "mod", "rem", "sign", "abs", "sin", "cos", "first-valid", "env")


def balanced_groups(text):
    """Every balanced parenthesized group: (start, end, function name before it or '')."""
    out, stack, quote, i = [], [], None, 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "(":
            m = re.search(r"([A-Za-z-]+)$", text[:i])
            stack.append((i, m.group(1).lower() if m else ""))
        elif c == ")" and stack:
            s, name = stack.pop()
            out.append((s, i + 1, name))
        i += 1
    return out


def condition_leaves(text):
    """The leaves of a media query or supports condition: a media feature or a declaration in parentheses, a supports function, a bare general-enclosed group."""
    res = []
    taken = []  # the text ranges of the leaves found: a group inside one is part of it
    for s, e, name in sorted(balanced_groups(text), key=lambda g: (g[0], -g[1])):
        if any(a <= s and e <= b for a, b in taken):
            continue
        inner = text[s + 1:e - 1]
        flat, depth = [], 0
        for ch in inner:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif depth == 0:
                flat.append(ch)
        top = "".join(flat)
        if name in FUNCS_SUPPORTS or (name and name not in VALUE_FUNCTIONS and not text[:s].rstrip().endswith(":")):
            res.append(text[s - len(name):e])
            taken.append((s - len(name), e))
        elif name:
            continue
        elif re.search(r"(?:^|\s)(and|or)(?:\s|$)", top) or re.match(r"\s*not\b", top) or inner.strip().startswith("(") or not inner.strip():
            continue
        else:
            res.append(text[s:e])
            taken.append((s, e))
    return res


def media_family(leaf):
    inner = leaf.strip()[1:-1].strip()
    m = re.search(r"([A-Za-z-][A-Za-z0-9-]*)", re.sub(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[A-Za-z%]*", " ", inner))
    name = m.group(1).lower() if m else "?"
    name = re.sub(r"^(?:min|max)-(?!webkit)", "", name)
    ops = re.search(r"[<>=]", inner)
    form = "range" if ops else ("plain" if ":" in inner else "bool")
    return "m:%s:%s" % (name, form)


def supports_family(leaf):
    m = re.match(r"\s*(selector|font-tech|font-format|at-rule|env|named-feature)\(\s*(.*?)\s*\)\s*$", leaf, re.S | re.I)
    if m:
        fn, arg = m.group(1).lower(), m.group(2)
        if fn == "selector":
            ps = re.findall(r"::?[A-Za-z-]+", arg)
            return "sel:%s" % (ps[0].lower() if ps else "type")
        return "%s:%s" % (fn, re.sub(r"\d+", "0", arg.lower())[:30])
    m = re.match(r"\s*\(\s*([A-Za-z-][A-Za-z0-9_-]*)\s*:\s*(.*)\)\s*$", leaf, re.S)
    if m:
        return "d:%s" % m.group(1).lower()
    return "ge"


def declaration_family(prefix, text):
    pieces = []
    depth, cur, quote = 0, [], None
    for c in text:
        if quote:
            cur.append(c)
            if c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "({[":
            depth += 1
            cur.append(c)
        elif c in ")}]":
            depth -= 1
            cur.append(c)
        elif c == ";" and depth == 0:
            pieces.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    pieces.append("".join(cur))
    out = []
    for piece in pieces:
        m = re.match(r"\s*(@?[A-Za-z-][A-Za-z0-9_-]*)\s*(:|\{)", piece.strip())
        if m:
            out.append("%s:%s" % (prefix, m.group(1).lower()))
    return out


def rule_leaves(text):
    """The parts of an at-rule a divergence is attributed to: its head, each declaration of its block and each nested rule."""
    m = re.match(r"\s*@([A-Za-z-]+)\s*([^{;]*)", text)
    at = m.group(1).lower() if m else "?"
    out = ["h:%s" % at]
    prelude = (m.group(2) if m else "").strip()
    if at in ("supports", "container", "media", "scope", "layer", "page", "keyframes", "property", "counter-style", "font-palette-values", "font-feature-values", "color-profile", "position-try"):
        shape = re.sub(r"[A-Za-z0-9_-]+", "w", prelude)
        shape = re.sub(r"\s+", " ", shape)[:20]
        out.append("p:%s:%s" % (at, shape))
    body = text[text.find("{") + 1:text.rfind("}")] if "{" in text else ""
    for fam in declaration_family("d", body):
        out.append("%s:%s" % (fam, at))
    return out


def leaf_families(query):
    kind = query[0]
    if kind == "M":
        return [media_family(l) for l in condition_leaves(query[1])] or ["m:?"]
    if kind in ("S", "R"):
        return [supports_family(l) for l in condition_leaves(query[1])] or ["ge"]
    if kind == "P":
        return ["p:%s" % query[1].lower()]
    if kind == "A":
        return rule_leaves(query[1])
    return ["?"]


ABBREVIATION = {"kept": "K", "dropped": "D", "ok": "O", "reject": "R", None: "-"}


def rule_feature(query, values):
    """A divergence of a rule: its kind of rule, the part (accepted or not, the prelude, a descriptor or nested rule by name) and who keeps it: ours/Chromium/Firefox as K (kept), D
    (dropped), O (accepted), R (rejected) or, for a prelude, whether each browser wrote what this library wrote (= or !)."""
    m = re.match(r"\s*@([A-Za-z-]+)", query[1])
    at = m.group(1).lower() if m else "?"
    part, o, c, f = values["part"], values["ours"][0], values["chromium"], values["firefox"]
    if part == "h":
        pat = "ch%sff%s" % ("=" if c == o else "!", "=" if f == o else "!")
    else:
        pat = "%s%s%s" % (ABBREVIATION.get(o, "?"), ABBREVIATION.get(c, "?"), ABBREVIATION.get(f, "?"))
    return "A:%s:%s:%s" % (at, part, pat)


def feature_of(rules, query, values, stats=None):
    """The feature a divergence is listed under: a hand-named family (a rule of COND or RULE whose pattern is found in the question), else the leaf of the question that is in the
    most divergent questions and in (almost) no agreeing one, else the question itself."""
    if query[0] == "A":
        return rule_feature(query, values)
    kind = query[0] + values.get("part", "")
    text = "\t".join(query[1:])
    for name, pattern, kinds in rules:
        if kind in kinds and re.search(pattern, text, re.I | re.S):
            return name
    fams = leaf_families(query)
    if stats:
        best = None
        for f in fams:
            div, allq = stats.get((kind, f), (0, 0))
            if not div:
                continue
            score = (div / allq >= 0.75, div, -allq)
            if best is None or score > best[0]:
                best = (score, f)
        if best:
            return "%s:%s" % (kind, best[1])
    return "q:%s:%s" % (kind, text.replace("\n", " ")[:120])
