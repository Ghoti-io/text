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
"""Record which properties Chromium and Firefox apply to ::first-line, ::first-letter and ::marker.

    css_pseudo_record.py [--cases FILE] [--out FILE] [--check]

A case (tools/conformance/css_pseudo_cases.tsv) is a property and a value that is valid and not the property's initial value. For each case and each pseudo-element the
document holds an element (a list item for ::marker) and the rule `#eN::pseudo { property: value }`; the record keeps what `getComputedStyle(element, pseudo)` answers for
the property (`pseudo`) and what it answers for an identical element that has no such rule (`base`). A property *applies* to the pseudo-element when the two differ: the
declaration changed the computed value of the pseudo-element, which is what the pages of web-platform-tests ask (css-pseudo/first-line-allowed-properties.html and kin).

The record is one JSON object a line in `tools/conformance/css_pseudo_answers.jsonl`, the first line naming the engines. tools/css/check-pseudo-apply.py holds the table of
src/css/css_pseudo_apply.c to it. The browsers run in the pinned image (tools/oracle/oracle_env.py: `--network none`, the repository read-only); a run takes a minute.
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

CASES = os.path.join(ROOT, "tools", "conformance", "css_pseudo_cases.tsv")
OUT = os.path.join(ROOT, "tools", "conformance", "css_pseudo_answers.jsonl")
DRIVER = os.path.join(HERE, "containers", "cssprop", "pseudo_driver.mjs")
ENGINES = ("chromium", "firefox")
PSEUDOS = ("::first-line", "::first-letter", "::marker")
PER_DOCUMENT = 40


def load_cases(path):
    cases = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 2:
                raise SystemExit("%s:%d: two tab-separated columns are expected" % (path, n))
            cases.append((cols[0], cols[1]))
    names = [c[0] for c in cases]
    if len(set(names)) != len(names):
        raise SystemExit("css_pseudo_record: a property is a case twice")
    return cases


def document(batch, pseudo):
    """One document: for each case an element (a list item for ::marker) with the rule that declares the property on its pseudo-element."""
    rules = []
    body = []
    queries = []
    for k, (prop, value) in enumerate(batch):
        eid = "e%d" % k
        rules.append("#%s%s { %s: %s }" % (eid, pseudo, prop, value))
        bid = "b%d" % k
        for i in (eid, bid):
            body.append('<li id="%s">text text text</li>' % i if pseudo == "::marker" else '<div id="%s">text text text</div>' % i)
        queries.append([eid, [prop], pseudo])
        queries.append([bid, [prop], pseudo])
    text = "<!doctype html><html><head><title>t</title><style>%s</style></head><body>%s</body></html>" % (
        html.escape("\n".join(rules), quote=False), ("<ul>%s</ul>" % "".join(body)) if pseudo == "::marker" else "".join(body))
    return {"mode": "h", "text": text, "queries": queries}


def run(argv, data):
    import oracle_env
    cmd = oracle_env.command("cssprop", argv)
    p = oracle_env.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if p.returncode != 0:
        raise SystemExit("css_pseudo_record: %s exited %d:\n%s" % (argv[-3:], p.returncode, oracle_env.reference_stderr(p.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = p.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return json.loads(lines[0][1:]), [json.loads(x) for x in lines[1:]]


def ask(engine, cases):
    jobs = []
    for pseudo in PSEUDOS:
        for i in range(0, len(cases), PER_DOCUMENT):
            jobs.append((pseudo, cases[i:i + PER_DOCUMENT]))
    docs = [document(b, p) for p, b in jobs]
    data = "".join(json.dumps(d, ensure_ascii=False) + "\n" for d in docs).encode("utf-8")
    inner = ["sh", "-c", 'cd /opt/cssprop && exec node "%s" %s light' % (DRIVER, engine)]
    header, rows = run(inner, data)
    if len(rows) != len(docs):
        raise SystemExit("css_pseudo_record: %s answered %d documents to %d" % (engine, len(rows), len(docs)))
    out = {}
    for (pseudo, batch), answers in zip(jobs, rows):
        for j in range(0, len(answers), 2):
            (_, prop, with_pseudo), (_, prop2, base) = answers[j], answers[j + 1]
            if prop != prop2:
                raise SystemExit("css_pseudo_record: the answers are out of step at %s" % prop)
            out.setdefault(prop, {})[pseudo] = {"pseudo": with_pseudo, "base": base}
    return header, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", default=CASES)
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    cases = load_cases(args.cases)
    print("css_pseudo_record: %d cases" % len(cases))
    if args.check:
        return 0
    headers = {}
    answers = {}
    for engine in ENGINES:
        headers[engine], answers[engine] = ask(engine, cases)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("#" + json.dumps({"engines": headers, "cases": len(cases), "pseudos": list(PSEUDOS)}, ensure_ascii=False, sort_keys=True) + "\n")
        for prop, value in cases:
            f.write(json.dumps({"property": prop, "value": value, "engines": {e: answers[e].get(prop, {}) for e in ENGINES}}, ensure_ascii=False, sort_keys=True) + "\n")
    print("css_pseudo_record: wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
