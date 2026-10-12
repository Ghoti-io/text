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
"""Score the HTML tree builder against the web-platform-tests tree-construction vectors.

    html_tree_suite.py <resources dir> <runner> <exceptions file> [<extra .dat>] [--propose] [--verbose]

The vectors are `html/syntax/parsing/resources/*.dat` of the pinned web-platform-tests
tree (tools/html/fetch.sh; html5lib-tests no longer keeps tree-construction vectors).
Each vector's input is parsed by the runner (html_tree_suite.c) and the tree it builds
is written in the vectors' own dump format and compared with the `#document` section.
`#errors` is compared by count and reported as its own score. It gates in two ways only: a floor on how
many vectors have exactly the listed count (HTML_TREE_MIN_EXACT_ERRORS), and no vector may have *fewer*
errors than listed. Where ours is higher (the vector lists none for a processing instruction, which the
tokenizer reports as an error) it is counted and named, not failed. The optional
extra file (tools/conformance/html_tree_extra.dat) holds hand-chosen cases in the same
format whose expected trees are what both reference parsers answer; they are scored like the
others, with no `#errors` comparison.

**Scope.** A vector is run when it is an initial-document vector (no
`#document-fragment`) and not one of the four `scripted_*.dat` files, whose trees are
what a script makes. One written for a scripting setting (`#script-on`, `#script-off`)
runs with it; one that names none runs with the flag off and with it on, and must give
the expected tree both times. Every run is also cut every way the runner knows (every
two-way split, a byte at a time, pieces of 2 to 9 bytes, random pieces) and the tree,
the errors and the status must be identical; the tree's invariant checker must hold.

**The exceptions file** (HTML_TREE_EXCEPTIONS) is the stage's honest scope: one line for
each vector that is not in scope yet, `<id>TAB<stage>TAB<what>TAB<reason>`, where the
stage (`3d` or `3e`) is the stage that will remove it and `what` is why it is out
(`table`, `template`, `foreign`, `fragment`). One more kind is not a stage: stage `none`,
what `dom`, which the tree builder cannot pass because the expected tree is made by a DOM
behaviour outside the parsing chapter (a fifth column locks the digest of the first
difference, so a different failure of the same vector cannot hide behind the entry). The
gate fails on

  UNEXPLAINED FAILURES  an in-scope vector that does not pass and is not listed, or that
                        is listed and now parses but gives the wrong tree
  STALE EXCEPTIONS      a listed vector that now passes, that now stops for another
                        reason or in another stage, or that does not exist
  below the floor       fewer vectors passing than HTML_TREE_MIN_PASS, or fewer loaded
                        than HTML_TREE_MIN_VECTORS

Copyright 2026 by Corey Pennycuff
"""
import collections
import hashlib
import os
import subprocess
import sys

SCRIPT_EXECUTING = ("scripted_adoption01.dat", "scripted_ark.dat", "scripted_foster01.dat",
                    "scripted_webkit01.dat")
SECTIONS = ("data", "errors", "new-errors", "document-fragment", "script-on", "script-off",
            "document", "description")


def parse_dat(path, name):
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    tests = []
    current = None
    section = None
    for line in lines:
        if line.startswith("#") and line[1:] in SECTIONS:
            heading = line[1:]
            if heading == "data":
                current = {s: [] for s in SECTIONS}
                current["flags"] = set()
                tests.append(current)
            if heading in ("script-on", "script-off"):
                current["flags"].add(heading)
            section = heading
            continue
        if current is not None and section is not None:
            current[section].append(line)
    out = []
    for n, t in enumerate(tests, 1):
        doc = t["document"]
        while doc and doc[-1] == "":
            doc.pop()
        flags = t["flags"]
        scripting = True if "script-on" in flags else False if "script-off" in flags else None
        # `#errors` is the count (html5lib's, tokenizer and tree together). `#new-errors` restates
        # the tokenizer's in the standard's names, so adding it counts them twice (it is 1129 of
        # 1256 passing vectors this way, 884 the other).
        errors = [l for l in t["errors"] if l.strip()]
        out.append({
            "id": "%s#%d" % (name, n),
            "data": "\n".join(t["data"]),
            "fragment": bool(t["document-fragment"]),
            "scripting": scripting,
            "expected": "\n".join(doc),
            "nerrors": len(errors),
        })
    return out


def load(dirpath):
    vectors, scripted, files = [], [], 0
    for name in sorted(os.listdir(dirpath)):
        if not name.endswith(".dat"):
            continue
        parsed = parse_dat(os.path.join(dirpath, name), name)
        if name in SCRIPT_EXECUTING:
            scripted.extend(parsed)
        else:
            files += 1
            vectors.extend(parsed)
    return vectors, scripted, files


def load_exceptions(path):
    table = {}
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#"):
                continue
            parts = raw.split("\t")
            ok = len(parts) >= 4 and (parts[1] in ("3d", "3e") and len(parts) == 4
                                      or parts[1] == "none" and len(parts) == 5)
            if not ok or not parts[3].strip():
                raise SystemExit("%s:%d: not <id> <3d|3e> <what> <reason> (or <id> none dom <reason> <digest>)" % (path, n))
            if parts[0] in table:
                raise SystemExit("%s:%d: %s is listed twice" % (path, n, parts[0]))
            table[parts[0]] = (parts[1], parts[2], parts[3], parts[4] if len(parts) == 5 else None)
    return table


def hexs(s):
    return "x" + s.encode("utf-8", "surrogatepass").hex()


LAST_STDERR = ""  # what the runner said on stderr the last time it failed (a sanitizer report, an abort)
TIMEOUT = int(os.environ.get("HTML_TREE_RUNNER_TIMEOUT", "900"))


def run_batch(runner, requests):
    global LAST_STDERR
    data = ("\n".join(requests) + "\n").encode("ascii")
    try:
        done = subprocess.run([runner], input=data, capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        LAST_STDERR = "the runner did not answer %d requests within %d seconds" % (len(requests), TIMEOUT)
        return None, -999
    if done.returncode != 0:
        LAST_STDERR = done.stderr.decode("utf-8", "replace")[-1500:]
        return None, done.returncode
    answers, cur = [], []
    for line in done.stdout.decode("utf-8", "replace").split("\n"):
        if not line:
            continue
        cur.append(line)
        if line.startswith("END "):
            answers.append(cur)
            cur = []
    if len(answers) != len(requests):
        return None, 0
    return answers, 0


def run_all(runner, requests):
    """Answers for every request; a request that kills the runner is isolated and reported."""
    answers = [None] * len(requests)
    crashed = []
    run_all.notes = {}
    step = 200
    for lo in range(0, len(requests), step):
        part = requests[lo:lo + step]
        got, _ = run_batch(runner, part)
        if got is not None:
            answers[lo:lo + len(part)] = got
            continue
        for i, r in enumerate(part):
            one, _ = run_batch(runner, [r])
            if one is None:
                crashed.append(lo + i)
                run_all.notes[lo + i] = LAST_STDERR
            else:
                answers[lo + i] = one[0]
    return answers, crashed


def parse_answer(lines):
    a = {"status": None, "notyet": None, "errors": 0, "dump": None, "cut": None, "inv": "OK",
         "end": None, "mode": None}
    for line in lines:
        f = line.split(" ")
        if f[0] == "STATUS":
            a["status"] = f[1]
        elif f[0] == "NOTYET":
            a["notyet"] = (f[1], f[2])
        elif f[0] == "ERRORS":
            a["errors"] = int(f[1]) + int(f[2])
        elif f[0] == "MODE":
            a["mode"] = f[1]
        elif f[0] == "INVARIANTS":
            a["inv"] = " ".join(f[1:])
        elif f[0] == "DUMP":
            a["dump"] = bytes.fromhex(f[1][1:]).decode("utf-8", "surrogatepass")
        elif f[0] == "CUT":
            a["cut"] = " ".join(f[1:])
        elif f[0] == "END":
            a["end"] = f[1]
    return a


def first_diff(expected, got):
    a, b = expected.split("\n"), got.split("\n")
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else "(end)"
        y = b[i] if i < len(b) else "(end)"
        if x != y:
            return "line %d: expected %r, got %r" % (i + 1, x[:100], y[:100])
    return "identical"


def v_has_errors(by_id, vid):
    return by_id[vid]["nerrors"] is not None


def digest(text):
    return hashlib.sha256(text.encode("utf-8", "surrogatepass")).hexdigest()[:10]


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    propose = "--propose" in argv
    verbose = "--verbose" in argv
    if len(args) not in (3, 4):
        raise SystemExit(__doc__)
    resources, runner, exceptions_path = args[:3]
    min_vectors = int(os.environ.get("HTML_TREE_MIN_VECTORS", "1900"))
    min_pass = int(os.environ.get("HTML_TREE_MIN_PASS", "0"))
    min_exact = int(os.environ.get("HTML_TREE_MIN_EXACT_ERRORS", "0"))
    vectors, scripted, files = load(resources)
    listed = load_exceptions(exceptions_path)
    extras = 0
    if len(args) == 4:
        more = parse_dat(args[3], os.path.basename(args[3]))
        for v in more:
            v["nerrors"] = None
        extras = len(more)
        vectors.extend(more)

    # Requests: each scripting setting a vector is run under.
    requests, owners = [], []
    for v in vectors:
        if v["fragment"]:
            continue
        settings = [v["scripting"]] if v["scripting"] is not None else [False, True]
        for sc in settings:
            requests.append("cuts %d %s" % (1 if sc else 0, hexs(v["data"])))
            owners.append((v, sc))
    answers, crashed = run_all(runner, requests)

    known_by_id = {v["id"]: v for v in vectors}
    outcome = {}  # id -> ("pass"|"notyet"|"fail"|"fragment", detail)
    err_exact = err_total = 0
    err_higher = collections.Counter()
    higher_ids = set()
    err_lower = []
    for v in vectors:
        if v["fragment"]:
            outcome[v["id"]] = ("fragment", ("3e", "fragment"))
    per = collections.defaultdict(list)
    for i, (v, sc) in enumerate(owners):
        per[v["id"]].append((i, v, sc))
    for vid, runs in per.items():
        problem = None
        notyet = None
        errs_ok = True
        for i, v, sc in runs:
            if i in crashed or answers[i] is None:
                problem = "the runner crashed or timed out (scripting %s): %s" % (
                    sc, getattr(run_all, "notes", {}).get(i, "")[-400:].replace("\n", " | "))
                break
            a = parse_answer(answers[i])
            if a["end"] != "DONE":
                problem = "the runner answered %s" % a["end"]
                break
            if a["cut"] != "OK":
                problem = "THE ANSWER DEPENDS ON HOW THE INPUT WAS CUT: %s" % a["cut"]
                break
            if a["status"] == "E_NOT_YET":
                notyet = notyet or a["notyet"]
                continue
            if a["status"] != "OK":
                problem = "status %s (scripting %s)" % (a["status"], sc)
                break
            if a["inv"] != "OK":
                problem = "invariant checker: %s" % a["inv"]
                break
            got = a["dump"][:-1] if a["dump"].endswith("\n") else a["dump"]
            if got != v["expected"]:
                problem = "scripting %s, %s" % (sc, first_diff(v["expected"], got))
                break
            if v["nerrors"] is not None and a["errors"] != v["nerrors"]:
                errs_ok = False
                if a["errors"] > v["nerrors"]:
                    higher_ids.add(v["id"])
                elif v["id"] not in err_lower:
                    err_lower.append(v["id"])
        if problem:
            outcome[vid] = ("fail", problem)
        elif notyet:
            outcome[vid] = ("notyet", (("3d" if notyet[1] == "3d" else "3e"), notyet[0]))
        else:
            outcome[vid] = ("pass", errs_ok)
            if v_has_errors(known_by_id, vid):
                err_total += 1
                err_exact += 1 if errs_ok else 0

    known = {v["id"]: v for v in vectors}
    for vid in higher_ids:
        err_higher[vid.split("#")[0]] += 1
    if propose:
        for vid in sorted(outcome, key=lambda k: (k.split("#")[0], int(k.split("#")[1]))):
            kind, detail = outcome[vid]
            if kind == "fail":
                print("%s\tnone\tdom\tUNREAD: %s\t%s" % (vid, detail[:100], digest(detail)))
            if kind in ("notyet", "fragment"):
                stage, what = detail
                why = {"table": "needs the table insertion modes",
                       "template": "needs the template insertion modes",
                       "foreign": "needs foreign content (svg, math)",
                       "fragment": "a fragment vector (the fragment parsing algorithm)"}[what]
                print("%s\t%s\t%s\t%s" % (vid, stage, what, why))
        return 0

    unexplained = []
    stale = []
    passes = 0
    for vid, (kind, detail) in outcome.items():
        tag = listed.get(vid)
        if kind == "pass":
            passes += 1
            if tag:
                stale.append("%s  now passes (listed %s %s)" % (vid, tag[0], tag[1]))
        elif kind == "fail":
            if tag and tag[0] == "none" and tag[3] == digest(detail):
                continue
            if tag and tag[0] == "none":
                stale.append("%s  fails differently now: %s [%s]" % (vid, detail[:100], digest(detail)))
                continue
            unexplained.append("%s  %s  [%s]" % (vid, detail, digest(detail)))
        else:
            if not tag:
                unexplained.append("%s  stops at %s (%s) and is not listed" % (vid, detail[1], detail[0]))
            elif tag[0] == "none" or (tag[0], tag[1]) != detail:
                stale.append("%s  listed %s %s, now %s %s" % (vid, tag[0], tag[1], detail[0], detail[1]))
    for vid in listed:
        if vid not in known:
            stale.append("%s  is not a vector" % vid)
    scored = sum(1 for k, _ in outcome.values() if k in ("pass", "fail"))
    in_scope_failing = sum(1 for vid, (k, _) in outcome.items() if k == "fail" and vid not in listed)
    stages = collections.Counter(listed[v][0] for v in listed)
    whats = collections.Counter(listed[v][1] for v in listed)
    print("vectors: %d in %d files, and %d extra (+%d not run: %s)" % (
        len(vectors) - extras, files, extras, len(scripted), ", ".join(SCRIPT_EXECUTING)))
    print("in scope (initial document): %d; passing: %d; listed as not parsing-chapter behaviour: %d; failing: %d" % (
        scored, passes, scored - passes - in_scope_failing, in_scope_failing))
    print("tagged exceptions: %d (%s); by reason: %s" % (
        len(listed), ", ".join("%s %d" % kv for kv in sorted(stages.items())),
        ", ".join("%s %d" % kv for kv in sorted(whats.items()))))
    print("#errors by count, on the %d passing vectors: %d exact; ours higher on %d (%s), lower on %d" % (
        err_total, err_exact, sum(err_higher.values()),
        ", ".join("%s %d" % kv for kv in err_higher.most_common(3)), len(err_lower)))
    failed = False
    if unexplained:
        failed = True
        sys.stderr.write("\033[0;31m\n### UNEXPLAINED FAILURES (%d) ###\033[0m\n" % len(unexplained))
        for line in unexplained[:60 if not verbose else 100000]:
            sys.stderr.write("  %s\n" % line)
    if stale:
        failed = True
        sys.stderr.write("\033[0;31m\n### STALE EXCEPTIONS (%d) ###\033[0m\n" % len(stale))
        for line in stale[:60 if not verbose else 100000]:
            sys.stderr.write("  %s\n" % line)
    if len(vectors) < min_vectors:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d vectors loaded, floor %d ###\033[0m\n" % (
            len(vectors), min_vectors))
    if err_exact < min_exact:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d vectors with the exact #errors count, floor %d ###\033[0m\n" % (
            err_exact, min_exact))
    if err_lower:
        failed = True
        sys.stderr.write("\033[0;31m\n### UNEXPLAINED ERROR COUNTS: fewer parse errors than the vector lists (%d) ###\033[0m\n" % len(err_lower))
        for vid in err_lower[:20]:
            sys.stderr.write("  %s\n" % vid)
    if passes < min_pass:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d passing, floor %d ###\033[0m\n" % (passes, min_pass))
    if failed:
        return 3
    print("html tree: every in-scope vector passes or is a tagged exception; none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
