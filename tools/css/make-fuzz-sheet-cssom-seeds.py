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
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "cssom-sheet")


def op(rng, weights):
    return bytes([rng.choices(range(8), weights)[0], rng.randrange(3), rng.randrange(256), rng.randrange(256), rng.randrange(256), 0])


def pool_size(source, name):
    """The number of strings in `const char * const name[] = {...};` of the harness (comment lines left out)."""
    body = source[source.index("const char * const %s[] =" % name):]
    body = body[: body.index("};\n")]
    body = "\n".join(line for line in body.split("\n") if not line.strip().startswith("//"))
    return len(re.findall(r'"(?:[^"\\]|\\.)*"', body))


EXT_MAGIC = b"\x80MX2"


def ext_seeds():
    """The seeds of the mode of mixins (the harness's `kExtMagic`): the four magic bytes, then the format as before, whose rule and sheet indices reach past the pools that were there.

    Appended after everything above and drawn from generators of their own, so the seeds above are what they were: the existing corpus does not change by a byte."""
    source = open(os.path.join(ROOT, "tests", "fuzz", "fuzz_cssom_sheet.cpp"), encoding="utf-8").read()
    base_rules, base_sheets = pool_size(source, "kRules"), pool_size(source, "kSheets")
    ext_rules, ext_sheets = pool_size(source, "kRulesExt"), pool_size(source, "kSheetsExt")
    out = set()

    def pick(base, n, j):
        t = base + j % n
        # the index is taken modulo the size of both pools, so a byte past it must be reduced first
        return t % (base + n)

    def seed(fault, fault_n, fault_op, ops):
        head = bytes([fault, fault_n & 255, fault_n >> 8, fault_op])
        return EXT_MAGIC + head + b"".join(bytes(o) for o in ops)

    # one rule at a time into each sheet, then the read-back that every operation does
    for j in range(ext_rules):
        for which in range(3):
            out.add(seed(0, 0, 0, [(0, which, 0, pick(base_rules, ext_rules, j), 0, 0), (0, which, 1, pick(base_rules, ext_rules, (j + 5) % ext_rules), 0, 0)]))
    # a whole sheet at a time into the constructed sheet, then the rules edited and removed
    for j in range(ext_sheets):
        out.add(seed(0, 0, 0, [(2, 1, 0, pick(base_sheets, ext_sheets, j), 0, 0), (3, 1, 0, 3, 0, 0), (4, 1, 1, 0, 1, 0), (1, 1, 0, 0, 0, 0)]))
        out.add(seed(0, 0, 0, [(2, 1, 0, pick(base_sheets, ext_sheets, j), 0, 0), (0, 1, 2, pick(base_rules, ext_rules, j), 0, 0), (1, 1, 1, 0, 0, 0)]))
    # sequences, with and without a failed allocation: every operation, the texts drawn from the new pools
    rng = random.Random(0x91e)
    for weights in ([4, 3, 2, 2, 2, 1, 1, 1], [2, 5, 4, 1, 1, 1, 1, 1]):
        for length in [2, 4, 8, 16, 32]:
            for k in range(6):
                fault = k % 2
                ops = []
                for _ in range(length):
                    o = rng.choices(range(8), weights)[0]
                    pool = rng.random() < 0.5
                    ops.append((o, rng.randrange(3), rng.randrange(256), pick(base_rules, ext_rules, rng.randrange(ext_rules)) if pool else pick(base_sheets, ext_sheets, rng.randrange(ext_sheets)) if o == 2 else rng.randrange(256), rng.randrange(256), 0))
                out.add(seed(fault, rng.randrange(600), rng.randrange(length), ops))
    return out


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
    seeds |= ext_seeds()
    for s in sorted(seeds):
        name = ("ext-%s.seed" if s.startswith(EXT_MAGIC) else "own-%s.seed") % hashlib.sha256(s).hexdigest()[:12]
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(s)
    print("wrote %d seeds to %s" % (len(seeds), OUT))


main()
