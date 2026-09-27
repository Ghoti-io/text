#!/usr/bin/env python3
"""Generate the UTS #46 mapping table.

Reads ``third_party/idna/<mapping version>/IdnaMappingTable.txt`` and writes
``src/idna/tables/uts46_tables.c``. Like
gen_tables.py, the output is committed so that a build needs neither the
network nor Python, and ``make check-idna-tables`` regenerates and diffs.

Two data sets with two version pins, because the mapping table is not part of
the UCD and the two are published on different schedules. Both are 17.0.0.
Only the characters the mapping table *changes* are taken from it - mapped and
ignored - and RFC 5892, derived in gen_tables.py from the UCD, still decides
what is valid. A character the UCD has and the mapping table does not is one
the mapping step leaves alone, which is what an unlisted character means in
any case.

Deviations are deliberately absent. Nontransitional processing - which is what
every browser does now and what this library does - leaves all four of them
alone, so an entry saying "change nothing" would be an entry that has to be
read to discover it does nothing.

NFC used to be generated here too, from UnicodeData.txt and
CompositionExclusions.txt. It is ghoti.io-unicode's now - one copy of that data
for the suite instead of one per library - so this generator reads
IdnaMappingTable.txt and nothing from the UCD.

Determinism is a requirement, not a nicety: the check target diffs the
regenerated output against the committed files, so everything is emitted from
sorted input in a fixed format.

Usage:
    tools/idna/gen_uts46.py [--mapping DIR] [--out DIR]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import sys

HANGUL_S_BASE = 0xAC00
HANGUL_S_COUNT = 11172


def die(message):
    sys.exit("gen_uts46.py: " + message)


def read_lines(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.readlines()
    except OSError as exc:
        die("%s: %s\nrun tools/idna/fetch.sh first" % (path, exc))


def parse_mapping(path):
    """{codepoint: ('mapped', [cps]) or ('ignored', [])}, expanded per point."""
    out = {}
    for line in read_lines(path):
        line = line.split("#")[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(";")]
        if len(fields) < 2:
            continue
        span, status = fields[0], fields[1]
        if status not in ("mapped", "ignored"):
            continue
        if ".." in span:
            lo, hi = [int(x, 16) for x in span.split("..")]
        else:
            lo = hi = int(span, 16)
        target = []
        if len(fields) > 2 and fields[2]:
            target = [int(x, 16) for x in fields[2].split()]
        if status == "mapped" and not target:
            die("%04X..%04X is mapped to nothing" % (lo, hi))
        if status == "ignored" and target:
            die("%04X..%04X is ignored and has a mapping" % (lo, hi))
        # Expanded rather than kept as a range. A range's members do not share
        # a mapping - FF21..FF3A is twenty-six different letters - so a range
        # form would need a per-member rule anyway, and there are only a few
        # hundred codepoints inside multi-codepoint ranges.
        for cp in range(lo, hi + 1):
            if cp in out:
                die("%04X appears twice in the mapping table" % cp)
            out[cp] = (status, target)
    if not out:
        die("the mapping table yielded nothing; is the file empty?")
    return out


def runs_of(values):
    """[(lo, hi, value)] over a {codepoint: value} mapping."""
    out = []
    for cp in sorted(values):
        value = values[cp]
        if out and out[-1][1] == cp - 1 and out[-1][2] == value:
            out[-1] = (out[-1][0], cp, value)
        else:
            out.append((cp, cp, value))
    return out


def wrap(out, opening, pieces, closing):
    out.append(opening)
    line = " "
    for piece in pieces:
        if len(line) + len(piece) > 78:
            out.append(line)
            line = " "
        line += piece
    if line.strip():
        out.append(line)
    out.append(closing)


# Every generated source carries the same licence notice as a hand-written
# one. It is emitted here rather than added afterwards, so that regenerating
# does not quietly drop it.
LICENSE_NOTICE = """\
/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
"""


BANNER = LICENSE_NOTICE + "\n" + """/**
 * @file
 *
 * GENERATED by tools/idna/gen_uts46.py. Do not edit.
 *
 * {what}
 */

#include "tables_internal.h"
"""


def emit_mapping(mapping, mapping_version):
    """uts46_tables.c: the characters the mapping step changes."""
    pool = []
    pool_index = {}
    entries = []
    for cp in sorted(mapping):
        status, target = mapping[cp]
        key = tuple(target)
        if key not in pool_index:
            pool_index[key] = len(pool)
            pool.extend(target)
        entries.append((cp, pool_index[key], len(target),
                        "GTEXT_UTS46_IGNORED" if status == "ignored"
                        else "GTEXT_UTS46_MAPPED"))

    out = [BANNER.format(what=(
        "UTS #46 mapping table %s: the characters the mapping step\n"
        " * changes, and nothing else. A character absent from this table is\n"
        " * one the step leaves alone." % mapping_version)).rstrip(), ""]

    wrap(out, "static const uint32_t uts46_pool[] = {",
         ["0x%04X," % cp for cp in pool], "};")
    out.append("")
    out.append("const GTEXT_UTS46_Entry gtext_uts46_map[] = {")
    line = " "
    for cp, offset, length, status in entries:
        piece = " {0x%04X,%d,%d,%s}," % (cp, offset, length, status)
        if len(line) + len(piece) > 78:
            out.append(line)
            line = " "
        line += piece
    if line.strip():
        out.append(line)
    out.append("};")
    out.append("")
    out.append("const size_t gtext_uts46_map_count =")
    out.append("    sizeof(gtext_uts46_map) / sizeof(gtext_uts46_map[0]);")
    out.append("")
    out.append("const uint32_t * const gtext_uts46_pool = uts46_pool;")
    out.append("")
    return "\n".join(out) + "\n", len(entries), len(pool)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    mapping_version = open(
        os.path.join(here, "IDNA_MAPPING_VERSION"),
        encoding="utf-8").read().strip()

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mapping", default=os.path.join(
        root, "third_party", "idna", mapping_version))
    parser.add_argument("--out", default=os.path.join(
        root, "src", "idna", "tables"))
    args = parser.parse_args()

    mapping = parse_mapping(
        os.path.join(args.mapping, "IdnaMappingTable.txt"))

    # A sanity check that has caught a wrong file before: the mapping table
    # must map the three stops UTS #46 section 4.5 treats as separators, or
    # `a。b` silently becomes one label instead of two.
    for stop in (0x3002, 0xFF0E, 0xFF61):
        entry = mapping.get(stop)
        if not entry or entry[0] != "mapped" or entry[1] != [0x002E]:
            die("U+%04X does not map to FULL STOP; wrong mapping table?" % stop)

    os.makedirs(args.out, exist_ok=True)
    text, entries, pool = emit_mapping(mapping, mapping_version)
    with open(os.path.join(args.out, "uts46_tables.c"), "w",
              encoding="ascii", newline="\n") as handle:
        handle.write(text)
    print("uts46_tables.c: %d characters, %d mapping codepoints"
          % (entries, pool))



if __name__ == "__main__":
    main()
