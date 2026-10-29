#!/usr/bin/env python3
"""Generate the media feature tables of the CSS conditional layer from the pinned webref `@media` data and the supplements.

Reads every ``@media`` descriptor (a media feature) of ``third_party/css/webref/ed/css/*.json`` (the pin of tools/css/WEBREF_COMMIT),
``tools/css/media-features-supplement.tsv`` (what the pin does not say about each feature) and ``tools/css/atrules-supplement.tsv`` (the media
types and the words the grammar of <media-type> excludes). It writes

  * ``tools/css/media-features.tsv``: one row per media feature, the joined table, the reviewable form;
  * ``src/css/css_media_tables.h``: the same rows as constant data (an enumeration of feature numbers, the rows, the keyword pool, the media
    types). Nothing is parsed at run time.

It fails (exit 1, every problem listed) when a feature of the pin has no row in the supplement, a row names a feature the pin does not have (a
stale row), a row has the wrong number of fields or a value outside the closed set of its column, names an environment member that
include/ghoti.io/text/css/css_media.h does not declare, the pin has more features than the 128 bits of the `supported` set, or (``--check``) the
committed files are not what this writes, or an implemented row has no probe in tests/css_media_rows.h.

``--controls`` shows each of those fail: it makes the mistake in a copy of the inputs and requires the report.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import contextlib
import glob
import io
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WEBREF_DEFAULT = os.path.join(ROOT, "third_party", "css", "webref", "ed", "css")
MEDIA_SUPPLEMENT_DEFAULT = os.path.join(HERE, "media-features-supplement.tsv")
ATRULES_SUPPLEMENT_DEFAULT = os.path.join(HERE, "atrules-supplement.tsv")
TSV_DEFAULT = os.path.join(HERE, "media-features.tsv")
HEADER_DEFAULT = os.path.join(ROOT, "src", "css", "css_media_tables.h")
MEDIA_HEADER_DEFAULT = os.path.join(ROOT, "include", "ghoti.io", "text", "css", "css_media.h")
TESTS_DEFAULT = os.path.join(ROOT, "tests", "css_media_rows.h")

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

UNITS = {"length": "MQU_LENGTH", "ratio": "MQU_RATIO", "resolution": "MQU_RESOLUTION", "integer": "MQU_INTEGER", "number": "MQU_NUMBER",
         "mq-boolean": "MQU_BOOLEAN", "keyword": "MQU_KEYWORD"}
DEPS = ["viewport", "device", "resolution", "color", "display", "input", "scheme", "preference", "environment", "extension"]
STATUSES = {"implemented": "MQS_IMPLEMENTED", "parse-only": "MQS_PARSE_ONLY"}
KINDS = {"range": "MQK_RANGE", "discrete": "MQK_DISCRETE"}
CLASSES = {"live": "MQT_LIVE", "deprecated": "MQT_DEPRECATED"}
MAX_FEATURES = 128


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
    """name -> {file, kind, value, keywords} for every @media descriptor; two files that disagree about kind or value are a problem."""
    pin = {}
    files = sorted(glob.glob(os.path.join(directory, "*.json")))
    if not files:
        raise SystemExit("gen-condition-tables: no webref data in %s (tools/css/fetch.sh)" % directory)
    for path in files:
        base = os.path.basename(path)
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        for a in data.get("atrules", []):
            if a["name"] != "@media":
                continue
            for d in a.get("descriptors", []):
                name = d["name"]
                kind = d.get("type", "")
                value = d.get("value", "")
                if name in pin:
                    p = pin[name]
                    if p["kind"] != kind:
                        raise SystemExit("gen-condition-tables: %s is %s in %s and %s in %s" % (name, p["kind"], p["files"][0], kind, base))
                    p["files"].append(base)
                    if len(value) > len(p["value"]):
                        p["value"] = value
                    continue
                pin[name] = {"files": [base], "kind": kind, "value": value}
    for name, p in pin.items():
        # The keywords: the idents of the value grammar that are not types (`<length>`, `<mq-boolean>`) or `infinite`.
        kws = [w.strip() for w in p["value"].split("|")]
        p["keywords"] = [w for w in kws if w and not w.startswith("<") and w != "infinite"] if p["kind"] == "discrete" else []
    return pin


def level_of(files):
    return 5 if "mediaqueries-5.json" in files else (4 if "mediaqueries.json" in files else 0)


def build(pin, mrows, arows, header_text):
    problems = []
    seen = {}
    feats = []
    for n, f in mrows:
        where = "media-features-supplement.tsv:%d" % n
        if len(f) != 7:
            problems.append("%s: %d fields, not seven" % (where, len(f)))
            continue
        name, unit, field, dep, status, falsekw, note = f
        if name in seen:
            problems.append("%s: %r is also on line %d" % (where, name, seen[name]))
            continue
        seen[name] = n
        if name not in pin:
            problems.append("%s: %r is not a media feature of the pin: stale row" % (where, name))
            continue
        if unit not in UNITS:
            problems.append("%s: unit %r is not one of %s" % (where, unit, ", ".join(UNITS)))
        if dep not in DEPS:
            problems.append("%s: dep %r is not one of %s" % (where, dep, ", ".join(DEPS)))
        if status not in STATUSES:
            problems.append("%s: status %r is not one of %s" % (where, status, ", ".join(STATUSES)))
        if header_text is not None and not re.search(r"\b%s\b" % re.escape(field), header_text):
            problems.append("%s: the environment member %r is not declared in css_media.h" % (where, field))
        p = pin[name]
        if p["kind"] not in KINDS:
            problems.append("%s: the pin's type %r of %s is not range or discrete" % (where, p["kind"], name))
            continue
        if p["kind"] == "discrete" and unit not in ("keyword", "mq-boolean"):
            problems.append("%s: a discrete feature has the unit keyword or mq-boolean, not %s" % (where, unit))
        if p["kind"] == "range" and unit in ("keyword", "mq-boolean"):
            problems.append("%s: a range feature has a numeric unit, not %s" % (where, unit))
        if unit == "keyword" and not p["keywords"]:
            problems.append("%s: a keyword feature needs keywords in the pin's value" % where)
        fkw = [] if falsekw == "-" else falsekw.split(",")
        for k in fkw:
            if k not in p["keywords"]:
                problems.append("%s: %r is not a keyword of %s (%s)" % (where, k, name, ",".join(p["keywords"])))
        if len(p["keywords"]) > 31:
            problems.append("%s: more than 31 keywords do not fit the mask of the boolean context" % where)
        feats.append({"name": name, "kind": p["kind"], "unit": unit, "field": field, "dep": dep, "status": status, "note": note,
                      "keywords": p["keywords"], "false_kw": fkw, "level": level_of(p["files"]), "files": ",".join(sorted(set(p["files"]))), "value": p["value"]})
    for name in sorted(pin):
        if name not in seen:
            problems.append("media-features-supplement.tsv has no row for %s (%s): add one" % (name, ",".join(pin[name]["files"])))
    if len(pin) > MAX_FEATURES:
        problems.append("the pin has %d media features and the `supported` set has %d bits" % (len(pin), MAX_FEATURES))
    types = []
    reserved = []
    cqfeats = []
    tseen = set()
    for n, f in arows:
        where = "atrules-supplement.tsv:%d" % n
        if f[0] not in ("media-type", "media-reserved", "container-feature"):
            continue
        if len(f) != 4:
            problems.append("%s: %d fields, not four" % (where, len(f)))
            continue
        sec, name, cls, note = f
        if (sec, name) in tseen:
            problems.append("%s: %s %r twice" % (where, sec, name))
        tseen.add((sec, name))
        if sec == "container-feature":
            logical = cls.startswith("logical:")
            row = cls[len("logical:"):] if logical else cls
            if row not in pin:
                problems.append("%s: the container feature %r names the media feature %r, which the pin does not have" % (where, name, row))
            cqfeats.append((name, row, logical, note))
            continue
        if sec == "media-type":
            if cls not in CLASSES:
                problems.append("%s: class %r is not one of %s" % (where, cls, ", ".join(CLASSES)))
            types.append((name, cls, note))
        else:
            reserved.append((name, note))
    feats.sort(key=lambda x: x["name"])
    return feats, types, reserved, cqfeats, problems


def ident(name):
    return re.sub(r"[^A-Za-z0-9]", "_", name.lstrip("-")).upper()


def emit_header(feats, types, reserved, cqfeats, digest):
    out = [LICENSE, "\n/* GENERATED by tools/css/gen-condition-tables.py from the pinned webref `@media` data, media-features-supplement.tsv and\n"
           " * atrules-supplement.tsv (%s). Do not edit: `make check-css-conditions` regenerates this file and fails on a difference. */\n\n" % digest,
           "#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_MEDIA_TABLES_H\n#define GHOTI_IO_GTEXT_SRC_CSS_CSS_MEDIA_TABLES_H\n\n#include <ghoti.io/text/macros.h>\n\n#include <stdint.h>\n\n",
           "/* The number of a media feature is its row in this table, in the order of the names; the `supported` set of an environment has one bit for each. */\n",
           "typedef enum {\n"]
    for i, f in enumerate(feats):
        out.append("  CSS_MQF_%s = %d,\n" % (ident(f["name"]), i))
    out.append("  CSS_MQF_COUNT = %d\n} css_mq_feature_id;\n\n" % len(feats))
    out.append("typedef enum { MQK_RANGE = 0, MQK_DISCRETE } css_mq_kind;\n")
    out.append("typedef enum { MQU_LENGTH = 0, MQU_RATIO, MQU_RESOLUTION, MQU_INTEGER, MQU_NUMBER, MQU_BOOLEAN, MQU_KEYWORD } css_mq_unit;\n")
    out.append("typedef enum { MQS_IMPLEMENTED = 0, MQS_PARSE_ONLY } css_mq_status;\n")
    out.append("typedef enum { MQD_VIEWPORT = 0, MQD_DEVICE, MQD_RESOLUTION, MQD_COLOR, MQD_DISPLAY, MQD_INPUT, MQD_SCHEME, MQD_PREFERENCE, MQD_ENVIRONMENT, MQD_EXTENSION } css_mq_dep;\n")
    out.append("typedef enum { MQT_LIVE = 0, MQT_DEPRECATED } css_mq_type_class;\n\n")
    out.append("typedef struct {\n  const char * name;\n  uint8_t kind;      /* css_mq_kind */\n  uint8_t unit;      /* css_mq_unit */\n  uint8_t dep;       /* css_mq_dep */\n"
               "  uint8_t status;    /* css_mq_status */\n  uint8_t level;     /* 4 or 5: the level of Media Queries that names it first (0: another module) */\n"
               "  uint16_t kw_first; /* first keyword in css_mq_keywords */\n  uint16_t kw_count; /* keywords */\n"
               "  uint32_t kw_false; /* bit k set: the k-th keyword of the feature is false in a boolean context */\n} css_mq_feature_row;\n\n")
    kw = []
    rows = []
    for f in feats:
        first = len(kw)
        kw.extend(f["keywords"])
        mask = 0
        for k in f["false_kw"]:
            mask |= 1 << f["keywords"].index(k)
        rows.append('  { "%s", %s, %s, MQD_%s, %s, %d, %d, %d, %du }' % (f["name"], KINDS[f["kind"]], UNITS[f["unit"]], f["dep"].upper(),
                    STATUSES[f["status"]], f["level"], first, len(f["keywords"]), mask))
    out.append("#define CSS_MQ_FEATURE_ROWS \\\n" + ",\\\n".join(rows) + "\n\n")
    out.append("#define CSS_MQ_KEYWORDS \\\n" + ",\\\n".join('  "%s"' % k for k in kw) + "\n\n")
    out.append("#define CSS_MQ_KEYWORD_COUNT %d\n\n" % len(kw))
    out.append("typedef struct {\n  const char * name;\n  uint8_t cls; /* css_mq_type_class */\n} css_mq_type_row;\n\n")
    out.append("#define CSS_MQ_TYPE_ROWS \\\n" + ",\\\n".join('  { "%s", %s }' % (n, CLASSES[c]) for n, c, _ in types) + "\n\n")
    out.append("/* The features a container condition has (Conditional 5, 5.2): a name, the media feature row it reads, and whether the name is the logical one (`inline-size`, `block-size`). */\n")
    out.append("typedef struct {\n  const char * name;\n  int16_t row;     /* css_mq_feature_id */\n  uint8_t logical; /* the width or height row mapped by the container's writing mode */\n} css_cq_feature_row;\n\n")
    out.append("#define CSS_CQ_FEATURE_ROWS \\\n" + ",\\\n".join('  { "%s", CSS_MQF_%s, %d }' % (n, ident(r), 1 if lg else 0) for n, r, lg, _ in cqfeats) + "\n\n")
    out.append("#define CSS_MQ_RESERVED_WORDS \\\n" + ",\\\n".join('  "%s"' % n for n, _ in reserved) + "\n\n")
    out.append("#endif\n")
    return "".join(out)


def emit_tsv(feats):
    out = ["# GENERATED by tools/css/gen-condition-tables.py from the pinned webref `@media` data and media-features-supplement.tsv. Do not edit:\n"
           "# `make check-css-conditions` regenerates this file and fails on a difference. Columns: name, kind, unit, keywords, false-in-boolean keywords, field, dep, level, status,\n"
           "# the files of the pin that name it, the pin's value grammar, the note of the supplement.\n"]
    for f in feats:
        out.append("\t".join([f["name"], f["kind"], f["unit"], ",".join(f["keywords"]) or "-", ",".join(f["false_kw"]) or "-", f["field"], f["dep"], str(f["level"]), f["status"],
                              f["files"], f["value"], f["note"]]) + "\n")
    return "".join(out)


def generate(args):
    pin = read_pin(args.webref)
    header_text = None
    if os.path.exists(args.media_header):
        with open(args.media_header, encoding="utf-8") as f:
            header_text = f.read()
    feats, types, reserved, cqfeats, problems = build(pin, read_tsv(args.media_supplement), read_tsv(args.atrules_supplement), header_text)
    if problems:
        return None, None, None, problems
    digest = "%d media features in %d files" % (len(pin), len({x for p in pin.values() for x in p["files"]}))
    return emit_header(feats, types, reserved, cqfeats, digest), emit_tsv(feats), feats, []


def covered_names(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return set(re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,', text))


def run(args):
    header, tsv, feats, problems = generate(args)
    if problems:
        for p in problems:
            print("gen-condition-tables: " + p, file=sys.stderr)
        return 1
    bad = []
    names = covered_names(args.tests)
    if names is not None:
        for r in feats:
            if r["status"] == "implemented" and r["name"] not in names:
                bad.append("tests/css_media_rows.h: %s has no probe: a row with no implementation test" % r["name"])
    elif args.check:
        bad.append("tests/css_media_rows.h is not here: the probe of each row is the gate's evidence of a test")
    if args.check:
        for path, text in ((args.header, header), (args.tsv, tsv)):
            try:
                with open(path, encoding="utf-8") as f:
                    have = f.read()
            except OSError:
                have = None
            if have != text:
                bad.append("%s is not what the generator writes from the pin and the supplements" % os.path.relpath(path, ROOT))
        if bad:
            for p in bad:
                print("gen-condition-tables: " + p, file=sys.stderr)
            return 1
        print("check-css-conditions: %d media features, the tables are current, every implemented row has a probe" % len(feats))
        return 0
    if bad:
        for p in bad:
            print("gen-condition-tables: " + p, file=sys.stderr)
        return 1
    with open(args.header, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    with open(args.tsv, "w", encoding="utf-8", newline="\n") as f:
        f.write(tsv)
    print("wrote %d rows to %s and %s" % (len(feats), os.path.relpath(args.header, ROOT), os.path.relpath(args.tsv, ROOT)))
    return 0


def controls(args):
    """Each mistake made in a copy of the inputs must be reported; the unedited copy must pass."""
    tmp = tempfile.mkdtemp(prefix="condctl")
    failed = False
    try:
        msup = os.path.join(tmp, "media.tsv")
        hdr = os.path.join(tmp, "tables.h")
        tsv = os.path.join(tmp, "features.tsv")
        with open(args.media_supplement, encoding="utf-8") as f:
            lines = f.read().split("\n")

        def attempt(name, mlines, want, tests_path=None, mutate=None):
            with open(msup, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(mlines))
            ns = argparse.Namespace(**vars(args))
            ns.media_supplement, ns.tsv, ns.header, ns.check = msup, tsv, hdr, True
            if tests_path:
                ns.tests = tests_path
            shutil.copy(args.header, hdr)
            shutil.copy(args.tsv, tsv)
            if mutate:
                mutate(hdr)
            err, out = io.StringIO(), io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                rc = run(ns)
            ok = (rc == 0) if want is None else (rc != 0 and want in err.getvalue())
            print("control %-52s %s" % (name, "ok" if ok else "NOT SEEN TO FAIL (rc=%d: %s)" % (rc, err.getvalue()[:200].strip())))
            return ok

        rows = [i for i, l in enumerate(lines) if l and not l.startswith("#")]
        i = [k for k in rows if lines[k].startswith("hover\t")][0]
        failed |= not attempt("unedited copy", lines, None)
        failed |= not attempt("a pin feature without a row", lines[:i] + lines[i + 1:], "has no row for hover")
        failed |= not attempt("a stale row", lines[:i] + [lines[i].replace("hover", "hovering", 1)] + lines[i + 1:], "stale row")
        failed |= not attempt("a row of six fields", lines[:i] + [lines[i].rsplit("\t", 1)[0]] + lines[i + 1:], "not seven")
        failed |= not attempt("a false-in-boolean keyword the feature lacks", lines[:i] + [lines[i].replace("\tnone\t", "\tbogus\t", 1)] + lines[i + 1:], "is not a keyword of hover")
        failed |= not attempt("an environment member that does not exist", lines[:i] + [lines[i].replace("\thover\tinput", "\thovered\tinput")] + lines[i + 1:], "not declared in css_media.h")
        failed |= not attempt("a status outside the closed set", lines[:i] + [lines[i].replace("implemented", "done")] + lines[i + 1:], "status")
        failed |= not attempt("a keyword feature given a numeric unit", lines[:i] + [lines[i].replace("\tkeyword\t", "\tlength\t", 1)] + lines[i + 1:], "discrete feature has the unit")

        def edit(path):
            with open(path, encoding="utf-8") as f:
                t = f.read()
            with open(path, "w", encoding="utf-8") as f:
                f.write(t.replace("MQS_IMPLEMENTED", "MQS_PARSE_ONLY", 1))
        failed |= not attempt("a hand edit of the generated header", lines, "not what the generator writes", mutate=edit)
        with open(args.tests, encoding="utf-8") as f:
            t = f.read()
        pt = os.path.join(tmp, "rows_missing.h")
        with open(pt, "w", encoding="utf-8") as f:
            f.write(t.replace('"hover"', '"hovered"', 1))
        failed |= not attempt("a row without a probe", lines, "no implementation test", tests_path=pt)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("--webref", default=WEBREF_DEFAULT)
    ap.add_argument("--media-supplement", default=MEDIA_SUPPLEMENT_DEFAULT)
    ap.add_argument("--atrules-supplement", default=ATRULES_SUPPLEMENT_DEFAULT)
    ap.add_argument("--tsv", default=TSV_DEFAULT)
    ap.add_argument("--header", default=HEADER_DEFAULT)
    ap.add_argument("--media-header", default=MEDIA_HEADER_DEFAULT)
    ap.add_argument("--tests", default=TESTS_DEFAULT)
    args = ap.parse_args()
    if args.controls:
        sys.exit(controls(args))
    sys.exit(run(args))


if __name__ == "__main__":
    main()
