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
"""Write the seed corpus of tests/fuzz/fuzz_cssom_decl.cpp into tests/fuzz/corpus/cssom-decl.

    tools/css/make-fuzz-cssom-seeds.py [directory]

A seed is a list of operations, each six bytes (the operation, then the indexes of the name, the value, the priority and the cssText in the harness's tables). The corpus is made of
sequences drawn from a fixed pseudo-random generator, short and long, so that every operation meets every table entry in several states of the declaration. Deterministic; the files
are named for a hash of what they hold. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""
import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "cssom-decl")


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = random.Random(0x5eed)
    seeds = set()
    for length in [1, 2, 3, 4, 6, 8, 12, 20, 40, 64]:
        for _ in range(30):
            body = bytearray()
            for _ in range(length):
                body += bytes([rng.randrange(5), rng.randrange(24), rng.randrange(24), rng.randrange(5), rng.randrange(12), 0])
            seeds.add(bytes(body))
    for s in sorted(seeds):
        name = "own-%s.seed" % hashlib.sha256(s).hexdigest()[:12]
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(s)
    print("wrote %d seeds to %s" % (len(seeds), OUT))


main()
