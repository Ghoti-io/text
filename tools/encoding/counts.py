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
"""Print the counts that the notes and the manual quote about the Encoding module's checks, measured from the files that define them.

    tools/encoding/counts.py

The number of plants, of seeds, of lines of the divergence list and of tests are written down once, in the plan record (notes/text/XML-HTML-PLAN.md, "Phase 3i as
built"), from this script's output; every other place says what is counted and where to see it. Re-run it when one of the lists changes.

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def lines_of(path):
    with open(path, encoding="utf-8") as fh:
        return [l.rstrip("\n") for l in fh if l.strip() and not l.startswith("#")]


def main():
    oracle = lines_of(os.path.join(ROOT, "tools", "oracle", "ENCODING_ORACLE_PLANTS"))
    unit = lines_of(os.path.join(HERE, "UNIT_PLANTS"))
    print("plants of the differential's controls (ENCODING_ORACLE_PLANTS): %d" % len(oracle))
    print("plants only a unit test can see (UNIT_PLANTS): %d" % len(unit))
    print("plants the unit tests are held to (both lists): %d" % (len(oracle) + len(unit)))
    # The fuzzer's plants: import the script's table without running it.
    sys.path.insert(0, HERE)
    import importlib.util
    spec = importlib.util.spec_from_file_location("plant_fuzz", os.path.join(HERE, "plant-fuzz.py"))
    pf = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pf)
    cjk = [n for n in pf.PLANTS if n.startswith("cjk-")]
    print("plants of the fuzzer (plant-fuzz.py): %d, of them %d in the multi-byte codecs, %d replayed from a reproducer" % (len(pf.PLANTS), len(cjk), len(pf.REPRODUCERS)))
    corpus = os.path.join(ROOT, "tests", "fuzz", "corpus", "encoding")
    names = [f for f in os.listdir(corpus) if f.endswith(".seed")]
    repro = [f for f in names if f.startswith("reproducer-")]
    with open(os.path.join(HERE, "FUZZ_SEED_COUNT")) as fh:
        generated = int(fh.read())
    print("fuzz seeds: %d generated (FUZZ_SEED_COUNT), %d reproducers (%d of them of the multi-byte codecs)" % (generated, len(repro), sum(1 for f in repro if "cjk" in f)))
    entries = lines_of(os.path.join(ROOT, "tools", "oracle", "encoding_divergences.txt"))
    mb = [l for l in entries if l.split("\t")[1].startswith("mb-")]
    by_ref = {}
    for l in mb:
        by_ref[l.split("\t")[0]] = by_ref.get(l.split("\t")[0], 0) + 1
    print("divergence list: %d lines, %d of the first release's populations and %d of the multi-byte ones (%s)" % (
        len(entries), len(entries) - len(mb), len(mb), ", ".join("%s %d" % kv for kv in sorted(by_ref.items()))))
    total_pass = total_fail = files = 0
    for l in lines_of(os.path.join(ROOT, "tools", "oracle", "encoding_wpt_expected.txt")):
        f, p, x = l.split("\t")
        total_pass += int(p)
        total_fail += int(x)
        files += 1
    print("standard's own tests: %d files, %d tests, %d pass, %d fail" % (files, total_pass + total_fail, total_pass, total_fail))
    # The tests are counted by the programs themselves (a macro makes some of them), so the programs must be built: `make test` does.
    import subprocess
    apps = os.path.join(ROOT, "build", "linux", "release", "apps")
    tests = programs = 0
    for f in sorted(os.listdir(os.path.join(ROOT, "tests"))):
        m = re.match(r"test-encoding-(.*)\.cpp$", f)
        if not m:
            continue
        exe = os.path.join(apps, "testEncoding" + "".join(w.capitalize() for w in m.group(1).split("-")))
        if not os.path.exists(exe):
            print("unit tests: %s is not built (make test builds it)" % exe)
            return
        out = subprocess.run([exe, "--gtest_list_tests"], capture_output=True, text=True, env=dict(os.environ, LD_LIBRARY_PATH=apps)).stdout
        tests += len([l for l in out.splitlines() if l.startswith("  ")])
        programs += 1
    print("unit tests: %d in %d programs (tests/test-encoding-*.cpp)" % (tests, programs))
    witnesses = len(lines_of(os.path.join(ROOT, "tools", "oracle", "encoding_mb_witnesses.txt")))
    print("witnesses of the aggregate classes: %d" % witnesses)


if __name__ == "__main__":
    main()
