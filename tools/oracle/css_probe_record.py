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
"""Record what Chromium and Firefox answer for the vectors of tentative web-platform-tests pages that are not declarations: selectors, values with no property of their own and
`@supports selector()` conditions.

    css_probe_record.py [--out FILE] [--check]

The vectors are the `wpt-tentative` lines of tools/conformance/CSS_SELECTOR_EXCEPTIONS, CSS_VALUES_EXCEPTIONS (a page named tentative) and CSS_CONDITIONS_EXCEPTIONS. For each,
one document of the CSSOM driver (tools/oracle/containers/cssprop/cssom_driver.mjs) asks both engines to read the text as a style rule (a selector), as the value of the vector's
property in a style rule, or as a `@supports` rule, and to write the rule back: a `TypeError` is a text the engine refused to make a rule of, and a rule without the declaration is a
value it dropped. The record is tools/conformance/css_probe_answers.jsonl, one JSON object a line (`id`, `kind`, `text`, `property`, `chromium`, `firefox`); a gate that cites it
names the vector as `[record: <vector id>]` in the reason of the exception. This needs the pinned image and a container engine.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

OUT = os.path.join(ROOT, "tools", "conformance", "css_probe_answers.jsonl")
LISTS = ("CSS_SELECTOR_EXCEPTIONS", "CSS_VALUES_EXCEPTIONS", "CSS_CONDITIONS_EXCEPTIONS")


def rows(name):
    path = os.path.join(ROOT, "tools", "conformance", name)
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        yield line.rstrip("\n").split("\t")


def cases():
    out = []
    for c in rows("CSS_SELECTOR_EXCEPTIONS"):
        if c[4] == "wpt-tentative":
            out.append({"id": c[0], "kind": "selector", "text": c[1], "property": "", "ops": [["S", c[1] + "{color:red}"], ["V", "0", "cssText"]]})
    values = {}
    vec = os.path.join(ROOT, "build", "css-values-wpt-vectors.json")
    if os.path.exists(vec):
        for v in json.load(open(vec, encoding="utf-8")):
            values["%s#%d" % (v["file"], v["n"])] = v
    for c in rows("CSS_VALUES_EXCEPTIONS"):
        if "tentative" not in c[0]:
            continue
        v = values.get(c[0])
        if v is None:
            raise SystemExit("css_probe_record: no vector %s (run make conformance-css-values first)" % c[0])
        out.append({"id": c[0], "kind": "value", "text": v["value"], "property": v["prop"], "ops": [["S", "a{}"], ["P", "0", v["prop"], v["value"], ""], ["V", "0", "cssText"]]})
    for c in rows("CSS_CONDITIONS_EXCEPTIONS"):
        if c[3] == "wpt-tentative":
            out.append({"id": c[0], "kind": "supports", "text": "selector(::-webkit-slider-thumb)", "property": "", "ops": [["S", "@supports selector(::-webkit-slider-thumb){a{}}"], ["V", "0", "cssText"]]})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    cs = cases()
    if a.check:
        print("css_probe_record: %d cases" % len(cs))
        return 0
    import css_cssom_diff as d
    docs = [{"g": "probe", "ops": c["ops"]} for c in cs]
    answers = {}
    headers = {}
    for e in d.ENGINES:
        header, rs = d.run_browsers(e, docs)
        headers[e] = header
        answers[e] = [r["r"][-1] for r in rs]
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("#" + json.dumps({"cases": len(cs), "engines": headers}, ensure_ascii=False) + "\n")
        for i, c in enumerate(cs):
            f.write(json.dumps({"id": c["id"], "kind": c["kind"], "text": c["text"], "property": c["property"], "chromium": answers["chromium"][i], "firefox": answers["firefox"][i]}, ensure_ascii=False) + "\n")
    print("css_probe_record: wrote %d cases to %s" % (len(cs), a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
