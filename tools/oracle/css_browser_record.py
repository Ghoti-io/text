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
"""Record what Chromium and Firefox answer for a list of declarations: the file other tools cite when the served text is silent.

    css_browser_record.py [--cases FILE]... [--defects FILE]... [--out FILE] [--work DIR] [--check]

A case is a declaration list (`style`) on an element inside an optional parent (`parent`, a declaration list too), the whole inside an element with
`display: none` so that a resolved value is the computed value, and the properties to read back (`read`). For each case and each engine the record holds

  accepted   for every declaration of `style` (split at top-level semicolons): whether `element.style.setProperty()` left it, and the specified value
             the engine serialized (the property oracle's driver, containers/cssprop/driver.mjs);
  computed   `getComputedStyle(element).getPropertyValue(p)` for every property named in `read` (the computed-style driver, style_driver.mjs), in the
             light color scheme and, for a case that says `dark` in its flags, in the dark one.

Cases come from `tools/conformance/css_browser_cases.tsv` (written by hand, one topic a line: id, flags, parent, style, read) and from the
defect lists of the computed and property gates (the property and value of every line, read back as the property: one case a line). The record is one
JSON object a line in `tools/conformance/css_browser_answers.jsonl`; the first line names the engines and the image. A tool that cites the record
reads a case by its id, never by position.

With --check the cases are assembled and counted and nothing is asked of a browser. The browsers run in the pinned image (tools/oracle/oracle_env.py:
`--network none`, the repository read-only), so this needs the image and a container engine; a run is a long list of small documents and takes minutes.
"""

import argparse
import html
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

CASES = os.path.join(ROOT, "tools", "conformance", "css_browser_cases.tsv")
DEFECTS = [os.path.join(ROOT, "tools", "conformance", n) for n in ("CSS_COMPUTED_DEFECTS", "CSS_PROPERTY_DEFECTS")]
OUT = os.path.join(ROOT, "tools", "conformance", "css_browser_answers.jsonl")
STYLE_DRIVER = os.path.join(HERE, "containers", "cssprop", "style_driver.mjs")
ENGINES = ("chromium", "firefox")
PER_DOCUMENT = 120


def unesc(s):
    return s.replace("\\t", "\t").replace("\\n", "\n").replace("\\\\", "\\")


def split_declarations(text):
    """The declarations of a style attribute: split at semicolons outside parentheses, brackets and strings."""
    out, cur, depth, quote = [], [], 0, None
    for ch in text:
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in "\"'":
            quote = ch
        elif ch in "([":
            depth += 1
        elif ch in ")]":
            depth = max(0, depth - 1)
        elif ch == ";" and depth == 0:
            if "".join(cur).strip():
                out.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def declaration_parts(d):
    name, _, value = d.partition(":")
    return name.strip(), value.strip()


def load_cases(case_files, defect_files):
    cases = {}
    order = []

    def add(cid, flags, parent, style, read, source):
        if cid in cases:
            raise SystemExit("css_browser_record: the case id %r is used twice" % cid)
        cases[cid] = {"id": cid, "flags": flags, "parent": parent, "style": style, "read": read, "source": source}
        order.append(cid)

    for path in case_files:
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                cols = [unesc(c) for c in line.split("\t")]
                if len(cols) != 5:
                    raise SystemExit("%s:%d: five tab-separated columns are expected, found %d" % (path, n, len(cols)))
                cid, flags, parent, style, read = cols
                add(cid, [x for x in flags.split(",") if x], parent, style, [x.strip() for x in read.split(",") if x.strip()], "hand")
    seen = set()
    for path in defect_files:
        base = os.path.basename(path)
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                cols = line.split("\t")
                if len(cols) < 3:
                    continue
                vector, prop, value = cols[0], cols[1], unesc(cols[2])
                key = (prop, value)
                if key in seen:
                    continue
                seen.add(key)
                add("%s:%s" % (base, vector), [], "", "%s: %s" % (prop, value), [prop], base)
    return cases, order


def document(batch, cases):
    parts = []
    queries = []
    for k, cid in enumerate(batch):
        c = cases[cid]
        eid = "e%d" % k
        inner = '<div id="%s" style="%s"></div>' % (eid, html.escape(c["style"], quote=True))
        if c["parent"]:
            inner = '<div style="%s">%s</div>' % (html.escape(c["parent"], quote=True), inner)
        parts.append(inner)
        queries.append([eid, c["read"]])
    text = '<!doctype html><html><head><title>t</title></head><body><div id="wrapper" style="display: none">' + "".join(parts) + "</div></body></html>"
    return {"mode": "h", "text": text, "queries": queries}


def run(argv, data):
    import oracle_env
    cmd = oracle_env.command("cssprop", argv)
    p = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if p.returncode != 0:
        import oracle_env as oe
        raise SystemExit("css_browser_record: %s exited %d:\n%s" % (argv[-3:], p.returncode, oe.reference_stderr(p.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = p.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return json.loads(lines[0][1:]), [json.loads(x) for x in lines[1:]]


def ask_computed(engine, scheme, ids, cases):
    batches = [ids[i:i + PER_DOCUMENT] for i in range(0, len(ids), PER_DOCUMENT)]
    docs = [document(b, cases) for b in batches]
    data = "".join(json.dumps(d, ensure_ascii=False) + "\n" for d in docs).encode("utf-8", "surrogatepass")
    inner = ["sh", "-c", 'cd /opt/cssprop && exec node "%s" %s %s' % (STYLE_DRIVER, engine, scheme)]
    header, rows = run(inner, data)
    if len(rows) != len(docs):
        raise SystemExit("css_browser_record: %s answered %d documents to %d" % (engine, len(rows), len(docs)))
    out = {}
    for b, answers in zip(batches, rows):
        for cid_index, prop, value in answers:
            cid = b[int(cid_index[1:])]
            out.setdefault(cid, {})[prop] = value
    return header, out


def ask_accept(engine, ids, cases):
    items = []
    owners = []
    for cid in ids:
        c = cases[cid]
        for d in split_declarations(c["style"]):
            p, v = declaration_parts(d)
            items.append([p, v])
            owners.append((cid, d))
        if c["parent"]:
            pass
    data = "".join(json.dumps(i, ensure_ascii=False) + "\n" for i in items).encode("utf-8", "surrogatepass")
    header, rows = run(["cssprop-driver", engine], data)
    if len(rows) != len(items):
        raise SystemExit("css_browser_record: %s answered %d items to %d" % (engine, len(rows), len(items)))
    out = {}
    for (cid, d), r in zip(owners, rows):
        out.setdefault(cid, {})[d] = {"accepted": r[0], "specified": r[1], "supports": r[3], "error": r[4]}
    return header, out


def compare(binary, answers, cases, prefix, metrics):
    """This library against the record: one line a (case, property): `=` when all three agree, `O` when only this library differs, `B` when the browsers differ."""
    import css_style_diff
    order = [cid for cid in sorted(cases) if cid.startswith(prefix)]
    record = {}
    with open(answers, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("#"):
                r = json.loads(line)
                record[r["id"]] = r
    order = [c for c in order if c in record]
    batches = [order[i:i + PER_DOCUMENT] for i in range(0, len(order), PER_DOCUMENT)]
    docs = [document(b, cases) for b in batches]
    ours = css_style_diff.run_ours(binary, docs, metrics, "light")
    out = []
    for b, lines in zip(batches, ours):
        for eid, prop, kind, value in lines:
            cid = b[int(eid[1:])]
            c = record[cid]["engines"]["chromium"]["computed"].get(prop)
            ff = record[cid]["engines"]["firefox"]["computed"].get(prop)
            mine = value if kind == "OK" else kind
            if c == ff == mine:
                tag = "="
            elif c == ff:
                tag = "O"
            elif mine == c or mine == ff:
                tag = "B"
            else:
                tag = "BO"
            out.append((tag, cid, record[cid]["style"], prop, c, ff, mine))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compare", metavar="BINARY", help="run this library (css-style-ours) over the cases of the record and print where it differs")
    ap.add_argument("--prefix", default="", help="with --compare: only the cases whose id begins so")
    ap.add_argument("--metrics", default=None)
    ap.add_argument("--all", action="store_true", help="with --compare: also the lines on which all three agree")
    ap.add_argument("--cases", action="append")
    ap.add_argument("--defects", action="append")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    cases, order = load_cases(args.cases or [CASES], args.defects if args.defects is not None else DEFECTS)
    print("css_browser_record: %d cases (%d by hand)" % (len(order), sum(1 for c in cases.values() if c["source"] == "hand")), file=sys.stderr if args.compare else sys.stdout)
    if args.compare:
        rows = compare(args.compare, args.out, cases, args.prefix, args.metrics)
        for tag, cid, style, prop, c, ff, mine in rows:
            if tag != "=" or args.all:
                print("%s\t%s\t%s\t%s\tC=%s\tF=%s\tO=%s" % (tag, cid, style, prop, c, ff, mine))
        counts = {}
        for r in rows:
            counts[r[0]] = counts.get(r[0], 0) + 1
        print("css_browser_record: %s" % ", ".join("%s %d" % kv for kv in sorted(counts.items())), file=sys.stderr)
        return 0
    if args.check:
        return 0
    headers = {}
    record = {cid: {"id": cid, "source": cases[cid]["source"], "parent": cases[cid]["parent"], "style": cases[cid]["style"], "read": cases[cid]["read"], "engines": {}} for cid in order}
    dark_ids = [cid for cid in order if "dark" in cases[cid]["flags"]]
    for engine in ENGINES:
        h1, comp = ask_computed(engine, "light", order, cases)
        headers[engine] = h1
        h2, acc = ask_accept(engine, order, cases)
        dark = {}
        if dark_ids:
            _, dark = ask_computed(engine, "dark", dark_ids, cases)
        for cid in order:
            e = {"computed": comp.get(cid, {}), "declarations": acc.get(cid, {})}
            if cid in dark:
                e["computed-dark"] = dark[cid]
            record[cid]["engines"][engine] = e
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("#" + json.dumps({"engines": {k: v for k, v in headers.items()}, "cases": len(order), "schemes": "light; dark only for cases that name it"}, ensure_ascii=False, sort_keys=True) + "\n")
        for cid in sorted(order):
            f.write(json.dumps(record[cid], ensure_ascii=False, sort_keys=True) + "\n")
    print("css_browser_record: wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
