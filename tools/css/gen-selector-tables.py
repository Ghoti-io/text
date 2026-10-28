#!/usr/bin/env python3
"""Generate the selector tables of the CSS layer from the pinned webref `selectors` data and tools/css/selectors-supplement.tsv.

Reads every ``selectors`` entry of ``third_party/css/webref/ed/css/*.json`` (the pin of tools/css/WEBREF_COMMIT) and the supplement, which
says for each selector what webref does not: the grammar of its argument, what may follow it, the state bits and callbacks a match asks the
caller for, its specificity rule, its dependency summary and its status in this library. It writes

  * ``tools/css/selectors.tsv``: one row per selector, the joined table (the supplement's columns and, from the pin, the specification file,
    the definition's address and the pin's own words for it) -- the reviewable form;
  * ``src/css/css_selector_tables.h``: the same rows as constant data (an enum of row numbers and an array of rows) for the parser and the
    matcher. Nothing is parsed at run time.

It fails (exit 1, every problem listed) when

  * a selector of the pin has no row of the supplement;
  * a row of the supplement names a selector the pin does not have and is not marked "extra" in its note (a stale row);
  * a row has the wrong number of fields or a value outside the closed set of its column, or names a state bit or a callback that does not exist;
  * (``--check``) the committed files are not what this writes.

``--controls`` shows each of those fail: it makes the mistake in a copy of the inputs and requires the report.

Usage:
    gen-selector-tables.py [--check] [--controls] [--webref DIR] [--supplement FILE] [--tsv FILE] [--header FILE] [--tests FILE]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import glob
import json
import os
import re
import sys
import tempfile
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WEBREF_DEFAULT = os.path.join(ROOT, "third_party", "css", "webref", "ed", "css")
SUPPLEMENT_DEFAULT = os.path.join(HERE, "selectors-supplement.tsv")
TSV_DEFAULT = os.path.join(HERE, "selectors.tsv")
HEADER_DEFAULT = os.path.join(ROOT, "src", "css", "css_selector_tables.h")
CI_DEFAULT = os.path.join(HERE, "html_ci_attributes.txt")
TESTS_DEFAULT = os.path.join(ROOT, "tests", "css_selector_rows.h")

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

KINDS = {"class": "SELK_CLASS", "element": "SELK_ELEMENT", "combinator": "SELK_COMBINATOR", "type": "SELK_TYPE"}
KIND_PREFIX = {"class": "PC", "element": "PE", "combinator": "CB", "type": "TY"}
ARGS = {
    "none": "SELARG_NONE", "ident": "SELARG_IDENT", "type": "SELARG_TYPE", "attr": "SELARG_ATTR", "compound": "SELARG_COMPOUND",
    "compound-list": "SELARG_COMPOUND_LIST", "forgiving": "SELARG_FORGIVING", "real": "SELARG_REAL", "relative": "SELARG_RELATIVE",
    "anb": "SELARG_ANB", "anb-of": "SELARG_ANB_OF", "lang": "SELARG_LANG", "dir": "SELARG_DIR", "levels": "SELARG_LEVELS",
    "state": "SELARG_STATE", "idents-comma": "SELARG_IDENTS_COMMA", "part-idents": "SELARG_PART_IDENTS",
    "name-or-star": "SELARG_NAME_OR_STAR", "pt-name": "SELARG_PT_NAME", "enum": "SELARG_ENUM",
}
SPECS = {"class": "SELSPEC_CLASS", "element": "SELSPEC_ELEMENT", "id": "SELSPEC_ID", "zero": "SELSPEC_ZERO", "max": "SELSPEC_MAX",
         "nth": "SELSPEC_NTH", "hostfn": "SELSPEC_HOSTFN", "slotted": "SELSPEC_SLOTTED", "nesting": "SELSPEC_NESTING"}
STATUSES = {"implemented": "SELS_IMPLEMENTED", "parse-only": "SELS_PARSE_ONLY", "unsupported": "SELS_UNSUPPORTED"}
FLAGS = {"ua": "SELF_UA", "tree": "SELF_TREE", "legacy": "SELF_LEGACY", "page": "SELF_PAGE"}
DEPS = ["name", "id", "class", "attr", "state", "tree", "sibling", "first-last", "nth", "empty", "has", "lang", "dir", "logical", "column",
        "scope", "host", "nesting", "pseudo"]
CALLBACKS = ["PARENT", "LOCAL_NAME", "NAMESPACE_URI", "ID", "CLASS_COUNT", "CLASS_AT", "ATTRIBUTE", "PREVIOUS_SIBLING", "NEXT_SIBLING",
             "SIBLING_INDEX", "SIBLING_COUNT", "COMPARE_ORDER", "STATE", "LANG", "DIRECTION", "FIRST_CHILD", "LAST_CHILD", "CHILD_CONTENT",
             "TREE_SCOPE", "CONTAINING_HOST", "SHADOW_SCOPE", "ASSIGNED_SLOT", "HAS_PART", "CUSTOM_STATE", "COLUMN_CELL"]
STATES = ["HOVER", "ACTIVE", "FOCUS", "FOCUS_VISIBLE", "FOCUS_WITHIN", "TARGET", "LINK", "VISITED", "CHECKED", "INDETERMINATE", "DISABLED",
          "ENABLED", "REQUIRED", "OPTIONAL", "READ_ONLY", "READ_WRITE", "PLACEHOLDER_SHOWN", "DEFAULT", "VALID", "INVALID", "IN_RANGE",
          "OUT_OF_RANGE", "FULLSCREEN", "MODAL", "OPEN", "DEFINED", "TARGET_WITHIN", "USER_VALID", "USER_INVALID", "AUTOFILL",
          "POPOVER_OPEN", "PLAYING", "PAUSED", "SEEKING", "BUFFERING", "STALLED", "MUTED", "VOLUME_LOCKED", "LOCAL_LINK", "CURRENT",
          "PAST", "FUTURE", "PICTURE_IN_PICTURE", "UNCHECKED", "BLANK", "INTEREST_SOURCE", "INTEREST_TARGET", "ACTIVE_VIEW_TRANSITION",
          "ANIMATED_IMAGE", "SNAPPED", "SNAPPED_X", "SNAPPED_Y", "SNAPPED_BLOCK", "SNAPPED_INLINE", "TARGET_BEFORE", "TARGET_AFTER",
          "TARGET_CURRENT", "NAVIGATION_SOURCE", "XR_OVERLAY", "HIGH_VALUE", "LOW_VALUE", "OPTIMAL_VALUE", "HAS_SLOTTED"]

# Enumerator names of the rows the parser and matcher refer to by number; every other row gets a name from its spelling.
SPECIAL_NAMES = {
    "E": "TY_TYPE", "*": "TY_UNIVERSAL", "#id": "TY_ID", ".class": "TY_CLASS", "[attr]": "TY_ATTR", "ns|": "TY_NS",
    ">": "CB_CHILD", "+": "CB_NEXT", "~": "CB_SUBSEQUENT", "||": "CB_COLUMN", " ": "CB_DESCENDANT", "&": "PC_NESTING",
}


def read_supplement(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip("\t ") or line.startswith("#"):
                continue
            rows.append((n, line.split("\t")))
    return rows


def read_pin(directory):
    pin = {}
    files = sorted(glob.glob(os.path.join(directory, "*.json")))
    if not files:
        raise SystemExit("gen-selector-tables: no webref data in %s (tools/css/fetch.sh)" % directory)
    for path in files:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        for s in data.get("selectors", []):
            name = s["name"]
            if name in pin:
                raise SystemExit("gen-selector-tables: %s is in two files of the pin (%s, %s)" % (name, pin[name]["file"], os.path.basename(path)))
            pin[name] = {"file": os.path.basename(path), "href": s.get("href", ""), "prose": s.get("prose", "")}
    return pin


def split_name(name):
    """(kind-independent) canonical text of a row: name without colons or parentheses, colons, functional."""
    colons = 0
    n = name
    while n.startswith(":"):
        colons += 1
        n = n[1:]
    functional = n.endswith("()")
    if functional:
        n = n[:-2]
    return n, colons, functional


def ident_of(name, kind, colons, functional):
    if name in SPECIAL_NAMES:
        return SPECIAL_NAMES[name]
    base, c, fn = split_name(name)
    s = KIND_PREFIX[kind] + "_" + re.sub(r"[^A-Za-z0-9]", "_", base).upper()
    if fn:
        s += "_FN"
    if kind == "element" and c == 1:
        s += "_LEGACY"
    return s


def check_list(value, allowed, what, where, problems, allow_dash=True):
    if value == "-":
        return []
    items = value.split(",")
    for i in items:
        if i not in allowed:
            problems.append("%s: %s %r is not one of %s" % (where, what, i, ", ".join(allowed)))
    return items


def build(pin, supplement_rows):
    problems = []
    rows = []
    seen = {}
    for n, f in supplement_rows:
        where = "selectors-supplement.tsv:%d" % n
        if len(f) != 12:
            problems.append("%s: %d fields, not twelve" % (where, len(f)))
            continue
        name, kind, arg, flags, pclass, follow, state, cb, spec, dep, status, note = f
        if name in seen:
            problems.append("%s: %r is also on line %d" % (where, name, seen[name]))
            continue
        seen[name] = n
        if kind not in KINDS:
            problems.append("%s: kind %r is not one of %s" % (where, kind, ", ".join(KINDS)))
            continue
        argbase = arg.split(":", 1)[0]
        if argbase not in ARGS:
            problems.append("%s: arg %r is not a grammar of the closed set" % (where, arg))
        if spec not in SPECS:
            problems.append("%s: spec %r is not one of %s" % (where, spec, ", ".join(SPECS)))
        if status not in STATUSES:
            problems.append("%s: status %r is not one of %s" % (where, status, ", ".join(STATUSES)))
        flag_items = []
        alias = None
        if flags != "-":
            for item in flags.split(","):
                if item.startswith("alias="):
                    alias = item[6:]
                elif item in FLAGS:
                    flag_items.append(item)
                else:
                    problems.append("%s: flag %r is not one of %s, alias=<name>" % (where, item, ", ".join(FLAGS)))
        if kind == "element":
            if pclass == "-":
                problems.append("%s: a pseudo-element needs a pclass" % where)
        elif pclass != "-":
            problems.append("%s: only a pseudo-element has a pclass" % where)
        states = check_list(state, STATES, "state bit", where, problems)
        cbs = check_list(cb, CALLBACKS, "callback", where, problems)
        deps = check_list(dep, DEPS, "dependency kind", where, problems)
        base, colons, functional = split_name(name)
        if kind in ("class", "element") and colons != (1 if kind == "class" else colons):
            pass
        if kind == "class" and colons not in (0, 1):
            pass
        rows.append({"n": n, "name": name, "kind": kind, "arg": arg, "flags": flag_items, "alias": alias, "pclass": pclass, "follow": follow,
                     "state": states, "cb": cbs, "spec": spec, "dep": deps, "status": status, "note": note})
    names = {r["name"] for r in rows}
    for name in sorted(pin):
        if name not in names:
            problems.append("selectors.json etc.: the pin lists %s (%s) and the supplement has no row for it" % (name, pin[name]["file"]))
    for r in rows:
        if r["name"] not in pin and "extra" not in r["note"] and "legacy" not in r["note"]:
            problems.append("selectors-supplement.tsv:%d: %s is not in the pin and the row is not marked extra (a stale row)" % (r["n"], r["name"]))
    return rows, problems


def sort_key(r):
    order = {"combinator": 0, "type": 1, "class": 2, "element": 3}
    base, colons, functional = split_name(r["name"])
    return (order[r["kind"]], base if r["kind"] in ("class", "element") else r["name"], functional, colons)


def finish(rows, pin):
    rows.sort(key=sort_key)
    idents = {}
    for i, r in enumerate(rows):
        base, colons, functional = split_name(r["name"])
        r["base"] = base
        r["colons"] = colons
        r["functional"] = functional
        r["ident"] = ident_of(r["name"], r["kind"], colons, functional)
        if r["ident"] in idents:
            raise SystemExit("gen-selector-tables: %s and %s would both be %s" % (idents[r["ident"]], r["name"], r["ident"]))
        idents[r["ident"]] = r["name"]
        r["index"] = i
    # aliases: a single-colon pseudo-element is the double-colon one; an alias= flag names the canonical pseudo-class by its base name
    by = {}
    for r in rows:
        by[(r["kind"], r["base"], r["functional"], r["colons"])] = r
    pes = [r for r in rows if r["kind"] == "element"]
    for k, r in enumerate(pes):
        r["pe_index"] = k
    for r in rows:
        r["alias_row"] = r["index"]
        if r["kind"] == "element" and r["colons"] == 1:
            tgt = by.get(("element", r["base"], r["functional"], 2))
            if tgt:
                r["alias_row"] = tgt["index"]
        elif r["alias"]:
            tgt = by.get(("class", r["alias"], r["functional"], 1))
            if not tgt:
                raise SystemExit("gen-selector-tables: %s is an alias of %s, which has no row" % (r["name"], r["alias"]))
            r["alias_row"] = tgt["index"]
    pe_names = {r["base"]: r for r in pes if r["colons"] == 2}
    for r in rows:
        r["follow_mask"] = 0
        r["pclass_kind"] = "PCLASS_NA"
        r["pclass_extra"] = []
        if r["kind"] != "element":
            continue
        if r["follow"] != "-":
            for item in r["follow"].split(","):
                if item == "*shadow":
                    for p in pes:
                        if p["colons"] == 2 and p["base"] not in ("part", "slotted"):
                            r["follow_mask"] |= 1 << p["pe_index"]
                elif item.startswith("-"):
                    cands = [p for p in pes if p["colons"] == 2 and p["base"] == item[1:]]
                    if not cands:
                        raise SystemExit("gen-selector-tables: %s: follow removes %s, which is not a pseudo-element" % (r["name"], item[1:]))
                    for p in cands:
                        r["follow_mask"] &= ~(1 << p["pe_index"])
                else:
                    cands = [p for p in pes if p["colons"] == 2 and p["base"] == item]
                    if not cands:
                        raise SystemExit("gen-selector-tables: %s: follow names %s, which is not a pseudo-element" % (r["name"], item))
                    for p in cands:
                        r["follow_mask"] |= 1 << p["pe_index"]
        items = r["pclass"].split(",")
        kinds = [i for i in items if i in ("none", "user", "nontree")]
        extra = [i for i in items if i not in ("none", "user", "nontree")]
        r["pclass_kind"] = {"none": "PCLASS_NONE", "user": "PCLASS_USER", "nontree": "PCLASS_NONTREE"}[kinds[0]] if kinds else "PCLASS_NONE"
        r["pclass_extra"] = extra
        for e in extra:
            if ("class", e, False, 1) not in by:
                raise SystemExit("gen-selector-tables: %s: pclass names %s, which is not a pseudo-class" % (r["name"], e))
    assert len(pes) <= 64, "more than 64 pseudo-elements: widen the follow mask"
    return rows


def c_expr(items, prefix):
    if not items:
        return "0"
    return " | ".join("(1ull << %s%s)" % (prefix, i) if prefix == "GTEXT_CSS_ELEMENT_CB_" else "%s%s" % (prefix, i) for i in items)


def dep_expr(items):
    if not items:
        return "0"
    return " | ".join("GTEXT_CSS_DEP_" + i.upper().replace("-", "_") for i in items)


def read_ci(path):
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def emit_header(rows, pin_hash, ci):
    out = [LICENSE]
    out.append("""
/**
 * @file
 *
 * GENERATED by tools/css/gen-selector-tables.py from the pinned webref `selectors` data and tools/css/selectors-supplement.tsv. Do not edit:
 * `make check-css-selectors` regenerates this file and fails on a difference.
 *
 * One row for each selector: the rows are sorted by kind, name, functional and colons, and the enumerators are the row numbers.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_SELECTOR_TABLES_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_SELECTOR_TABLES_H

#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/macros.h>
#include <stddef.h>
#include <stdint.h>

""")
    out.append("/* The pin: %s */\n\n" % pin_hash)
    out.append("""typedef enum { SELK_CLASS = 0, SELK_ELEMENT, SELK_COMBINATOR, SELK_TYPE } sel_kind;
typedef enum {
  SELARG_NONE = 0, SELARG_IDENT, SELARG_TYPE, SELARG_ATTR, SELARG_COMPOUND, SELARG_COMPOUND_LIST, SELARG_FORGIVING, SELARG_REAL,
  SELARG_RELATIVE, SELARG_ANB, SELARG_ANB_OF, SELARG_LANG, SELARG_DIR, SELARG_LEVELS, SELARG_STATE, SELARG_IDENTS_COMMA,
  SELARG_PART_IDENTS, SELARG_NAME_OR_STAR, SELARG_PT_NAME, SELARG_ENUM
} sel_arg;
typedef enum {
  SELSPEC_CLASS = 0, SELSPEC_ELEMENT, SELSPEC_ID, SELSPEC_ZERO, SELSPEC_MAX, SELSPEC_NTH, SELSPEC_HOSTFN, SELSPEC_SLOTTED, SELSPEC_NESTING
} sel_spec;
typedef enum { SELS_IMPLEMENTED = 0, SELS_PARSE_ONLY, SELS_UNSUPPORTED } sel_status;
typedef enum { PCLASS_NA = 0, PCLASS_NONE, PCLASS_USER, PCLASS_NONTREE } sel_pclass;
#define SELF_UA 0x1u
#define SELF_TREE 0x2u
#define SELF_LEGACY 0x4u
#define SELF_PAGE 0x8u

/// One selector of the tables.
typedef struct {
  const char * name;          ///< The name as webref spells it (":nth-child()").
  const char * base;          ///< Without colons or parentheses ("nth-child"); a type row's own spelling.
  const char * enum_values;   ///< SELARG_ENUM: the values, comma separated; else NULL.
  const char * note;          ///< The supplement's note.
  uint64_t state;             ///< GTEXT_CSS_STATE_* bits a match asks for.
  uint64_t follow;            ///< A pseudo-element: the pseudo-elements (by `pe_index`) that may follow it.
  uint32_t callbacks;         ///< Callbacks a match asks for (bit per GTEXT_CSS_Element_Callback).
  uint32_t dep;               ///< GTEXT_CSS_DEP_* bits.
  uint16_t alias_row;         ///< The row this one is the same selector as (itself, if none).
  uint16_t pclass_extra[3];   ///< Pseudo-classes (rows) that may follow it besides its class, 0xffff terminated.
  uint8_t kind;               ///< sel_kind.
  uint8_t arg;                ///< sel_arg.
  uint8_t spec;               ///< sel_spec.
  uint8_t status;             ///< sel_status.
  uint8_t pclass;             ///< sel_pclass.
  uint8_t flags;              ///< SELF_*.
  uint8_t colons;             ///< 0, 1 or 2 colons in the name.
  uint8_t functional;         ///< Takes parentheses.
  uint8_t pe_index;           ///< A pseudo-element: its number among them.
} sel_row;

""")
    out.append("enum {\n")
    for r in rows:
        out.append("  GTEXT_CSS_SEL_%s = %d, /* %s */\n" % (r["ident"], r["index"], r["name"] if r["name"] != " " else "descendant"))
    out.append("  GTEXT_CSS_SEL_ROW_COUNT = %d\n};\n\n" % len(rows))
    out.append("static const sel_row sel_rows[GTEXT_CSS_SEL_ROW_COUNT] = {\n")
    for r in rows:
        flags = " | ".join(FLAGS[f] for f in r["flags"]) or "0"
        extra = [("GTEXT_CSS_SEL_PC_" + re.sub(r"[^A-Za-z0-9]", "_", e).upper()) for e in r["pclass_extra"]]
        extra += ["0xffff"] * (3 - len(extra))
        if len(r["pclass_extra"]) > 3:
            raise SystemExit("gen-selector-tables: %s has more than three extra pseudo-classes" % r["name"])
        enum_values = "NULL"
        if r["arg"].startswith("enum:"):
            enum_values = '"%s"' % r["arg"][5:]
        name = r["name"].replace("\\", "\\\\").replace('"', '\\"')
        out.append("  [GTEXT_CSS_SEL_%s] = { \"%s\", \"%s\", %s, \"%s\", %s, 0x%xull, %s, %s, GTEXT_CSS_SEL_%s, { %s }, %s, %s, %s, %s, %s, %s, %d, %d, %d },\n" % (
            r["ident"], name, r["base"].replace("\\", "\\\\").replace('"', '\\"'), enum_values, r["note"].replace('"', '\\"'),
            c_expr(r["state"], "GTEXT_CSS_STATE_"), r["follow_mask"], c_expr(r["cb"], "GTEXT_CSS_ELEMENT_CB_"), dep_expr(r["dep"]),
            [x for x in rows if x["index"] == r["alias_row"]][0]["ident"], ", ".join(extra), KINDS[r["kind"]], ARGS[r["arg"].split(":")[0]],
            SPECS[r["spec"]], STATUSES[r["status"]], r["pclass_kind"], flags, r["colons"], 1 if r["functional"] else 0, r.get("pe_index", 0)))
    out.append("};\n\n")
    out.append("/// The attributes whose values are compared ignoring case in an HTML document (tools/css/html_ci_attributes.txt).\nstatic const char * const html_ci_attributes[] = {\n")
    for a in ci:
        out.append('  "%s",\n' % a)
    out.append("};\n\n#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_SELECTOR_TABLES_H\n")
    return "".join(out)


def emit_tsv(rows, pin):
    out = ["# GENERATED by tools/css/gen-selector-tables.py from the pinned webref `selectors` data and selectors-supplement.tsv. Do not edit:\n",
           "# `make check-css-selectors` regenerates this file and fails on a difference. Columns: the twelve of the supplement, then the pin's\n",
           "# specification file, the address of the definition, and whether the name is in the pin (pin) or an extra of the supplement.\n"]
    for r in rows:
        p = pin.get(r["name"])
        out.append("\t".join([r["name"], r["kind"], r["arg"], ",".join(["alias=" + r["alias"]] if r["alias"] else []) if False else
                              (",".join(r["flags"] + (["alias=" + r["alias"]] if r["alias"] else [])) or "-"), r["pclass"], r["follow"],
                              ",".join(r["state"]) or "-", ",".join(r["cb"]) or "-", r["spec"], ",".join(r["dep"]) or "-", r["status"], r["note"],
                              p["file"] if p else "-", p["href"] if p else "-", "pin" if p else "extra"]) + "\n")
    return "".join(out)


def emit_tests(rows):
    """The list of the names the probe table of the tests must cover (checked by the gate, not generated into the tests)."""
    return "".join(r["name"] + "\n" for r in rows if r["status"] != "unsupported")


def generate(webref, supplement, ci_path):
    pin = read_pin(webref)
    srows = read_supplement(supplement)
    rows, problems = build(pin, srows)
    if problems:
        return None, None, None, problems
    rows = finish(rows, pin)
    digest = "%d selectors in %d files; supplement %d rows" % (len(pin), len({p["file"] for p in pin.values()}), len(rows))
    return emit_header(rows, digest, read_ci(ci_path)), emit_tsv(rows, pin), rows, []


def covered_names(tests_path):
    """The names the probe table of the tests mentions, as C string literals."""
    if not os.path.exists(tests_path):
        return None
    with open(tests_path, encoding="utf-8") as f:
        text = f.read()
    return set(re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,', text.replace('\\"', '"')))


def run(args):
    header, tsv, rows, problems = generate(args.webref, args.supplement, args.ci)
    if problems:
        for p in problems:
            print("gen-selector-tables: " + p, file=sys.stderr)
        return 1
    bad = []
    names = covered_names(args.tests)
    if names is not None:
        for r in rows:
            if r["status"] != "unsupported" and r["name"] not in names:
                bad.append("tests/css_selector_rows.h: %s (%s) has no probe: a row with no implementation test" % (r["name"], r["status"]))
    elif args.check:
        bad.append("tests/css_selector_rows.h is not here: the probe of each row is the gate's evidence of a test")
    if args.check:
        for path, text in ((args.header, header), (args.tsv, tsv)):
            try:
                with open(path, encoding="utf-8") as f:
                    have = f.read()
            except OSError:
                have = None
            if have != text:
                bad.append("%s is not what the generator writes from the pin and the supplement" % os.path.relpath(path, ROOT))
        if bad:
            for p in bad:
                print("gen-selector-tables: " + p, file=sys.stderr)
            return 1
        print("check-css-selectors: %d selector rows, the tables are current, every implemented row has a probe" % len(rows))
        return 0
    if bad:
        for p in bad:
            print("gen-selector-tables: " + p, file=sys.stderr)
        return 1
    with open(args.header, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    with open(args.tsv, "w", encoding="utf-8", newline="\n") as f:
        f.write(tsv)
    print("wrote %d rows to %s and %s" % (len(rows), os.path.relpath(args.header, ROOT), os.path.relpath(args.tsv, ROOT)))
    return 0


def controls(args):
    """Each mistake made in a copy of the inputs must be reported; the unedited copy must pass."""
    tmp = tempfile.mkdtemp(prefix="selctl")
    failed = False
    try:
        sup = os.path.join(tmp, "supplement.tsv")
        with open(args.supplement, encoding="utf-8") as f:
            lines = f.read().split("\n")
        hdr = os.path.join(tmp, "tables.h")
        tsv = os.path.join(tmp, "selectors.tsv")
        tests = os.path.join(tmp, "rows.h")
        shutil.copy(args.tests, tests)

        def make(path, lines_):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(lines_))

        def attempt(name, sup_lines, want, check=True, tests_path=None, mutate_committed=None):
            make(sup, sup_lines)
            ns = argparse.Namespace(webref=args.webref, supplement=sup, tsv=tsv, header=hdr, tests=tests_path or args.tests, check=check, ci=args.ci)
            if mutate_committed:
                shutil.copy(args.header, hdr)
                shutil.copy(args.tsv, tsv)
                mutate_committed(hdr)
            elif check:
                shutil.copy(args.header, hdr)
                shutil.copy(args.tsv, tsv)
            import io
            import contextlib
            err = io.StringIO()
            out = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                rc = run(ns)
            ok = (rc == 0) if want is None else (rc != 0 and want in err.getvalue())
            print("control %-52s %s" % (name, "ok" if ok else "NOT SEEN TO FAIL (rc=%d: %s)" % (rc, err.getvalue()[:200].strip())))
            return ok

        rows = [i for i, l in enumerate(lines) if l and not l.startswith("#")]
        ok = attempt("unedited copy", lines, None)
        failed |= not ok
        i = [k for k in rows if lines[k].startswith(":hover\t")][0]
        ok = attempt("a pin selector without a row", lines[:i] + lines[i + 1:], "has no row for it")
        failed |= not ok
        ok = attempt("a stale row", lines[:i] + [lines[i].replace(":hover", ":not-a-selector", 1)] + lines[i + 1:], "stale row")
        failed |= not ok
        ok = attempt("a row of eleven fields", lines[:i] + [lines[i].rsplit("\t", 1)[0]] + lines[i + 1:], "not twelve")
        failed |= not ok
        ok = attempt("a state bit that does not exist", lines[:i] + [lines[i].replace("HOVER", "HOVERED")] + lines[i + 1:], "state bit")
        failed |= not ok
        ok = attempt("a status outside the closed set", lines[:i] + [lines[i].replace("implemented", "done")] + lines[i + 1:], "status")
        failed |= not ok

        def edit_header(path):
            with open(path, encoding="utf-8") as f:
                text = f.read()
            with open(path, "w", encoding="utf-8") as f:
                f.write(text.replace("SELS_IMPLEMENTED", "SELS_PARSE_ONLY", 1))
        ok = attempt("a hand edit of the generated header", lines, "not what the generator writes", mutate_committed=edit_header)
        failed |= not ok
        with open(args.tests, encoding="utf-8") as f:
            t = f.read()
        pt = os.path.join(tmp, "rows_missing.h")
        with open(pt, "w", encoding="utf-8") as f:
            f.write(t.replace('"%s"' % ":hover", '"%s"' % ":hovered", 1))
        ok = attempt("a row without a probe", lines, "no implementation test", tests_path=pt)
        failed |= not ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("--webref", default=WEBREF_DEFAULT)
    ap.add_argument("--supplement", default=SUPPLEMENT_DEFAULT)
    ap.add_argument("--tsv", default=TSV_DEFAULT)
    ap.add_argument("--header", default=HEADER_DEFAULT)
    ap.add_argument("--tests", default=TESTS_DEFAULT)
    ap.add_argument("--ci", default=CI_DEFAULT)
    args = ap.parse_args()
    if args.controls:
        sys.exit(controls(args))
    sys.exit(run(args))


if __name__ == "__main__":
    main()
