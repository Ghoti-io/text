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
"""Which leaves of a condition explain the divergences of a run of css_cond_diff.py (a tool for writing css_cond_features.py, not a gate).

    css_cond_explain.py --kind cond|rule --work DIR [--top N] [--of ours|browsers|both|all]

A query is a combination of leaves (a media feature in parentheses, a supports declaration, `selector(...)`), and the divergence of a combination is nearly always the divergence of a
leaf. For every leaf, with its numbers abstracted, this prints how many divergent queries hold it and how many queries hold it at all: a leaf that is in divergent queries and
in no agreeing one is the cause; a leaf in both kinds is innocent or only sometimes decisive. The first rows are the families to name in css_cond_features.py.
"""

import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import css_cond_diff  # noqa: E402

FUNCS = ("selector", "font-tech", "font-format", "at-rule", "env", "named-feature", "style", "scroll-state", "anchored", "supports", "media")


def groups(text):
    """Every balanced parenthesized group: (start, end, name) where name is the function name before it, if any."""
    out, stack = [], []
    quote = None
    i = 0
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


def leaves(text):
    """The leaves of a media query or supports condition, as text."""
    gs = groups(text)
    res = []
    for s, e, name in gs:
        inner = text[s + 1:e - 1]
        # the part of the inside that is not inside a nested group
        flat, depth = [], 0
        for ch in inner:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif depth == 0:
                flat.append(ch)
        top = "".join(flat)
        if name in ("selector", "font-tech", "font-format", "at-rule", "env", "named-feature"):
            res.append(text[s - len(name):e])
            continue
        if name and name not in ("calc", "min", "max", "clamp", "var", "url", "attr", "rgb", "rgba", "hsl", "if", "round", "mod", "rem", "sign", "abs", "sin", "cos"):
            res.append(text[s - len(name):e])
            continue
        if name:
            continue  # a value function inside a leaf
        if re.search(r"(?:^|\s)(and|or)(?:\s|$)", top) or re.match(r"\s*not\b", top) or inner.strip().startswith("(") or not inner.strip():
            continue
        res.append(text[s:e])
    return res


def key(leaf):
    k = re.sub(r"\d+", "0", leaf)
    k = re.sub(r"\s+", " ", k)
    return k[:70]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kind", default="cond")
    ap.add_argument("--work", required=True)
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--of", default="all")
    a = ap.parse_args()
    docs = css_cond_diff.load_population(a.work)
    rows = {}
    for line in open(os.path.join(a.work, "all-divergences.tsv"), encoding="utf-8"):
        k, cls, js = line.rstrip("\n").split("\t")
        rows[k] = (cls, json.loads(js))
    div, allq = collections.Counter(), collections.Counter()
    example = {}
    for i, d in enumerate(docs):
        for j, q in enumerate(d["q"]):
            ks = {key(l) for l in leaves(q[1])} if q[0] in ("M", "S") else {key(q[1]) if len(q) > 1 else ""}
            rs = [rows.get(css_cond_diff.key_of(i, j, q) + part) for part in ("", "m", "s")]
            r = next((x for x in rs if x), None)
            for k in ks:
                allq[k] += 1
                if r and (a.of == "all" or r[0] == a.of):
                    div[k] += 1
                    example.setdefault(k, r[1])
    ranked = sorted(div, key=lambda k: (-(div[k] / allq[k] >= 0.9), -div[k]))
    for k in ranked[:a.top]:
        ex = example[k]
        print("%4d/%-4d %s   e.g. ours=%s chromium=%s firefox=%s" % (div[k], allq[k], k, json.dumps(ex["ours"])[:40], str(ex["chromium"])[:40], str(ex["firefox"])[:40]))


if __name__ == "__main__":
    main()
