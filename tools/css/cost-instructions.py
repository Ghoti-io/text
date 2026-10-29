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
"""The instructions behind the cost tests of the condition and at-rule layers.

    cost-instructions.py [--bin DIR] [--scale K] [--limit R] [program ...]

The cost tests (`testCssAtruleCost`, `testCssMediaCost`) hold the growth of a counter, or of the time, to a bound. A counter sees only what its author put beside it, and
a time is noisy. This runs each test of each program under `valgrind --tool=callgrind` twice, with the sizes of its shapes multiplied by 1 and by K (GTEXT_COST_SCALE,
default 4), and compares the instructions executed: work linear in the size grows by about K, quadratic work by about K squared. A test whose instructions grow by more
than R (default 2 K) fails the run. The instructions are the machine's own work, so a copy or a rescan that no counter sees is in them.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PROGRAMS = ["testCssAtruleCost", "testCssMediaCost"]


def tests_of(binary):
    out = subprocess.run([binary, "--gtest_list_tests"], capture_output=True, text=True).stdout.splitlines()
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
        raise SystemExit("cost-instructions: callgrind collected nothing for %s:\n%s" % (name, done.stderr[-400:]))
    if "FAILED" in done.stdout:
        raise SystemExit("cost-instructions: %s failed at scale %d:\n%s" % (name, scale, done.stdout[-400:]))
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "linux", "release", "apps"))
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--limit", type=float)
    ap.add_argument("programs", nargs="*", default=PROGRAMS)
    a = ap.parse_args()
    limit = a.limit or 2.0 * a.scale
    bad = 0
    for prog in a.programs:
        binary = os.path.join(a.bin, prog)
        for name in tests_of(binary):
            lo, hi = instructions(binary, name, 1), instructions(binary, name, a.scale)
            ratio = hi / lo
            flag = ratio > limit
            bad += flag
            print("%-70s %12d %12d  x%.2f%s" % (name, lo, hi, ratio, "   FLAGGED" if flag else ""))
    print("cost-instructions: %d flagged (limit x%.1f at scale %d)" % (bad, limit, a.scale))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
