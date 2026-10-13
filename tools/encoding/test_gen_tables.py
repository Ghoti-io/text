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
"""Show that gen_tables.py refuses what its 16-bit forward tables cannot hold (run by make check-encoding-tables).

    tools/encoding/test_gen_tables.py [indexes.json]

A code point at or above U+10000 in any index but Big5 would be cut to its low 16 bits without a word; Big5 has a plane bit and a pinned set. Each case below
changes one entry of the real index and the generator must stop with a message naming the problem; the unchanged index must pass.

Copyright 2026 by Corey Pennycuff
"""

import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_tables  # noqa: E402


def refused(indexes, needle):
    try:
        gen_tables.check_cjk_code_points(indexes)
    except SystemExit as exc:
        return needle in str(exc)
    return False


def main(argv):
    path = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(HERE)), "third_party", "encoding", "indexes.json")
    with open(path, encoding="utf-8") as fh:
        base = json.load(fh)
    failures = []
    sup = gen_tables.check_cjk_code_points(base)
    if len(sup) != 1713:
        failures.append("the real index does not give 1,713 supplementary Big5 code points")

    def case(name, needle, mutate):
        indexes = copy.deepcopy(base)
        mutate(indexes)
        if not refused(indexes, needle):
            failures.append(name)

    for name in ("jis0208", "jis0212", "gb18030", "euc-kr", "iso-2022-jp-katakana"):
        def plant(ix, name=name):
            ix[name][ix[name].index(next(c for c in ix[name] if c is not None))] = 0x20000
        case(name + " above the BMP", "only Big5 has a plane bit", plant)
    case("big5 gains one supplementary code point", "pinned set", lambda ix: ix["big5"].__setitem__(ix["big5"].index(next(c for c in ix["big5"] if c is not None)), 0x20001))
    case("big5 loses one supplementary code point", "pinned set", lambda ix: ix["big5"].__setitem__(next(p for p, c in enumerate(ix["big5"]) if c and c >= 0x10000), None))
    case("big5 moves one supplementary code point", "pinned set", lambda ix: ix["big5"].__setitem__(next(p for p, c in enumerate(ix["big5"]) if c and c >= 0x10000), 0x2FFFF))
    case("big5 code point in plane 3", "outside what the tables hold", lambda ix: ix["big5"].__setitem__(next(p for p, c in enumerate(ix["big5"]) if c and c >= 0x10000), 0x30000))
    case("a surrogate", "outside what the tables hold", lambda ix: ix["jis0208"].__setitem__(0, 0xD800))
    if failures:
        sys.stderr.write("gen_tables.py did not refuse: %s\n" % "; ".join(failures))
        return 1
    print("gen_tables.py refuses every planted index (%d cases)" % 10)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
