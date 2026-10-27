#!/usr/bin/env python3
"""Generate the SVG presentation-attribute table of the CSS layer from tools/css/svg_presentation.tsv.

Writes ``src/css/css_presentation_table.c``: one row for each attribute (the attribute, the property it sets, whether it needs the SVG
namespace, the elements it applies to, how its value is spelled), in the order of the file.

It fails (exit 1, every problem listed) when

  * a row does not have five fields, an attribute is there twice for the same element, a namespace or a syntax is not one of the closed set,
    an element list is empty or has an empty name;
  * a property of the table is not in svg_presentation.txt (the properties that must be computable), or one there has no attribute;
  * (``--check``) the committed table is not what this writes;
  * (``--registry FILE``, a tsv of the registry's names) a property is not a property of the pin.

``--controls`` shows each of those fail: it makes the mistake in a copy of the input and requires the report.

Usage:
    gen-presentation-table.py [--out FILE] [--check] [--controls]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT_DEFAULT = os.path.join(ROOT, "src", "css", "css_presentation_table.c")

NAMESPACES = {"svg": "true", "*": "false"}
SYNTAXES = {"css": "GTEXT_CSS_PRESENTATION_CSS", "transform": "GTEXT_CSS_PRESENTATION_TRANSFORM", "path": "GTEXT_CSS_PRESENTATION_PATH"}

LICENSE = """/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
"""


def read_rows(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            rows.append((n, line.split("\t")))
    return rows


def read_names(path):
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def check(rows, must_compute, registry=None):
    problems = []
    seen = {}
    props = set()
    for n, f in rows:
        if len(f) != 5:
            problems.append("svg_presentation.tsv:%d: %d fields, not five (attribute, property, namespace, elements, syntax)" % (n, len(f)))
            continue
        attribute, prop, ns, elements, syntax = f
        if ns not in NAMESPACES:
            problems.append("svg_presentation.tsv:%d: the namespace %r is not svg or *" % (n, ns))
        if syntax not in SYNTAXES:
            problems.append("svg_presentation.tsv:%d: the syntax %r is not css, transform or path" % (n, syntax))
        names = elements[1:].split(",") if elements.startswith("!") else elements.split(",")
        if elements != "*" and (not elements or any(not x for x in names) or "*" in names or "!" in "".join(names)):
            problems.append("svg_presentation.tsv:%d: the element list %r is empty, has an empty name or mixes * with names" % (n, elements))
        if not attribute or not prop:
            problems.append("svg_presentation.tsv:%d: an empty attribute or property" % n)
        for el in (["*"] if elements == "*" or elements.startswith("!") else names):
            key = (attribute, ns, el)
            for (a2, n2, e2), line in list(seen.items()):
                if a2 == attribute and (e2 == el or e2 == "*" or el == "*"):
                    problems.append("svg_presentation.tsv:%d: %s is there twice for %s (line %d)" % (n, attribute, el, line))
            seen[key] = n
        props.add(prop)
        if registry is not None and prop not in registry:
            problems.append("svg_presentation.tsv:%d: %s is not a property of the pin" % (n, prop))
    for p in sorted(props - set(must_compute)):
        problems.append("svg_presentation.tsv: %s is not in svg_presentation.txt" % p)
    for p in sorted(set(must_compute) - props):
        problems.append("svg_presentation.txt: %s has no attribute in svg_presentation.tsv" % p)
    return problems


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(rows):
    lines = []
    for _, (attribute, prop, ns, elements, syntax) in rows:
        lines.append("  {%s, %s, %s, %s, %s}," % (cstr(attribute), cstr(prop), NAMESPACES[ns], "NULL" if elements == "*" else cstr(elements), SYNTAXES[syntax]))
    return LICENSE + """
/**
 * @file
 *
 * GENERATED by tools/css/gen-presentation-table.py from tools/css/svg_presentation.tsv (SVG 2, 6.6 "Presentation attributes"). Do not edit.
 *
 * Regenerate with:  tools/css/gen-presentation-table.py
 * Verify with:      make check-css-presentation
 */

#include <ghoti.io/text/css/css_presentation.h>

static const GTEXT_CSS_Presentation_Row gtext_css_tbl_presentation_rows[%d] = {
%s
};

const GTEXT_CSS_Presentation_Table gtext_css_tbl_presentation = {gtext_css_tbl_presentation_rows, %d};
""" % (len(rows), "\n".join(lines), len(rows))


def load(tsv, txt):
    return read_rows(tsv), read_names(txt)


def controls(tsv, txt):
    rows, must = load(tsv, txt)
    if check(rows, must):
        print("controls: the committed input does not pass; fix it first")
        return 1
    mistakes = []
    mistakes.append(("a short row", [(n, f[:4]) if i == 0 else (n, f) for i, (n, f) in enumerate(rows)], must, "not five"))
    mistakes.append(("a bad namespace", [(n, f[:2] + ["xhtml"] + f[3:]) if i == 0 else (n, f) for i, (n, f) in enumerate(rows)], must, "namespace"))
    mistakes.append(("a bad syntax", [(n, f[:4] + ["svg"]) if i == 0 else (n, f) for i, (n, f) in enumerate(rows)], must, "syntax"))
    mistakes.append(("an empty element name", [(n, f[:3] + ["circle,,ellipse"] + f[4:]) if i == 0 else (n, f) for i, (n, f) in enumerate(rows)], must, "empty"))
    mistakes.append(("a duplicate attribute", rows + [(9999, list(rows[0][1]))], must, "there twice"))
    mistakes.append(("a property that cannot be computed", rows + [(9999, ["zz", "zz-not-required", "svg", "*", "css"])], must, "not in svg_presentation.txt"))
    mistakes.append(("a property without an attribute", rows, must + ["zz-missing"], "has no attribute"))
    status = 0
    for what, r, m, word in mistakes:
        got = check(r, m)
        if any(word in p for p in got):
            print("control %-40s caught" % what)
        else:
            print("control %-40s NOT caught: %r" % (what, got))
            status = 1
    return status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT_DEFAULT)
    ap.add_argument("--check", action="store_true", help="fail unless the committed table is what this writes")
    ap.add_argument("--controls", action="store_true", help="show each check fail")
    ap.add_argument("--registry", help="a file with the names of the registry's properties, one a line")
    args = ap.parse_args()
    tsv = os.path.join(HERE, "svg_presentation.tsv")
    txt = os.path.join(HERE, "svg_presentation.txt")
    rows, must = load(tsv, txt)
    registry = set(read_names(args.registry)) if args.registry else None
    problems = check(rows, must, registry)
    if problems:
        for p in problems:
            sys.stderr.write("gen-presentation-table: %s\n" % p)
        return 1
    text = render(rows)
    status = 0
    if args.check:
        with open(args.out, encoding="utf-8") as f:
            if f.read() != text:
                sys.stderr.write("gen-presentation-table: %s is not what tools/css/gen-presentation-table.py writes from svg_presentation.tsv\n" % args.out)
                status = 1
    else:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    if args.controls:
        status |= controls(tsv, txt)
    return status


if __name__ == "__main__":
    sys.exit(main())
