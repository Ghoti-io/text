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
"""Fail if a planted-defect edit no longer applies to the source it edits.

    tools/check-fuzz-plant-anchors.py

The plant tools (tools/{xml,html,css,encoding}/plant-fuzz.py) each hold a table of
edits: a text that must appear exactly once in one source file, and what to put
there. They apply an edit in a scratch copy and run a fuzzer for a minute, so a
plant whose text a later commit has moved is found only by someone who runs a
gate that builds a fuzzer, and then it reads "its anchor matches 0 times", which
is a stale table and not a missing defect. This reads the sources and nothing
else, in a moment, so that `make test` finds it on the commit that moves the text.

For every plant of the four tables: its file exists, its text matches exactly once,
the edit changes something, and a recorded reproducer (a seed file, or a unit test
named by its suite and name) exists.

Copyright 2026 by Corey Pennycuff
"""

import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load(tool):
    path = os.path.join(HERE, tool, "plant-fuzz.py")
    spec = importlib.util.spec_from_file_location("plant_fuzz_" + tool, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_exists(suite_and_name):
    """Is there a TEST/TEST_F of that suite and name under tests/?"""
    suite, _, name = suite_and_name.partition(".")
    pattern = re.compile(r"\bTEST(?:_F|_P)?\(\s*%s\s*,\s*%s\s*\)" % (re.escape(suite), re.escape(name)))
    for entry in os.listdir(os.path.join(ROOT, "tests")):
        if entry.endswith(".cpp"):
            with open(os.path.join(ROOT, "tests", entry), encoding="utf-8", errors="replace") as fh:
                if pattern.search(fh.read()):
                    return True
    return False


def plants_of(tool, module):
    """(name, source path from ROOT, old, new, reproducer or None, kind of reproducer) for each plant of a tool."""
    out = []
    reproducers = getattr(module, "REPRODUCERS", {})
    for name, entry in module.PLANTS.items():
        if tool == "css":
            rel, old, new, seed = entry
            out.append((name, "src/css/" + rel, old, new,
                        "tests/fuzz/corpus/css-syntax/" + seed if seed else None, "file"))
        elif tool == "encoding":
            path, old, new = entry
            out.append((name, path, old, new, reproducers.get(name), "file"))
        else:
            _target, path, old, new = entry
            repro = reproducers.get(name)
            if isinstance(repro, tuple):
                # (test program, --gtest_filter=Suite.Name)
                out.append((name, path, old, new, repro[1].partition("=")[2], "test"))
            else:
                out.append((name, path, old, new, repro, "file"))
    return out


def main():
    problems = []
    count = 0
    for tool in ("xml", "html", "css", "encoding"):
        module = load(tool)
        for name, path, old, new, repro, kind in plants_of(tool, module):
            count += 1
            label = "%s plant %s" % (tool, name)
            full = os.path.join(ROOT, path)
            if not os.path.isfile(full):
                problems.append("%s: %s does not exist" % (label, path))
                continue
            with open(full, encoding="utf-8") as fh:
                matches = fh.read().count(old)
            if matches != 1:
                problems.append("%s: its text matches %d times in %s, not once" % (label, matches, path))
            if old == new:
                problems.append("%s: its edit changes nothing" % label)
            if repro and kind == "file" and not os.path.isfile(os.path.join(ROOT, repro)):
                problems.append("%s: its reproducer %s does not exist" % (label, repro))
            if repro and kind == "test" and not test_exists(repro):
                problems.append("%s: its reproducer test %s does not exist" % (label, repro))
        for name in getattr(module, "REPRODUCERS", {}):
            if name not in module.PLANTS:
                problems.append("%s: a reproducer for %s, which is not a plant" % (tool, name))
    for problem in problems:
        sys.stderr.write(problem + "\n")
    if problems:
        sys.stderr.write("check-fuzz-plant-anchors: %d of %d plants are stale; the table is in tools/<module>/plant-fuzz.py\n"
                         % (len(problems), count))
        return 1
    print("Every planted edit (%d) still applies to its source exactly once." % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
