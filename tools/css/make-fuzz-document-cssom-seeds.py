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
"""Write the seed corpus of tests/fuzz/fuzz_cssom_document.cpp into tests/fuzz/corpus/cssom-document.

    tools/css/make-fuzz-document-cssom-seeds.py [directory]

A seed is four header bytes (a flag whose bit 0 asks for one failed allocation, the allocation as two bytes, the operation it fails in) and a list of operations, six bytes each (the
operation, four arguments and a spare). The corpus is sequences from a fixed generator: short and long, with and without a failed allocation, in three mixes (edits of the Markup tree
leaning on text changes and moves, CSSOM calls on the sheets the binding made, syncs and the one-element path interleaved with batches). Deterministic; the files are named for a hash of
what they hold. Reproducers the fuzzer finds are kept beside them as `reproducer-*.seed`.
"""
import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "cssom-document")


def op(rng, weights):
    return bytes([rng.choices(range(17), weights)[0], rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(256), 0])


def main():
    os.makedirs(OUT, exist_ok=True)
    seeds = set()
    mixes = [
        (0x5eed, [4, 3, 2, 3, 2, 2, 1, 1, 1, 1, 3, 3, 1, 1, 1, 1, 1]),
        (0x0de7, [1, 1, 1, 1, 1, 1, 4, 3, 3, 2, 3, 3, 1, 1, 2, 1, 1]),
        (0xfa17, [2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 3, 3, 2, 2, 1]),
    ]
    for batch, (seed, weights) in enumerate(mixes):
        rng = random.Random(seed)
        for length in [1, 2, 3, 4, 6, 8, 12, 20, 32, 40]:
            for _ in range(10):
                fault = rng.randrange(2) if batch == 2 else 0
                head = bytes([fault, rng.randrange(256), rng.randrange(256), rng.randrange(40)])
                body = b"".join(op(rng, weights) for _ in range(length))
                seeds.add(head + body)
    for s in sorted(seeds):
        name = "own-%s.seed" % hashlib.sha256(s).hexdigest()[:12]
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(s)
    print("wrote %d seeds to %s" % (len(seeds), OUT))


main()
