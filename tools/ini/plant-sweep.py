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
"""Plant each defect of tools/ini/INI_SWEEP_PLANTS in the INI sources and show the sweep of the writer finds it.

    tools/ini/plant-sweep.py [--only PREFIX] [name ...]

tests/test-ini-sweep.cpp asks every dialect, every option of the write and every entry point of the document model whether what the model accepts is refused by the writer or
read back as it was given. A sweep that has never failed has not been shown able to, and a population that cannot see a defect returns clean for it. Each plant is one edit to
one file of src/ini/: a rule of the writer, the document model or the value codec undone, most of them the defects the fuzzer found one round at a time. The edited file is
compiled by itself, linked ahead of the library archive into the sweep program (so that it supplies every symbol of its file and the archive's copy is never pulled in), and the
sweep is run; it must fail, and the test that fails is named. A rule that only a test of a decoding can see (a plant of the reader, whose raw values the sweep compares as they are)
is shown to the module's unit tests, which run after the sweep. A plant that neither sees is a population that is missing, and the run says so and exits 1.

The programs as they are run first and must pass: a program that fails by itself would make every plant read as caught.

PREFIX (the dependencies' prefix, as for any build here), DEP_PCS (the pkg-config names) and ARCHIVE (the library's static archive) are read from the environment; the Makefile
passes all three.

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
PLANTS = os.path.join(HERE, "INI_SWEEP_PLANTS")
# the sweep first, which is what the plants are for; the module's unit tests after it, for the rule that only a test of a decoding can see (a plant of the reader)
PROGRAMS = ("test-ini-sweep", "test-ini")


def read_plants():
    out = {}
    with open(PLANTS, encoding="utf-8") as fh:
        for number, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 4:
                raise SystemExit("%s:%d: not a plant (name, file, old text, new text): %r" % (PLANTS, number, line[:60]))
            if parts[0] in out:
                raise SystemExit("%s:%d: plant %s is listed twice" % (PLANTS, number, parts[0]))
            out[parts[0]] = (parts[1], parts[2].replace("\\n", "\n"), parts[3].replace("\\n", "\n"))
    return out


def pkg(*args):
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(os.environ["PREFIX"], "share", "pkgconfig"))
    done = subprocess.run(["pkg-config"] + list(args), capture_output=True, text=True, env=env)
    if done.returncode != 0:
        raise SystemExit("pkg-config %s: %s" % (" ".join(args), done.stderr.strip()))
    return done.stdout.split()


def main(argv):
    only = None
    names = []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--only":
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

    def run_sweep(tmp, mutant):
        """Link the programs (the mutant object ahead of the archive when there is one) and run them in turn. Returns None when every one passed, else the first test that failed."""
        for program in PROGRAMS:
            exe = os.path.join(tmp, program)
            link = subprocess.run(["g++", "-std=c++20", "-O1", "-I", os.path.join(ROOT, "include"), "-I", generated, "-I", os.path.join(ROOT, "tests")] + cflags
                                  + ["-o", exe, os.path.join(ROOT, "tests", program + ".cpp")] + ([mutant] if mutant else []) + [archive, "-lstdc++", "-lm",
                                     "-Wl,-rpath," + os.path.join(os.environ["PREFIX"], "lib", "ghoti.io")] + libs + gtest, capture_output=True, text=True)
            if link.returncode != 0:
                raise SystemExit("%s does not link%s:\n%s" % (program, " with the plant" if mutant else "", link.stderr[-800:]))
            done = subprocess.run([exe], capture_output=True, text=True, cwd=ROOT)
            if done.returncode != 0:
                failed = [line[len("[  FAILED  ] "):].split(",")[0] for line in done.stdout.splitlines() if re.match(r"\[  FAILED  \] \w+(/\w+)?\.\w+(/\w+)?", line)]
                return "%s: %s" % (program, failed[0] if failed else "exit %d" % done.returncode)
        return None

    base = tempfile.mkdtemp(prefix="plant-ini-base-")
    try:
        broken = run_sweep(base, None)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    if broken:
        raise SystemExit("the programs fail without any plant (%s): a plant would read as caught whatever it was" % broken)
    print("the unmutated programs pass", flush=True)
    missed = []
    for name in names:
        if name not in plants:
            raise SystemExit("no plant %r" % name)
        file, old, new = plants[name]
        tmp = tempfile.mkdtemp(prefix="plant-ini-")
        try:
            # the sources include headers of src/ by a relative path (`../text_number_internal.h`), so the copy sits in a src/ of its own that links them
            os.makedirs(os.path.join(tmp, "src"))
            for header in os.listdir(os.path.join(ROOT, "src")):
                if header.endswith(".h"):
                    os.symlink(os.path.join(ROOT, "src", header), os.path.join(tmp, "src", header))
            shutil.copytree(os.path.join(ROOT, "src", "ini"), os.path.join(tmp, "src", "ini"))
            path = os.path.join(tmp, "src", "ini", file)
            with open(path, encoding="utf-8") as fh:
                source = fh.read()
            if source.count(old) != 1:
                raise SystemExit("plant %s: its text is in %s %d times, not once" % (name, file, source.count(old)))
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(source.replace(old, new))
            built = subprocess.run(["cc", "-std=c17", "-O1", "-fPIC", "-DGTEXT_BUILD", "-I", os.path.join(tmp, "src", "ini"), "-I", os.path.join(ROOT, "include"),
                                    "-I", generated] + cflags + ["-c", path, "-o", os.path.join(tmp, "mutant.o")], capture_output=True, text=True)
            if built.returncode != 0:
                raise SystemExit("plant %s does not compile:\n%s" % (name, built.stderr[-800:]))
            caught = run_sweep(tmp, os.path.join(tmp, "mutant.o"))
            print("%-48s %s" % (name, caught or "NOT CAUGHT by the sweep or the unit tests"), flush=True)
            if not caught:
                missed.append(name)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    if missed:
        sys.stderr.write("planted defects that neither the sweep nor the unit tests see: %s\n" % ", ".join(missed))
        return 1
    print("ini sweep plants: %d planted defects, every one caught by the sweep or the unit tests" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
