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
"""Plant a defect in the selector layer and show the tests that must see it fail.

    PREFIX=... DEP_PCS=... ARCHIVE=... tools/css/plant-selectors.py [name ...]

A gate that has never failed has not been shown to be able to. Each plant is a line of tools/conformance/CSS_SELECTOR_PLANTS: one edit (or the
edits of one defect, as lines of one name) to one file of src/css/, made in a copy of src/ in a scratch directory (the working tree is never
touched). The edited file is compiled alone and linked ahead of the library's archive into the test programs the plant names, which are run; each
must fail. The **control** is the same procedure with no edit, and all programs must pass.

Columns (tab separated): name, file under src/css/, the text (exactly once), what replaces it (\\n is a line feed), the programs that must fail
(comma separated, a key of PROGRAMS).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

PROGRAMS = {
    "selector": "test-css-selector.cpp",
}
EXTRA_PROGRAMS = os.path.join(ROOT, "tools", "conformance", "CSS_SELECTOR_PROGRAMS")


def load_programs():
    programs = dict(PROGRAMS)
    if os.path.exists(EXTRA_PROGRAMS):
        for line in open(EXTRA_PROGRAMS, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            key, source = line.rstrip("\n").split("\t")
            programs[key] = source
    return programs


def load_plants():
    grouped = {}
    order = []
    with open(os.path.join(ROOT, "tools", "conformance", "CSS_SELECTOR_PLANTS"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            name, rel, old, new, programs = line.rstrip("\n").split("\t")
            if name not in grouped:
                grouped[name] = (rel, [], [], programs.split(","))
                order.append(name)
            if grouped[name][0] != rel:
                raise SystemExit("CSS_SELECTOR_PLANTS: %s edits two files" % name)
            grouped[name][1].append(old.replace("\\n", "\n"))
            grouped[name][2].append(new.replace("\\n", "\n"))
    return [(n, grouped[n][0], grouped[n][1] if len(grouped[n][1]) > 1 else grouped[n][1][0], grouped[n][2] if len(grouped[n][2]) > 1 else grouped[n][2][0], grouped[n][3]) for n in order]


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)

def main(argv):
    prefix, dep_pcs, archive = (os.environ.get(k) for k in ("PREFIX", "DEP_PCS", "ARCHIVE"))
    if not (prefix and dep_pcs and archive):
        sys.stderr.write("PREFIX, DEP_PCS and ARCHIVE must be set; the Makefile (check-css-selector-plants) passes them\n")
        return 2
    archive = os.path.abspath(archive)
    generated = os.path.join(os.path.dirname(os.path.dirname(archive)), "generated")
    env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(prefix, "share", "pkgconfig"))
    cflags = sh(["pkg-config", "--cflags"] + dep_pcs.split(), env=env).stdout.split()
    libs = sh(["pkg-config", "--libs"] + dep_pcs.split(), env=env).stdout.split()
    gtest = sh(["pkg-config", "--libs", "--cflags", "gtest", "gtest_main"], env=env).stdout.split()
    programs_all = load_programs()
    PLANT_LIST = load_plants()
    needed = {k for p in PLANT_LIST for k in p[4]}
    programs_all = {k: v for k, v in programs_all.items() if k in needed}
    names = argv[1:] or ["control"] + [p[0] for p in PLANT_LIST]
    unknown = [n for n in names if n not in [p[0] for p in PLANT_LIST] + ["control"]]
    if unknown:
        sys.stderr.write("no such plant: %s\n" % " ".join(unknown))
        return 2
    scratch = tempfile.mkdtemp(prefix="css-selector-plants-")
    try:
        objects = {}
        for key, source in programs_all.items():
            obj = os.path.join(scratch, key + ".o")
            done = sh(["g++", "-std=c++20", "-O1", "-g", "-c", "-I", os.path.join(ROOT, "include"), "-I", generated] + cflags
                      + [os.path.join(ROOT, "tests", source), "-o", obj])
            if done.returncode != 0:
                sys.stderr.write(done.stderr[-3000:])
                return 2
            objects[key] = obj
        failures = 0
        for name in names:
            plant = next((p for p in PLANT_LIST if p[0] == name), None)
            work = os.path.join(scratch, name)
            os.makedirs(work)
            objs = []
            if plant:
                _, rel, old, new, _must = plant
                olds, news = (old, new) if isinstance(old, list) else ([old], [new])
                shutil.copytree(os.path.join(ROOT, "src"), os.path.join(work, "src"))
                path = os.path.join(work, "src", "css", rel)
                text = open(path, encoding="utf-8").read()
                for o_ in olds:
                    if text.count(o_) != 1:
                        sys.stderr.write("plant %s: the text is in %s %d times, not once\n" % (name, rel, text.count(o_)))
                        return 2
                for o_, n_ in zip(olds, news):
                    text = text.replace(o_, n_)
                open(path, "w", encoding="utf-8").write(text)
                obj = os.path.join(work, "plant.o")
                done = sh(["cc", "-std=c17", "-O2", "-fPIC", "-fvisibility=hidden", "-DGTEXT_BUILD", "-w", "-I", os.path.join(ROOT, "include"),
                           "-I", generated, "-I", os.path.join(work, "src", "css"), "-I", os.path.join(work, "src", "markup")] + cflags + ["-c", path, "-o", obj])
                if done.returncode != 0:
                    sys.stderr.write("plant %s did not compile:\n%s" % (name, done.stderr[-3000:]))
                    return 2
                objs = [obj]
            failed = []
            for key, tobj in objects.items():
                if plant and key not in plant[4]:
                    continue
                exe = os.path.join(work, key)
                done = sh(["g++", "-o", exe, tobj] + objs + [archive, "-lstdc++", "-lm", "-Wl,-rpath," + os.path.join(prefix, "lib", "ghoti.io")]
                          + libs + gtest + ["-Wl,--allow-multiple-definition"])
                if done.returncode != 0:
                    sys.stderr.write("plant %s did not link %s:\n%s" % (name, key, done.stderr[-3000:]))
                    return 2
                run = sh([exe, "--gtest_brief=1"], timeout=900, cwd=ROOT)
                if run.returncode != 0:
                    failed.append(key)
            if not plant:
                if failed:
                    print("control: FAILED, the unedited library does not pass %s" % ", ".join(failed))
                    failures += 1
                else:
                    print("control: passes")
                continue
            missing = [k for k in plant[4] if k not in failed]
            if missing:
                print("plant %s: NOT CAUGHT by %s (failed: %s)" % (name, ", ".join(missing), ", ".join(failed) or "nothing"))
                failures += 1
            else:
                print("plant %s: caught by %s" % (name, ", ".join(failed)))
        print("selector plants: %s" % ("all caught, control passes" if failures == 0 else "%d problems" % failures))
        return 0 if failures == 0 else 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
