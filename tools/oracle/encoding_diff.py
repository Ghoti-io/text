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
"""The Encoding differential: this library against Node's `TextDecoder`/`TextEncoder` and CPython's codecs, with the standard's data as arbiter.

    python3 tools/oracle/oracle_run.py encoding -- python3 tools/oracle/encoding_diff.py <encoding-ours> [options]

Two references, in the pinned image (containers/IMAGES, `encoding`), and **neither is the standard**:

- Node's `TextDecoder` and `TextEncoder`. The API is the standard's own - the label table, streaming, the fatal flag, ignoreBOM - and the
  decoders under it are ICU's converters. They agree with the standard for UTF-8 and UTF-16 (every sequence of one to three bytes: no
  difference) and differ from its single-byte indexes in places.
- CPython 3.13's codecs: a second reading where they agree, listed where they do not.

**Where a reference and this library differ, the standard decides, and the decision is data, not opinion:** `third_party/encoding/indexes.json`
and `encodings.json` are read here by Python (not by the library, whose tables are generated from them by a different program), and every
divergence is checked against them - the byte's code point in the index, the label's encoding in the table. A divergence the data does not
side with this library on is a defect and fails the run; one it does side with it on is a **line of tools/oracle/encoding_divergences.txt**,
which says what the reference does differently, why the standard does not, and how many of what were counted. A line whose count has changed,
a line for a divergence that is gone, and a divergence with no line all fail.

Populations (the numbers are printed; the defaults are those of the list, and ENCODING_ORACLE_COUNT/_SEED/_FATAL_STRIDE size a hunt):

  labels          every one of the 228 labels, in 13 spellings (case, the five ASCII white space characters before and after, and the
                  characters that are not ASCII white space: VT, NBSP, EM SPACE, NUL, ...), against Node's `TextDecoder(label).encoding`
  single-byte     every byte of every one of the 28 single-byte encodings, in replacement and fatal mode, against Node and Python
  sb-encode       every code point of the Basic Multilingual Plane, in every single-byte encoding, against Python's encoders and the index
  utf            every sequence of one to three bytes in UTF-8, UTF-16LE and UTF-16BE, in replacement mode (and in fatal mode: all of one and
                  two bytes, one in FATAL_STRIDE of three), against Node, and (every 7th of three) against Python
  ignore-bom      the first byte of every single-byte encoding with Node's ignoreBOM: true, which the standard reads for UTF-8 and UTF-16 only
  streaming       random and mutated byte strings, cut at random places, in every encoding whose reference agrees, against Node with
                  {stream: true}; fatal and replacement; ignoreBOM both ways (this library's BOM removal)
  encode-modes    random text in every single-byte encoding, in HTML and replacement modes, cut anywhere, against the index (the first pointer)
  sniff           the standard's "decode" (a byte order mark sniffed, whole or cut or nearly one) against Python's codecs
  utf8-encode     random byte strings, this library's UTF-8 encoder against Node's decode then TextEncoder
  buffer          the one-shot buffer form against the streaming one

--plant NAME corrupts one answer or the list, to show the gate fails (run-encoding-oracle.sh --controls): ours-extra-char, ours-lost-char,
stale-entry, change-count, no-list, witness. Exit status: 0 clean, 3 a failure (with a heading on stderr), 2 an unusable request or reference.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import encoding_mb as MB  # noqa: E402
import encoding_model as M  # noqa: E402
import encoding_ref as R  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
DATA = os.path.join(ROOT, "third_party", "encoding")
LIST = os.path.join(HERE, "encoding_divergences.txt")

# The Python codec that stands for each single-byte encoding (there is none for x-user-defined or replacement).
PY_CODEC = {
    "IBM866": "cp866", "macintosh": "mac_roman", "windows-874": "cp874", "x-mac-cyrillic": "mac_cyrillic",
    "KOI8-R": "koi8_r", "KOI8-U": "koi8_u", "ISO-8859-8-I": "iso8859_8",
}
for _n in (2, 3, 4, 5, 6, 7, 8, 10, 13, 14, 15, 16):
    PY_CODEC["ISO-8859-%d" % _n] = "iso8859_%d" % _n
for _n in range(1250, 1259):
    PY_CODEC["windows-%d" % _n] = "cp%d" % _n


class Failure(Exception):
    pass


class Run:
    def __init__(self, args):
        self.args = args
        self.sections = []
        self.divergences = defaultdict(lambda: {"count": 0, "examples": []})
        self.executed = set()   # (population, who) of the multi-byte populations that ran: what the list's lines for them are checked against
        self.unexplained = []   # divergences with no entry in the list, that the data does not settle
        self.defects = []       # divergences the standard's data does not side with this library on
        self.requests = 0
        self.ran = set()        # the populations that ran (by the names --only and --except use)

    def note(self, line):
        print(line, flush=True)

    def diverge(self, reference, population, cls, example, standard_agrees):
        """One divergence: counted under its class; a defect if the standard's data does not side with this library."""
        if not standard_agrees:
            self.defects.append("%s/%s/%s: %s" % (reference, population, cls, example if len(example) < 200 else example[:197] + "..."))
            return
        self.add(reference, population, cls, 1, example)

    def add(self, reference, population, cls, count, example):
        """`count` divergences of one class at once (the multi-byte populations count in worker processes)."""
        d = self.divergences[(reference, population, cls)]
        d["count"] += count
        if len(d["examples"]) < 3:
            d["examples"].append(example if len(example) < 200 else example[:197] + "...")


# ---- the standard's data -------------------------------------------------------------------------------------------------

class Standard:
    def __init__(self, corrupt_witness=False):
        with open(os.path.join(DATA, "encodings.json"), encoding="utf-8") as fh:
            groups = json.load(fh)
        self.names = []
        self.labels = {}
        self.single_byte = []
        for g in groups:
            for e in g["encodings"]:
                self.names.append(e["name"])
                for label in e["labels"]:
                    self.labels[label] = e["name"]
                if g["heading"] == "Legacy single-byte encodings":
                    self.single_byte.append(e["name"])
        with open(os.path.join(DATA, "indexes.json"), encoding="utf-8") as fh:
            self.indexes = json.load(fh)
        if corrupt_witness:
            # A control: the standard's data no longer sides with this library about windows-1252's 0x80.
            self.indexes["windows-1252"][0] = 0x0080

    def index(self, name):
        key = name.lower()
        if key == "iso-8859-8-i":
            key = "iso-8859-8"
        return self.indexes[key]

    def decode_byte(self, name, b):
        """The code point the standard's single-byte decoder gives for byte b, or None for 'error'."""
        if b < 0x80:
            return b
        return self.index(name)[b - 0x80]

    def encode_cp(self, name, cp):
        """The byte the standard's single-byte encoder gives for a code point (the first pointer), or None."""
        if cp < 0x80:
            return cp
        for p, c in enumerate(self.index(name)):
            if c == cp:
                return 0x80 + p
        return None


# ---- this library -----------------------------------------------------------------------------------------------------------

class Ours:
    def __init__(self, binary, plant=None):
        self.binary = binary
        self.plant = plant

    def run(self, lines):
        done = subprocess.run([self.binary, "serve"], input=("\n".join(lines) + "\n").encode("ascii"), capture_output=True)
        if done.returncode != 0:
            raise Failure("the library's runner exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-1500:]))
        out = done.stdout.decode("ascii").split("\n")
        if out and out[-1] == "":
            out.pop()
        if len(out) != len(lines):
            raise Failure("sent %d requests, got %d answers" % (len(lines), len(out)))
        if self.plant == "ours-extra-char":
            out = [("OUT %s%s" % (a.split()[1] if a.split()[1] != "-" else "", "e2") + a[len("OUT " + a.split()[1]):]) if a.startswith("OUT ") and i % 997 == 3 else a
                   for i, a in enumerate(out)]
        elif self.plant == "ours-lost-char":
            out = [("OUT -" if a.startswith("OUT ") and len(a) > 6 else a) if i % 991 == 5 else a for i, a in enumerate(out)]
        return out

    def encenum(self, label_hex, mode, first, last):
        done = subprocess.run([self.binary, "encenum", label_hex, mode, str(first), str(last)], capture_output=True)
        if done.returncode != 0:
            raise Failure("the library's encenum exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-1500:]))
        return done.stdout

    def enumx(self, label_hex, flags, spec):
        done = subprocess.run([self.binary, "enumx", label_hex, flags, spec], capture_output=True)
        if done.returncode != 0:
            raise Failure("the library's enumx exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-1500:]))
        return done.stdout

    def enum(self, label_hex, n, flags, start, count):
        done = subprocess.run([self.binary, "enum", label_hex, str(n), flags, str(start), str(count)], capture_output=True)
        if done.returncode != 0:
            raise Failure("the library's enum exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-1500:]))
        return done.stdout


def hx(b):
    return "-" if not b else b.hex()


def label_hex(s):
    return hx(s.encode("latin-1"))


def parse_ours(answer):
    """('OUT', bytes) or ('ERR', status) from one answer line of D / BUF / LIMIT."""
    t = answer.split()
    if t[0] == "OUT":
        return "OUT", R.unhex(t[1])
    return "ERR", t[1]


def parse_ref(answer):
    t = answer.split()
    if t[0] == "OUT":
        return "OUT", R.unhex(t[1])
    return "ERR", t[1]


def same(ours, ref):
    """Does an answer of this library agree with a reference's? A failure is a failure whatever its name."""
    return (ours[0] == ref[0]) and (ours[0] == "ERR" or ours[1] == ref[1])


# ---- populations --------------------------------------------------------------------------------------------------------

SPELLINGS = ("exact", "upper", "alternate", "sp-before", "sp-after", "tab-lf-before", "ff-cr-after", "all-ws-both", "vt-before",
             "vt-after", "nbsp-before", "em-space-after", "nul-after", "x-after")


def spell(label, kind):
    if kind == "exact":
        return label
    if kind == "upper":
        return label.upper()
    if kind == "alternate":
        return "".join(c.upper() if i % 2 else c.lower() for i, c in enumerate(label))
    if kind == "sp-before":
        return " " + label
    if kind == "sp-after":
        return label + " "
    if kind == "tab-lf-before":
        return "\t\n" + label
    if kind == "ff-cr-after":
        return label + "\f\r"
    if kind == "all-ws-both":
        return " \t\n\f\r" + label + "\r\f\n\t "
    if kind == "vt-before":
        return "\x0b" + label
    if kind == "vt-after":
        return label + "\x0b"
    if kind == "nbsp-before":
        return "\xa0" + label
    if kind == "em-space-after":
        return label + " "
    if kind == "nul-after":
        return label + "\x00"
    return label + "x"


def population_labels(run, std, ours):
    """Every label in every spelling: the name each reference gives, against the standard's table."""
    cases = []
    for label, name in sorted(std.labels.items()):
        for kind in SPELLINGS:
            s = spell(label, kind)
            # What the standard says: the trimmed, lowercased spelling is a label, or nothing is.
            trimmed = "".join(c.lower() if c.isascii() else c for c in s.strip(" \t\n\f\r"))
            want = std.labels.get(trimmed)
            cases.append((s, want))
    reqs = ["L " + hx(s.encode("utf-8")) for s, _ in cases]
    node = R.ask_node(reqs)
    mine = ours.run(reqs)
    run.requests += len(reqs)
    wrong = 0
    for (s, want), n, o in zip(cases, node, mine):
        want_name = want.lower() if want else "none"
        o_name = o.split()[1]
        if o_name != want_name:
            wrong += 1
            run.defects.append("labels: %r -> %s, the standard's table says %s" % (s, o_name, want_name))
        n_name = n.split()[1]
        if n_name != o_name:
            run.diverge("node", "labels", o_name, "%r: Node %s, the standard %s" % (s, n_name, o_name), o_name == want_name)
    run.note("labels: %d spellings of %d labels (%d requests), the standard's table agrees with the library in all but %d" %
             (len(SPELLINGS), len(std.labels), len(reqs), wrong))


def population_single_byte_decode(run, std, ours):
    """Every byte of every single-byte encoding, both modes: Node and Python against the library, the index deciding."""
    names = std.single_byte
    cases = [(name, b, fatal) for name in names for b in range(256) for fatal in (False, True)]
    reqs_node = ["D %s %s %02x" % (label_hex(n.lower()), "f" if f else "-", b) for n, b, f in cases]
    reqs_ours = ["BUF %s %s %02x" % (label_hex(n.lower()), "f" if f else "-", b) for n, b, f in cases]
    node = R.ask_node(reqs_node)
    mine = ours.run(reqs_ours)
    run.requests += len(cases) * 2
    py_cases = [(n, b) for n in names for b in range(256)]
    py = R.ask_python(["PD %s strict %02x" % (PY_CODEC[n], b) for n, b in py_cases])
    run.requests += len(py_cases)
    unmapped = 0
    mine_by = {}
    exclusions = defaultdict(set)
    for (name, b, fatal), n_ans, o_ans in zip(cases, node, mine):
        cp = std.decode_byte(name, b)
        o = parse_ours(o_ans)
        if cp is None:
            unmapped += 1
            expected = ("ERR", "E_MALFORMED") if fatal else ("OUT", "�".encode("utf-8"))
        else:
            expected = ("OUT", chr(cp).encode("utf-8"))
        if o != expected:
            run.defects.append("single-byte: %s byte %02x (%s) -> %r, the index says %r" % (name, b, "fatal" if fatal else "replacement", o, expected))
        mine_by[(name, b, fatal)] = o
        r = parse_ref(n_ans)
        if r[0] == "ERR" and r[1] == "RangeError":
            # Not constructible in Node at all (ICU has no converter): one class for the whole encoding.
            run.diverge("node", "single-byte", name, "%s %02x: RangeError" % (name, b), True)
            exclusions[name.lower()].update(range(256))
            continue
        if not same(o, ("ERR", None) if r[0] == "ERR" else r):
            exclusions[name.lower()].add(b)
            run.diverge("node", "single-byte", name, "%s %02x %s: Node %r, this library %r" % (name, b, "fatal" if fatal else "replacement", r, o), o == expected)
    for (name, b), p_ans in zip(py_cases, py):
        o = mine_by[(name, b, False)]
        p = parse_ref(p_ans)
        if p[0] == "ERR":
            cp = std.decode_byte(name, b)
            # Python raises where the standard has no code point and replacement mode writes U+FFFD; where the standard has one, it is a skew.
            if cp is None:
                continue
            run.diverge("python", "single-byte", name, "%s %02x: Python UnicodeDecodeError, the index says U+%04X" % (name, b, cp), o == ("OUT", chr(cp).encode("utf-8")))
        elif o != p:
            cp = std.decode_byte(name, b)
            run.diverge("python", "single-byte", name, "%s %02x: Python %r, the index says %r" % (name, b, p, cp), cp is not None and o == ("OUT", chr(cp).encode("utf-8")))
    run.note("single-byte: %d encodings x 256 bytes x 2 modes against Node (%d requests), x 1 against Python; the index leaves %d (byte, mode) pairs without a code point" %
             (len(names), len(cases), unmapped))
    return exclusions


def population_single_byte_encode(run, std, ours):
    """Every code point of the Basic Multilingual Plane in every single-byte encoding, against Python's encoders and the index."""
    names = std.single_byte
    cps = [cp for cp in range(0x80, 0x10000) if not 0xD800 <= cp <= 0xDFFF]
    total = 0
    for name in names:
        reqs = ["NE 0 %s F" % label_hex(name.lower())]
        for cp in cps:
            reqs.append("E 0 1 %s" % hx(chr(cp).encode("utf-8")))
        reqs.append("FREE 0")
        out = ours.run(reqs)
        run.requests += len(reqs)
        py = R.ask_python(["PE %s strict %s" % (PY_CODEC[name], hx(chr(cp).encode("utf-8"))) for cp in cps])
        run.requests += len(cps)
        for cp, o, p in zip(cps, out[1:-1], py):
            want = std.encode_cp(name, cp)
            if o.startswith("OUT"):
                got = R.unhex(o.split()[1])
                ok = want is not None and got == bytes([want])
            else:
                got = None
                ok = want is None
            if not ok:
                run.defects.append("sb-encode: %s U+%04X -> %r, the index says %r" % (name, cp, o, want))
            pr = parse_ref(p)
            if (pr[0] == "OUT") != (got is not None) or (pr[0] == "OUT" and pr[1] != got):
                run.diverge("python", "sb-encode", name, "%s U+%04X: Python %r, this library %r" % (name, cp, pr, got), ok)
            total += 1
    run.note("sb-encode: %d code points (U+0080 to U+FFFF but the surrogates) x %d encodings, each against Python and the index" % (len(cps), len(names)))


def population_utf(run, std, ours):
    """Every sequence of one to three bytes in UTF-8, UTF-16LE and UTF-16BE against Node (all of them) and Python (every 7th of three)."""
    stride = run.args.fatal_stride
    total = 0
    for name, pycodec in (("utf-8", "utf-8"), ("utf-16le", "utf-16-le"), ("utf-16be", "utf-16-be")):
        lab = hx(name.encode())
        for n in range(1, run.args.utf_bytes + 1):
            size = 256 ** n
            step = 1 << 22
            for fatal in (False, True):
                flags_ref = "fi" if fatal else "i"
                flags_ours = "f" if fatal else "-"
                if fatal and n == 3:
                    # Node throws an exception for each failing sequence; one in `stride` is compared (fatal = replacement made something, which
                    # the replacement-mode comparison above covers for every sequence).
                    seqs = range(0, size, stride)
                    count_ref = 0
                    for start in range(0, size, step):
                        cnt = min(step, size - start)
                        picks = [v for v in range(start, start + cnt) if v % stride == 0]
                        reqs = ["D %s %s %s" % (lab, flags_ref, hx(v.to_bytes(n, "big"))) for v in picks]
                        reqs_o = ["D 0 1 %s" % hx(v.to_bytes(n, "big")) for v in picks]
                        node = R.ask_node(reqs)
                        mine = ours.run(["ND 0 %s %s" % (lab, flags_ours)] + reqs_o)[1:]
                        for v, a, b in zip(picks, node, mine):
                            r = parse_ref(a)
                            o = parse_ours(b)
                            if not same(o, ("ERR", None) if r[0] == "ERR" else r):
                                run.diverge("node", "utf", "%s-fatal" % name, "%s: Node %r, this library %r" % (hx(v.to_bytes(n, 'big')), r, o), False)
                        count_ref += len(picks)
                        run.requests += 2 * len(picks)
                    total += count_ref
                    continue
                for start in range(0, size, step):
                    cnt = min(step, size - start)
                    node = R.enum_stream(R.node_command("enum", lab, n, flags_ref, start, cnt))
                    mine = ours.enum(lab, n, flags_ours, start, cnt)
                    if run.args.plant == "ours-extra-char" and n == 1 and start == 0 and not fatal and mine:
                        mine = mine[:1] + b"\x00" + mine[1:]
                    run.requests += cnt
                    total += cnt
                    if node == mine:
                        continue
                    rn = R.records(node)
                    ro = R.records(mine)
                    if len(rn) != len(ro):
                        run.defects.append("utf: %s %d-byte records %d vs %d" % (name, n, len(rn), len(ro)))
                        continue
                    for k, (a, b) in enumerate(zip(rn, ro)):
                        if a != b:
                            run.diverge("node", "utf", "%s%s" % (name, "-fatal" if fatal else ""), "%s: Node %r, this library %r" % (hx((start + k).to_bytes(n, "big")), a, b), False)
        # Python, every 7th three-byte sequence and all of one and two (replacement mode).
        for n in range(1, run.args.utf_bytes + 1):
            size = 256 ** n
            seq = [v for v in range(size) if n < 3 or v % 7 == 0]
            reqs = ["PD %s replace %s" % (pycodec, hx(v.to_bytes(n, "big"))) for v in seq]
            py = R.ask_python(reqs)
            mine = ours.run(["ND 0 %s -" % lab] + ["D 0 1 %s" % hx(v.to_bytes(n, "big")) for v in seq])[1:]
            run.requests += 2 * len(seq)
            for v, a, b in zip(seq, py, mine):
                p = parse_ref(a)
                o = parse_ours(b)
                if not same(o, p):
                    run.diverge("python", "utf", name, "%s: Python %r, this library %r" % (hx(v.to_bytes(n, 'big')), p, o), False)
    run.note("utf: UTF-8, UTF-16LE and UTF-16BE, every sequence of one to three bytes in replacement mode against Node (%d decodes in all, fatal mode included)" % total)


def random_bytes(rng, name, avoid):
    """A byte string of the kind the encoding's decoder has the most to say about."""
    length = rng.choice((0, 1, 2, 3, 4, 5, 8, 13, 40, 120, 300))
    out = bytearray()
    while len(out) < length:
        kind = rng.randrange(8)
        if name == "utf-8":
            if kind < 2:
                out.append(rng.randrange(256))
            elif kind < 5:
                out += chr(rng.choice((rng.randrange(0x80, 0x800), rng.randrange(0x800, 0xD800), rng.randrange(0xE000, 0x10000),
                                       rng.randrange(0x10000, 0x110000)))).encode("utf-8")
            elif kind == 5:
                out += bytes(rng.choice((0xC0, 0xC2, 0xE0, 0xE1, 0xED, 0xEF, 0xF0, 0xF1, 0xF4, 0xF5, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF)) for _ in range(rng.randrange(1, 4)))
            elif kind == 6:
                out += bytes((0xEF, 0xBB, 0xBF)) if rng.randrange(4) == 0 else b"ab"
            else:
                out.append(rng.randrange(0x20, 0x7F))
        elif name.startswith("utf-16"):
            unit = rng.choice((rng.randrange(0x20, 0x7F), rng.randrange(0xD800, 0xDC00), rng.randrange(0xDC00, 0xE000), rng.randrange(0x10000),
                               0xFEFF, 0xFFFE, 0xFFFF, rng.randrange(0x80, 0xD800)))
            big = name.endswith("be")
            if kind == 7:
                out.append(rng.randrange(256))
            else:
                out += unit.to_bytes(2, "big" if big else "little")
        else:
            b = rng.randrange(256) if kind < 5 else rng.randrange(0x20, 0x7F)
            while b in avoid:
                b = rng.randrange(0x20, 0x7F)
            out.append(b)
    return bytes(out)


def cuts_of(rng, data):
    n = rng.choice((1, 1, 2, 3, 5))
    points = sorted(rng.randrange(len(data) + 1) for _ in range(n - 1)) if data else []
    chunks = []
    prev = 0
    for p in points:
        chunks.append(data[prev:p])
        prev = p
    chunks.append(data[prev:])
    return chunks


def population_streaming(run, std, ours, exclusions):
    """Random and mutated byte strings, cut at random places, against Node with {stream: true}."""
    rng = random.Random(run.args.seed)
    count = run.args.count
    encs = ["utf-8", "utf-16le", "utf-16be"] + [n.lower() for n in std.single_byte if n not in ("ISO-8859-16",)]
    per = max(1, count // len(encs))
    total = 0
    for name in encs:
        avoid = exclusions.get(name, set())
        cases = []
        for _ in range(per):
            data = random_bytes(rng, name, avoid)
            chunks = cuts_of(rng, data)
            fatal = rng.randrange(3) == 0
            remove_bom = name in ("utf-8", "utf-16le", "utf-16be") and rng.randrange(2) == 0
            if name not in ("utf-8", "utf-16le", "utf-16be"):
                remove_bom = None  # no BOM handling for the legacy encodings (Node's ignoreBOM is left at its default, see population_ignore_bom)
            cases.append((chunks, fatal, remove_bom))
        node_reqs = []
        ours_reqs = []
        for i, (chunks, fatal, remove) in enumerate(cases):
            node_reqs.append("D %s %s %s" % (hx(name.encode()), ("f" if fatal else "") + ("" if remove is False else "") + ("" if remove is None or remove else "i") or "-",
                                            ",".join(hx(c) for c in chunks)))
            ours_reqs.append("ND 0 %s %s" % (hx(name.encode()), ("f" if fatal else "") + ("r" if remove else "") or "-"))
            for j, c in enumerate(chunks):
                ours_reqs.append("D 0 %d %s" % (1 if j == len(chunks) - 1 else 0, hx(c)))
        node = R.ask_node(node_reqs)
        mine = ours.run(ours_reqs)
        run.requests += len(node_reqs) + len(ours_reqs)
        k = 0
        for (chunks, fatal, remove), a in zip(cases, node):
            k += 1  # the ND line
            out = bytearray()
            failed = None
            for j, c in enumerate(chunks):
                line = mine[k]
                k += 1
                if failed is not None:
                    continue
                t = line.split()
                if t[0] == "OUT":
                    out += R.unhex(t[1])
                else:
                    failed = t[1]
                    out += R.unhex(t[3]) if len(t) > 3 else b""
            o = ("ERR", failed) if failed else ("OUT", bytes(out))
            r = parse_ref(a)
            if not same(o, ("ERR", None) if r[0] == "ERR" else r):
                run.diverge("node", "streaming", name, "%s %s%s: Node %r, this library %r" % (
                    "|".join(hx(c) for c in chunks), "fatal " if fatal else "", "bom-removal" if remove else "", r, o), False)
            total += 1
    run.note("streaming: %d random and mutated byte strings in %d encodings, cut into 1 to 5 pieces, fatal one in three, BOM removal half of the Unicode ones" % (total, len(encs)))
    return total


def population_ignore_bom(run, std, ours, exclusions):
    """Node's `ignoreBOM: true` on the legacy encodings, which the standard says has no meaning there.

    "If decoder's encoding is UTF-8 or UTF-16BE/LE, and decoder's ignore BOM and BOM seen are false" is the only place the flag is read:
    for a single-byte encoding it changes nothing. Node 20.19.2 drops a leading 0xFF byte of windows-1252 when it is set , so every first byte of every single-byte encoding is decoded both ways.
    """
    names = [n for n in std.single_byte if n != "ISO-8859-16"]
    cases = [(n, b) for n in names for b in range(256) if b not in exclusions.get(n.lower(), ())]
    node = R.ask_node(["D %s i %s" % (hx(n.lower().encode()), hx(bytes([b, 0x41]))) for n, b in cases])
    mine = ours.run(["BUF %s - %s" % (hx(n.lower().encode()), hx(bytes([b, 0x41]))) for n, b in cases])
    run.requests += 2 * len(cases)
    for (n, b), a, o in zip(cases, node, mine):
        r = parse_ref(a)
        mine_ans = parse_ours(o)
        if not same(mine_ans, r):
            # The standard: the flag is not read for this encoding, so the answer is the one without it, which is this library's.
            run.diverge("node", "ignore-bom", n, "%s %02x41 with ignoreBOM: Node %r, this library %r" % (n, b, r, mine_ans), True)
    run.note("ignore-bom: %d (encoding, first byte) pairs decoded with ignoreBOM: true in Node, which the standard reads for UTF-8 and UTF-16 only" % len(cases))


def population_encode_modes(run, std, ours):
    """The three error modes of the single-byte encoders over random text, cut anywhere, against the index (the first pointer) and the standard's modes."""
    rng = random.Random(run.args.seed + 3)
    names = std.single_byte
    cases = []
    for name in names:
        for _ in range(60):
            cps = []
            for _ in range(rng.choice((0, 1, 2, 5, 20, 80))):
                k = rng.randrange(6)
                if k == 0:
                    cps.append(rng.randrange(0x20, 0x7F))
                elif k <= 2:
                    # a code point the index has, so that the encodable ones are well represented
                    idx = [c for c in std.index(name) if c is not None]
                    cps.append(rng.choice(idx))
                elif k == 3:
                    cps.append(rng.randrange(0x80, 0xD800))
                elif k == 4:
                    cps.append(rng.randrange(0xE000, 0x110000))
                else:
                    cps.append(rng.choice((0xFFFD, 0x20AC, 0x1F4A9, 0xA0, 0x7F, 0xFEFF)))
            cases.append((name, cps))
    lines = []
    plan = []
    for name, cps in cases:
        text = "".join(chr(c) for c in cps).encode("utf-8")
        cuts = sorted(rng.randrange(len(text) + 1) for _ in range(rng.choice((0, 1, 3)))) if text else []
        chunks = []
        prev = 0
        for c in cuts:
            chunks.append(text[prev:c])
            prev = c
        chunks.append(text[prev:])
        for mode in "HR":
            lines.append("NE 0 %s %s" % (label_hex(name.lower()), mode))
            for j, ch in enumerate(chunks):
                lines.append("E 0 %d %s" % (1 if j == len(chunks) - 1 else 0, hx(ch)))
            plan.append((name, cps, mode, len(chunks)))
    out = ours.run(lines)
    run.requests += len(lines)
    k = 0
    total = 0
    for name, cps, mode, nchunks in plan:
        k += 1
        got = b""
        for _ in range(nchunks):
            t = out[k].split()
            k += 1
            got += R.unhex(t[1]) if t[0] == "OUT" else b"<%s>" % t[1].encode()
        want = bytearray()
        for cp in cps:
            b = std.encode_cp(name, cp)
            if b is not None:
                want.append(b)
            else:
                want += (("&#%d;" % cp) if mode == "H" else "?").encode()
        if got != bytes(want):
            run.defects.append("encode-modes: %s %s %s -> %r, the standard says %r" % (name, "html" if mode == "H" else "replacement", cps[:8], got[:60], bytes(want)[:60]))
        total += 1
    run.note("encode-modes: %d random texts in %d single-byte encodings, in HTML and replacement modes, cut anywhere, against the index" % (total, len(names)))


def population_sniff(run, std, ours):
    """The standard's "decode" (byte order mark sniffing) against Python's codecs: strip a mark, decode the rest in the encoding the mark chose."""
    rng = random.Random(run.args.seed + 4)
    fallbacks = [("utf-8", "utf-8"), ("utf-16le", "utf-16-le"), ("utf-16be", "utf-16-be"), ("iso-8859-2", "iso8859_2"), ("koi8-r", "koi8_r")]
    marks = [b"", b"\xef\xbb\xbf", b"\xfe\xff", b"\xff\xfe", b"\xef\xbb", b"\xef", b"\xfe", b"\xff", b"\xef\xbb\xbe", b"\xff\xff", b"\xfe\xfe"]
    cases = []
    for _ in range(4000):
        label, pycodec = rng.choice(fallbacks)
        mark = rng.choice(marks)
        body = random_bytes(rng, "utf-8" if rng.randrange(2) else rng.choice(("utf-16le", "utf-16be")), ())
        data = mark + body
        # What the standard says: a mark chooses the encoding and is dropped; anything else is decoded as the fallback.
        if data.startswith(b"\xef\xbb\xbf"):
            enc, rest = "utf-8", data[3:]
        elif data.startswith(b"\xfe\xff"):
            enc, rest = "utf-16-be", data[2:]
        elif data.startswith(b"\xff\xfe"):
            enc, rest = "utf-16-le", data[2:]
        else:
            enc, rest = pycodec, data
        cases.append((label, data, enc, rest))
    py = R.ask_python(["PD %s replace %s" % (enc, hx(rest)) for _, _, enc, rest in cases])
    lines = []
    chunk_plan = []
    for label, data, _, _ in cases:
        chunks = cuts_of(rng, data)
        chunk_plan.append(len(chunks))
        lines.append("ND 0 %s s" % hx(label.encode()))
        for j, c in enumerate(chunks):
            lines.append("D 0 %d %s" % (1 if j == len(chunks) - 1 else 0, hx(c)))
    out = ours.run(lines)
    run.requests += len(lines) + len(cases)
    k = 0
    for (label, data, enc, rest), p, n in zip(cases, py, chunk_plan):
        k += 1
        got = b""
        for _ in range(n):
            t = out[k].split()
            k += 1
            got += R.unhex(t[1]) if t[0] == "OUT" else b"<%s>" % t[1].encode()
        want = R.unhex(p.split()[1]) if p.startswith("OUT") else None
        if got != want:
            run.defects.append("sniff: fallback %s, %s: this library %r, Python %r" % (label, hx(data), got[:50], (want or b"")[:50]))
    # Fatal mode after a mark: the offset of an error counts the mark. (Python has no such offset, so this is the library's sniffed decode against its own decode of what follows
    # the mark, with the mark's length added: it is the accounting that is checked, not the decoding.)
    labels_of = {"utf-8": "utf-8", "utf-16-le": "utf-16le", "utf-16-be": "utf-16be", "iso8859_2": "iso-8859-2", "koi8_r": "koi8-r"}
    lines = []
    for label, data, enc, rest in cases:
        skip = len(data) - len(rest)
        lines.append("ND 0 %s sf" % hx(label.encode()))
        lines.append("D 0 1 %s" % hx(data))
        lines.append("ND 1 %s f" % hx(labels_of[enc].encode()))
        lines.append("D 1 1 %s" % hx(rest))
    out = ours.run(lines)
    run.requests += len(lines)
    for k, (label, data, enc, rest) in enumerate(cases):
        skip = len(data) - len(rest)
        a, b = out[4 * k + 1].split(), out[4 * k + 3].split()
        if a[0] != b[0] or (a[0] == "ERR" and (a[1] != b[1] or int(a[2]) != skip + int(b[2]))):
            run.defects.append("sniff offsets: fallback %s, %s: sniffed fatal decode said %s, the rest alone %s (+%d)" % (label, hx(data), a[:3], b[:3], skip))
    run.note("sniff: %d inputs, a byte order mark (whole, cut, or nearly one) in front of random bytes, five fallback encodings, cut at random places, against Python's codecs" % len(cases))


def population_utf8_encode(run, std, ours):
    """This library's UTF-8 encoder (replacement mode) against Node's decode then TextEncoder, for random byte strings."""
    rng = random.Random(run.args.seed + 1)
    count = max(1000, run.args.count // 4)
    datas = [random_bytes(rng, "utf-8", ()) for _ in range(count)]
    node = R.ask_node(["E " + hx(d) for d in datas])
    mine = ours.run(["NE 0 %s R" % hx(b"utf-8")] + ["E 0 1 %s" % hx(d) for d in datas])[1:]
    run.requests += 2 * count
    for d, a, b in zip(datas, node, mine):
        r = parse_ref(a)
        t = b.split()
        o = ("OUT", R.unhex(t[1])) if t[0] == "OUT" else ("ERR", t[1])
        if o != r:
            run.diverge("node", "utf8-encode", "utf-8", "%s: Node %r, this library %r" % (hx(d), r, o), False)
    run.note("utf8-encode: %d random byte strings, the library's encoder against Node's decode then TextEncoder" % count)


def population_buffer(run, std, ours, rng_seed):
    """The one-shot buffer form is the streaming form fed once."""
    rng = random.Random(rng_seed)
    names = [n.lower() for n in std.single_byte if n != "ISO-8859-16"] + ["utf-8", "utf-16le", "utf-16be", "x-user-defined", "replacement"]
    datas = [(rng.choice(names), random_bytes(rng, "utf-8", ()), rng.choice(("-", "f", "s", "r", "sr", "fs"))) for _ in range(3000)]
    a = ours.run(["BUF %s %s %s" % (hx(n.encode()), f, hx(d)) for n, d, f in datas])
    lines = []
    for n, d, f in datas:
        lines.append("ND 0 %s %s" % (hx(n.encode()), f))
        lines.append("D 0 1 %s" % hx(d))
    s = ours.run(lines)
    run.requests += len(datas) * 3
    for i, ((n, d, f), x) in enumerate(zip(datas, a)):
        y = s[2 * i + 1]
        # An error from the buffer form has no output (a failure leaves no buffer); the stream's has the prefix, which is
        # not compared here.
        if x.split()[:3 if x.startswith("ERR") else 2] != y.split()[:3 if y.startswith("ERR") else 2]:
            run.defects.append("buffer: %s %s %s: the buffer form said %r, the stream %r" % (n, f, hx(d), x, y))
    run.note("buffer: %d decodes through the buffer form and the stream agree" % len(datas))


# ---- the multi-byte encodings ------------------------------------------------------------------------------------------------------------

BOUNDARY_BYTES = "00,0A,0E,0F,1B,20,21,24,28,2F,30,39,3A,40,41,42,49,4A,5C,5F,60,7E,7F,80,81,8E,8F,9F,A0,A1,DF,E0,FC,FD,FE,FF"
# ISO-2022-JP's escape sequences, its three kinds of byte, a control and a high byte.
ISO_ESCAPE_BYTES = "1B,24,28,40,42,49,4A,21,5C,0E,80"


def mb_populations(run):
    """[(population name, encodings, source, shapes)]: every sequence of n bytes, then the stratified products (see encoding_mb.decode_job)."""
    encs = [e for e in MB.MB_ENCODINGS if not run.args.mb_only or e in run.args.mb_only]
    pops = []
    for n in range(1, run.args.mb_bytes + 1):
        pops.append(("mb-decode-%d" % n, encs, n, True))
    if run.args.mb_bytes >= 3 and not run.args.no_strata:
        # Four bytes: the valid form of gb18030's long characters (every one), the bytes that matter to some decoder (every sequence), and random sets.
        pops.append(("mb-decode-4-valid-form", [e for e in encs if e in ("gbk", "gb18030")], "81-FE:30-39:81-FE:30-39", True))
        pops.append(("mb-decode-4-boundary", encs, ":".join([BOUNDARY_BYTES] * 4), False))
        for seed in range(1, run.args.mb_seeds + 1):
            rng = random.Random(1000 + seed)
            sets = []
            for _ in range(4):
                values = set(rng.sample(range(0x80, 0x100), 12)) | set(rng.sample(range(0x100), 12))
                sets.append(sorted(values))
            pops.append(("mb-decode-4-random-%d" % seed, encs, MB.spec_of(sets), False))
        pops.append(("mb-decode-iso-2022-jp-escapes", [e for e in encs if e == "iso-2022-jp"], ":".join([ISO_ESCAPE_BYTES] * 6), False))
    return pops


def population_mb_decode(run, std, ours):
    """Every sequence of one to three bytes of each multi-byte decoder, in both modes, against Node and each Python codec (see encoding_mb.py)."""
    import concurrent.futures
    jobs = []
    names = []
    for name, encs, source, shapes in mb_populations(run):
        for enc in encs:
            jobs.append((ours.binary, enc, "node", None, source, True, run.args.plant, shapes))
            names.append(name)
            for codec in MB.CODECS[enc]:
                jobs.append((ours.binary, enc, "python", codec, source, True, run.args.plant, shapes))
                names.append(name)
    order = sorted(range(len(jobs)), key=lambda i: -(jobs[i][4] if isinstance(jobs[i][4], int) else 2))
    summary = defaultdict(lambda: [0, 0, 0, 0, 0, 0])
    witnessed = 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=run.args.jobs) as pool:
        results = list(pool.map(MB.decode_job, [jobs[i] for i in order]))
    for i, result in zip(order, results):
        job, population = jobs[i], names[i]
        _, enc, ref, codec, _, _, _, _ = job
        who = codec if ref == "python" else enc
        run.executed.add((population, who))
        run.executed.add((population + "-fatal", who))
        for d in result["defects"]:
            run.defects.append("%s: %s" % (population, d))
        for name, (count, examples) in result["classes"].items():
            run.add(ref, population, "%s %s" % (who, name), count, examples[0])
        if result["composed"]:
            run.add(ref, population, "%s composed %s" % (who, MB.signature(result["composed_sig"])), result["composed"], "differences made of differences of shorter pieces")
        for what, (count, example) in result["fatal"].items():
            run.add(ref, population + "-fatal", "%s fatal: %s" % (who, what), count, example)
        st = result["stats"]
        key = (population, ref)
        summary[key][0] += st["compared"]
        summary[key][1] += st["diverged"]
        summary[key][2] += st["fatal_compared"]
        summary[key][3] += st["fatal_diverged"]
        witnessed += st.get("witness_planted", 0)
        summary[key][4] += st["checked_against_model"] + st["fatal_checked_against_model"]
        summary[key][5] += st["checked_against_cpp_model"] + st["fatal_checked_against_cpp_model"]
        run.requests += st["compared"] + st["fatal_compared"]
    # The witnesses of the aggregate classes (encoding_mb_witnesses.txt), for the encodings that ran.
    wit_defects, wit_checked = MB.check_witnesses(ours.binary, {e for j in jobs for e in (j[1],)}, run.args.plant)
    for d in wit_defects:
        run.defects.append("mb-decode: " + d)
    run.requests += wit_checked
    run.note("mb-decode: %d witnesses of the aggregate classes (composed, uncomposed, the three kinds of fatal difference), worked out from the standard, hold" % wit_checked
             if not wit_defects else "mb-decode: %d witnesses checked, %d do not hold" % (wit_checked, len(wit_defects)))
    if run.args.plant in ("mb-witness", "cpp-model-witness") and not witnessed:
        # A control that was not put in place cannot fail: say so, and stop (the control would otherwise read as passing nothing, or failing for another reason).
        raise Failure("the %s control was planted in no worker process of the multi-byte populations" % run.args.plant)
    for (population, ref), v in sorted(summary.items()):
        run.note("%s against %s: %d sequence comparisons in each mode, %d differ in replacement mode and %d in fatal mode%s" % (
            population, ref, v[0], v[1], v[3],
            "; the library is also equal to the Python model on %d and to the C++ model on %d sequence decodes (both modes)" % (v[4], v[5]) if ref == "node" else ""))

def population_mb_encode(run, std, ours):
    """Every code point through each multi-byte encoder, against the standard (the model), against each Python codec's encoder and, through Node's decoder, against Node."""
    import concurrent.futures
    jobs = []
    for enc in MB.MB_ENCODINGS:
        if run.args.mb_only and enc not in run.args.mb_only:
            continue
        jobs.append((ours.binary, enc, None, True, run.args.plant))
        for codec in MB.CODECS[enc]:
            jobs.append((ours.binary, enc, codec, False, run.args.plant))
    with concurrent.futures.ProcessPoolExecutor(max_workers=run.args.jobs) as pool:
        results = list(pool.map(MB.encode_job, jobs))
    totals = defaultdict(int)
    for job, result in zip(jobs, results):
        _, enc, codec, node, _ = job
        run.executed.add(("mb-encode", codec or enc))
        for d in result["defects"]:
            run.defects.append("mb-encode: " + d)
        for name, (count, examples) in result["classes"].items():
            ref = "node" if name.startswith("node-reads: ") else "python"
            run.add(ref, "mb-encode", "%s %s" % (codec or enc, name), count, examples[0])
        for k, v in result["stats"].items():
            totals[k] += v
        run.requests += result["stats"]["code_points"]
    run.note("mb-encode: %d encoders x %d code points (U+0000 to U+10FFFF but the surrogates), each checked against the standard (the model, %d checks) and %d Python "
             "encoders (%d code points differ), and what the library writes through Node's decoder (%d differ)" % (
                 len({j[1] for j in jobs}), jobs and results[0]["stats"]["code_points"], totals["model_checked"], sum(1 for j in jobs if j[2]),
                 totals["python_differs"], totals["node_differs"]))


def mb_piece_catalogue(std, enc, node_agrees):
    """The complete pieces a stream of `enc` is made of when it is to be decoded alike by the standard and by Node: [(kind, bytes)].

    A piece is complete when the standard decodes it, and decodes it followed by an ASCII byte, as itself and the byte (it leaves no lead byte waiting); and when
    Node decodes it as the standard does. ISO-2022-JP's pieces are whole runs between escape sequences."""
    pieces = []
    if enc == "iso-2022-jp":
        rows = std.indexes["jis0208"]
        two = [bytes([p // 94 + 0x21, p % 94 + 0x21]) for p, cp in enumerate(rows[:8836]) if cp is not None]
        pieces.append(("ascii", b"abc 123 .,;"))
        pieces.append(("ascii", b"The quick brown fox"))
        for k in (1, 2, 5):
            for _ in range(8):
                pieces.append(("jis", None, k))
        pieces.append(("roman", None, 3))
        pieces.append(("katakana", None, 3))
        return pieces, two
    # A piece is complete when whatever follows it is decoded as if it stood alone: tried with every byte for a single byte, and with the bytes that matter for a
    # pair (a piece that leaves a lead byte waiting, or that an error consumes a following byte of, is not one).
    followers_all = list(range(256))
    followers_few = [0x00, 0x20, 0x41, 0x7F, 0x80, 0x81, 0x8E, 0x8F, 0xA1, 0xDF, 0xFE, 0xFF, 0x30, 0x1B, 0x24, 0x28]
    alone = {y: M.decode(enc, bytes([y]))[0] for y in range(256)}

    def complete(data, followers):
        cps, _ = M.decode(enc, data)
        return all(M.decode(enc, data + bytes([y]))[0] == cps + alone[y] for y in followers)

    for b in range(256):
        data = bytes([b])
        if complete(data, followers_all):
            pieces.append(("byte", data))
    for lead in range(0x81, 0xFF):
        for trail in range(256):
            data = bytes([lead, trail])
            cps, _ = M.decode(enc, data)
            if 0xFFFD in cps:
                continue
            if complete(data, followers_few):
                pieces.append(("pair", data))
    if enc == "euc-jp":
        # JIS X 0212's three-byte form: a sample of those the standard decodes (Node is asked below, with the rest).
        rng_ = random.Random(77)
        for _ in range(300):
            data = bytes([0x8F, rng_.randrange(0xA1, 0xFF), rng_.randrange(0xA1, 0xFF)])
            cps, _ = M.decode(enc, data)
            if 0xFFFD not in cps and complete(data, followers_few):
                pieces.append(("triple", data))
    return pieces, None


def population_mb_stream(run, std, ours):
    """Random and mutated streams of each multi-byte encoding, cut at random places, in both error modes: the library, chunked, against the standard (the model) for all of
    them, and against Node's TextDecoder with {stream: true} for those made of pieces on which Node and the standard agree. And ISO-2022-JP's escape sequences, fuzzed."""
    rng = random.Random(run.args.seed + 21)
    encs = [e for e in MB.MB_ENCODINGS if not run.args.mb_only or e in run.args.mb_only]
    per = max(8, run.args.count // (len(encs) * 4))
    total = {"agree": 0, "mutated": 0, "random": 0, "escapes": 0}
    node_total = 0
    for enc in encs:
        # The pieces on which Node and the standard agree (from the one- and two-byte populations, both of which are enumerated exhaustively above).
        pieces, jis = mb_piece_catalogue(std, enc, None)
        label = hx(enc.encode())
        if enc != "iso-2022-jp":
            probe = [p[1] for p in pieces]
            node_ans = R.ask_node(["D %s i %s" % (label, hx(d)) for d in probe])
            keep = []
            for (kind, d), a in zip(pieces, node_ans):
                t = a.split()
                cps, _ = M.decode(enc, d)
                if t[0] == "OUT" and R.unhex(t[1]) == MB.u8(cps):
                    keep.append((kind, d))
            pieces = keep
            four = []
            if enc == "gb18030":
                for _ in range(400):
                    d = bytes([rng.randrange(0x81, 0xFF), rng.randrange(0x30, 0x3A), rng.randrange(0x81, 0xFF), rng.randrange(0x30, 0x3A)])
                    cps, _ = M.decode(enc, d)
                    if 0xFFFD not in cps:
                        four.append(d)
                if four:
                    ans = R.ask_node(["D %s i %s" % (label, hx(d)) for d in four])
                    four = [d for d, a in zip(four, ans) if a.split()[0] == "OUT" and R.unhex(a.split()[1]) == MB.u8(M.decode(enc, d)[0])]
                pieces += [("four", d) for d in four]
        cases = []  # (kind, bytes, compare with Node)
        for kind in ("agree", "mutated", "random", "escapes"):
            if kind == "escapes" and enc != "iso-2022-jp":
                continue
            for _ in range(per):
                if kind in ("agree", "mutated"):
                    data = bytearray()
                    for _ in range(rng.choice((0, 1, 2, 3, 5, 8, 20, 60))):
                        pc = rng.choice(pieces)
                        if enc == "iso-2022-jp":
                            if pc[0] == "ascii":
                                data += pc[1]
                            elif pc[0] == "jis":
                                data += b"\x1b$B" + b"".join(rng.choice(jis) for _ in range(pc[2])) + b"\x1b(B"
                            elif pc[0] == "roman":
                                data += b"\x1b(J" + bytes(rng.choice(b"abcxyz 019\\~") for _ in range(pc[2])) + b"\x1b(B"
                            else:
                                data += b"\x1b(I" + bytes(rng.randrange(0x21, 0x60) for _ in range(pc[2])) + b"\x1b(B"
                        else:
                            data += pc[1]
                    if kind == "mutated" and data:
                        for _ in range(rng.choice((1, 1, 2, 3))):
                            op = rng.randrange(3)
                            at = rng.randrange(len(data) + 1)
                            if op == 0 and at < len(data):
                                data[at] = rng.randrange(256)
                            elif op == 1:
                                data.insert(at, rng.choice((0x1B, 0x24, 0x28, 0x42, 0x8E, 0x8F, 0x80, 0xFF, 0x30, 0x81, rng.randrange(256))))
                            elif at < len(data):
                                del data[at]
                    cases.append((kind, bytes(data), kind == "agree" and enc != "iso-2022-jp"))
                elif kind == "random":
                    cases.append((kind, bytes(rng.choice((rng.randrange(256), 0x1B, 0x24, 0x28, 0x81, 0xA1, 0x8E, 0x8F, 0x30, 0x40, 0xFF)) for _ in range(rng.choice((1, 2, 3, 4, 6, 12, 40)))), False))
                else:
                    alphabet = bytes.fromhex(ISO_ESCAPE_BYTES.replace(",", ""))
                    cases.append((kind, bytes(rng.choice(alphabet) for _ in range(rng.randrange(1, 40))), False))
        for i in range(0, len(cases), 4000):
            batch = cases[i:i + 4000]
            lines = []
            plan = []
            for kind, data, with_node in batch:
                for fatal in (False, True):
                    cuts = sorted(rng.randrange(len(data) + 1) for _ in range(rng.choice((0, 1, 2, 4)))) if data else []
                    chunks = []
                    prev = 0
                    for c in cuts:
                        chunks.append(data[prev:c])
                        prev = c
                    chunks.append(data[prev:])
                    lines.append("ND 0 %s %s" % (label, "f" if fatal else "-"))
                    for j, ch in enumerate(chunks):
                        lines.append("D 0 %d %s" % (1 if j == len(chunks) - 1 else 0, hx(ch)))
                    plan.append((kind, data, fatal, chunks, with_node))
            out = ours.run(lines)
            run.requests += len(lines)
            k = 0
            node_lines = []
            node_plan = []
            for kind, data, fatal, chunks, with_node in plan:
                k += 1
                got = bytearray()
                failed_at = None
                for ch in chunks:
                    t = out[k].split()
                    k += 1
                    if failed_at is not None:
                        continue
                    if t[0] == "OUT":
                        got += R.unhex(t[1])
                    else:
                        failed_at = int(t[2])
                        got += R.unhex(t[3]) if len(t) > 3 else b""
                cps, offset = M.decode(enc, data, fatal=fatal)
                want = MB.u8(cps)
                if offset is None:
                    ok = failed_at is None and bytes(got) == want
                else:
                    ok = failed_at == offset and bytes(got) == want
                if not ok:
                    run.defects.append("mb-stream: %s %s %s cut %s: the library %s [%s] at %s, the standard (the model) %s [%s]" % (
                        enc, kind, "fatal" if fatal else "replacement", "|".join(c.hex() for c in chunks), "failed" if failed_at is not None else "decoded",
                        bytes(got).decode("utf-8", "replace"), failed_at, "fails" if offset is not None else "decodes", want.decode("utf-8", "replace")))
                total[kind] += 1
                if with_node:
                    node_lines.append("D %s %s %s" % (label, "fi" if fatal else "i", ",".join(hx(c) for c in chunks)))
                    node_plan.append((data, fatal, offset, want))
            if node_lines:
                answers = R.ask_node(node_lines)
                for (data, fatal, offset, want), a in zip(node_plan, answers):
                    t = a.split()
                    if offset is not None:
                        ok = t[0] == "ERR" and t[1] == "TypeError"
                    else:
                        ok = t[0] == "OUT" and R.unhex(t[1]) == want
                    if not ok:
                        run.diverge("node", "mb-stream", "%s agree-stream" % enc, "%s %s: Node %s, the standard %s" % (
                            data.hex(), "fatal" if fatal else "replacement", a[:80], "error" if offset is not None else want.decode("utf-8", "replace")), False)
                    node_total += 1
                run.requests += len(node_lines)
    run.note("mb-stream: %d streams in %d encodings (%d of pieces on which Node agrees, %d mutated, %d random, %d of ISO-2022-JP escape sequences), cut into 1 to 5 pieces, in "
             "both modes, the library against the standard (the model) in all, and against Node (%d comparisons) for the first kind" % (
                 sum(total.values()) // 2, len(encs), total["agree"] // 2, total["mutated"] // 2, total["random"] // 2, total["escapes"] // 2, node_total))


def population_mb_encode_text(run, std, ours):
    """Random text through each multi-byte encoder in HTML mode, cut anywhere inside its UTF-8: the library against the standard (the model, byte for byte), and against each
    Python codec's encoder (`xmlcharrefreplace`) for the text made of characters on which the two agree one by one - what is left to differ is the state of ISO-2022-JP:
    the Roman state, which the standard leaves only when a character needs another and Python's codec leaves after each character."""
    rng = random.Random(run.args.seed + 31)
    count = max(200, run.args.count // 60)
    total = 0
    roman_differences = defaultdict(int)
    roman_example = {}
    for enc in MB.MB_ENCODINGS:
        if run.args.mb_only and enc not in run.args.mb_only:
            continue
        label = hx(enc.encode())
        index_chars = []
        for _ in range(400):
            while True:
                lead = rng.randrange(0x81, 0xFF)
                data = bytes([lead, rng.randrange(0x40, 0xFF)]) if enc != "iso-2022-jp" else bytes([0x1B, 0x24, 0x42, rng.randrange(0x21, 0x7F), rng.randrange(0x21, 0x7F)])
                cps, _ = M.decode(enc, data)
                if cps and 0xFFFD not in cps and cps[0] > 0x7F:
                    index_chars.append(cps[0])
                    break
        special = [0xA5, 0x203E, 0x2212, 0xFF0D, 0xFF71, 0xFF9F, 0x80, 0x20AC, 0xE9, 0x1F4A9, 0xFFFD, 0xE5E5, 0x2550, 0x5341, 0x3042, 0x10000, 0x10FFFF]
        if enc == "iso-2022-jp":
            special += [0x0E, 0x0F, 0x1B]
        pool = index_chars + special * 6 + list(range(0x20, 0x7F)) * 2
        texts = [[rng.choice(pool) for _ in range(rng.choice((0, 1, 2, 3, 6, 12, 30, 80)))] for _ in range(count)]
        lines = ["NE 0 %s H" % label]
        plan = []
        for cps in texts:
            data = "".join(chr(c) for c in cps).encode("utf-8")
            cuts = sorted(rng.randrange(len(data) + 1) for _ in range(rng.choice((0, 1, 3)))) if data else []
            chunks = []
            prev = 0
            for c in cuts:
                chunks.append(data[prev:c])
                prev = c
            chunks.append(data[prev:])
            for j, ch in enumerate(chunks):
                lines.append("E 0 %d %s" % (1 if j == len(chunks) - 1 else 0, hx(ch)))
            plan.append((cps, len(chunks)))
        out = ours.run(lines)
        run.requests += len(lines)
        k = 1
        mine = []
        for cps, nchunks in plan:
            got = bytearray()
            for _ in range(nchunks):
                t = out[k].split()
                k += 1
                got += R.unhex(t[1]) if t[0] == "OUT" else b"<" + t[1].encode() + b">"
            mine.append(bytes(got))
        # the standard
        for cps, got in zip(texts, mine):
            want, _ = M.encode(enc, cps, "html")
            if got != want:
                run.defects.append("mb-encode-text: %s %s -> %r, the standard (the model) says %r" % (enc, ["U+%04X" % c for c in cps][:12], got, want))
            total += 1
        # Python: the characters on which it agrees with the library, one by one
        for codec in MB.CODECS[enc]:
            chars = sorted(set(pool))
            single = ours.run(["NE 0 %s H" % label] + ["E 0 1 %s" % hx(chr(c).encode("utf-8")) for c in chars])[1:]
            py = R.ask_python(["PE %s xmlcharrefreplace %s" % (codec, hx(chr(c).encode("utf-8"))) for c in chars])
            run.requests += 2 * len(chars)
            agree = set()
            for c, o, p_ in zip(chars, single, py):
                if o.split()[0] == "OUT" and p_.split()[0] == "OUT" and R.unhex(o.split()[1]) == R.unhex(p_.split()[1]):
                    agree.add(c)
            roman = {0xA5, 0x203E, 0x5C, 0x7E}
            tests = [cps for cps in texts if cps and all(c in agree for c in cps)]
            if not tests:
                continue
            py_out = R.ask_python(["PE %s xmlcharrefreplace %s" % (codec, hx("".join(chr(c) for c in cps).encode("utf-8"))) for cps in tests])
            run.requests += len(tests)
            for cps, p_ in zip(tests, py_out):
                want, _ = M.encode(enc, cps, "html")
                if p_.split()[0] == "OUT" and R.unhex(p_.split()[1]) == want:
                    continue
                key = (codec, "Roman" if (enc == "iso-2022-jp" and roman & set(cps)) else "other")
                example = "%s: standard %r, %s %r" % ("".join(chr(c) for c in cps)[:20].encode("unicode_escape").decode(), want, codec, R.unhex(p_.split()[1]) if p_.split()[0] == "OUT" else p_)
                if key[1] == "Roman":
                    roman_differences[key] += 1
                    roman_example.setdefault(key, example)
                else:
                    run.diverge("python", "mb-encode-text", "%s other" % codec, example, False)
    for (codec, kind), n in roman_differences.items():
        run.add("python", "mb-encode-text", "%s text with U+00A5 or U+203E (the Roman state)" % codec, n, roman_example[(codec, kind)])
    run.note("mb-encode-text: %d random texts (cut inside their UTF-8) through the encoders in HTML mode, each equal to the standard's (the model); against the Python encoders where the "
             "characters agree one by one" % total)


# ---- the list of divergences ---------------------------------------------------------------------------------------------------

def read_list(path):
    """{(reference, population, class): (count, reason)} from encoding_divergences.txt."""
    entries = {}
    with open(path, encoding="utf-8") as fh:
        for number, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 5 or not parts[3].isdigit():
                raise SystemExit("%s:%d: not five tab-separated fields (reference, population, class, count, reason): %r" % (path, number, line[:80]))
            key = (parts[0], parts[1], parts[2])
            if key in entries:
                raise SystemExit("%s:%d: %s listed twice" % (path, number, "/".join(key)))
            entries[key] = (int(parts[3]), parts[4])
    return entries


def heading(text):
    sys.stderr.write("\033[0;31m\n### %s ###\033[0m\n" % text)


def judge(run, listed):
    """Print what was found and return the exit status."""
    failures = 0
    if run.defects:
        heading("The library disagrees with the standard's data")
        for d in run.defects[:40]:
            sys.stderr.write("  %s\n" % d)
        if len(run.defects) > 40:
            sys.stderr.write("  ... and %d more\n" % (len(run.defects) - 40))
        failures += 1
    found = run.divergences
    missing = sorted(k for k in found if k not in listed)
    if missing:
        heading("Divergences with no entry in tools/oracle/encoding_divergences.txt")
        for k in missing:
            sys.stderr.write("  %s: %d, e.g. %s\n" % ("/".join(k), found[k]["count"], found[k]["examples"][0]))
        failures += 1
    def in_scope(key):
        """Is a line of the list checked against this run? The multi-byte lines are, when the population ran for the encoding or codec the class names; the others
        when their population ran (not left out by --only or --except) at the default size: a hunt of other sizes checks that every divergence has a line, and not the counts."""
        _, population, cls = key
        if population.startswith("mb-"):
            who = cls.split(" ", 1)[0]
            return (population, who) in run.executed or (population == "mb-encode-text" and "mb-encode-text" in run.ran and not run.args.sized and not run.args.mb_only)
        return population in run.ran and not run.args.sized

    gone = sorted(k for k in listed if k not in found and in_scope(k))
    if gone:
        heading("Listed divergences that are gone")
        for k in gone:
            sys.stderr.write("  %s: listed with %d\n" % ("/".join(k), listed[k][0]))
        failures += 1
    changed = sorted(k for k in listed if k in found and in_scope(k) and found[k]["count"] != listed[k][0])
    if changed:
        heading("Listed divergences whose count has changed")
        for k in changed:
            sys.stderr.write("  %s: listed %d, found %d, e.g. %s\n" % ("/".join(k), listed[k][0], found[k]["count"], found[k]["examples"][0]))
        failures += 1
    return 3 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary", help="encoding-ours, built by run-encoding-oracle.sh")
    ap.add_argument("--plant", choices=("ours-extra-char", "ours-lost-char", "stale-entry", "change-count", "no-list", "witness", "stale-mb-entry", "change-mb-count", "mb-witness", "cpp-model-witness", "stale-witness", "change-signature"))
    ap.add_argument("--seed", type=int, default=int(os.environ.get("ENCODING_ORACLE_SEED", "1")))
    ap.add_argument("--count", type=int, default=int(os.environ.get("ENCODING_ORACLE_COUNT", "60000")),
                    help="random byte strings for the streaming comparison (the list describes the default)")
    ap.add_argument("--fatal-stride", type=int, default=int(os.environ.get("ENCODING_ORACLE_FATAL_STRIDE", "13")),
                    help="one in this many three-byte sequences is compared in fatal mode")
    ap.add_argument("--utf-bytes", type=int, default=3, help="longest sequence of the utf population (3 is every one of one to three bytes)")
    ap.add_argument("--mb-bytes", type=int, default=int(os.environ.get("ENCODING_ORACLE_MB_BYTES", "3")),
                    help="longest sequence of the mb-decode population (3 is every one of one to three bytes)")
    ap.add_argument("--propose", metavar="FILE", help="write the lines of the list for what this run found (with the reasons of encoding_mb.reason) and exit 0, instead of judging")
    ap.add_argument("--mb-seeds", type=int, default=int(os.environ.get("ENCODING_ORACLE_MB_SEEDS", "3")),
                    help="how many random strata of four bytes the multi-byte decoders are compared on")
    ap.add_argument("--no-strata", action="store_true", help="leave the stratified four-byte and escape-sequence populations out of mb-decode")
    ap.add_argument("--mb-only", action="append", default=[], help="run the multi-byte populations for this encoding only (a name of MB_ENCODINGS)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("ENCODING_ORACLE_JOBS", "6")), help="worker processes of the multi-byte populations")
    ap.add_argument("--only", action="append", default=[], help="run only this population")
    ap.add_argument("--except", dest="skip", action="append", default=[], help="leave this population out: the lines of the list for a population that did not run are not checked (there is nothing to count), and the run is a hunt")
    args = ap.parse_args(argv)
    default_size = (args.count == 60000 and args.seed == 1 and args.fatal_stride == 13)
    # A hunt is any run that is not the default: of another size or seed, of fewer populations or encodings, of other strata. `sized` is the part of it that changes the
    # counts of the populations that did run (the generated ones and the utf and multi-byte sizes).
    args.sized = not default_size or args.utf_bytes != 3
    args.hunt = args.sized or bool(args.only) or bool(args.skip) or args.mb_bytes != 3 or bool(args.mb_only) or args.mb_seeds != 3 or args.no_strata

    run = Run(args)
    std = Standard(corrupt_witness=args.plant == "witness")
    # (the "mb-witness" control is applied in the worker processes of the multi-byte populations, which is where the model is read: see encoding_mb.py)
    ours = Ours(args.binary, args.plant)
    try:
        R.check_image()
        def wanted(name):
            ok = (not args.only or name in args.only) and name not in args.skip
            if ok:
                run.ran.add(name)
            return ok
        exclusions = {}
        if wanted("labels"):
            population_labels(run, std, ours)
        if wanted("single-byte"):
            exclusions = population_single_byte_decode(run, std, ours)
        if wanted("sb-encode"):
            population_single_byte_encode(run, std, ours)
        if wanted("utf"):
            population_utf(run, std, ours)
        if (wanted("streaming") or wanted("ignore-bom")) and not exclusions:
            exclusions = population_single_byte_decode(run, std, ours)
        if wanted("streaming"):
            population_streaming(run, std, ours, exclusions)
        if wanted("ignore-bom"):
            population_ignore_bom(run, std, ours, exclusions)
        if wanted("encode-modes"):
            population_encode_modes(run, std, ours)
        if wanted("sniff"):
            population_sniff(run, std, ours)
        if wanted("utf8-encode"):
            population_utf8_encode(run, std, ours)
        if wanted("buffer"):
            population_buffer(run, std, ours, args.seed + 2)
        if wanted("mb-decode"):
            population_mb_decode(run, std, ours)
        if wanted("mb-encode"):
            population_mb_encode(run, std, ours)
        if wanted("mb-stream"):
            population_mb_stream(run, std, ours)
        if wanted("mb-encode-text"):
            population_mb_encode_text(run, std, ours)
    except Failure as why:
        sys.stderr.write("%s\n" % why)
        return 2

    if args.propose:
        with open(args.propose, "w", encoding="utf-8") as fh:
            for (reference, population, cls), d in sorted(run.divergences.items()):
                why = MB.reason(reference, population, cls, d["examples"][0]) if population.startswith("mb-") else None
                if why:
                    fh.write("%s\t%s\t%s\t%d\t%s\n" % (reference, population, cls, d["count"], why))
                    if os.environ.get("ENCODING_ORACLE_PROPOSE_EXAMPLES"):
                        sys.stderr.write("example\t%s\t%s\t%s\t%s\n" % (reference, population, cls, " | ".join(d["examples"])))
        if run.defects:
            heading("The library disagrees with the standard's data")
            for d in run.defects[:40]:
                sys.stderr.write("  %s\n" % d)
            return 3
        print("proposed %d classes to %s" % (len(run.divergences), args.propose))
        return 0
    listed = {} if args.plant == "no-list" else read_list(LIST)
    if args.plant == "stale-entry":
        listed[("node", "labels", "no-such-encoding")] = (3, "planted")
    if args.plant == "change-count" and listed:
        key = sorted(k for k in listed if not k[1].startswith("mb-"))[0]
        listed[key] = (listed[key][0] + 1, listed[key][1])
    if args.plant == "stale-mb-entry":
        listed[("node", "mb-decode-1", "gb18030 no such class")] = (3, "planted")
    if args.plant == "change-mb-count":
        key = sorted(k for k in listed if k[1] == "mb-decode-1" and k[0] == "node" and k[2].startswith("shift_jis"))[0]
        listed[key] = (listed[key][0] + 1, listed[key][1])
    if args.plant == "change-signature":
        # A control: one aggregate class has the members it has, but not the ones it had (its signature is another).
        key = sorted(k for k in run.divergences if " composed sig=" in k[2])[0]
        run.divergences[(key[0], key[1], key[2][:-8] + "deadbeef")] = run.divergences.pop(key)
    status = judge(run, listed)
    if status == 0:
        for key in sorted(run.divergences):
            print("  divergence %-52s %6d  listed" % ("/".join(key), run.divergences[key]["count"]))
    print("encoding differential: %d requests; %d divergences in %d classes, all explained by the standard's data; %s" % (
        run.requests, sum(d["count"] for d in run.divergences.values()), len(run.divergences),
        "clean" if status == 0 else "FAILED"))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
