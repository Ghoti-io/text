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
"""Write the seed corpus of tests/fuzz/fuzz_css_transition.cpp into tests/fuzz/corpus/css-transition.

Byte 0 selects the out-of-memory mode (low two bits 0), byte 1 the first base list of transition declarations, then triples of (value, value, time) per style change event.
Twelve base lists times two scripts (a hover that reverses, and a script that sweeps the pool), forty random scripts and one that moves time backwards.
"""
import os
import random
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests", "fuzz", "corpus", "css-transition")
os.makedirs(out, exist_ok=True)
rnd = random.Random(20261107)
seeds = []
for base in range(12):
    seeds.append(bytes([1, base, 0, 1, 4, 1, 0, 4, 0, 1, 4, 2, 2, 15, 3, 4, 8]))
    seeds.append(bytes([0, base, 3, 4, 2, 4, 3, 4, 3, 4, 1, 5, 5, 0, 6, 7, 7]))
for _ in range(40):
    seeds.append(bytes(rnd.randrange(256) for _ in range(rnd.randrange(4, 40))))
seeds.append(bytes([0, 0, 0, 1, 255, 1, 0, 255, 0, 1, 255]))
for i, b in enumerate(seeds):
    with open(os.path.join(out, "s%03d.seed" % i), "wb") as f:
        f.write(b)
print("%d seeds in %s" % (len(seeds), out))
