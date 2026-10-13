#!/usr/bin/env python3
"""Generate the Encoding Standard tables: labels, names and the single-byte indexes.

Reads ``third_party/encoding/encodings.json`` and ``indexes.json`` (fetched and
verified against their pinned hashes by ``tools/encoding/fetch.sh``) and writes
``src/encoding/tables/encoding_tables.c``. The output is committed, because a
build must need neither the network nor Python; this script is run by a person
when a pin changes, and by ``make check-encoding-tables`` to prove that what is
committed is what this script produces.

What the tables are, from WHATWG Encoding: the label table of section 4.2 (every
label lowercased and sorted by bytes, so that a lookup is a binary search after
trimming and lowercasing the input), the 40 encodings' names indexed by the public
enum, the 27 single-byte indexes of section 5 as decode tables (byte - 0x80 to
code point) and as encode tables (the first pointer for each code point, sorted by
code point for a binary search). The multi-byte indexes (gb18030, big5, jis0208,
jis0212, euc-kr, gb18030 ranges, ISO-2022-JP katakana) go to a second file,
``src/encoding/tables/encoding_tables_cjk.c``: each index as a forward array
(pointer to code point, one 16-bit load) and, for those an encoder reads, a
two-level reverse table (code point to pointer, with the standard's tie-breaking:
the first pointer, the last for six Big5 code points, and "index Shift_JIS pointer"
and "index Big5 pointer" as the standard excludes entries from their indexes).

The public ``GTEXT_Encoding`` enum is hand-written in encoding_core.h; this
script reads it and fails unless it lists exactly the standard's encodings in the
standard's order, so that the enum, the names and the table cannot drift.

Determinism is a requirement: the check target diffs the regenerated output
against the committed file, so everything is emitted from sorted input in a
fixed format with no timestamps and no dict-ordering dependence.

Usage:
    tools/encoding/gen_tables.py [--encodings FILE] [--indexes FILE] [--out DIR]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

LICENSE = """\
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

# The C side stores a label's pool offset in 16 bits, its length in 8, and an
# encoding's number in 8; a name must fit its 16-byte slot. Asserted here rather
# than assumed, so a future encodings.json that outgrows them fails the generator
# instead of producing a table that wraps.
MAX_POOL = 0xFFFF
MAX_LABEL = 64  # the key of the lookup (GTEXT_ENCODING_LABEL_KEY in encoding_internal.h)
NAME_SLOT = 16
NULL_CP = 0xFFFF  # an index entry that maps to nothing; no index uses U+FFFF


def enumerator(name):
    return "GTEXT_ENCODING_" + re.sub(r"[^A-Za-z0-9]", "_", name).upper()


def sha_of(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def check_pin(path, pin_file):
    got = sha_of(path)
    with open(os.path.join(HERE, pin_file), encoding="ascii") as handle:
        pinned = handle.read().strip()
    if got != pinned:
        raise SystemExit("%s is %s, pinned %s; run tools/encoding/fetch.sh"
                         % (os.path.basename(path), got, pinned))
    return got


def load_encodings(path):
    """[(name, [labels])] in the standard's order, with the invariants the C side relies on."""
    with open(path, encoding="utf-8") as handle:
        groups = json.load(handle)
    encodings = []
    for group in groups:
        for entry in group["encodings"]:
            encodings.append((entry["name"], list(entry["labels"])))
    names = [n for n, _ in encodings]
    if len(set(names)) != len(names):
        raise SystemExit("duplicate encoding name")
    seen = {}
    for name, labels in encodings:
        if not name.isascii() or len(name) >= NAME_SLOT:
            raise SystemExit("name %r does not fit %d bytes" % (name, NAME_SLOT))
        # "For each encoding, ASCII-lowercasing its name yields one of its labels."
        if name.lower() not in labels:
            raise SystemExit("%s: lowercased name is not among its labels" % name)
        for label in labels:
            # The lookup lowercases its input and compares with these, so a label
            # that is not lowercase ASCII could never match.
            if not label.isascii() or label != label.lower() or not label:
                raise SystemExit("label %r is not non-empty lowercase ASCII" % label)
            if len(label) > MAX_LABEL:
                raise SystemExit("label %r is too long" % label)
            if label in seen:
                raise SystemExit("label %r names both %s and %s" % (label, seen[label], name))
            seen[label] = name
    return encodings


def check_header(encodings, header_path):
    """The public enum must list exactly the standard's encodings (NONE first, COUNT last)."""
    with open(header_path, encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"typedef enum \{(.*?)\} GTEXT_Encoding;", text, re.S)
    if not match:
        raise SystemExit("%s: no GTEXT_Encoding enum" % header_path)
    found = re.findall(r"^\s*(GTEXT_ENCODING_[A-Z0-9_]+)\b", match.group(1), re.M)
    want = ["GTEXT_ENCODING_NONE"] + [enumerator(n) for n, _ in encodings] + ["GTEXT_ENCODING_COUNT"]
    if found != want:
        extra = [f for f in found if f not in want]
        missing = [w for w in want if w not in found]
        raise SystemExit("the GTEXT_Encoding enum in %s is not the standard's list (in order):"
                         " extra %s, missing %s" % (header_path, extra, missing))


def emit(encodings, indexes, single_group, enc_sha, idx_sha):
    names = [n for n, _ in encodings]
    single_names = [n for n in single_group]
    tables = {}
    order = []
    for name in single_names:
        key = name.lower()
        if key == "iso-8859-8-i":
            continue
        if key not in indexes or len(indexes[key]) != 128:
            raise SystemExit("single-byte encoding %s has no 128-entry index" % name)
        tables[name] = len(order)
        order.append((name, indexes[key]))
    tables["ISO-8859-8-I"] = tables["ISO-8859-8"]
    for name, index in order:
        for cp in index:
            # The C side holds a code point in 16 bits and uses 0xFFFF for "none".
            if cp is not None and (cp >= NULL_CP or 0xD800 <= cp <= 0xDFFF or cp < 0x80):
                raise SystemExit("%s: U+%04X is not a BMP scalar value above ASCII" % (name, cp))
    # Every 128-entry index must be one of those tables.
    wanted = {name.lower() for name, _ in order}
    for key, value in indexes.items():
        if len(value) == 128 and key not in wanted:
            raise SystemExit("index %s has 128 entries and no encoding" % key)

    out = [LICENSE]
    out.append("""
/**
 * @file
 *
 * GENERATED by tools/encoding/gen_tables.py from the Encoding Standard's
 * encodings.json (sha256 %s)
 * and indexes.json (sha256 %s).
 * Do not edit.
 *
 * Regenerate with:  tools/encoding/gen_tables.py
 * Verify with:      make check-encoding-tables
 */

#include "../encoding_internal.h"

_Static_assert(GTEXT_ENCODING_SB_TABLES == %d, "the generator and encoding_internal.h disagree");

""" % (enc_sha, idx_sha, len(order)))

    # Names: one fixed slot each, indexed by the public enum, so that no pointer needs a relocation.
    out.append("/// The standard's name of each encoding, indexed by GTEXT_Encoding.\n")
    out.append("const char gtext_encoding_names[GTEXT_ENCODING_COUNT][%d] = {\n" % NAME_SLOT)
    for name in names:
        out.append('  [%s] = "%s",\n' % (enumerator(name), name))
    out.append("};\n\n")

    # Labels, sorted by bytes, in one pool without terminators.
    labels = sorted((label, name) for name, ls in encodings for label in ls)
    pool = bytearray()
    rows = []
    for label, name in labels:
        rows.append((len(pool), len(label), enumerator(name)))
        pool += label.encode("ascii")
    if len(pool) > MAX_POOL:
        raise SystemExit("label pool is %d bytes; the offset is 16 bits" % len(pool))
    longest = max(len(label) for label, _ in labels)
    out.append("/// How many labels the table holds.\n")
    out.append("const size_t gtext_encoding_label_total = %d;\n" % len(labels))
    out.append("_Static_assert(%d <= GTEXT_ENCODING_LABEL_KEY, \"the longest label fits the lookup's key\");\n" % longest)
    out.append("/// The length of the longest label.\n")
    out.append("const size_t gtext_encoding_label_longest = %d;\n\n" % longest)
    out.append("/// The labels, lowercase, run together without terminators.\n")
    out.append("const char gtext_encoding_label_pool[%d] = {\n" % len(pool))
    lines, line = [], ""
    for byte in pool:
        piece = "'%s'," % chr(byte)
        if line and len(line) + 1 + len(piece) > 76:
            lines.append(line)
            line = ""
        line = line + " " + piece if line else piece
    lines.append(line)
    out.append("\n".join("  " + l for l in lines))
    out.append("\n};\n\n")
    out.append("/// The labels sorted by bytes: where each is in the pool, its length and its encoding.\n")
    out.append("const GTEXT_Encoding_Label_Entry gtext_encoding_label_table[%d] = {\n" % len(rows))
    for off, ln, enum in rows:
        out.append("  {%d, %d, %s},\n" % (off, ln, enum))
    out.append("};\n\n")

    # Which single-byte table an encoding uses (-1: none).
    out.append("/// The single-byte table an encoding uses, or -1.\n")
    out.append("const int8_t gtext_encoding_sb_index[GTEXT_ENCODING_COUNT] = {\n")
    for name in names:
        out.append("  [%s] = %d,\n" % (enumerator(name), tables.get(name, -1)))
    out.append("};\n\n")

    # Decode tables: byte - 0x80 -> code point, 0xFFFF for none.
    out.append("/// The decode tables: byte - 0x80 to code point, 0xFFFF for none.\n")
    out.append("const uint16_t gtext_encoding_sb_decode[GTEXT_ENCODING_SB_TABLES][128] = {\n")
    for name, index in order:
        out.append("  /* %s */ {\n" % name)
        cells = ["0x%04X" % (NULL_CP if v is None else v) for v in index]
        for i in range(0, 128, 8):
            out.append("    " + ", ".join(cells[i:i + 8]) + ",\n")
        out.append("  },\n")
    out.append("};\n\n")

    # Encode tables: the code points of each index in order, each with its byte. "The index
    # pointer for a code point is the first pointer whose code point is that one", so for a code
    # point that occurs twice the lower pointer is kept (no single-byte index has one: the
    # generator would otherwise have to say so here).
    counts = []
    cps_rows = []
    byte_rows = []
    for name, index in order:
        first = {}
        for pointer, cp in enumerate(index):
            if cp is None:
                continue
            if cp in first:
                continue
            first[cp] = pointer
        pairs = sorted(first.items())
        counts.append(len(pairs))
        cps_rows.append([cp for cp, _ in pairs])
        byte_rows.append([0x80 + p for _, p in pairs])
    out.append("/// How many code points each index has: the length of its encode arrays.\n")
    out.append("const uint8_t gtext_encoding_sb_encode_count[GTEXT_ENCODING_SB_TABLES] = {\n  %s\n};\n\n"
               % ", ".join(str(c) for c in counts))
    out.append("/// The code points of each index, ascending: what the encoder searches.\n")
    out.append("const uint16_t gtext_encoding_sb_encode_cp[GTEXT_ENCODING_SB_TABLES][128] = {\n")
    for (name, _), cps in zip(order, cps_rows):
        cells = ["0x%04X" % c for c in cps] + ["0x0000"] * (128 - len(cps))
        out.append("  /* %s */ {\n" % name)
        for i in range(0, 128, 8):
            out.append("    " + ", ".join(cells[i:i + 8]) + ",\n")
        out.append("  },\n")
    out.append("};\n\n")
    out.append("/// The byte of each code point of gtext_encoding_sb_encode_cp.\n")
    out.append("const uint8_t gtext_encoding_sb_encode_byte[GTEXT_ENCODING_SB_TABLES][128] = {\n")
    for (name, _), bs in zip(order, byte_rows):
        cells = ["0x%02X" % b for b in bs] + ["0x00"] * (128 - len(bs))
        out.append("  /* %s */ {\n" % name)
        for i in range(0, 128, 8):
            out.append("    " + ", ".join(cells[i:i + 8]) + ",\n")
        out.append("  },\n")
    out.append("};\n")
    return "".join(out), len(order), len(labels)


# ---- the multi-byte indexes ------------------------------------------------------------------------

# The pointer counts the C side declares (GTEXT_*_POINTERS in encoding_internal.h); the generated file
# includes that header, so a count that differs is a compile error as well as a failure here.
POINTERS = {"jis0208": 11280, "jis0212": 8836, "gb18030": 23940, "big5": 19782, "euc-kr": 23940,
            "iso-2022-jp-katakana": 63}
RANGES = 207
PTR_NONE = 0xFFFF
SIX_LAST = (0x2550, 0x255E, 0x2561, 0x256A, 0x5341, 0x5345)  # Big5: "return the last pointer"
BIG5_FIRST_POINTER = (0xA1 - 0x81) * 157                       # index Big5 excludes pointers below this
SJIS_EXCLUDED = range(8272, 8835 + 1)                              # index Shift_JIS excludes these jis0208 pointers


def first_pointers(index, skip=lambda p: False, last=()):
    """code point -> pointer: 'the index pointer for a code point' is the first pointer of the (filtered) index."""
    found = {}
    for pointer, cp in enumerate(index):
        if cp is None or skip(pointer):
            continue
        if cp in found and cp not in last:
            continue
        found[cp] = pointer
    return found


def reverse_family(maps):
    """Two-level reverse tables for several code point -> pointer maps that share one block pool.

    The BMP part of each map is split into blocks of 1 << shift pointers (the shift that makes the family smallest);
    a block that repeats is stored once and block 0 is the empty block. Code points above the BMP go to a sorted list.
    Returns (shift, pool, [(block_index, limit, supplementary_cps, supplementary_pointers)])."""
    best = None
    for shift in (5, 6, 7, 8):
        size = 1 << shift
        pool = [tuple([PTR_NONE] * size)]
        where = {pool[0]: 0}
        entries = []
        for m in maps:
            bmp = {c: p for c, p in m.items() if c < 0x10000}
            limit = (max(bmp) + 1) if bmp else 0
            nblocks = (limit + size - 1) // size
            block_index = []
            for b in range(nblocks):
                key = tuple(bmp.get(c, PTR_NONE) for c in range(b * size, (b + 1) * size))
                if key not in where:
                    where[key] = len(pool)
                    pool.append(key)
                block_index.append(where[key])
            sup = sorted((c, p) for c, p in m.items() if c >= 0x10000)
            entries.append((block_index, limit, [c for c, _ in sup], [p for _, p in sup]))
        total = len(pool) * size * 2 + sum(len(e[0]) * 2 for e in entries)
        if best is None or total < best[0]:
            best = (total, shift, pool, entries)
    if len(best[2]) > 0xFFFF:
        raise SystemExit("reverse block pool too large for 16-bit block numbers")
    return best[1], best[2], best[3]


def cells(values, fmt, per_line):
    """Lines of `per_line` cells; PTR_NONE is written N (defined at the top of the file) so that the many empty cells cost two bytes."""
    out = []
    for i in range(0, len(values), per_line):
        out.append("  " + ",".join("N" if v == PTR_NONE and fmt == "%d" else fmt % v for v in values[i:i + per_line]) + ",\n")
    return "".join(out)


# The code points of Big5 above the BMP, which the tables keep as 16 bits plus a plane bit (gtext_encoding_big5_plane2): the exact set is pinned, so that a
# changed index is a decision made here and not a silent truncation. 1,713 pointers, all in plane 2 (U+20000 to U+2FFFF), as served on 2026-10-13.
BIG5_SUPPLEMENTARY_COUNT = 1713
BIG5_SUPPLEMENTARY_SHA256 = "4c2d9ddc8cff8eaeb360e69a3b4252646b0439d7d2a79d02527a2b14bd6f0252"


def check_cjk_code_points(indexes):
    """Refuse an index the 16-bit forward tables cannot hold; return Big5's (pointer, code point) pairs above the BMP."""
    for name in ("jis0208", "jis0212", "gb18030", "big5", "euc-kr", "iso-2022-jp-katakana"):
        for cp in indexes[name]:
            if cp is None:
                continue
            if 0xD800 <= cp <= 0xDFFF or cp == 0xFFFF:
                raise SystemExit("%s: U+%04X is outside what the tables hold" % (name, cp))
            if cp >= 0x10000 and name != "big5":
                raise SystemExit("%s: U+%04X is above the BMP and only Big5 has a plane bit; the 16-bit table would truncate it" % (name, cp))
            if cp > 0x2FFFF:
                raise SystemExit("%s: U+%04X is outside what the tables hold" % (name, cp))
    sup = [(p, c) for p, c in enumerate(indexes["big5"]) if c is not None and c >= 0x10000]
    if any(not 0x20000 <= c <= 0x2FFFF for _, c in sup):
        raise SystemExit("big5: a code point above the BMP is outside plane 2")
    digest = hashlib.sha256(repr(sup).encode()).hexdigest()
    if len(sup) != BIG5_SUPPLEMENTARY_COUNT or digest != BIG5_SUPPLEMENTARY_SHA256:
        raise SystemExit("big5: the supplementary code points are not the pinned set (%d, sha256 %s); decide the change here" % (len(sup), digest))
    return sup


def emit_cjk(indexes, enc_sha, idx_sha):
    for name, count in POINTERS.items():
        if len(indexes[name]) != count:
            raise SystemExit("index %s has %d entries; the C side declares %d" % (name, len(indexes[name]), count))
    ranges = indexes["gb18030-ranges"]
    if len(ranges) != RANGES:
        raise SystemExit("gb18030-ranges has %d rows; the C side declares %d" % (len(ranges), RANGES))
    # The two functions over the ranges need both columns ascending ("the last pointer ... less than or equal").
    for a, b in zip(ranges, ranges[1:]):
        if not (a[0] < b[0] and a[1] < b[1]):
            raise SystemExit("gb18030-ranges are not ascending in both columns")
    big5_sup = check_cjk_code_points(indexes)

    out = [LICENSE]
    out.append("""
/**
 * @file
 *
 * GENERATED by tools/encoding/gen_tables.py from the Encoding Standard's
 * indexes.json (sha256 %s;
 * encodings.json sha256 %s).
 * Do not edit.
 *
 * The multi-byte indexes: forward arrays (pointer to code point, 0xFFFF for none)
 * and two-level reverse tables (code point to pointer).
 *
 * Regenerate with:  tools/encoding/gen_tables.py
 * Verify with:      make check-encoding-tables
 */

#include "../encoding_internal.h"

/** An empty cell, which most of the reverse tables' cells are (undefined again at the end). */
#define N GTEXT_INDEX_NONE

""" % (idx_sha, enc_sha))

    def forward(cname, macro, doc, index, plane_bit=False):
        if not plane_bit and any(c is not None and c >= 0x10000 for c in index):
            raise SystemExit("%s: a code point above the BMP and no plane bit for it" % cname)
        out.append("/// %s\n" % doc)
        out.append("const uint16_t %s[%s] = {\n" % (cname, macro))
        out.append(cells([PTR_NONE if c is None else (c & 0xFFFF) for c in index], "%d", 24))
        out.append("};\n\n")

    forward("gtext_encoding_jis0208_decode", "GTEXT_JIS0208_POINTERS", "index jis0208: pointer to code point.", indexes["jis0208"])
    forward("gtext_encoding_jis0212_decode", "GTEXT_JIS0212_POINTERS", "index jis0212: pointer to code point.", indexes["jis0212"])
    forward("gtext_encoding_gb18030_decode", "GTEXT_GB18030_POINTERS", "index gb18030: pointer to code point.", indexes["gb18030"])
    forward("gtext_encoding_euc_kr_decode", "GTEXT_EUC_KR_POINTERS", "index EUC-KR: pointer to code point.", indexes["euc-kr"])
    # Big5 holds 1,713 code points of plane 2: the low 16 bits are in the array and a bit says "plus 0x20000".
    forward("gtext_encoding_big5_decode", "GTEXT_BIG5_POINTERS", "index Big5: the low 16 bits of the code point of each pointer.", indexes["big5"], plane_bit=True)
    plane2 = bytearray((len(indexes["big5"]) + 7) // 8)
    for p, c in big5_sup:
        plane2[p // 8] |= 1 << (p % 8)
    out.append("/// index Big5: bit `pointer` is set when the code point is in plane 2 (add 0x20000 to the low 16 bits).\n")
    out.append("const uint8_t gtext_encoding_big5_plane2[GTEXT_BIG5_PLANE2_BYTES] = {\n")
    out.append(cells(list(plane2), "%d", 32))
    out.append("};\n\n")
    forward("gtext_encoding_iso_2022_jp_katakana_decode", "GTEXT_ISO_2022_JP_KATAKANA_POINTERS",
            "index ISO-2022-JP katakana: pointer to code point (halfwidth to fullwidth).", indexes["iso-2022-jp-katakana"])

    out.append("/// index gb18030 ranges: the pointer of each row, ascending.\n")
    out.append("const uint32_t gtext_encoding_gb18030_ranges_pointer[GTEXT_GB18030_RANGES] = {\n")
    out.append(cells([r[0] for r in ranges], "%d", 16))
    out.append("};\n\n")
    out.append("/// index gb18030 ranges: the code point of each row, ascending.\n")
    out.append("const uint32_t gtext_encoding_gb18030_ranges_cp[GTEXT_GB18030_RANGES] = {\n")
    out.append(cells([r[1] for r in ranges], "%d", 16))
    out.append("};\n\n")

    # The reverse tables. jis0208 has two (its own and index Shift_JIS's) over one pool; the others one each.
    jis_first = first_pointers(indexes["jis0208"])
    jis_sjis = first_pointers(indexes["jis0208"], skip=lambda p: p in SJIS_EXCLUDED)
    big5_map = first_pointers(indexes["big5"], skip=lambda p: p < BIG5_FIRST_POINTER, last=SIX_LAST)
    families = [
        ("jis0208", [("jis0208", jis_first), ("shift_jis", jis_sjis)]),
        ("gb18030", [("gb18030", first_pointers(indexes["gb18030"]))]),
        ("big5", [("big5", big5_map)]),
        ("euc_kr", [("euc_kr", first_pointers(indexes["euc-kr"]))]),
    ]
    sizes = []
    for fam, members in families:
        shift, pool, entries = reverse_family([m for _, m in members])
        size = 1 << shift
        out.append("/// The reverse tables of %s: blocks of %d pointers (0xFFFF for none).\n" % (fam, size))
        out.append("static const uint16_t %s_pool[%d] = {\n" % (fam, len(pool) * size))
        flat = [v for blk in pool for v in blk]
        out.append(cells(flat, "%d", 24))
        out.append("};\n\n")
        total = len(pool) * size * 2
        for (name, _), (bidx, limit, sup_cp, sup_ptr) in zip(members, entries):
            out.append("static const uint16_t %s_index[%d] = {\n" % (name, max(len(bidx), 1)))
            out.append(cells(bidx if bidx else [0], "%d", 32))
            out.append("};\n")
            total += len(bidx) * 2 + len(sup_cp) * 6
            if sup_cp:
                out.append("static const uint32_t %s_sup_cp[%d] = {\n" % (name, len(sup_cp)))
                out.append(cells(sup_cp, "0x%05X", 8))
                out.append("};\n")
                out.append("static const uint16_t %s_sup_ptr[%d] = {\n" % (name, len(sup_ptr)))
                out.append(cells(sup_ptr, "%d", 16))
                out.append("};\n")
            out.append("/// Code point to pointer for %s.\n" % name)
            out.append("const GTEXT_Rev_Index gtext_encoding_%s_rev = {\n" % name)
            out.append("  %s_index, %s_pool, %du, %d,\n" % (name, fam, limit, shift))
            if sup_cp:
                out.append("  %s_sup_cp, %s_sup_ptr, %du,\n" % (name, name, len(sup_cp)))
            else:
                out.append("  NULL, NULL, 0u,\n")
            out.append("};\n\n")
        sizes.append((fam, total))
    out.append("#undef N\n")
    return "".join(out), sizes


def single_byte_names(path):
    """The names of the single-byte encodings: the group the standard gives that heading."""
    with open(path, encoding="utf-8") as handle:
        groups = json.load(handle)
    found = [g for g in groups if g["heading"] == "Legacy single-byte encodings"]
    if len(found) != 1:
        raise SystemExit("encodings.json: no single group 'Legacy single-byte encodings'")
    return [e["name"] for e in found[0]["encodings"]]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    third = os.path.join(ROOT, "third_party", "encoding")
    ap.add_argument("--encodings", default=os.path.join(third, "encodings.json"))
    ap.add_argument("--indexes", default=os.path.join(third, "indexes.json"))
    ap.add_argument("--header", default=os.path.join(
        ROOT, "include", "ghoti.io", "text", "encoding", "encoding_core.h"))
    ap.add_argument("--out", default=os.path.join(ROOT, "src", "encoding", "tables"))
    args = ap.parse_args(argv)

    enc_sha = check_pin(args.encodings, "ENCODINGS_SHA256")
    idx_sha = check_pin(args.indexes, "INDEXES_SHA256")
    encodings = load_encodings(args.encodings)
    check_header(encodings, args.header)
    with open(args.indexes, encoding="utf-8") as handle:
        indexes = json.load(handle)
    text, tables, labels = emit(encodings, indexes, single_byte_names(args.encodings), enc_sha, idx_sha)
    cjk_text, sizes = emit_cjk(indexes, enc_sha, idx_sha)
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "encoding_tables.c"), "w", encoding="ascii", newline="\n") as handle:
        handle.write(text)
    with open(os.path.join(args.out, "encoding_tables_cjk.c"), "w", encoding="ascii", newline="\n") as handle:
        handle.write(cjk_text)
    print("%d encodings, %d labels, %d single-byte tables, multi-byte reverse tables %s bytes" % (
        len(encodings), labels, tables, ", ".join("%s %d" % z for z in sizes)))


if __name__ == "__main__":
    main(sys.argv[1:])
