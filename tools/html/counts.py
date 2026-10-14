#!/usr/bin/env python3
"""The numbers of the HTML exit-gate work (Phase 3k, part A), counted from the files that define them.

    tools/html/counts.py [--binary build/linux/release/apps/testQuadratic]

Nothing here is typed by hand: the shapes are listed by the quadratic suite itself, the plants are read from the plant lists, the classes from the
pipeline differential's list. The notes and the manual say what is counted, and quote these when they must.

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def main(argv):
    binary = os.path.join(ROOT, "build", "linux", "release", "apps", "testQuadratic")
    if "--binary" in argv:
        binary = argv[argv.index("--binary") + 1]
    env = dict(os.environ, GTEXT_QUAD_LIST="1")
    out = subprocess.run([binary, "--gtest_filter=Quadratic.ListShapes"], capture_output=True, text=True, env=env).stdout
    groups = collections.Counter(l.split("\t")[1] for l in out.splitlines() if l.startswith("SHAPE\t"))
    print("quadratic shapes: %d (%s)" % (sum(groups.values()), ", ".join("%s %d" % kv for kv in sorted(groups.items()))))
    plants = re.findall(r'^    \("([a-z0-9-]+)"', open(os.path.join(HERE, "plant-quadratic.py")).read(), re.M)
    print("quadratic plants: %d" % len(plants))
    pl = [l for l in open(os.path.join(ROOT, "tools", "oracle", "HTML_PIPELINE_ORACLE_PLANTS")) if l.strip() and not l.startswith("#")]
    layers = collections.Counter(l.split("\t")[0].split("-")[0] for l in pl)
    print("pipeline plants: %d (%s)" % (len(pl), ", ".join("%s %d" % kv for kv in sorted(layers.items()))))
    classes = collections.Counter()
    total = 0
    hunt = 0
    for l in open(os.path.join(ROOT, "tools", "oracle", "html_pipeline_divergences.txt")):
        if l.startswith("#") or not l.strip():
            continue
        c = l.split("\t")
        classes[c[0].split(":")[0]] += 1
        if c[1] == "hunt":
            hunt += 1
        else:
            total += int(c[1])
    print("pipeline classes: %d (%s); of them %d only in hunts; documents in the others: %d" % (
        sum(classes.values()), ", ".join("%s %d" % kv for kv in sorted(classes.items())), hunt, total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
