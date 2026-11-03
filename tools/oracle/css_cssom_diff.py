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
"""The CSSOM differential: the mutation sequences of tools/oracle/css_cssom_gen.py in this library, Chromium (Blink) and Firefox (Gecko, whose style engine is Stylo).

    css_cssom_diff.py --ours BINARY --work DIR [--seed N] [--count M] [--stage populate|browsers|ours|classify|all] [--replay DIR] [--list FILE]

A document is a sequence of operations on style sheets, their rules, declarations and media lists, on constructed and adopted sheets and on `matchMedia()` (the ops are listed in
css_cssom_ours.cpp). The three readers run it and answer, for each operation, "ok", the name of the exception it threw or the string it read, and then, for every sheet the document
made, the text of its rules, its media text and its `disabled`. The driver is containers/cssprop/cssom_driver.mjs (WebDriver BiDi, the pinned image, no network). The first documents of
the population are fixed: the pairs of adjacent conditions of the lists of 'The order of checks', the page properties of Paged Media 3 Appendix A, the nested-rule runs and preserved kinds, and the `cssText` setters, so each is asked of both browsers in every run.

**What is a divergence.** An operation the three do not answer alike, or, when every operation is answered alike, a sheet the three do not write alike. By who differs it is `ours` (the
browsers agree and this library does not), `browsers` (they disagree with each other; the library is with one of them) or `both` (it is with neither of two that disagree). Each is
abstracted to a **feature** (the kind of operation, what it read, and the pattern of the three answers: the exception names, or which of them are alike when the answers are texts), and
the list (css_cssom_divergences.txt) gives each feature its class, its count in the default population (seed 1, 600 documents) and its reason, a sentence of the served text or the
browsers' known deviation, in the classes of the other differentials. **None may stay unexplained.** The gate fails (exit 3) on a divergence whose feature is not listed, a listed feature
that does not occur, a count that moved, and a reason that is short or blanket. The classes are `served-text` and `browsers-differ`, and the names the other differentials of the library use (`standard-newer`,
`reference-old`, `reference-wrong`, `reference-narrow`, `legacy-compat`), which TABLE below maps onto the first two for the report; the number of independent lineages is two (Blink and
Gecko), and a divergence between them is `browsers-differ` unless the served text decides it.

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

import css_cssom_gen  # noqa: E402
import oracle_env  # noqa: E402

GATE_EXIT = 3
DRIVER = os.path.join(HERE, "containers", "cssprop", "cssom_driver.mjs")
ENGINES = ["chromium", "firefox"]
CLASSES = ("served-text", "standard-newer", "reference-old", "reference-wrong", "reference-narrow", "legacy-compat", "env-fixed", "browsers-differ", "ours")
# a divergence in which the browsers agree and this library does not is allowed in these classes: the library follows a served sentence (or a newer draft) they do not. Class `ours` is a
# defect of the library with an owner, and there may be at most MAX_OURS of them (a ceiling that only goes down and ends at 0).
OURS_OK = ("served-text", "standard-newer", "reference-old", "env-fixed")
MAX_OURS = int(os.environ.get("CSS_CSSOM_ORACLE_MAX_OURS", "0"))
LIST = os.path.join(HERE, "css_cssom_divergences.txt")
# the other differentials' names of a class, and what each is for the report (one vocabulary: the first two are the classes `served-text` and `browsers-differ`)
TABLE = {"standard-newer": "served-text", "reference-old": "browsers-differ", "reference-wrong": "browsers-differ", "reference-narrow": "browsers-differ", "legacy-compat": "browsers-differ", "env-fixed": "browsers-differ"}


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def population(work, seed, count):
    path = os.path.join(work, "population.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        for i in range(count):
            f.write(json.dumps(css_cssom_gen.Gen(seed, i).document(), ensure_ascii=False) + "\n")
        # after the `count` documents, so that the documents before them are the ones a population of that size always had
        for d in css_cssom_gen.media_general_documents():
            f.write(json.dumps(d, ensure_ascii=False) + "\n")
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
        raise SystemExit("css_cssom_diff: the %s driver exited %d:\n%s" % (engine, run.returncode, oracle_env.reference_stderr(run.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    header = json.loads(lines[0][1:])
    rows = [json.loads(x) for x in lines[1:]]
    if len(rows) != len(docs):
        raise SystemExit("css_cssom_diff: the %s driver answered %d documents to %d" % (engine, len(rows), len(docs)))
    return header, rows


def run_ours(binary, docs):
    run = subprocess.run([binary], input=ops_tsv(docs).encode("utf-8", "surrogatepass"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_cssom_diff: the library's reader exited %d:\n%s" % (run.returncode, run.stderr.decode("utf-8", "replace")[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    rows = [json.loads(x) for x in lines]
    if len(rows) != len(docs):
        raise SystemExit("css_cssom_diff: the library's reader answered %d documents to %d" % (len(rows), len(docs)))
    return rows


def rule_kind(text):
    """The part that names a rule: its at-rule, or style."""
    if text.startswith("@"):
        return text.split()[0].split("{")[0].split(";")[0]
    return "style"


SHORT = set(["ok", "true", "false", "undefined", "null", "hang", "", "0", "1"])


def short(x):
    if x is None:
        return "null"
    if x in SHORT or (x.isalpha() and len(x) < 40 and (x.endswith("Error") or x.endswith("error"))):
        return x or "empty"
    return None


def pattern(a):
    """The pattern of three answers (chromium, firefox, ours): the short answers as they are; texts as which of them are alike."""
    names = ("C", "F", "O")
    if all(short(x) is not None for x in a):
        return "/".join(short(x) for x in a)
    groups = {}
    for n, x in zip(names, a):
        groups.setdefault(json.dumps(x), []).append(n)
    return "text:" + "|".join("".join(g) for g in sorted(groups.values()))


def who_differs(a):
    return "ours" if a[0] == a[1] else ("browsers" if a[2] in (a[0], a[1]) else "both")


def op_name(op):
    return op[0] + (":" + str(op[2]) if op[0] in ("V", "E") else "")


def kept(text):
    """Whether the text of a rule read after a declaration was put in it still holds one: a block with nothing in it is `{ }`."""
    return isinstance(text, str) and ": " in text.split("{", 1)[-1]


def divergences(docs, ans):
    """The divergences: (feature, who, witness), at most one for a document. After the first operation the three answer differently the sheets are no longer the same, so what follows is a
    consequence and is not counted; a document whose operations are all answered alike is compared by the sheets it leaves."""
    out = []
    for i, d in enumerate(docs):
        c, f, o = ans["chromium"][i], ans["firefox"][i], ans["ours"][i]
        group = d.get("g", "r")
        if group == "page-prop":
            ctx, prop, listed = d["p"]
            bits = tuple(1 if kept(x["r"][1]) else 0 for x in (c, f, o))
            if len(set(bits)) != 1 or (bits[2] == 1) != (listed == "listed"):
                out.append(("page-prop:%s:%s:C%dF%dO%d" % (ctx, listed, *bits), who_differs(bits), "doc %d %s %s" % (i, ctx, prop)))
            continue
        first = None
        for k, op in enumerate(d["ops"]):
            a = (c["r"][k], f["r"][k], o["r"][k])
            if len(set(json.dumps(x) for x in a)) == 1:
                continue
            if op[0] == "F":
                a = tuple(("text" if x is not None else "null") for x in a)
                if len(set(a)) == 1:
                    continue
            first = ("%s:%s:%s" % (group, op_name(op), pattern(a)), who_differs(a), "doc %d op %d %s" % (i, k, json.dumps(op)))
            break
        if first:
            out.append(first)
            continue
        ts = (c["t"], f["t"], o["t"])
        if len(set(json.dumps(x) for x in ts)) == 1:
            continue
        if len(set(len(x) for x in ts)) != 1:
            out.append((group + ":state:sheets", who_differs([json.dumps(x) for x in ts]), "doc %d" % i))
            continue
        done = False
        for s in range(len(ts[0])):
            cells = [x[s] for x in ts]
            for field, label in (("m", "media"), ("d", "disabled")):
                cell = tuple(json.dumps(x[field]) for x in cells)
                if len(set(cell)) != 1:
                    out.append(("%s:state:%s" % (group, label), who_differs(cell), "doc %d sheet %d" % (i, s)))
                    done = True
                    break
            if done:
                break
            rows = [x["r"] for x in cells]
            for j in range(max(len(x) for x in rows)):
                cell = tuple(x[j] if j < len(x) else None for x in rows)
                if len(set(cell)) == 1:
                    continue
                kind = "@keyframes" if any(x and "@keyframes" in x for x in cell) else rule_kind(next(x for x in cell if x is not None))
                out.append(("%s:rules:%s:%s" % (group, kind, pattern(cell)), who_differs(cell), "doc %d sheet %d rule %d" % (i, s, j)))
                done = True
                break
            if done:
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
        if cls not in CLASSES + tuple(TABLE):
            problems.append("%s:%d: class %r is not one of %s" % (os.path.basename(path), n, cls, ", ".join(CLASSES)))
        if len(reason.split()) < 8:
            problems.append("%s:%d: the reason is too short to be an explanation" % (os.path.basename(path), n))
        rows[feature] = (cls, int(count), reason)
    return rows, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("CSS_CSSOM_ORACLE_SEED", "1")))
    ap.add_argument("--count", type=int, default=int(os.environ.get("CSS_CSSOM_ORACLE_COUNT", "600")))
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
        for e in ENGINES:
            mine = os.path.join(a.work, "answers-%s.json" % e)
            if a.replay and not os.path.exists(mine):
                json.dump(json.load(open(os.path.join(a.replay, "answers-%s.json" % e))), open(mine, "w"))
        ans = {e: json.load(open(os.path.join(a.work, "answers-%s.json" % e))) for e in ENGINES + ["ours"]}
        found = divergences(docs, ans)
        counts = collections.Counter(f for f, _, _ in found)
        witness = {}
        for f, who, w in found:
            witness.setdefault(f, (who, w))
        listed, problems = load_list(a.list)
        default_population = a.seed == 1 and a.count == 600
        bad = []
        for f in sorted(counts):
            who = witness[f][0]
            if f not in listed:
                bad.append("UNLISTED %s (%s) x%d, e.g. %s" % (f, who, counts[f], witness[f][1]))
                if who == "ours":
                    bad.append("OURS %s x%d: the browsers agree and this library does not; e.g. %s" % (f, counts[f], witness[f][1]))
            else:
                cls = listed[f][0]
                if who == "ours" and cls not in OURS_OK + ("ours",):
                    bad.append("OURS %s x%d: a divergence in which the browsers agree and this library does not is a defect to fix, or the served text decides against both (class served-text); e.g. %s" % (f, counts[f], witness[f][1]))
                if default_population and listed[f][1] != counts[f]:
                    bad.append("COUNT %s: listed %d, found %d" % (f, listed[f][1], counts[f]))
        if default_population:
            for f in sorted(listed):
                if f not in counts and listed[f][1] > 0:
                    bad.append("STALE %s: listed, and it does not occur" % f)
        owned = [f for f in listed if listed[f][0] == "ours"]
        if len(owned) > MAX_OURS:
            bad.append("OURS-CEILING %d features are of class ours, the ceiling is %d" % (len(owned), MAX_OURS))
        print("css cssom differential: %d documents, %d operations; %d divergences in %d features" % (len(docs), sum(len(d["ops"]) for d in docs), len(found), len(counts)))
        for p in problems:
            print("LIST PROBLEM " + p)
        for b in bad:
            print(b)
        if bad or problems:
            return GATE_EXIT
        by_class = collections.Counter(TABLE.get(listed[f][0], listed[f][0]) for f in counts if f in listed)
        print("css cssom differential: agrees, or every divergence is listed; two independent lineages (Blink, Gecko); features by class: %s" % ", ".join("%s %d" % kv for kv in sorted(by_class.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
