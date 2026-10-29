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
"""Write the seed corpus of tests/fuzz/fuzz_css_container.cpp into tests/fuzz/corpus/css-container.

    tools/css/make-fuzz-container-seeds.py [directory]

A seed is a byte string the harness reads as a generator: the shape of a tree, the container types, names, sizes and custom properties of its elements, and a list of
container conditions. The corpus is deterministic pseudo-random payloads of several lengths, and a few shapes that matter (a chain of containers, a flat tree with no
container, every element a container of one name). Files are named for a hash of what they hold. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""
import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-container")


def put(out, data):
    out[hashlib.sha256(data).hexdigest()[:12]] = bytes(data)


def main():
    out = {}
    rng = random.Random(0xC0A7A1)
    for length in (8, 16, 32, 64, 128, 256, 400):
        for _ in range(70):
            put(out, bytes(rng.randrange(256) for _ in range(length)))
    # a chain: every element the child of the one before, all size containers of one name, then a condition
    chain = [0, 0, 22]
    for i in range(1, 23):
        chain += [i - 1 if i > 1 else 0, 1, 1, 5, 5, 2]
    chain += [1, 1, 0, 0, 3, 2, 1]
    put(out, bytes(chain))
    # a flat tree with no container
    flat = [0, 0, 10]
    for i in range(1, 11):
        flat += [0, 0, 0, 3, 3, 0]
    flat += [1, 0, 0, 0, 4, 1, 0, 1, 2]
    put(out, bytes(flat))
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith(".seed") and not name.startswith("reproducer-"):
            os.remove(os.path.join(OUT, name))
    for name, data in sorted(out.items()):
        with open(os.path.join(OUT, name + ".seed"), "wb") as f:
            f.write(data)
    print("wrote %d seeds to %s" % (len(out), OUT))


if __name__ == "__main__":
    main()
