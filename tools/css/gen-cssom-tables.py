#!/usr/bin/env python3
"""Generate the CSSOM tables: the IDL attribute names of CSSStyleDeclaration.

CSSOM defines the attributes of ``CSSStyleDeclaration`` for every supported CSS property by an algorithm ("CSS property to IDL attribute"): a camel-cased
attribute for every property (``font-size`` is ``fontSize``, ``-webkit-transform`` is ``WebkitTransform``), a dashed attribute for every property that has a hyphen,
a webkit-cased attribute (``webkitTransform``) for every property that begins ``-webkit-``, and ``cssFloat`` for ``float``. This script runs that algorithm over the
properties of the registry (the pin of tools/css/WEBREF_COMMIT, aliases included, custom property patterns not) and writes

  * ``tools/css/cssom_idl_names.tsv``: ``attribute``, ``property``, ``kind`` (camel, dashed, webkit, special), one row per attribute, in the byte order of the attribute;
  * ``src/css/cssom_tables.h``: the same as constant data for the run-time lookup (nothing is computed at run time but the lookup).

It fails (exit 1, every problem listed) when a property has no camel-cased attribute or more than one, two properties share an attribute, a dashed attribute is not the
property's name, the names the pin itself lists for a property (``styleDeclaration``) are not exactly the attributes this derives (the algorithm and the pin must agree),
or (``--check``) a committed file is not what this writes. ``--controls`` makes each mistake in a copy and requires the report.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TSV_DEFAULT = os.path.join(HERE, "cssom_idl_names.tsv")
HEADER_DEFAULT = os.path.join(ROOT, "src", "css", "cssom_tables.h")

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


def load_pin():
    """The properties of the registry and the attribute names the pin lists for each: {name: [styleDeclaration...]}."""
    spec = importlib.util.spec_from_file_location("gen_css_tables", os.path.join(HERE, "gen-css-tables.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, HERE)
    spec.loader.exec_module(mod)
    model = mod.wm.load(mod.wm.DATA, None)
    built = mod.build(model)
    props = {n: list(p["style"]) for n, p in built["props"].items() if not n.startswith("--")}
    commit = open(os.path.join(HERE, "WEBREF_COMMIT")).read().strip()
    return props, commit


def css_property_to_idl_attribute(prop, lowercase_first=False):
    """CSSOM, "CSS property to IDL attribute": a hyphen makes the next character upper case (ASCII only); with the flag the first character is removed first."""
    out = []
    uppercase_next = False
    text = prop[1:] if lowercase_first else prop
    for c in text:
        if c == "-":
            uppercase_next = True
        elif uppercase_next:
            uppercase_next = False
            out.append(c.upper() if "a" <= c <= "z" else c)
        else:
            out.append(c)
    return "".join(out)


def derive(props):
    """The rows (attribute, property, kind), the pin's lists the attributes must equal, and the problems found."""
    rows = {}
    problems = []
    per_property = {}

    def add(attr, prop, kind):
        if attr in rows and rows[attr][0] != prop:
            problems.append("the attribute %s is both %s and %s" % (attr, rows[attr][0], prop))
            return
        if attr in rows:
            problems.append("the attribute %s is derived twice for %s" % (attr, prop))
            return
        rows[attr] = (prop, kind)
        per_property.setdefault(prop, []).append((attr, kind))

    for prop in sorted(props):
        add(css_property_to_idl_attribute(prop), prop, "camel")
        if "-" in prop:
            add(prop, prop, "dashed")
        if prop.startswith("-webkit-"):
            add(css_property_to_idl_attribute(prop, True), prop, "webkit")
        if prop == "float":
            add("cssFloat", prop, "special")
    for prop in sorted(props):
        camels = [a for a, k in per_property.get(prop, []) if k == "camel"]
        if len(camels) != 1:
            problems.append("%s has %d camel-cased attributes, not one" % (prop, len(camels)))
        for a, k in per_property.get(prop, []):
            if k == "dashed" and a != prop:
                problems.append("the dashed attribute of %s is %s" % (prop, a))
        # the pin's own list for the property is the same attributes, and no others
        mine = sorted(a for a, k in per_property.get(prop, []) if k != "special")
        pin = sorted(props[prop])
        if mine != pin:
            problems.append("%s: the pin lists %s and the algorithm gives %s" % (prop, pin, mine))
    return rows, problems


def tsv_text(rows, commit):
    lines = ["# tools/css/gen-cssom-tables.py: the IDL attributes of CSSStyleDeclaration (CSSOM, \"CSS property to IDL attribute\") for the properties of the registry",
             "# (the pin of tools/css/WEBREF_COMMIT, %s). Columns, tab separated: attribute, property, kind (camel, dashed, webkit, special)." % commit,
             "# Generated: do not edit; `make check-cssom` regenerates it and fails on a difference.", "attribute\tproperty\tkind"]
    for attr in sorted(rows, key=lambda a: a.encode("utf-8")):
        lines.append("%s\t%s\t%s" % (attr, rows[attr][0], rows[attr][1]))
    return "\n".join(lines) + "\n"


def header_text(rows, commit):
    out = [LICENSE, """
/**
 * @file
 *
 * GENERATED by tools/css/gen-cssom-tables.py from the properties of the registry (w3c/webref commit %s). Do not edit.
 *
 * The IDL attributes of CSSStyleDeclaration (css_cssom_decl.h), sorted by the bytes of the attribute, for a binary search.
 * Regenerate with:  tools/css/gen-cssom-tables.py
 * Verify with:      make check-cssom
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_TABLES_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_TABLES_H

#include <ghoti.io/text/macros.h>

#include <stddef.h>

typedef struct {
  const char * attribute;
  const char * property;
} cssom_idl_row;

static const size_t cssom_idl_row_count = %d;
static const cssom_idl_row cssom_idl_rows[%d] = {
""" % (commit, len(rows), len(rows))]
    for attr in sorted(rows, key=lambda a: a.encode("utf-8")):
        out.append('  {"%s", "%s"},\n' % (attr, rows[attr][0]))
    out.append("};\n\n#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_TABLES_H\n")
    return "".join(out)


def run(args):
    props, commit = load_pin()
    rows, problems = derive(props)
    tsv = tsv_text(rows, commit)
    header = header_text(rows, commit)
    if args.check:
        for path, text in ((args.tsv, tsv), (args.header, header)):
            try:
                with open(path, encoding="utf-8") as f:
                    have = f.read()
            except OSError:
                have = None
            if have != text:
                problems.append("%s is not what the generator writes" % os.path.relpath(path, ROOT))
        if problems:
            for p in problems:
                print("gen-cssom-tables: " + p, file=sys.stderr)
            return 1
        print("check-cssom: %d attributes of %d properties, the tables are current and agree with the pin" % (len(rows), len(props)))
        return 0
    if problems:
        for p in problems:
            print("gen-cssom-tables: " + p, file=sys.stderr)
        return 1
    with open(args.tsv, "w", encoding="utf-8", newline="\n") as f:
        f.write(tsv)
    with open(args.header, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print("wrote %d attributes to %s and %s" % (len(rows), os.path.relpath(args.tsv, ROOT), os.path.relpath(args.header, ROOT)))
    return 0


def controls(args):
    """Each mistake is made in a copy of the input, and the gate must name it."""
    props, commit = load_pin()
    failed = False

    def attempt(label, mutate, expect):
        nonlocal failed
        p = {k: list(v) for k, v in props.items()}
        mutate(p)
        _, problems = derive(p)
        hit = any(expect in x for x in problems)
        print("control %-58s %s" % (label, "ok" if hit else "NOT SEEN"))
        failed = failed or not hit

    _, base = derive(props)
    print("control %-58s %s" % ("the unedited input", "ok" if not base else "NOT OK " + str(base[:3])))
    failed = failed or bool(base)
    attempt("a property whose pin list lacks an attribute", lambda p: p["font-size"].remove("fontSize"), "font-size: the pin lists")
    attempt("a property whose pin list has an attribute too many", lambda p: p["font-size"].append("fontSizes"), "font-size: the pin lists")
    attempt("a property with a name that collides with another's attribute", lambda p: p.update({"fontSize": ["fontSize"]}), "is both")
    attempt("a property whose camel-cased name is the other's", lambda p: p.update({"font--size": ["fontSize", "font--size"]}), "is both")
    attempt("a webkit property without its webkit-cased attribute", lambda p: p["-webkit-transform"].remove("webkitTransform"), "-webkit-transform: the pin lists")
    # a hand edit of a generated file
    tmp = tempfile.mkdtemp(prefix="cssomctl")
    try:
        a = argparse.Namespace(**vars(args))
        a.tsv = os.path.join(tmp, "n.tsv")
        a.header = os.path.join(tmp, "h.h")
        a.check = False
        run(a)
        with open(a.tsv, encoding="utf-8") as f:
            t = f.read()
        with open(a.tsv, "w", encoding="utf-8") as f:
            f.write(t.replace("fontSize\tfont-size\tcamel", "fontSizes\tfont-size\tcamel"))
        a.check = True
        import io
        import contextlib
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = run(a)
        hit = rc == 1 and "is not what the generator writes" in err.getvalue()
        print("control %-58s %s" % ("a hand edit of a generated file", "ok" if hit else "NOT SEEN"))
        failed = failed or not hit
    finally:
        for f in os.listdir(tmp):
            os.remove(os.path.join(tmp, f))
        os.rmdir(tmp)
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tsv", default=TSV_DEFAULT)
    ap.add_argument("--header", default=HEADER_DEFAULT)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--controls", action="store_true")
    args = ap.parse_args()
    if args.controls:
        return controls(args)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
