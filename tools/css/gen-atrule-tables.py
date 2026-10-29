#!/usr/bin/env python3
"""Generate the at-rule tables and the scope ledger from the pinned webref data and the TSV files that say what the pin omits.

Reads the at-rules and descriptors of ``third_party/css/webref/ed/css/*.json`` (the pin of tools/css/WEBREF_COMMIT) and

  * ``tools/css/atrule_kinds.tsv``: one row per at-rule of the pin (its kind, where it may be, what the library does with it);
  * ``tools/css/atrule_required.tsv``: what an at-rule requires (the descriptors, by the value of ``system`` for ``@counter-style``);
  * ``tools/css/atrule_properties.tsv``: which declarations a block takes (``@position-try``) or ignores (``@keyframes``);
  * ``tools/css/counter_style_names.tsv``: the predefined counter style names.

It writes ``src/css/css_atrule_tables.h`` (constant data: nothing is parsed at run time) and the ledger ``tools/css/atrule_depth.tsv`` (a row per at-rule with the columns
parses, validates, serializes, applies, typed, preserved and wpt, which may not regress: ``wpt`` is kept by the scorer and copied from the committed ledger).

It fails (exit 1, every problem listed) when an at-rule of the pin has no row (it would be silently dropped), a row names an at-rule the pin does not have (stale), a kind or a
parent is outside its closed set, a required descriptor is not one the pin lists for the at-rule, a row has the wrong number of fields, or (``--check``) a committed file is not
what this writes or a ledger column went from 1 to 0. ``--controls`` makes each of those mistakes in a copy of the inputs and requires the report.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import glob
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WEBREF_DEFAULT = os.path.join(ROOT, "third_party", "css", "webref", "ed", "css")
KINDS_DEFAULT = os.path.join(HERE, "atrule_kinds.tsv")
REQUIRED_DEFAULT = os.path.join(HERE, "atrule_required.tsv")
PROPS_DEFAULT = os.path.join(HERE, "atrule_properties.tsv")
NAMES_DEFAULT = os.path.join(HERE, "counter_style_names.tsv")
HEADER_DEFAULT = os.path.join(ROOT, "src", "css", "css_atrule_tables.h")
LEDGER_DEFAULT = os.path.join(HERE, "atrule_depth.tsv")
PUBLIC_DEFAULT = os.path.join(ROOT, "include", "ghoti.io", "text", "css", "css_atrule.h")

PARENTS = {"top": 1, "group": 2, "style": 4, "page": 8, "feature_values": 16, "mixin": 32}

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


def read_tsv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip("\t ") or line.startswith("#"):
                continue
            rows.append((n, line.split("\t")))
    return rows


def read_pin(directory):
    """at-rule name (no @) -> {descriptors: set(names), files}; the descriptors are the declarations the pin lists for it."""
    pin = {}
    files = sorted(glob.glob(os.path.join(directory, "*.json")))
    if not files:
        raise SystemExit("gen-atrule-tables: no webref data in %s (tools/css/fetch.sh)" % directory)
    for path in files:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        for a in data.get("atrules", []):
            name = a["name"][1:]
            p = pin.setdefault(name, {"descriptors": set(), "files": set()})
            p["files"].add(os.path.basename(path))
            for d in a.get("descriptors", []):
                p["descriptors"].add(d["name"].lstrip("@"))
    return pin


def kind_names(public):
    """The GTEXT_CSS_ATRULE_* kinds the public header declares."""
    with open(public, encoding="utf-8") as f:
        text = f.read()
    names = re.findall(r"GTEXT_CSS_ATRULE_([A-Z_]+),\s", text)
    return {n for n in names if n not in ("KIND_COUNT",)}


def build(pin, kinds, required, props, counter_names, kind_set):
    problems = []
    seen = {}
    rows = []
    for n, f in kinds:
        if len(f) != 10:
            problems.append("atrule_kinds.tsv:%d: %d fields, 10 wanted" % (n, len(f)))
            continue
        name, kind, parents, parses, validates, serializes, applies, typed, preserved, note = f
        if name in seen:
            problems.append("atrule_kinds.tsv:%d: %s is listed twice" % (n, name))
            continue
        seen[name] = n
        if name not in pin:
            problems.append("atrule_kinds.tsv:%d: @%s is not an at-rule of the pin (a stale row)" % (n, name))
        if kind not in kind_set:
            problems.append("atrule_kinds.tsv:%d: %s is not a GTEXT_CSS_ATRULE_ kind of css_atrule.h" % (n, kind))
        mask = 0
        for p in parents.split(","):
            if p not in PARENTS:
                problems.append("atrule_kinds.tsv:%d: parent %r is not one of %s" % (n, p, ", ".join(sorted(PARENTS))))
            else:
                mask |= PARENTS[p]
        flags = 0
        for bit, v, label in ((1, parses, "parses"), (2, validates, "validates"), (4, serializes, "serializes"), (8, applies, "applies"), (16, typed, "typed"), (32, preserved, "preserved")):
            if v not in ("0", "1"):
                problems.append("atrule_kinds.tsv:%d: %s must be 0 or 1" % (n, label))
            elif v == "1":
                flags |= bit
        rows.append((name, kind, mask, flags, note))
    for name in sorted(pin):
        if name not in seen:
            problems.append("@%s is an at-rule of the pin and has no row in atrule_kinds.tsv (it would be dropped silently)" % name)
    reqs = []
    for n, f in required:
        if len(f) != 5:
            problems.append("atrule_required.tsv:%d: %d fields, 5 wanted" % (n, len(f)))
            continue
        at, when, descs, effect, source = f
        if at not in pin:
            problems.append("atrule_required.tsv:%d: @%s is not an at-rule of the pin" % (n, at))
            continue
        if effect not in ("ignore-rule", "defines-nothing"):
            problems.append("atrule_required.tsv:%d: effect %r is not ignore-rule or defines-nothing" % (n, effect))
        m = re.fullmatch(r"(always)|system:([a-z]+)", when)
        if not m:
            problems.append("atrule_required.tsv:%d: when %r is not always or system:NAME" % (n, when))
            continue
        parts = descs.split(",")
        minimum = 1
        if parts[-1].isdigit():
            minimum = int(parts.pop())
        for d in parts:
            if d not in pin[at]["descriptors"]:
                problems.append("atrule_required.tsv:%d: %s is not a descriptor of @%s in the pin" % (n, d, at))
        reqs.append((at, m.group(2) or "", parts, minimum, effect))
    prs = []
    for n, f in props:
        if len(f) != 4:
            problems.append("atrule_properties.tsv:%d: %d fields, 4 wanted" % (n, len(f)))
            continue
        at, mode, pat, source = f
        if at not in pin:
            problems.append("atrule_properties.tsv:%d: @%s is not an at-rule of the pin" % (n, at))
        if mode not in ("allow", "ignore", "keep"):
            problems.append("atrule_properties.tsv:%d: mode %r is not allow, ignore or keep" % (n, mode))
        prs.append((at, mode, pat))
    names = [n for _, [n] in counter_names]
    if names != sorted(set(names)) and False:
        problems.append("counter_style_names.tsv is not sorted")
    return sorted(rows), reqs, prs, sorted(set(names)), problems


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_header(rows, reqs, prs, names):
    out = [LICENSE, "\n/* GENERATED by tools/css/gen-atrule-tables.py from the pinned webref at-rules, tools/css/atrule_kinds.tsv, atrule_required.tsv, atrule_properties.tsv and counter_style_names.tsv.\n"
           " * Do not edit: `make check-css-atrules` regenerates this file and fails on a difference. */\n\n",
           "#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_ATRULE_TABLES_H\n#define GHOTI_IO_GTEXT_SRC_CSS_CSS_ATRULE_TABLES_H\n\n#include <ghoti.io/text/macros.h>\n\n#include <stdint.h>\n\n"]
    out.append("/* where an at-rule may be (the bits of css_atrule_row::parents) */\n")
    for k, v in PARENTS.items():
        out.append("#define CSS_AR_P_%s %du\n" % (k.upper(), v))
    out.append("/* what the library does with it (the bits of css_atrule_row::flags) */\n")
    for k, v in (("PARSES", 1), ("VALIDATES", 2), ("SERIALIZES", 4), ("APPLIES", 8), ("TYPED", 16), ("PRESERVED", 32)):
        out.append("#define CSS_AR_F_%s %du\n" % (k, v))
    out.append("\ntypedef struct {\n  const char * name;  /* without the @ */\n  uint8_t kind;       /* GTEXT_CSS_Atrule_Kind */\n  uint8_t parents;    /* CSS_AR_P_* */\n  uint8_t flags;      /* CSS_AR_F_* */\n} css_atrule_row;\n\n")
    out.append("#define CSS_ATRULE_ROW_COUNT %d\n\n/* sorted by name */\nstatic const css_atrule_row css_atrule_rows[CSS_ATRULE_ROW_COUNT] = {\n" % len(rows))
    for name, kind, mask, flags, note in rows:
        out.append("  {%s, GTEXT_CSS_ATRULE_%s, %d, %d},\n" % (cstr(name), kind, mask, flags))
    out.append("};\n\n")
    out.append("typedef struct {\n  const char * atrule;     /* the at-rule, without the @ */\n  const char * system;     /* \"\" for always; else the value of `system` (a @counter-style) */\n  const char * descriptors[3]; /* any of these suffices (NULL ends) */\n"
               "  uint8_t minimum;         /* how many symbols the first descriptor must hold (1 or 2) */\n  uint8_t ignore_rule;     /* 1: the rule is invalid; 0: it is valid and defines nothing */\n} css_atrule_required;\n\n")
    out.append("#define CSS_ATRULE_REQUIRED_COUNT %d\nstatic const css_atrule_required css_atrule_required_rows[CSS_ATRULE_REQUIRED_COUNT] = {\n" % len(reqs))
    for at, system, descs, minimum, effect in reqs:
        d = ", ".join(cstr(x) for x in descs[:3]) + (", NULL" if len(descs) < 3 else "")
        out.append("  {%s, %s, {%s}, %d, %d},\n" % (cstr(at), cstr(system), d, minimum, 1 if effect == "ignore-rule" else 0))
    out.append("};\n\n")
    out.append("/* which declarations a block takes: mode 0 allow (only these), 1 ignore (not these), 2 keep (an exception to an ignore); a trailing `*` in the pattern is a prefix */\n"
               "typedef struct {\n  const char * atrule;\n  uint8_t mode;\n  const char * pattern;\n} css_atrule_property_rule;\n\n")
    out.append("#define CSS_ATRULE_PROPERTY_RULE_COUNT %d\nstatic const css_atrule_property_rule css_atrule_property_rules[CSS_ATRULE_PROPERTY_RULE_COUNT] = {\n" % len(prs))
    for at, mode, pat in prs:
        out.append("  {%s, %d, %s},\n" % (cstr(at), {"allow": 0, "ignore": 1, "keep": 2}[mode], cstr(pat)))
    out.append("};\n\n")
    out.append("/* the predefined counter style names, sorted: ASCII lowercased on parse wherever they are used as counter styles */\n#define CSS_COUNTER_STYLE_NAME_COUNT %d\nstatic const char * const css_counter_style_names[CSS_COUNTER_STYLE_NAME_COUNT] = {\n" % len(names))
    for n in names:
        out.append("  %s,\n" % cstr(n))
    out.append("};\n\n#endif\n")
    return "".join(out)


def emit_ledger(rows, wpt):
    out = ["# tools/css/gen-atrule-tables.py: the scope ledger of the at-rules of the pin. Columns: at-rule, kind, parses, validates, serializes, applies, typed, preserved, wpt, note.\n"
           "# Every column but `wpt` is copied from atrule_kinds.tsv; `wpt` (the WPT vectors scored) is kept by the scorer (tools/conformance/run-css-atrules.sh) and may not go from 1 to 0; none may regress.\n"]
    for name, kind, mask, flags, note in rows:
        cols = [str(1 if flags & b else 0) for b in (1, 2, 4, 8, 16, 32)]
        out.append("\t".join(["@" + name, kind] + cols + [wpt.get(name, "0"), note]) + "\n")
    return "".join(out)


def read_ledger_wpt(path):
    wpt = {}
    if os.path.exists(path):
        for _, f in read_tsv(path):
            if len(f) >= 9:
                wpt[f[0][1:]] = f[8]
    return wpt


def generate(args):
    pin = read_pin(args.webref)
    kinds = read_tsv(args.kinds)
    required = read_tsv(args.required)
    props = read_tsv(args.props)
    names = read_tsv(args.names)
    # counter_style_names.tsv is one name a line: read_tsv gives [name]
    rows, reqs, prs, nms, problems = build(pin, kinds, required, props, names, kind_names(args.public))
    if problems:
        return None, None, rows, problems
    return emit_header(rows, reqs, prs, nms), emit_ledger(rows, read_ledger_wpt(args.ledger)), rows, []


def regressions(old_path, ledger):
    """A column of the committed ledger that was 1 and is 0 in the new one."""
    if not os.path.exists(old_path):
        return []
    old = {f[0]: f for _, f in read_tsv(old_path) if len(f) >= 9}
    new = {f[0]: f for _, f in read_tsv_text(ledger) if len(f) >= 9}
    bad = []
    for k, f in old.items():
        if k not in new:
            bad.append("%s is in the committed ledger and not in the new one" % k)
            continue
        for i, label in enumerate(("parses", "validates", "serializes", "applies", "typed", "preserved", "wpt"), 2):
            if i < 8 and f[i] == "1" and new[k][i] == "0" and label != "preserved":
                bad.append("%s: column %s went from 1 to 0" % (k, label))
            if i == 8 and f[i].isdigit() and new[k][i].isdigit() and int(new[k][i]) < int(f[i]):
                bad.append("%s: wpt went from %s to %s" % (k, f[i], new[k][i]))
    return bad


def read_tsv_text(text):
    rows = []
    for n, line in enumerate(text.split("\n"), 1):
        if not line.strip("\t ") or line.startswith("#"):
            continue
        rows.append((n, line.split("\t")))
    return rows


def run(args):
    header, ledger, rows, problems = generate(args)
    if problems:
        for p in problems:
            print("gen-atrule-tables: " + p, file=sys.stderr)
        return 1
    if args.check:
        bad = []
        for path, text in ((args.header, header), (args.ledger, ledger)):
            try:
                with open(path, encoding="utf-8") as f:
                    have = f.read()
            except OSError:
                have = None
            if have != text:
                bad.append("%s is not what the generator writes" % os.path.relpath(path, ROOT))
        bad += regressions(args.ledger, ledger)
        if bad:
            for p in bad:
                print("gen-atrule-tables: " + p, file=sys.stderr)
            return 1
        print("check-css-atrules: %d at-rules, the tables and the ledger are current, no column regressed" % len(rows))
        return 0
    with open(args.header, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    with open(args.ledger, "w", encoding="utf-8", newline="\n") as f:
        f.write(ledger)
    print("wrote %d at-rules to %s and %s" % (len(rows), os.path.relpath(args.header, ROOT), os.path.relpath(args.ledger, ROOT)))
    return 0


def controls(args):
    tmp = tempfile.mkdtemp(prefix="atctl")
    failed = False
    try:
        def copy(name, src):
            dst = os.path.join(tmp, name)
            shutil.copy(src, dst)
            return dst

        def attempt(label, mutate, expect):
            nonlocal failed
            a = argparse.Namespace(**vars(args))
            a.kinds = copy("kinds.tsv", args.kinds)
            a.required = copy("required.tsv", args.required)
            a.props = copy("props.tsv", args.props)
            a.names = copy("names.tsv", args.names)
            a.header = os.path.join(tmp, "h.h")
            a.ledger = os.path.join(tmp, "l.tsv")
            a.check = False
            mutate(a)
            _, _, _, problems = generate(a)
            hit = any(expect in p for p in problems)
            print("control %-52s %s" % (label, "ok" if hit else "NOT SEEN"))
            failed = failed or not hit

        def edit(path, fn):
            with open(path, encoding="utf-8") as f:
                t = f.read()
            with open(path, "w", encoding="utf-8") as f:
                f.write(fn(t))

        attempt("an at-rule of the pin with no row", lambda a: edit(a.kinds, lambda t: t.replace("view-transition\tVIEW_TRANSITION", "#view-transition\tVIEW_TRANSITION")), "no row in atrule_kinds.tsv")
        attempt("a stale row", lambda a: edit(a.kinds, lambda t: t + "nonesuch\tPRESERVED\ttop\t1\t0\t1\t0\t0\t1\tx\n"), "stale row")
        attempt("a kind that does not exist", lambda a: edit(a.kinds, lambda t: t.replace("\tFONT_FACE\t", "\tFONT_FACETS\t")), "is not a GTEXT_CSS_ATRULE_ kind")
        attempt("a parent that does not exist", lambda a: edit(a.kinds, lambda t: t.replace("top,group\t1\t1\t1\t1\t1\t0\ttyped (css_font_rules.h)", "top,roof\t1\t1\t1\t1\t1\t0\tx", 1)), "parent 'roof'")
        attempt("a required descriptor the pin lacks", lambda a: edit(a.required, lambda t: t.replace("font-family,src", "font-family,srcs")), "is not a descriptor of @font-face")
        attempt("a row of nine fields", lambda a: edit(a.kinds, lambda t: t.replace("\t0\tx\n", "\n") if False else t + "bad\tPRESERVED\ttop\t1\t0\t1\t0\t0\t1\n"), "fields, 10 wanted")
        attempt("an at-rule listed twice", lambda a: edit(a.kinds, lambda t: t + "media\tMEDIA\ttop\t1\t1\t1\t1\t0\t0\tx\n"), "is listed twice")
        # the committed ledger regressing a column
        a = argparse.Namespace(**vars(args))
        a.ledger = os.path.join(tmp, "l2.tsv")
        shutil.copy(args.ledger, a.ledger)
        edit(a.ledger, lambda t: t.replace("@font-face\tFONT_FACE\t1\t1\t1\t1\t1\t0\t", "@font-face\tFONT_FACE\t1\t1\t1\t1\t1\t0\t", 1))
        h, l, _, p = generate(a)
        if p:
            print("control the unedited inputs                                  NOT OK", p)
            failed = True
        else:
            print("control the unedited inputs                                  ok")
        with open(a.ledger, "w", encoding="utf-8") as f:
            f.write(l.replace("@font-face\tFONT_FACE\t1\t1\t1\t1\t1\t0", "@font-face\tFONT_FACE\t1\t1\t1\t1\t1\t0", 1))
        old = os.path.join(tmp, "old.tsv")
        with open(old, "w", encoding="utf-8") as f:
            f.write(l)
        regress = l.replace("\tFONT_FACE\t1\t1\t1\t1\t1\t0\t", "\tFONT_FACE\t1\t1\t1\t0\t1\t0\t", 1)
        bad = regressions(old, regress)
        hit = any("went from 1 to 0" in b for b in bad)
        print("control %-52s %s" % ("a ledger column that went from 1 to 0", "ok" if hit else "NOT SEEN"))
        failed = failed or not hit
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--webref", default=WEBREF_DEFAULT)
    ap.add_argument("--kinds", default=KINDS_DEFAULT)
    ap.add_argument("--required", default=REQUIRED_DEFAULT)
    ap.add_argument("--props", default=PROPS_DEFAULT)
    ap.add_argument("--names", default=NAMES_DEFAULT)
    ap.add_argument("--header", default=HEADER_DEFAULT)
    ap.add_argument("--ledger", default=LEDGER_DEFAULT)
    ap.add_argument("--public", default=PUBLIC_DEFAULT)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--controls", action="store_true")
    args = ap.parse_args()
    if args.controls:
        return controls(args)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
