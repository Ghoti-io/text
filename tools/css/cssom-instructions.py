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
"""The instruction cross-check of the CSSOM cost shapes (C5e).

    cssom-instructions.py [--bin DIR] [--scale K] [--measure] [--replay FILE] [--save FILE] [--controls]

The counters of the cost tests say what the library did. This runs each test of `testCssomInstructions` (tests/test-cssom-instructions.cpp: one shape each, its sizes multiplied
by GTEXT_COST_SCALE) under `valgrind --tool=callgrind` at scale 1 and at scale K (default 4) and compares the instructions the machine executed with
tools/conformance/CSS_CSSOM_KNOWN_COST: one row for each shape, its class (`linear`: work proportional to the size, so the instructions grow by at most 2K; `nlogn`: the same limit; `known-quadratic`:
the one shape that costs the size times the document until C6's incremental update, with its own ceiling), the largest ratio the shape may show from scale 1 to scale K, and the
largest number of instructions it may take at scale 1. **A ceiling only falls**: a measurement above a row fails, a row that no test has fails, a test with no row fails, and a
`known-quadratic` shape whose ratio fell to a linear one's fails too (the shape has changed, and the row says what it was). With --controls the same checks are made on the measured
numbers changed in each of those ways, and each must fail.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
KNOWN = os.path.join(ROOT, "tools", "conformance", "CSS_CSSOM_KNOWN_COST")
PROGRAM = "testCssomInstructions"
CLASSES = ("linear", "nlogn", "known-quadratic")


def tests_of(binary):
    out = subprocess.run([binary, "--gtest_list_tests"], capture_output=True, text=True, check=True).stdout.splitlines()
    suite, names = None, []
    for line in out:
        if not line.startswith(" "):
            suite = line.strip()
        elif line.strip():
            names.append(suite + line.strip())
    return names


def instructions(binary, name, scale):
    env = dict(os.environ, GTEXT_COST_SCALE=str(scale))
    done = subprocess.run(["valgrind", "--tool=callgrind", "--callgrind-out-file=/dev/null", binary, "--gtest_filter=" + name, "--gtest_brief=1"],
                          capture_output=True, text=True, env=env)
    m = re.search(r"Collected : (\d+)", done.stderr)
    if not m:
        raise SystemExit("cssom-instructions: callgrind collected nothing for %s:\n%s" % (name, done.stderr[-400:]))
    if "FAILED" in done.stdout:
        raise SystemExit("cssom-instructions: %s failed at scale %d:\n%s" % (name, scale, done.stdout[-400:]))
    return int(m.group(1))


def load_known(path):
    rows = {}
    problems = []
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 5:
            problems.append("%s:%d has %d fields, not five" % (os.path.basename(path), n, len(f)))
            continue
        name, cls, ratio, instr, reason = f
        if cls not in CLASSES:
            problems.append("%s:%d: class %r is not one of %s" % (os.path.basename(path), n, cls, ", ".join(CLASSES)))
        if len(reason.split()) < 8:
            problems.append("%s:%d: the reason is too short to be one" % (os.path.basename(path), n))
        rows[name] = (cls, float(ratio), int(instr), reason)
    return rows, problems


def judge(measured, known, scale):
    """The problems in the measured {test: (lo, hi)} against the known rows."""
    bad = []
    for name in sorted(measured):
        lo, hi = measured[name]
        ratio = hi / lo
        if name not in known:
            bad.append("UNLISTED %s: %d instructions at scale 1, x%.2f at scale %d" % (name, lo, ratio, scale))
            continue
        cls, max_ratio, max_instr, _ = known[name]
        if ratio > max_ratio:
            bad.append("RATIO %s grew x%.2f from scale 1 to %d, the row allows x%.2f" % (name, ratio, scale, max_ratio))
        if lo > max_instr:
            bad.append("INSTRUCTIONS %s took %d at scale 1, the row allows %d" % (name, lo, max_instr))
        if cls != "known-quadratic" and max_ratio > 2.0 * scale:
            bad.append("LIMIT %s is %s and its row allows x%.2f, above the 2K = x%.1f of linear work" % (name, cls, max_ratio, 2.0 * scale))
        if cls == "known-quadratic" and ratio < 1.25 * scale / 2:
            bad.append("SHAPE %s is listed as known-quadratic and grew only x%.2f: the shape has changed, edit the row" % (name, ratio))
    for name in sorted(known):
        if name not in measured:
            bad.append("STALE %s: a row, and no test of that name" % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "linux", "release", "apps"))
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--measure", action="store_true", help="print rows for the file instead of judging")
    ap.add_argument("--replay", help="judge the measurements of an earlier --save instead of running valgrind")
    ap.add_argument("--save")
    ap.add_argument("--controls", action="store_true")
    a = ap.parse_args()
    if a.replay:
        measured = {k: tuple(v) for k, v in json.load(open(a.replay)).items()}
    else:
        binary = os.path.join(a.bin, PROGRAM)
        measured = {}
        for name in tests_of(binary):
            measured[name] = (instructions(binary, name, 1), instructions(binary, name, a.scale))
            print("%-80s %12d %12d  x%.2f" % (name, measured[name][0], measured[name][1], measured[name][1] / measured[name][0]))
        if a.save:
            json.dump(measured, open(a.save, "w"))
    if a.measure:
        for name, (lo, hi) in sorted(measured.items()):
            print("%s\t?\t%.2f\t%d\t?" % (name, hi / lo, lo))
        return 0
    known, problems = load_known(KNOWN)
    bad = problems + judge(measured, known, a.scale)
    for b in bad:
        print(b)
    if bad:
        return 1
    print("cssom-instructions: %d shapes within CSS_CSSOM_KNOWN_COST (scale 1 and %d)" % (len(measured), a.scale))
    if not a.controls:
        return 0
    name = sorted(n for n in known if known[n][0] != "known-quadratic")[0]
    quad = next(n for n in sorted(known) if known[n][0] == "known-quadratic")
    lo, hi = measured[name]
    cases = {
        "RATIO": (dict(measured, **{name: (lo, hi * 4)}), known),
        "INSTRUCTIONS": (dict(measured, **{name: (lo * 100, hi * 100)}), known),
        "UNLISTED": (dict(measured, **{"CssomInstructions.NoSuchShape": (1000, 4000)}), known),
        "STALE": (measured, dict(known, **{"CssomInstructions.NoSuchRow": ("linear", 8.0, 1, "a row for a shape that no test of the program has")})),
        "SHAPE": (dict(measured, **{quad: (measured[quad][0], measured[quad][0] * 1.1)}), known),
    }
    for want, (m, k) in cases.items():
        found = judge(m, k, a.scale)
        if not any(f.startswith(want) for f in found):
            print("CONTROL %s: not reported (%r)" % (want, found[:2]))
            return 1
        print("control %s: caught" % want)
    return 0


if __name__ == "__main__":
    sys.exit(main())
