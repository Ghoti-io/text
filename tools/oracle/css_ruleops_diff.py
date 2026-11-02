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
"""The rule-operations differential: `insertRule()`, `deleteRule()`, `appendRule()`, `findRule()` and the keyframes `deleteRule(select)` in this library, Chromium (Blink) and
Firefox (Gecko, whose style engine is Stylo).

    css_ruleops_diff.py --ours BINARY --work DIR [--seed N] [--count M] [--stage populate|browsers|ours|classify|all] [--replay DIR] [--list FILE]

A document (css_ruleops_gen.py) is a sequence of operations on a style sheet and the rules in it. The three readers run it and answer, for each operation, "ok" or the name of the
exception it threw (for `findRule` the text of the rule found), and then the text of every rule of the sheet that is left. The driver is containers/cssprop/rules_driver.mjs
(WebDriver BiDi, the pinned image, no network); the first documents of the population are the pairs of adjacent conditions of the lists of 'The order of checks' of the
specification, so each pair is asked of both browsers in every run.

**What is a divergence.** An operation the three do not answer alike, or, when every operation is answered alike, a sheet whose rules the three do not write alike. By who differs
it is `ours` (the browsers agree and this library does not), `browsers` (they disagree with each other; the library is with one of them) or `both` (it is with neither of two that
disagree). Each is abstracted to a **feature** (the kind of operation, the three answers; or the kind of the first rule that is written differently), and the list
(css_ruleops_divergences.txt) gives each feature its class, its count in the default population (seed 1, 200 documents) and its reason, a sentence of the served text or the
browsers' known deviation, in the classes of the other differentials. **None may stay unexplained.** The gate fails (exit 3) on a divergence whose feature is not listed, a listed
feature that does not occur, a count that moved, and a reason that is short or blanket. A divergence the specification's text decides against both browsers is `served-text`: it
says what the draft says and what each browser answers.

--replay DIR reads the browsers' answers from an earlier run's directory instead of asking the browsers, and this library is asked again.
"""

import argparse
import collections
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import css_ruleops_gen  # noqa: E402
import oracle_env  # noqa: E402

GATE_EXIT = 3
DRIVER = os.path.join(HERE, "containers", "cssprop", "rules_driver.mjs")
ENGINES = ["chromium", "firefox"]
CLASSES = ("served-text", "standard-newer", "reference-old", "reference-wrong", "reference-narrow", "legacy-compat", "browsers-differ")
LIST = os.path.join(HERE, "css_ruleops_divergences.txt")


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def population(work, seed, count):
    path = os.path.join(work, "population.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        for i in range(count):
            f.write(json.dumps(css_ruleops_gen.Gen(seed, i).document(), ensure_ascii=False) + "\n")
    return path


def load_population(work):
    return [json.loads(line) for line in open(os.path.join(work, "population.jsonl"), encoding="utf-8")]


def ops_tsv(docs):
    out = []
    for d in docs:
        out.append("D")
        for op in d["ops"]:
            cells = [op[0]] + [("-" if v is None else str(v)) for v in op[1:]]
            out.append("\t".join(esc(c) for c in cells))
    return "\n".join(out) + "\n"


def run_browsers(engine, docs):
    inner = ["sh", "-c", 'cd /opt/cssprop && exec node "%s" %s' % (DRIVER, engine)]
    data = "".join(json.dumps(d, ensure_ascii=False) + "\n" for d in docs)
    run = subprocess.run(oracle_env.command("cssprop", inner), input=data.encode("utf-8", "surrogatepass"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_ruleops_diff: the %s driver exited %d:\n%s" % (engine, run.returncode, oracle_env.reference_stderr(run.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    header = json.loads(lines[0][1:])
    rows = [json.loads(x) for x in lines[1:]]
    if len(rows) != len(docs):
        raise SystemExit("css_ruleops_diff: the %s driver answered %d documents to %d" % (engine, len(rows), len(docs)))
    return header, rows


def run_ours(binary, docs):
    run = subprocess.run([binary], input=ops_tsv(docs).encode("utf-8", "surrogatepass"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_ruleops_diff: the library's reader exited %d:\n%s" % (run.returncode, run.stderr.decode("utf-8", "replace")[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    rows = [json.loads(x) for x in lines]
    if len(rows) != len(docs):
        raise SystemExit("css_ruleops_diff: the library's reader answered %d documents to %d" % (len(rows), len(docs)))
    return rows


def rule_kind(text):
    """The part that names a rule: its at-rule, or style."""
    if text.startswith("@"):
        return text.split()[0].split("{")[0].split(";")[0]
    return "style"


def divergences(docs, ans):
    """The divergences: (feature, who, witness), at most one for a document. After the first operation the three answer differently the sheets are no longer the same sheet, so
    what follows is a consequence and is not counted; a document whose operations are all answered alike is compared by the text of the rules it leaves."""
    out = []
    for i, d in enumerate(docs):
        c, f, o = ans["chromium"][i], ans["firefox"][i], ans["ours"][i]
        first = None
        for k, op in enumerate(d["ops"]):
            a = (c["r"][k], f["r"][k], o["r"][k])
            if len(set(json.dumps(x) for x in a)) == 1:
                continue
            if op[0] == "F":
                a = tuple(("text" if x is not None else "null") for x in a)
                if len(set(a)) == 1:
                    continue
            who = "ours" if a[0] == a[1] else ("browsers" if a[2] in (a[0], a[1]) else "both")
            first = ("%s:%s/%s/%s" % (op[0], a[0], a[1], a[2]), who, "doc %d op %d %s" % (i, k, json.dumps(op)))
            break
        if first:
            out.append(first)
            continue
        ts = (c["t"], f["t"], o["t"])
        if len(set(json.dumps(x) for x in ts)) == 1:
            continue
        for j in range(max(len(x) for x in ts)):
            cell = tuple(x[j] if j < len(x) else None for x in ts)
            if len(set(cell)) == 1:
                continue
            kind = "@keyframes" if any(x and "@keyframes" in x for x in cell) else rule_kind(next(x for x in cell if x is not None))
            who = "ours" if cell[0] == cell[1] else ("browsers" if cell[2] in (cell[0], cell[1]) else "both")
            out.append(("rules:%s" % kind, who, "doc %d rule %d" % (i, j)))
            break
    return out


def load_list(path):
    rows = {}
    problems = []
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 4:
            problems.append("%s:%d has %d fields, not four" % (os.path.basename(path), n, len(f)))
            continue
        feature, cls, count, reason = f
        if cls not in CLASSES:
            problems.append("%s:%d: class %r is not one of %s" % (os.path.basename(path), n, cls, ", ".join(CLASSES)))
        if len(reason.split()) < 8:
            problems.append("%s:%d: the reason is too short to be an explanation" % (os.path.basename(path), n))
        rows[feature] = (cls, int(count), reason)
    return rows, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("CSS_RULEOPS_ORACLE_SEED", "1")))
    ap.add_argument("--count", type=int, default=int(os.environ.get("CSS_RULEOPS_ORACLE_COUNT", "200")))
    ap.add_argument("--stage", default="all", choices=["populate", "browsers", "ours", "classify", "all"])
    ap.add_argument("--replay")
    ap.add_argument("--list", default=LIST)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    stages = ["populate", "browsers", "ours", "classify"] if a.stage == "all" else [a.stage]
    if "populate" in stages:
        population(a.work, a.seed, a.count)
    docs = load_population(a.work)
    if "browsers" in stages:
        if a.replay:
            for e in ENGINES:
                json.dump(json.load(open(os.path.join(a.replay, "answers-%s.json" % e))), open(os.path.join(a.work, "answers-%s.json" % e), "w"))
        else:
            for e in ENGINES:
                header, rows = run_browsers(e, docs)
                json.dump(rows, open(os.path.join(a.work, "answers-%s.json" % e), "w"), ensure_ascii=False)
                json.dump(header, open(os.path.join(a.work, "header-%s.json" % e), "w"), ensure_ascii=False)
    if "ours" in stages:
        json.dump(run_ours(a.ours, docs), open(os.path.join(a.work, "answers-ours.json"), "w"), ensure_ascii=False)
    if "classify" in stages:
        ans = {e: json.load(open(os.path.join(a.work, "answers-%s.json" % e))) for e in ENGINES + ["ours"]}
        found = divergences(docs, ans)
        counts = collections.Counter(f for f, _, _ in found)
        witness = {}
        for f, who, w in found:
            witness.setdefault(f, (who, w))
        listed, problems = load_list(a.list)
        default_population = a.seed == 1 and a.count == 200
        bad = []
        for f in sorted(counts):
            who = witness[f][0]
            if f not in listed:
                bad.append("UNLISTED %s (%s) x%d, e.g. %s" % (f, who, counts[f], witness[f][1]))
            else:
                cls = listed[f][0]
                if who == "ours" and cls != "served-text":
                    bad.append("OURS %s x%d: a divergence in which the browsers agree and this library does not is a defect to fix, or the served text decides against both (class served-text); e.g. %s" % (f, counts[f], witness[f][1]))
                if default_population and listed[f][1] != counts[f]:
                    bad.append("COUNT %s: listed %d, found %d" % (f, listed[f][1], counts[f]))
        if default_population:
            for f in sorted(listed):
                if f not in counts and listed[f][1] > 0:
                    bad.append("STALE %s: listed, and it does not occur" % f)
        print("css ruleops differential: %d documents, %d operations; %d divergences in %d features" % (len(docs), sum(len(d["ops"]) for d in docs), len(found), len(counts)))
        for p in problems:
            print("LIST PROBLEM " + p)
        for b in bad:
            print(b)
        if bad or problems:
            return GATE_EXIT
        print("css ruleops differential: agrees, or every divergence is listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
