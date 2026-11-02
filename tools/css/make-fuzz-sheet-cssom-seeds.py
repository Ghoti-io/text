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
"""Write the seed corpus of tests/fuzz/fuzz_cssom_sheet.cpp into tests/fuzz/corpus/cssom-sheet.

    tools/css/make-fuzz-sheet-cssom-seeds.py [directory]

A seed is three header bytes (a flag whose bit 0 asks for one failed allocation, the allocation, the operation it fails in) and a list of operations, six bytes each (the operation, the
sheet, an index, a text, an argument, a spare). The corpus is sequences from a fixed generator: short and long, with and without a failed allocation, with a batch that leans on
the operations that detach rules (delete, replaceSync) so that the held-rule properties meet many removed rules. Deterministic; the files are named for a hash of what they hold.
Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""
import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "cssom-sheet")


def op(rng, weights):
    return bytes([rng.choices(range(8), weights)[0], rng.randrange(3), rng.randrange(256), rng.randrange(256), rng.randrange(256), 0])


def main():
    os.makedirs(OUT, exist_ok=True)
    seeds = set()
    for batch, (seed, weights) in enumerate([(0x5eed, [4, 3, 2, 2, 2, 1, 1, 1]), (0x0de7, [2, 5, 4, 1, 1, 1, 1, 1]), (0xfa17, [3, 2, 1, 3, 3, 2, 2, 2])]):
        rng = random.Random(seed)
        for length in [1, 2, 3, 4, 6, 8, 12, 20, 32, 48]:
            for _ in range(12):
                fault = rng.randrange(2) if batch == 2 else 0
                head = bytes([fault, rng.randrange(256), rng.randrange(4), rng.randrange(48)])
                body = b"".join(op(rng, weights) for _ in range(length))
                seeds.add(head[:1] + head[1:3] + head[3:4] + body)
    for s in sorted(seeds):
        name = "own-%s.seed" % hashlib.sha256(s).hexdigest()[:12]
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(s)
    print("wrote %d seeds to %s" % (len(seeds), OUT))


main()
