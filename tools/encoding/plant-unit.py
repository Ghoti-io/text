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
"""Plant the defects of tools/oracle/ENCODING_ORACLE_PLANTS and tools/encoding/UNIT_PLANTS in the Encoding sources and show the unit tests find them.

    tools/encoding/plant-unit.py [--tests SPEC[,SPEC...]] [--only PREFIX] [name ...]

Each plant is one edit to one file of src/encoding/. The edited file is compiled by itself, linked ahead of the library archive into the unit test programs
(so that it supplies every symbol of its file and the archive's copy is never pulled in), and the programs are run, cheapest first, until one fails. The defect
must make a test fail, whichever test: a plant that no test sees is a test that is missing, and the run says so and exits 1.

The first list is the one the differential's controls use (tools/oracle/run-encoding-oracle.sh --controls), so the same defects are shown to the unit tests as to the
references. The second, UNIT_PLANTS, is the defects only a unit test can see because the references cannot: an output limit (Node and Python have none), a
decoder's state after a failure that is not an error in the data, a count of handler steps.

PREFIX (the dependencies' prefix, as for any build here) and ARCHIVE (the library's static archive, as for the conformance scripts) are read from the environment; the
Makefile passes both. A SPEC is a test program, `test-encoding-cjk`, optionally with a gtest filter after a colon.

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LISTS = (os.path.join(ROOT, "tools", "oracle", "ENCODING_ORACLE_PLANTS"), os.path.join(HERE, "UNIT_PLANTS"))
DEFAULT_TESTS = ("test-encoding-cjk:-*Exhaustive*", "test-encoding-labels", "test-encoding-fuzz-seeds", "test-encoding-contract", "test-encoding-oom",
                 "test-encoding-decode", "test-encoding-encode", "test-encoding-cjk:*Exhaustive*")


def read_plants():
    out = {}
    for path in LISTS:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) < 4:
                    raise SystemExit("%s: not a plant: %r" % (path, line[:60]))
                out[parts[0]] = (parts[1], parts[2].replace("\\n", "\n"), parts[3].replace("\\n", "\n"))
    return out


def pkg(*args):
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(os.environ["PREFIX"], "share", "pkgconfig"))
    done = subprocess.run(["pkg-config"] + list(args), capture_output=True, text=True, env=env)
    if done.returncode != 0:
        raise SystemExit("pkg-config %s: %s" % (" ".join(args), done.stderr.strip()))
    return done.stdout.split()


def main(argv):
    tests = list(DEFAULT_TESTS)
    only = None
    names = []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--tests":
            tests = args.pop(0).split(",")
        elif a == "--only":
            only = args.pop(0)
        else:
            names.append(a)
    if "PREFIX" not in os.environ or "ARCHIVE" not in os.environ or "DEP_PCS" not in os.environ:
        raise SystemExit("PREFIX, DEP_PCS and ARCHIVE must be set; the Makefile passes them")
    archive = os.path.abspath(os.environ["ARCHIVE"])
    generated = os.path.join(os.path.dirname(os.path.dirname(archive)), "generated")
    pcs = os.environ["DEP_PCS"].split()
    cflags = pkg("--cflags", *pcs)
    libs = pkg("--libs", *pcs)
    gtest = pkg("--libs", "--cflags", "gtest", "gtest_main")
    plants = read_plants()
    names = names or [n for n in plants if only is None or n.startswith(only)]

    def run_programs(tmp, mutant):
        """Link the unit test programs (ahead of the library archive, `mutant` first when there is one) and run them, cheapest first, until one fails.
        Returns None when every one passed, else the first failure."""
        linked = set()
        for spec in tests:
            program, _, filt = spec.partition(":")
            exe = os.path.join(tmp, program)
            if program not in linked:
                link = subprocess.run(["g++", "-std=c++20", "-O1", "-I", os.path.join(ROOT, "include"), "-I", generated, "-I", os.path.join(ROOT, "tests")] + cflags
                                      + ["-o", exe, os.path.join(ROOT, "tests", program + ".cpp")] + ([mutant] if mutant else []) + [archive, "-lstdc++", "-lm",
                                         "-Wl,-rpath," + os.path.join(os.environ["PREFIX"], "lib", "ghoti.io")] + libs + gtest, capture_output=True, text=True)
                if link.returncode != 0:
                    raise SystemExit("%s does not link%s:\n%s" % (program, " with the plant" if mutant else "", link.stderr[-800:]))
                linked.add(program)
            done = subprocess.run([exe] + (["--gtest_filter=" + filt] if filt else []), capture_output=True, text=True, cwd=ROOT)
            if done.returncode != 0:
                failed = [m.group(0)[len("[  FAILED  ] "):] for line in done.stdout.splitlines() for m in [re.match(r"\[  FAILED  \] \w+\.\w+$", line)] if m]
                replay = re.search(r"being replayed was (\S+)", done.stderr)
                return "%s: %s" % (spec, failed[0] if failed else (os.path.basename(replay.group(1)) if replay else "exit %d" % done.returncode))
        return None

    # The programs as they are, first: a missing pinned index or a program that fails by itself would make every plant "caught".
    base = tempfile.mkdtemp(prefix="plant-unit-base-")
    try:
        broken = run_programs(base, None)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    if broken:
        raise SystemExit("the unit test programs fail without any plant (%s): a plant would read as caught whatever it was" % broken)
    print("the unmutated unit test programs pass", flush=True)
    missed = []
    for name in names:
        if name not in plants:
            raise SystemExit("no plant %r" % name)
        file, old, new = plants[name]
        tmp = tempfile.mkdtemp(prefix="plant-unit-")
        try:
            shutil.copytree(os.path.join(ROOT, "src", "encoding"), os.path.join(tmp, "encoding"))
            path = os.path.join(tmp, "encoding", file)
            with open(path, encoding="utf-8") as fh:
                source = fh.read()
            if source.count(old) != 1:
                raise SystemExit("plant %s: its text is in %s %d times, not once" % (name, file, source.count(old)))
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(source.replace(old, new))
            built = subprocess.run(["cc", "-std=c17", "-O1", "-fPIC", "-DGTEXT_BUILD", "-I", os.path.join(tmp, "encoding"), "-I", os.path.join(ROOT, "include"),
                                    "-I", generated] + cflags + ["-c", path, "-o", os.path.join(tmp, "mutant.o")], capture_output=True, text=True)
            if built.returncode != 0:
                raise SystemExit("plant %s does not compile:\n%s" % (name, built.stderr[-800:]))
            caught = run_programs(tmp, os.path.join(tmp, "mutant.o"))
            print("%-40s %s" % (name, caught or "NOT CAUGHT by any unit test"), flush=True)
            if not caught:
                missed.append(name)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    if missed:
        sys.stderr.write("planted defects that no unit test sees: %s\n" % ", ".join(missed))
        return 1
    print("encoding unit plants: %d planted defects, every one caught by a unit test" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
