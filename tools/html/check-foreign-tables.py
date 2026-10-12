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
"""Check every hand-kept copy of the foreign-content name tables against tools/html/foreign_tables.txt.

    tools/html/check-foreign-tables.py [--list-copies]

The copies are extracted from the sources by their declarations, so a copy that moves or is renamed makes
this fail ("not found") rather than pass having read nothing. Each comparison is of whole sets, in both
directions, and the counts of the canonical file are asserted too (37, 58, 1, 11, as the chapter serves).
Exit 0 only if every copy agrees.
"""
import os
import re
import sys

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
problems = []


def read(path):
    with open(os.path.join(root, path), encoding="utf-8") as f:
        return f.read()


def block(text, start, where, end="};"):
    i = text.find(start)
    if i < 0:
        problems.append("%s: declaration not found: %s" % (where, start))
        return ""
    j = text.find(end, i)
    return text[i:j]


def strings(text):
    return re.findall(r'"([^"]*)"', text)


def compare(label, want, got):
    want, got = set(want), set(got)
    if not got:
        problems.append("%s: read nothing" % label)
        return
    for n in sorted(want - got):
        problems.append("%s: missing %s" % (label, n))
    for n in sorted(got - want):
        problems.append("%s: has %s, which the canonical table does not" % (label, n))


def superset(label, want, got):
    """The copy may carry names of its own (a fuzz pool has ordinary names); it must hold all of want."""
    missing = sorted(set(want) - set(got))
    for n in missing:
        problems.append("%s: missing %s" % (label, n))


canon = {"svg-tag": [], "svg-attr": [], "math-attr": [], "foreign-attr": []}
for line in read("tools/html/foreign_tables.txt").splitlines():
    if not line.strip() or line.startswith("#"):
        continue
    parts = line.split()
    canon[parts[0]].append(parts[1] if parts[0] != "foreign-attr" else (parts[1], parts[2]))
for kind, count in (("svg-tag", 37), ("svg-attr", 58), ("math-attr", 1), ("foreign-attr", 11)):
    if len(canon[kind]) != count or len(set(canon[kind])) != count:
        problems.append("foreign_tables.txt: %d %s entries, the chapter serves %d" % (len(canon[kind]), kind, count))
tags, attrs = canon["svg-tag"], canon["svg-attr"]
lower = lambda xs: [x.lower() for x in xs]

# src/html/html_tree_foreign.c: pairs (lower, restored); the lower form must be the lower-cased restored form.
f = read("src/html/html_tree_foreign.c")
for name, want, start in (("tags", tags, "k_svg_tags[] = {"), ("attrs", attrs, "k_svg_attrs[] = {")):
    pairs = re.findall(r'\{"([^"]+)", "([^"]+)"\}', block(f, start, "html_tree_foreign.c"))
    compare("html_tree_foreign.c svg %s" % name, want, [b for _, b in pairs])
    for a, b in pairs:
        if a != b.lower():
            problems.append("html_tree_foreign.c svg %s: %s is keyed %s" % (name, b, a))
fa = re.findall(r'\{"([^"]+)", (\d)\}', block(f, "k_foreign_attrs[] = {", "html_tree_foreign.c"))
compare("html_tree_foreign.c foreign attributes", ["%s %s" % x for x in canon["foreign-attr"]], ["%s %s" % x for x in fa])
if not re.search(r'"definitionurl"', read("src/html/html_tree.c")) or '"definitionURL"' not in read("src/html/html_tree.c"):
    problems.append("html_tree.c: the MathML definitionURL adjustment is not found")

# src/markup/markup_html.c
m = read("src/markup/markup_html.c")
compare("markup_html.c k_svg_tags", tags, strings(block(m, "k_svg_tags[] = {", "markup_html.c")))
compare("markup_html.c k_svg_attributes", attrs, strings(block(m, "k_svg_attributes[] = {", "markup_html.c")))
compare("markup_html.c k_math_attributes", canon["math-attr"], strings(block(m, "k_math_attributes[] = {", "markup_html.c")))

# tests/test-markup-html.cpp
t = read("tests/test-markup-html.cpp")
compare("test-markup-html.cpp tags", tags, strings(block(t, "const char * const tags[] = {", "test-markup-html.cpp")))
compare("test-markup-html.cpp attrs", attrs, strings(block(t, "const char * const attrs[] = {", "test-markup-html.cpp")))

# tools/oracle/html_tree_diff.py: SVG_ADJUSTED holds the lower-cased tags exactly; SVG_ATTRS holds, among
# its ordinary names, a spelling of attributes it exercises, so only the entries that are restored forms
# must be real ones.
d = read("tools/oracle/html_tree_diff.py")
compare("html_tree_diff.py SVG_ADJUSTED", lower(tags), strings(block(d, "SVG_ADJUSTED = [", "html_tree_diff.py", "]")))
svg_attrs = strings(block(d, "SVG_ATTRS = [", "html_tree_diff.py", "]"))
for a in svg_attrs:
    if a != a.lower() and ":" not in a and a not in attrs:
        problems.append("html_tree_diff.py SVG_ATTRS: %s is a mixed-case name the table does not have" % a)
superset("html_tree_diff.py SVG_ATTRS (foreign attributes)", [n for n, _ in canon["foreign-attr"]], svg_attrs)
math_attrs = strings(block(d, "MATH_ATTRS = [", "html_tree_diff.py", "]"))
superset("html_tree_diff.py MATH_ATTRS", ["definitionURL", "definitionurl"], math_attrs)

# tools/html/make-tree-fuzz-seeds.py: the two seed lists are the lower-cased tables.
s = read("tools/html/make-tree-fuzz-seeds.py")
for label, key, want in (("svg-name-table", "svg-name-table", lower(tags)), ("svg-attribute-table", "svg-attribute-table", lower(attrs))):
    i = s.find('("%s"' % key)
    if i < 0:
        problems.append("make-tree-fuzz-seeds.py: seed %s not found" % key)
        continue
    j = s.find("\n", i)
    inner = s[i:j]
    k = inner.find("for n in (")
    compare("make-tree-fuzz-seeds.py %s" % label, want, strings(inner[k:inner.index("))", k)]))

if problems:
    print("foreign-content name tables DISAGREE with tools/html/foreign_tables.txt:", file=sys.stderr)
    for p in problems:
        print("  " + p, file=sys.stderr)
    sys.exit(1)
print("foreign-content name tables agree: %d svg tags, %d svg attributes, %d foreign attributes, in every copy"
      % (len(tags), len(attrs), len(canon["foreign-attr"])))
