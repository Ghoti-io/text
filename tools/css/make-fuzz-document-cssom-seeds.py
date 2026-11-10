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
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "cssom-document")


def op(rng, weights):
    return bytes([rng.choices(range(17), weights)[0], rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(256), 0])


def pool_size(source, name):
    """The number of strings in `const char * const name[] = {...};` of the harness (comment lines left out)."""
    body = source[source.index("const char * const %s[] =" % name):]
    body = body[: body.index("};\n")]
    body = "\n".join(line for line in body.split("\n") if not line.strip().startswith("//"))
    return len(re.findall(r'"(?:[^"\\]|\\.)*"', body))


EXT_MAGIC = b"\x80MX2"


def ext_seeds():
    """The seeds of the mode of mixins (the harness's `kExtMagic`): the four magic bytes, then the format as before, whose text indices reach past the pools that were there.

    Appended after everything above and drawn from generators of their own, so the seeds above are what they were: the existing corpus does not change by a byte."""
    source = open(os.path.join(ROOT, "tests", "fuzz", "fuzz_cssom_document.cpp"), encoding="utf-8").read()
    base_t, base_r = pool_size(source, "kTexts"), pool_size(source, "kRules")
    ext_t, ext_r = pool_size(source, "kTextsExt"), pool_size(source, "kRulesExt")
    out = set()

    def seed(fault, fault_n, fault_op, ops):
        head = bytes([fault, fault_n & 255, fault_n >> 8, fault_op])
        return EXT_MAGIC + head + b"".join(bytes(o) for o in ops)

    # each text of the pool into each style element, then a sync (the reference document is made from the serialized sheets and must agree)
    for j in range(ext_t):
        for style in range(3):
            out.add(seed(0, 0, 0, [(0, style, 0, base_t + j, 0, 0), (11, 0, 0, 0, 0, 0)]))
    # two edits that each need the other: a mixin set in one style element and applied in another, then the one that defines it changed, detached and put back
    for j in range(ext_t):
        k = (j + 1) % ext_t
        out.add(seed(0, 0, 0, [(0, 0, 0, base_t + j, 0, 0), (0, 1, 0, base_t + k, 0, 0), (11, 0, 0, 0, 0, 0), (0, 0, 0, base_t + k, 0, 0), (11, 0, 0, 0, 0, 0),
                              (1, 0, 0, 0, 0, 0), (11, 0, 0, 0, 0, 0), (2, 0, 0, 0, 0, 0), (11, 0, 0, 0, 0, 0)]))
    # rules inserted into a sheet and deleted, a new style element, an adopted sheet
    for j in range(ext_r):
        out.add(seed(0, 0, 0, [(6, 0, 0, base_r + j, 0, 0), (11, 0, 0, 0, 0, 0), (7, 0, 0, 0, 0, 0), (11, 0, 0, 0, 0, 0)]))
        out.add(seed(0, 0, 0, [(0, 1, 0, base_t, 0, 0), (6, 1, 0, base_r + j, 0, 0), (11, 0, 0, 0, 0, 0)]))
    for j in range(ext_t):
        out.add(seed(0, 0, 0, [(4, 0, 0, base_t + j, 0, 0), (11, 0, 0, 0, 0, 0), (14, 0, 0, base_t + (j + 3) % ext_t, 0, 0), (11, 0, 0, 0, 0, 0)]))
    # sequences, with and without a failed allocation, the texts of the new pools
    rng = random.Random(0x91e)
    for weights in ([4, 3, 2, 3, 2, 2, 1, 1, 1, 1, 3, 3, 1, 1, 1, 1, 1], [2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 3, 3, 2, 2, 1]):
        for length in [2, 4, 8, 16, 32]:
            for k in range(6):
                ops = []
                for _ in range(length):
                    o = rng.choices(range(17), weights)[0]
                    t = base_r + rng.randrange(ext_r) if o == 6 else base_t + rng.randrange(ext_t)
                    ops.append((o, rng.randrange(256), rng.randrange(256), t, rng.randrange(256), 0))
                out.add(seed(k % 2, rng.randrange(3000), rng.randrange(length), ops))
    return out


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
    seeds |= ext_seeds()
    for s in sorted(seeds):
        name = ("ext-%s.seed" if s.startswith(EXT_MAGIC) else "own-%s.seed") % hashlib.sha256(s).hexdigest()[:12]
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(s)
    print("wrote %d seeds to %s" % (len(seeds), OUT))


main()
