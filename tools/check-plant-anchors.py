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
"""Fail if an edit of a plant list does not apply exactly once to the source it names.

    tools/check-plant-anchors.py [--controls]

A planted defect is a text that must occur exactly once in one source file and what replaces it. A plant whose text matches nothing
(a later commit moved the code) edits nothing, so the gate that is meant to fail on it passes: it is a gate that cannot fail. A text
that matches twice is edited in the wrong place or refused. The plant tools refuse both when they run, but they run only in the gates
that build and execute programs; this reads the lists and the sources and nothing else, so `make test` finds a stale anchor on the
commit that moves the code. It covers every tab separated plant list of tools/conformance, tools/oracle and tools/encoding (the
tables inside tools/*/plant-fuzz.py are tools/check-fuzz-plant-anchors.py's), and checks that the lists which name plants of other
lists (the oracle populations) name plants that exist.

--controls makes each mistake in memory (an anchor that matches nothing, one that matches twice, an edit that changes nothing, a
name that does not exist, a plant that edits two files) and requires the check to report it.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# list path, layout. Layouts: "nfoc" = name, file, old, new[, ...]; "html-serializer" = name, letter, old, new (the file is fixed);
# "serialize-oracle" = name, old, new (fixed file). Directories under src/ that a file column may be relative to.
SRC_DIRS = ["src/css", "src", "src/html", "src/markup", "src/encoding", "src/xml"]
LISTS = [
    ("tools/conformance/CSS_ATRULES_PLANTS", "nfoc"),
    ("tools/conformance/CSS_COLOR_PLANTS", "nfoc"),
    ("tools/conformance/CSS_COMPUTED_PLANTS", "nfoc"),
    ("tools/conformance/CSS_CONDITIONS_PLANTS", "nfoc"),
    ("tools/conformance/CSS_CSSOM_PLANTS", "nfoc"),
    ("tools/conformance/CSS_PROPERTY_PLANTS", "nfoc"),
    ("tools/conformance/CSS_SELECTOR_CONFORMANCE_PLANTS", "nfoc"),
    ("tools/conformance/CSS_SELECTOR_DOM_PLANTS", "nfoc"),
    ("tools/conformance/CSS_SELECTOR_PLANTS", "nfoc"),
    ("tools/conformance/CSS_VARIABLES_PLANTS", "nfoc"),
    ("tools/conformance/HTML_SERIALIZER_PLANTS", "html-serializer"),
    ("tools/oracle/CSS_PROPERTY_ORACLE_PLANTS", "nfoc"),
    ("tools/oracle/CSS_STYLE_PLANTS", "nfoc"),
    ("tools/oracle/ENCODING_ORACLE_PLANTS", "nfoc"),
    ("tools/oracle/HTML_PIPELINE_ORACLE_PLANTS", "nfoc"),
    ("tools/oracle/HTML_SERIALIZE_ORACLE_PLANTS", "serialize-oracle"),
    ("tools/oracle/HTML_SNIFF_ORACLE_PLANTS", "nfoc"),
    ("tools/encoding/UNIT_PLANTS", "nfoc"),
]
SERIALIZER = "src/html/html_serialize.c"
# lists whose runner applies the lines of one name to several files
MULTIFILE = {"tools/conformance/CSS_COMPUTED_PLANTS", "tools/oracle/CSS_STYLE_PLANTS"}
# lists of names: the file, and the lists whose names it may use
NAME_LISTS = [
    ("tools/oracle/CSS_COND_ORACLE_PLANTS", ["tools/conformance/CSS_CONDITIONS_PLANTS", "tools/conformance/CSS_ATRULES_PLANTS"]),
    ("tools/oracle/CSS_SHEET_ORACLE_PLANTS", ["tools/conformance/CSS_SELECTOR_PLANTS"]),
]


def rows(text):
    """Data lines of a list: (line number, fields). Comments and blank lines are skipped."""
    out = []
    for n, line in enumerate(text.split("\n"), 1):
        if line.startswith("#") or not line.strip():
            continue
        out.append((n, line.split("\t")))
    return out


def unescape(s):
    return s.replace("\\n", "\n")


def edits_of(path, old, new):
    """The edits one line stands for: run-css-variables.sh reads its list as Python string escapes and splits a line into several
    edits of one file at U+001F; every other runner replaces only \\n."""
    if path.endswith("CSS_VARIABLES_PLANTS"):
        olds = old.encode().decode("unicode_escape").split("\x1f")
        news = new.encode().decode("unicode_escape").split("\x1f")
        return list(zip(olds, news)) if len(olds) == len(news) else None
    return [(unescape(old), unescape(new))]


def resolve(rel):
    for d in SRC_DIRS:
        if os.path.isfile(os.path.join(ROOT, d, rel)):
            return os.path.join(d, rel)
    return None


def check_list(path, layout, text, sources, problems):
    """Check one list's text. sources maps a path (from ROOT) to its text."""
    edits = {}
    for n, f in rows(text):
        where = "%s:%d" % (path, n)
        if layout == "nfoc":
            if len(f) < 4:
                problems.append("%s: %d fields, not at least four" % (where, len(f)))
                continue
            name, rel, old, new = f[0], f[1], f[2], f[3]
            src = resolve(rel)
            if src is None:
                problems.append("%s: plant %s names %s, which is in none of %s" % (where, name, rel, ", ".join(SRC_DIRS)))
                continue
        elif layout == "html-serializer":
            if len(f) != 4:
                problems.append("%s: %d fields, not four" % (where, len(f)))
                continue
            name, old, new, src = f[0], f[2], f[3], SERIALIZER
        else:
            if len(f) != 3:
                problems.append("%s: %d fields, not three" % (where, len(f)))
                continue
            name, old, new, src = f[0], f[1], f[2], SERIALIZER
        if src not in sources:
            sources[src] = open(os.path.join(ROOT, src), encoding="utf-8", errors="replace").read()
        pairs = edits_of(path, old, new)
        if pairs is None:
            problems.append("%s: plant %s has a different number of old and new edits" % (where, name))
            continue
        for o, nw in pairs:
            if o == "":
                problems.append("%s: plant %s has an empty anchor" % (where, name))
                continue
            count = sources[src].count(o)
            if count != 1:
                problems.append("%s: plant %s: its text is in %s %d times, not once" % (where, name, src, count))
            if o == nw:
                problems.append("%s: plant %s replaces its text with itself" % (where, name))
        if name in edits and edits[name] != src and path not in MULTIFILE:
            problems.append("%s: plant %s edits two files (%s and %s)" % (where, name, edits[name], src))
        edits.setdefault(name, src)
    return set(edits)


def check_names(path, text, known, problems):
    for n, f in rows(text):
        if f[0] not in known:
            problems.append("%s:%d: names the plant %s, which is in none of its lists" % (path, n, f[0]))


def run(overrides=None):
    """Check every list; overrides maps a list path to replacement text (the controls). Returns the problems."""
    overrides = overrides or {}
    problems = []
    sources = {}
    names = {}
    for path, layout in LISTS:
        text = overrides.get(path)
        if text is None:
            text = open(os.path.join(ROOT, path), encoding="utf-8").read()
        names[path] = check_list(path, layout, text, sources, problems)
    for path, of in NAME_LISTS:
        text = overrides.get(path)
        if text is None:
            text = open(os.path.join(ROOT, path), encoding="utf-8").read()
        known = set()
        for o in of:
            known |= names[o]
        check_names(path, text, known, problems)
    return problems


def controls():
    """Each mistake made in memory must be reported."""
    lists = {p: open(os.path.join(ROOT, p), encoding="utf-8").read() for p, _ in LISTS}
    failures = []

    def expect(label, overrides):
        if not run(overrides):
            failures.append(label)

    first = "tools/conformance/CSS_CSSOM_PLANTS"
    lines = lists[first].split("\n")
    idx = next(i for i, l in enumerate(lines) if l and not l.startswith("#"))
    f = lines[idx].split("\t")
    # an anchor that matches nothing
    g = list(f); g[2] = "this text is not in any source file at all"
    expect("zero-match anchor", {first: "\n".join(lines[:idx] + ["\t".join(g)] + lines[idx + 1:])})
    # an anchor that matches twice (a single token that occurs everywhere)
    g = list(f); g[2] = "static"
    expect("two-match anchor", {first: "\n".join(lines[:idx] + ["\t".join(g)] + lines[idx + 1:])})
    # an edit that changes nothing
    g = list(f); g[3] = g[2]
    expect("no-op edit", {first: "\n".join(lines[:idx] + ["\t".join(g)] + lines[idx + 1:])})
    # a file that does not exist
    g = list(f); g[1] = "no_such_file.c"
    expect("missing file", {first: "\n".join(lines[:idx] + ["\t".join(g)] + lines[idx + 1:])})
    # an empty anchor
    g = list(f); g[2] = ""
    expect("empty anchor", {first: "\n".join(lines[:idx] + ["\t".join(g)] + lines[idx + 1:])})
    # one name editing two files
    other = next(l for l in lines[idx + 1:] if l and not l.startswith("#") and l.split("\t")[1] != f[1])
    h = other.split("\t"); h[0] = f[0]
    expect("one plant, two files", {first: "\n".join(lines + ["\t".join(h)])})
    # the serializer list's old text and the serializer-oracle list's
    for p, layout in LISTS:
        if layout != "nfoc":
            ls = lists[p].split("\n")
            i = next(i for i, l in enumerate(ls) if l and not l.startswith("#"))
            ff = ls[i].split("\t")
            col = 2 if layout == "html-serializer" else 1
            ff[col] = "this text is not in any source file at all"
            expect("zero-match anchor in " + p, {p: "\n".join(ls[:i] + ["\t".join(ff)] + ls[i + 1:])})
    # a name in an oracle list that no plant list has
    p = "tools/oracle/CSS_COND_ORACLE_PLANTS"
    t = open(os.path.join(ROOT, p), encoding="utf-8").read()
    expect("unknown plant name", {p: t + "no-such-plant\trule\n"})
    return failures


def main(argv):
    if "--controls" in argv:
        failures = controls()
        for label in failures:
            sys.stderr.write("control NOT CAUGHT: %s\n" % label)
        if failures:
            return 1
        print("plant anchors: every control caught")
        return 0
    problems = run()
    for p in problems:
        sys.stderr.write(p + "\n")
    if problems:
        sys.stderr.write("plant anchors: %d problems\n" % len(problems))
        return 1
    total = sum(len(rows(open(os.path.join(ROOT, p), encoding="utf-8").read())) for p, _ in LISTS)
    print("plant anchors: %d edits in %d lists apply exactly once" % (total, len(LISTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
