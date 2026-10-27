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

The plant tools (tools/{xml,html,css,encoding,markup}/plant-fuzz.py) each hold a table of
edits: a text that must appear exactly once in one source file, and what to put
there. They apply an edit in a scratch copy and run a fuzzer for a minute, so a
plant whose text a later commit has moved is found only by someone who runs a
gate that builds a fuzzer, and then it reads "its anchor matches 0 times", which
is a stale table and not a missing defect. This reads the sources and nothing
else, in a moment, so that `make test` finds it on the commit that moves the text.

For every plant of the five tables: its file exists, its text matches exactly once,
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
        elif tool in ("encoding", "markup"):
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
    if tool == "css":
        for name, (rel, old, new, seed) in module.BYTES_PLANTS.items():
            out.append((name, "src/css/" + rel, old, new, "tests/fuzz/corpus/css-bytes/" + seed if seed else None, "file"))
        for name, (rel, old, new, seed) in module.GRAMMAR_PLANTS.items():
            out.append((name, "src/css/" + rel, old, new, "tests/fuzz/corpus/css-grammar/" + seed if seed else None, "file"))
        for name, (rel, old, new, seed) in module.VALUES_PLANTS.items():
            out.append((name, "src/css/" + rel, old, new, "tests/fuzz/corpus/css-values/" + seed if seed else None, "file"))
        for name, (rel, old, new, seed) in module.REGISTRY_PLANTS.items():
            out.append((name, "src/css/" + rel, old, new, "tests/fuzz/corpus/css-registry/" + seed if seed else None, "file"))
        for name, (rel, old, new, seed) in module.COMPUTE_PLANTS.items():
            out.append((name, "src/css/" + rel, old, new, "tests/fuzz/corpus/css-compute/" + seed if seed else None, "file"))
        for name, (rel, old, new, seed) in module.CUSTOM_PLANTS.items():
            out.append((name, "src/css/" + rel, old, new, "tests/fuzz/corpus/css-custom/" + seed if seed else None, "file"))
        for name, (rel, old, new, seed) in module.CASCADE_PLANTS.items():
            out.append((name, "src/css/" + rel, old, new, "tests/fuzz/corpus/css-cascade/" + seed if seed else None, "file"))
        for name, edits in getattr(module, "COMPUTE_EXTRA", {}).items():
            for rel, old, new in edits:
                out.append((name + " (extra)", "src/css/" + rel, old, new, None, "file"))
    return out


def main():
    problems = []
    count = 0
    for tool in ("xml", "html", "css", "encoding", "markup"):
        module = load(tool)
        for name, path, old, new, repro, kind in plants_of(tool, module):
            count += 1
            label = "%s plant %s" % (tool, name)
            full = os.path.join(ROOT, path)
            if not os.path.isfile(full):
                problems.append("%s: %s does not exist" % (label, path))
                continue
            with open(full, encoding="utf-8") as fh:
                source = fh.read()
            olds, news = (old, new) if isinstance(old, list) else ([old], [new])
            for o_, n_ in zip(olds, news):
                matches = source.count(o_)
                if matches != 1:
                    problems.append("%s: its text matches %d times in %s, not once" % (label, matches, path))
                if o_ == n_:
                    problems.append("%s: its edit changes nothing" % label)
            if repro and kind == "file" and not os.path.isfile(os.path.join(ROOT, repro)):
                problems.append("%s: its reproducer %s does not exist" % (label, repro))
            if repro and kind == "test" and not test_exists(repro):
                problems.append("%s: its reproducer test %s does not exist" % (label, repro))
        for name in getattr(module, "REPRODUCERS", {}):
            if name not in module.PLANTS:
                problems.append("%s: a reproducer for %s, which is not a plant" % (tool, name))
    # the plants of the Value Definition Syntax engine (tools/css/plant-grammar.py): the same check, for edits that may be several
    grammar = importlib.util.module_from_spec(importlib.util.spec_from_file_location("plant_grammar", os.path.join(HERE, "css", "plant-grammar.py")))
    grammar.__spec__.loader.exec_module(grammar)
    for name, rel, old, new, must in grammar.PLANTS:
        count += 1
        label = "css grammar plant %s" % name
        full = os.path.join(ROOT, "src", "css", rel)
        olds, news = (old, new) if isinstance(old, list) else ([old], [new])
        if not os.path.isfile(full):
            problems.append("%s: src/css/%s does not exist" % (label, rel))
            continue
        with open(full, encoding="utf-8") as fh:
            text = fh.read()
        for o_, n_ in zip(olds, news):
            if text.count(o_) != 1:
                problems.append("%s: its text matches %d times in src/css/%s, not once" % (label, text.count(o_), rel))
            if o_ == n_:
                problems.append("%s: its edit changes nothing" % label)
        for key in must:
            if key not in grammar.PROGRAMS or not os.path.isfile(os.path.join(ROOT, "tests", grammar.PROGRAMS[key])):
                problems.append("%s: the program %s it must fail does not exist" % (label, key))
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
