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
jis0212, euc-kr) are in indexes.json too and are read by the next stage; they are
not emitted yet, because a table nothing reads is a table nothing checks.

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
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "encoding_tables.c"), "w", encoding="ascii", newline="\n") as handle:
        handle.write(text)
    print("%d encodings, %d labels, %d single-byte tables" % (len(encodings), labels, tables))


if __name__ == "__main__":
    main(sys.argv[1:])
