#!/usr/bin/env python3
"""Generate the CSSOM tables: the IDL attribute names of CSSStyleDeclaration, and what CSSOM does with each kind of rule.

CSSOM defines the attributes of ``CSSStyleDeclaration`` for every supported CSS property by an algorithm ("CSS property to IDL attribute"): a camel-cased
attribute for every property (``font-size`` is ``fontSize``, ``-webkit-transform`` is ``WebkitTransform``), a dashed attribute for every property that has a hyphen,
a webkit-cased attribute (``webkitTransform``) for every property that begins ``-webkit-``, and ``cssFloat`` for ``float``. This script runs that algorithm over the
properties of the registry (the pin of tools/css/WEBREF_COMMIT, aliases included, custom property patterns not) and writes

  * ``tools/css/cssom_idl_names.tsv``: ``attribute``, ``property``, ``kind`` (camel, dashed, webkit, special), one row per attribute, in the byte order of the attribute;
  * ``src/css/cssom_tables.h``: the same as constant data for the run-time lookup (nothing is computed at run time but the lookup).

The second table is the kinds of rule. ``tools/css/cssom-supplement.tsv`` has a row for each of the 29 kinds (the 27 of ``css_atrule.h``, ``UNKNOWN`` included, and the
style rule and the nested-declarations rule); this script checks it against the enumeration of ``css_atrule.h`` and the ledger ``tools/css/atrule_kinds.tsv`` (a kind with no
row, a row that is not a kind, a duplicate, a value outside its domain, a kind that is not a rule yet insertable, an insertable at-rule that no place allows) and writes
``src/css/cssom_kind_tables.h``, in the order of the enumeration, with the places each at-rule may be (the ledger's ``parents``) beside it.

The third table is the resolved values (CSS Object Model, section 9). ``tools/css/resolved_value.tsv`` has a row for each longhand whose resolved value is not its computed value (and
a few that say the computed value is the used one), each with the category (the ``CSS_RESOLVED_*`` names of ``src/css/cssom_computed_internal.h``) and the sentence of the served text
that says so; this script checks it against the properties of the pin (a row must name a longhand, once) and the enumeration, and writes ``src/css/cssom_resolved_tables.h`` sorted by
name. With ``--served DIR`` it also requires every sentence to be in ``DIR/<source>.html`` (whitespace and markup aside): **a row whose sentence the served text does not contain does
not exist**.

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
KINDS_HEADER_DEFAULT = os.path.join(ROOT, "src", "css", "cssom_kind_tables.h")
SUPPLEMENT_DEFAULT = os.path.join(HERE, "cssom-supplement.tsv")
LEDGER_DEFAULT = os.path.join(HERE, "atrule_kinds.tsv")
ATRULE_HEADER = os.path.join(ROOT, "include", "ghoti.io", "text", "css", "css_atrule.h")
RESOLVED_DEFAULT = os.path.join(HERE, "resolved_value.tsv")
RESOLVED_HEADER_DEFAULT = os.path.join(ROOT, "src", "css", "cssom_resolved_tables.h")
COMPUTED_INTERNAL = os.path.join(ROOT, "src", "css", "cssom_computed_internal.h")

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


def load_longhands():
    """The longhands of the registry: not an alias, not a shorthand, not a custom property."""
    spec = importlib.util.spec_from_file_location("gen_css_tables", os.path.join(HERE, "gen-css-tables.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, HERE)
    spec.loader.exec_module(mod)
    model = mod.wm.load(mod.wm.DATA, None)
    built = mod.build(model)
    return {n for n, p in built["props"].items() if not n.startswith("--") and not p["alias_of"] and not p["longhands"]}


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


# ---- the kinds of rule ------------------------------------------------------------------------------------------------------------------------

EXTRA_KINDS = ["style", "nested-declarations"]
NOT_IN_LEDGER = {"unknown", "keyframe", "feature-block"}   # kinds of the enumeration that are no row of the ledger of at-rules: the parse makes them
LIST_DOMAIN = ("none", "rules", "keyframes", "margins")
STYLE_DOMAIN = ("none", "properties", "descriptors")
PARENT_BITS = {"top": 1, "group": 2, "style": 4, "page": 8, "mixin": 16}
FLAG_RULE, FLAG_LIST_RULES, FLAG_LIST_KEYFRAMES, FLAG_LIST_MARGINS, FLAG_STYLE_PROPERTIES, FLAG_STYLE_DESCRIPTORS, FLAG_INSERTABLE = 1, 2, 4, 8, 16, 32, 64


def enum_kinds(header_text):
    """The kinds of css_atrule.h in the order of the enumeration, as the names gtext_css_atrule_kind_name() gives."""
    import re
    m = re.search(r"typedef enum \{(.*?)\} GTEXT_CSS_Atrule_Kind;", header_text, re.S)
    if not m:
        raise SystemExit("gen-cssom-tables: the enumeration GTEXT_CSS_Atrule_Kind is not in css_atrule.h")
    names = re.findall(r"GTEXT_CSS_ATRULE_([A-Z_]+)\s*(?:=\s*\d+)?\s*,", m.group(1) + ",")
    return [n.lower().replace("_", "-") for n in names if n != "kind-count" and n != "KIND_COUNT"]


def read_tsv(text):
    rows = []
    for line in text.split("\n"):
        if not line.strip() or line.startswith("#"):
            continue
        rows.append(line.split("\t"))
    return rows


def derive_kinds(supplement_text, ledger_text, header_text):
    """The rows of the kinds table in order, and the problems found."""
    problems = []
    order = enum_kinds(header_text) + EXTRA_KINDS
    parents = {}
    for r in read_tsv(ledger_text):
        if len(r) < 3:
            continue
        kind = r[1].lower().replace("_", "-")
        mask = 0
        for part in r[2].split(","):
            if part not in PARENT_BITS:
                problems.append("the ledger gives %s a parent %r the table does not know" % (r[0], part))
            else:
                mask |= PARENT_BITS[part]
        parents[kind] = parents.get(kind, 0) | mask
    rows = {}
    for r in read_tsv(supplement_text):
        if len(r) != 8:
            problems.append("a row of cssom-supplement.tsv has %d columns, not 8: %s" % (len(r), r[0] if r else "?"))
            continue
        name, interface, typ, lst, style, rule, insertable, note = r
        if name in rows:
            problems.append("the kind %s has two rows" % name)
            continue
        if name not in order:
            problems.append("the row %s is not a kind: css_atrule.h has no such kind and it is not %s" % (name, " or ".join(EXTRA_KINDS)))
            continue
        if lst not in LIST_DOMAIN:
            problems.append("%s: list is %r, not one of %s" % (name, lst, ", ".join(LIST_DOMAIN)))
        if style not in STYLE_DOMAIN:
            problems.append("%s: style is %r, not one of %s" % (name, style, ", ".join(STYLE_DOMAIN)))
        if rule not in ("yes", "no") or insertable not in ("yes", "no"):
            problems.append("%s: rule and insertable are yes or no" % name)
        if not typ.isdigit():
            problems.append("%s: the legacy type %r is not a number" % (name, typ))
        if rule == "no" and (insertable == "yes" or lst != "none" or style != "none"):
            problems.append("%s is not a rule and yet it is insertable or has a list or a block" % name)
        rows[name] = (interface, int(typ) if typ.isdigit() else 0, lst, style, rule == "yes", insertable == "yes")
    for k in order:
        if k not in rows:
            problems.append("the kind %s has no row in cssom-supplement.tsv" % k)
    for k in parents:
        if k not in order:
            problems.append("the ledger has a kind %s that css_atrule.h does not" % k)
    for k in order:
        if k in EXTRA_KINDS or k in NOT_IN_LEDGER:
            continue
        if k not in parents:
            problems.append("the kind %s of css_atrule.h has no row in the ledger atrule_kinds.tsv" % k)
    types = {}
    for k, v in rows.items():
        if v[1]:
            if v[1] in types:
                problems.append("the legacy type %d is both %s and %s" % (v[1], types[v[1]], k))
            types[v[1]] = k
        if v[5] and k in parents and not parents[k] & (PARENT_BITS["top"] | PARENT_BITS["group"]):
            problems.append("%s is insertable and the ledger lets it be neither at the top nor in a group" % k)
        if v[2] != "none" and not v[4]:
            problems.append("%s has a list and is not a rule" % k)
    out = []
    for k in order:
        if k not in rows:
            continue
        interface, typ, lst, style, rule, insertable = rows[k]
        flags = 0
        if rule:
            flags |= FLAG_RULE
        flags |= {"none": 0, "rules": FLAG_LIST_RULES, "keyframes": FLAG_LIST_KEYFRAMES, "margins": FLAG_LIST_MARGINS}[lst] if lst in LIST_DOMAIN else 0
        flags |= {"none": 0, "properties": FLAG_STYLE_PROPERTIES, "descriptors": FLAG_STYLE_DESCRIPTORS}[style] if style in STYLE_DOMAIN else 0
        if insertable:
            flags |= FLAG_INSERTABLE
        out.append((k, "" if interface == "-" else interface, typ, flags, parents.get(k, 0)))
    return out, problems


def kinds_header_text(rows):
    out = [LICENSE, """
/**
 * @file
 *
 * GENERATED by tools/css/gen-cssom-tables.py from tools/css/cssom-supplement.tsv, checked against css_atrule.h and tools/css/atrule_kinds.tsv. Do not edit.
 *
 * What CSSOM does with each kind of rule (css_cssom_rule.h), in the order of GTEXT_CSSOM_Rule_Kind: the table `cssom_kind_rows` is indexed by the kind.
 * Regenerate with:  tools/css/gen-cssom-tables.py
 * Verify with:      make check-cssom
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_KIND_TABLES_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_KIND_TABLES_H

#include <ghoti.io/text/macros.h>

#define CSSOM_KIND_RULE 1u              /* a parse makes a rule object of it */
#define CSSOM_KIND_LIST_RULES 2u        /* its cssRules holds style, nested-declarations and at-rules */
#define CSSOM_KIND_LIST_KEYFRAMES 4u    /* its cssRules holds keyframe rules */
#define CSSOM_KIND_LIST_MARGINS 8u      /* its cssRules holds margin rules */
#define CSSOM_KIND_STYLE_PROPERTIES 16u /* its style is a block of property declarations */
#define CSSOM_KIND_STYLE_DESCRIPTORS 32u/* its style is a block of descriptors */
#define CSSOM_KIND_INSERTABLE 64u       /* insertRule can make one at the top level or in a group */

#define CSSOM_PARENT_TOP 1u             /* the places an at-rule may be: the ledger's parents */
#define CSSOM_PARENT_GROUP 2u
#define CSSOM_PARENT_STYLE 4u
#define CSSOM_PARENT_PAGE 8u
#define CSSOM_PARENT_MIXIN 16u

typedef struct {
  const char * name;
  const char * iface;
  unsigned legacy_type;
  unsigned flags;
  unsigned parents;
} cssom_kind_row;

#define CSSOM_KIND_ROW_COUNT %d
static const cssom_kind_row cssom_kind_rows[CSSOM_KIND_ROW_COUNT] = {
""" % len(rows)]
    for name, interface, typ, flags, parents in rows:
        out.append('  {"%s", "%s", %d, %du, %du},\n' % (name, interface, typ, flags, parents))
    out.append("};\n\n#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_KIND_TABLES_H\n")
    return "".join(out)


# ---- the resolved values ----------------------------------------------------------------------------------------------------------------------------

def enum_resolved(header_text):
    """The categories of cssom_computed_internal.h in the order of the enumeration, as the names the table uses (lower case, without CSS_RESOLVED_)."""
    import re
    m = re.search(r"typedef enum \{(.*?)\} css_resolved_category;", header_text, re.S)
    if not m:
        raise SystemExit("gen-cssom-tables: the enumeration css_resolved_category is not in cssom_computed_internal.h")
    names = re.findall(r"CSS_RESOLVED_([A-Z_]+)\s*(?:=\s*\d+)?\s*,", m.group(1) + ",")
    return [n.lower() for n in names if n != "count"]


def squeeze(text):
    """A text with every white space dropped, for the comparison of a quoted sentence with the served text."""
    import re
    return re.sub(r"\s+", "", text)


def served_text(directory, source, cache={}):
    """The text of DIR/SOURCE.html with the markup and the character references gone and the white space dropped; None if the file is not there."""
    import html
    import re
    path = os.path.join(directory, source + ".html")
    if path in cache:
        return cache[path]
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read()
    except OSError:
        cache[path] = None
        return None
    raw = re.sub(r"<script.*?</script>|<style.*?</style>", "", raw, flags=re.S)
    raw = re.sub(r"<[^>]+>", " ", raw)
    cache[path] = squeeze(html.unescape(raw))
    return cache[path]


def derive_resolved(tsv, longhands, enum_names, served):
    """The rows (property, category number) sorted by the bytes of the name, and the problems found."""
    problems = []
    rows = {}
    for r in read_tsv(tsv):
        if len(r) != 4:
            problems.append("a row of resolved_value.tsv has %d columns, not 4: %s" % (len(r), r[0] if r else "?"))
            continue
        name, category, source, sentence = r
        if name in rows:
            problems.append("the property %s has two rows" % name)
            continue
        if name not in longhands:
            problems.append("%s is not a longhand of the registry" % name)
            continue
        if category not in enum_names:
            problems.append("%s: the category %r is not one of %s" % (name, category, ", ".join(enum_names)))
            continue
        if not sentence.strip() or not source.strip():
            problems.append("%s: a row quotes a sentence and names its source" % name)
            continue
        if served is not None:
            text = served_text(served, source)
            if text is None:
                problems.append("%s: the served text %s.html is not in %s" % (name, source, served))
            elif squeeze(sentence) not in text:
                problems.append("%s: the served text %s does not contain the sentence: %s" % (name, source, sentence))
        rows[name] = enum_names.index(category)
    return [(n, rows[n]) for n in sorted(rows, key=lambda x: x.encode("utf-8"))], problems


def resolved_header_text(rows, names):
    out = [LICENSE, """
/**
 * @file
 *
 * GENERATED by tools/css/gen-cssom-tables.py from tools/css/resolved_value.tsv, checked against the properties of the registry and the categories of cssom_computed_internal.h. Do not edit.
 *
 * The longhands whose resolved value (CSS Object Model, section 9) is not their computed value, sorted by the bytes of the name, for a binary search; the category is the number of
 * the enumerator of css_resolved_category. A longhand with no row is the computed value.
 * Regenerate with:  tools/css/gen-cssom-tables.py
 * Verify with:      make check-cssom
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_RESOLVED_TABLES_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_RESOLVED_TABLES_H

#include <ghoti.io/text/macros.h>

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char * name;
  size_t length;
  uint8_t category;   /* %s */
} css_resolved_row;

#define CSS_RESOLVED_ROW_COUNT %d
static const css_resolved_row css_resolved_rows[CSS_RESOLVED_ROW_COUNT] = {
""" % (", ".join("%d %s" % (i, n) for i, n in enumerate(names)), len(rows))]
    for name, cat in rows:
        out.append('  {"%s", %d, %d},\n' % (name, len(name), cat))
    out.append("};\n\n#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_RESOLVED_TABLES_H\n")
    return "".join(out)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


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
    kinds, kind_problems = derive_kinds(read(args.supplement), read(args.ledger), read(ATRULE_HEADER))
    problems += kind_problems
    resolved, resolved_problems = derive_resolved(read(args.resolved), load_longhands(), enum_resolved(read(COMPUTED_INTERNAL)), args.served)
    problems += resolved_problems
    resolved_header = resolved_header_text(resolved, enum_resolved(read(COMPUTED_INTERNAL)))
    tsv = tsv_text(rows, commit)
    header = header_text(rows, commit)
    kinds_header = kinds_header_text(kinds)
    if args.check:
        for path, text in ((args.tsv, tsv), (args.header, header), (args.kinds_header, kinds_header), (args.resolved_header, resolved_header)):
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
        print("check-cssom: %d attributes of %d properties, %d kinds of rule, %d resolved-value rows%s, the tables are current and agree with the pin and the ledger" %
              (len(rows), len(props), len(kinds), len(resolved), " (every sentence found in the served text)" if args.served else ""))
        return 0
    if problems:
        for p in problems:
            print("gen-cssom-tables: " + p, file=sys.stderr)
        return 1
    with open(args.tsv, "w", encoding="utf-8", newline="\n") as f:
        f.write(tsv)
    with open(args.header, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    with open(args.kinds_header, "w", encoding="utf-8", newline="\n") as f:
        f.write(kinds_header)
    with open(args.resolved_header, "w", encoding="utf-8", newline="\n") as f:
        f.write(resolved_header)
    print("wrote %d attributes to %s and %s, and %d kinds to %s" % (len(rows), os.path.relpath(args.tsv, ROOT), os.path.relpath(args.header, ROOT), len(kinds),
                                                                     os.path.relpath(args.kinds_header, ROOT)))
    print("wrote %d resolved-value rows to %s" % (len(resolved), os.path.relpath(args.resolved_header, ROOT)))
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
    supp, ledger, atheader = read(SUPPLEMENT_DEFAULT), read(LEDGER_DEFAULT), read(ATRULE_HEADER)
    _, base_kinds = derive_kinds(supp, ledger, atheader)
    print("control %-58s %s" % ("the unedited kinds", "ok" if not base_kinds else "NOT OK " + str(base_kinds[:3])))
    failed = failed or bool(base_kinds)

    def attempt_kinds(label, mutate, expect):
        nonlocal failed
        s, l, h = supp, ledger, atheader
        s, l, h = mutate(s, l, h)
        _, problems = derive_kinds(s, l, h)
        hit = any(expect in x for x in problems)
        print("control %-58s %s" % (label, "ok" if hit else "NOT SEEN"))
        failed = failed or not hit

    def drop_row(name):
        return lambda s, l, h: ("\n".join(x for x in s.split("\n") if not x.startswith(name + "\t")), l, h)

    attempt_kinds("a kind with no row", drop_row("media"), "the kind media has no row")
    attempt_kinds("a row that is not a kind", lambda s, l, h: (s + "bogus\t-\t0\tnone\tnone\tyes\tno\tx\n", l, h), "the row bogus is not a kind")
    attempt_kinds("a kind with two rows", lambda s, l, h: (s + "media\tCSSMediaRule\t4\trules\tnone\tyes\tyes\tx\n", l, h), "the kind media has two rows")
    attempt_kinds("a list outside its domain", lambda s, l, h: (s.replace("media\tCSSMediaRule\t4\trules", "media\tCSSMediaRule\t4\tstuff"), l, h), "list is 'stuff'")
    attempt_kinds("a kind that is not a rule and is insertable", lambda s, l, h: (s.replace("charset\t-\t0\tnone\tnone\tno\tno", "charset\t-\t0\tnone\tnone\tno\tyes"), l, h), "is not a rule and yet")
    attempt_kinds("a legacy type given twice", lambda s, l, h: (s.replace("namespace\tCSSNamespaceRule\t10", "namespace\tCSSNamespaceRule\t3"), l, h), "the legacy type 3 is both")
    attempt_kinds("a kind of the ledger that css_atrule.h lacks", lambda s, l, h: (s, l + "frobnicate\tFROBNICATE\ttop\t1\t1\t1\t1\t1\t0\tx\n", h), "the ledger has a kind frobnicate")
    attempt_kinds("a kind of css_atrule.h the ledger lacks", lambda s, l, h: (s, "\n".join(x for x in l.split("\n") if not x.startswith("media\t")), h), "the kind media of css_atrule.h has no row")
    attempt_kinds("an insertable at-rule no place allows", lambda s, l, h: (s, l.replace("font-face\tFONT_FACE\ttop,group", "font-face\tFONT_FACE\tmixin"), h), "is insertable and the ledger lets it be")
    # the resolved values
    longhands = load_longhands()
    names = enum_resolved(read(COMPUTED_INTERNAL))
    rv = read(args.resolved)
    _, base_rv = derive_resolved(rv, longhands, names, args.served)
    print("control %-58s %s" % ("the unedited resolved values", "ok" if not base_rv else "NOT OK " + str(base_rv[:3])))
    failed = failed or bool(base_rv)

    def attempt_rv(label, mutate, expect, served=args.served):
        nonlocal failed
        _, problems = derive_resolved(mutate(rv), longhands, names, served)
        hit = any(expect in x for x in problems)
        print("control %-58s %s" % (label, "ok" if hit else "NOT SEEN"))
        failed = failed or not hit

    attempt_rv("a row for a shorthand", lambda s: s + "margin\tcomputed\tcssom\tx\n", "margin is not a longhand")
    attempt_rv("a row for a property that is not one", lambda s: s + "bogus\tcomputed\tcssom\tx\n", "bogus is not a longhand")
    attempt_rv("a property with two rows", lambda s: s + "width\tused_if_applies\tcssom\tx\n", "the property width has two rows")
    attempt_rv("a category that is not one", lambda s: s.replace("height\tused_if_applies", "height\tuse_it", 1), "the category 'use_it'")
    attempt_rv("a row with no sentence", lambda s: s + "zoom\tcomputed\tcssom\t \n", "quotes a sentence")
    if args.served:
        attempt_rv("a sentence the served text does not contain", lambda s: s.replace("The resolved value is the used value.", "The resolved value is always the used value.", 1), "does not contain the sentence")
        attempt_rv("a source that is not served", lambda s: s.replace("\tcssom\t", "\tcssom-9\t", 1), "is not in")
    # a hand edit of a generated file
    tmp = tempfile.mkdtemp(prefix="cssomctl")
    try:
        a = argparse.Namespace(**vars(args))
        a.tsv = os.path.join(tmp, "n.tsv")
        a.header = os.path.join(tmp, "h.h")
        a.kinds_header = os.path.join(tmp, "k.h")
        a.resolved_header = os.path.join(tmp, "r.h")
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
    ap.add_argument("--kinds-header", dest="kinds_header", default=KINDS_HEADER_DEFAULT)
    ap.add_argument("--resolved", default=RESOLVED_DEFAULT)
    ap.add_argument("--resolved-header", dest="resolved_header", default=RESOLVED_HEADER_DEFAULT)
    ap.add_argument("--served", default=None, help="a directory of the served documents (<source>.html): every sentence of the resolved-value table must be in its source")
    ap.add_argument("--supplement", default=SUPPLEMENT_DEFAULT)
    ap.add_argument("--ledger", default=LEDGER_DEFAULT)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--controls", action="store_true")
    args = ap.parse_args()
    if args.controls:
        return controls(args)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
