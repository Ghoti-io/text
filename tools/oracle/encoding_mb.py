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
"""The multi-byte part of the Encoding differential (tools/oracle/encoding_diff.py): GBK, gb18030, Big5, EUC-JP, ISO-2022-JP, Shift_JIS and EUC-KR.

Three transcriptions of the standard are held against each other here: this library (C, tables generated from indexes.json), tools/oracle/encoding_model.py
(Python, the standard's handlers written again over indexes.json itself) and tests/encoding_cjk_model.h (C++, run as tools/oracle/encoding_model_enum.cpp) - all
written from the same text by one author, so they can disagree through a slip and not through another reading. Node's ICU converters and CPython's codecs, for the
decoders, are the independent ones, and are neither. **The standard decides**: the library must agree with both models on **every** sequence of every population (else the library is wrong and the run fails), whatever a
reference says; wherever a reference and the library differ the difference is then the reference's. A divergence is counted under a class that says
what kind of difference it is, with the bytes it covers; tools/oracle/encoding_divergences.txt has a line for each class with the count and why.

**Populations** (all sequences of N bytes are the numbers 0 .. 256^N - 1, big-endian): every sequence of one, two and three bytes in replacement mode and
in fatal mode, against Node and against each Python codec of CODECS; and every code point (but the surrogates) through each encoder, against Python's
encoders, with Node's decoder checking that what the library writes reads back (Node has no legacy encoder).

**Classes.** A difference of one or two bytes (or the four bytes of gb18030's long form) is classified by its *shape*: the classes of the output the
standard gives and of the output the reference gives (A ASCII, C a C1 control, F U+FFFD, P private use, U anything else), how the standard's handler
cut the bytes into units (an error, an error that restores a byte, a character), and for two characters that differ the relation between them (the same
character after NFKC, a private use code point for a character of the index, ...), together with the lead and trail bytes the class covers. A difference
of three bytes is **explained by composition** when the bytes split into pieces of one and two such that both the standard and the reference decode the whole as the
concatenation of their decodes of the pieces (an exact equality, tried for [2,1], [1,2] and [1,1,1]): then it is nothing new, and is counted as composed
(the classes of its pieces being classes already). When no split works, the reference's behaviour depends on the context, and it is a class of its own.
In fatal mode a difference is a difference of *whether* the sequence is an error; it is counted under the replacement-mode class of the same bytes.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys
import unicodedata
import zlib
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import encoding_model as M  # noqa: E402
import encoding_ref as R  # noqa: E402

MB_ENCODINGS = ["shift_jis", "euc-jp", "iso-2022-jp", "euc-kr", "big5", "gbk", "gb18030"]

# The Python codecs that stand for each encoding. CPython has no codec that is the standard's: Shift_JIS is CP932 on the web, EUC-KR is CP949, Big5 is
# Big5-HKSCS plus the ETEN extensions, GBK is CP936. Both the strict and the extended codec of each are asked.
CODECS = {
    "shift_jis": ["shift_jis", "cp932"],
    "euc-jp": ["euc_jp"],
    "iso-2022-jp": ["iso2022_jp"],
    "euc-kr": ["euc_kr", "cp949"],
    "big5": ["big5", "big5hkscs"],
    "gbk": ["gbk"],
    "gb18030": ["gb18030"],
}


def u8(cps):
    return "".join(chr(c) for c in cps).encode("utf-8")


def hx(b):
    return b.hex() if b else "-"


def cclass(ch):
    o = ord(ch)
    if o == 0xFFFD:
        return "F"
    if o < 0x80:
        return "A"
    if o < 0xA0:
        return "C"
    if 0xE000 <= o <= 0xF8FF or o >= 0xF0000:
        return "P"
    return "U"


def sig(data):
    """The classes of the characters of a UTF-8 output."""
    return "".join(cclass(c) for c in data.decode("utf-8", "replace"))


def ranges(values):
    """'81-9F,E0-FC' for a set of byte values."""
    values = sorted(values)
    out = []
    a = b = None
    for x in values:
        if a is None:
            a = b = x
        elif x == b + 1:
            b = x
        else:
            out.append((a, b))
            a = b = x
    if a is not None:
        out.append((a, b))
    return ",".join("%02X" % a if a == b else "%02X-%02X" % (a, b) for a, b in out)


def relation(std, ref):
    """For two outputs that are each one character and differ: how the reference's character relates to the standard's."""
    s, r = std.decode("utf-8"), ref.decode("utf-8")
    if len(s) != 1 or len(r) != 1:
        return "-"
    if cclass(r) == "P" and cclass(s) != "P":
        return "ref-private-use"
    if cclass(s) == "P" and cclass(r) != "P":
        return "std-private-use"
    if cclass(s) == "C" or cclass(r) == "C":
        return "c1"
    if unicodedata.normalize("NFKC", s) == unicodedata.normalize("NFKC", r):
        return "same-after-nfkc"
    return "other-character"


# ---- shapes ---------------------------------------------------------------------------------------------------------------------

def unit_kinds(enc, data):
    """How the standard's handler cut `data`: 'ok' for a character, 'err', and 'rst' for an error that restored bytes."""
    kinds = []
    for start, end, kind, cps in M.decode_units(enc, data):
        kinds.append({"ok": "ok", "err": "err", "err-restore": "rst"}[kind])
    return ",".join(kinds)


def shape(enc, data, std, ref):
    """The class of one difference of up to four bytes: the standard's output, the reference's, the cut, the relation."""
    return "std=%s ref=%s cut=%s rel=%s" % (sig(std) or "-", sig(ref) or "-", unit_kinds(enc, data) or "-", relation(std, ref))


def fingerprint(*parts):
    """What one member of an aggregate class adds to its signature: a checksum of the bytes and of the outputs of the standard and of the reference."""
    return zlib.crc32(b"\x00".join(p if p is not None else b"\xff" for p in parts))


def signature(total):
    """The name of the signature of an aggregate class: the sum, modulo 2^32, of the fingerprints of its members (the order of the members does not matter)."""
    return "sig=%08x" % total


class Classes:
    """Counts of differing sequences by shape, with the lead and trail bytes each covers and a few examples."""

    def __init__(self):
        self.count = defaultdict(int)
        self.leads = defaultdict(set)
        self.trails = defaultdict(set)
        self.examples = defaultdict(list)
        self.sig = defaultdict(int)

    def add(self, key, data, example, fp=0):
        self.count[key] += 1
        self.sig[key] = (self.sig[key] + fp) & 0xFFFFFFFF
        self.leads[key].add(data[0])
        if len(data) > 1:
            self.trails[key].add(data[-1])
        if len(self.examples[key]) < 3:
            self.examples[key].append(example)

    def named(self):
        """{class name: (count, examples)}: the shape with the lead and trail bytes."""
        out = {}
        for key, n in self.count.items():
            name = "%s leads=%s trails=%s" % (key, ranges(self.leads[key]), ranges(self.trails[key]) or "-")
            if key.startswith("uncomposed"):
                name += " " + signature(self.sig[key])  # an aggregate by construction: the exact set of its members is part of its name
            out[name] = (n, self.examples[key])
        return out


# ---- the references' records ------------------------------------------------------------------------------------------------------

def records_from(stream):
    """List of bytes or None (a failure) from an enum stream (see encoding_ours.c)."""
    out = []
    i = 0
    n = len(stream)
    while i < n:
        length = stream[i]
        i += 1
        if length == 255:
            out.append(None)
        else:
            out.append(stream[i:i + length])
            i += length
    return out


def ours_records(ours, enc, n, fatal, start, count):
    return records_from(ours.enum(hx(enc.encode()), n, "f" if fatal else "-", start, count))


def node_records(enc, n, fatal, start, count):
    return records_from(R.enum_stream(R.node_command("enum", hx(enc.encode()), n, "fi" if fatal else "i", start, count)))


def python_records(codec, n, fatal, start, count):
    return records_from(R.enum_stream(R.python_command("enum", codec, n, "strict" if fatal else "replace", start, count)))


ROOT = os.path.dirname(os.path.dirname(HERE))


def cpp_model_records(binary, enc, source, n, fatal, start, count, plant=None):
    """The records of the C++ model (tools/oracle/encoding_model_enum.cpp, tests/encoding_cjk_model.h) for the same population: the second reading of the
    standard, beside this library's own and encoding_model.py's. The program is built next to the library's runner by run-encoding-oracle.sh."""
    import subprocess
    exe = os.path.join(os.path.dirname(binary), "encoding-model")
    if isinstance(source, str):
        args = [exe, "enumx", hx(enc.encode()), "f" if fatal else "-", source]
    else:
        args = [exe, "enum", hx(enc.encode()), str(n), "f" if fatal else "-", str(start), str(count)]
    done = subprocess.run(args, capture_output=True, cwd=ROOT)
    if done.returncode != 0:
        raise RuntimeError("the C++ model exited %d: %s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-500:]))
    out = records_from(done.stdout)
    if plant == "cpp-model-witness" and out and start == 0:
        out[0] = b"planted"  # a control: the second model no longer agrees with the library about the first sequence of the population
    return out


def model_output(enc, data):
    """The standard's replacement-mode decode of `data` as UTF-8, and whether it is an error in fatal mode."""
    cps, _ = M.decode(enc, data)
    _, failed = M.decode(enc, data, fatal=True)
    return u8(cps), failed is not None


# ---- one job: an encoding, a reference, a population ------------------------------------------------------------------------------

import itertools


def parse_spec(spec):
    """[[bytes of position 1], ...] from a spec like '81-FE:30-39:81-FE:30-39' (see encoding_ours.c)."""
    sets = []
    for item_list in spec.split(":"):
        values = set()
        for item in item_list.split(","):
            lo, hi = (int(x, 16) for x in item.split("-")) if "-" in item else (int(item, 16),) * 2
            values.update(range(lo, hi + 1))
        sets.append(sorted(values))
    return sets


def spec_of(sets):
    """The spec of a list of byte sets (the inverse of parse_spec)."""
    return ":".join(ranges(values).replace(" ", "") for values in sets)


def cuts_of_positions(n, longest):
    """Every way to cut positions 0 .. n-1 into blocks of at most `longest` positions and fewer than n: [(0, 1), (2, 2)] is the block of the first two positions and the third."""
    out = []

    def go(at, blocks):
        if at == n:
            out.append(blocks)
            return
        for end in range(min(n - 1, at + longest - 1), at - 1, -1):
            go(end + 1, blocks + [(at, end)])

    go(0, [])
    return out


class Pieces:
    """The decodes of the shorter pieces of a population of sequences, by the library and by the reference: for every run of consecutive positions (up to
    `longest`) the records of the product of the byte sets of those positions. They are what composition is made of."""

    def __init__(self, ours, enc, ref, codec, sets, longest):
        self.sets = sets
        self.n = len(sets)
        self.rank = [{b: i for i, b in enumerate(values)} for values in sets]
        self.longest = min(longest, self.n - 1)
        self.table = {}
        for i in range(self.n):
            for j in range(i, min(self.n - 1, i + self.longest - 1) + 1):
                if (j - i + 1) > self.longest:
                    continue
                sub = spec_of(sets[i:j + 1])
                o = records_from(ours.enumx(hx(enc.encode()), "-", sub))
                if ref == "node":
                    r = records_from(R.enum_stream(R.node_command("enumx", hx(enc.encode()), "i", sub)))
                else:
                    r = records_from(R.enum_stream(R.python_command("enumx", codec, "replace", sub)))
                self.table[(i, j)] = (o, r)

    def index(self, data, i, j):
        k = 0
        for p in range(i, j + 1):
            k = k * len(self.sets[p]) + self.rank[p][data[p]]
        return k

    def cuts(self):
        return [c for c in cuts_of_positions(self.n, self.longest) if all(b in self.table for b in c)]

    def made_of(self, data, which, target):
        for cut in self.cuts():
            cat = b""
            for (i, j) in cut:
                cat += self.table[(i, j)][which][self.index(data, i, j)] or b""
            if cat == target:
                return True
        return False

    def explain(self, data, std, ref_out):
        """Is the difference made of differences of shorter pieces?

        The standard's decode of the bytes is the concatenation of its decodes of the pieces of some way to cut them, and the reference's decode is the
        concatenation of *its* decodes of the pieces of some way to cut them: the two ways may differ, as when the reference resumes after the lead byte of an
        invalid pair and the standard after the pair. The pieces are decoded by themselves. An exact equality, not a likeness. When the standard's decode of the
        bytes is not made of its decodes of shorter pieces (its unit is as long as the bytes: a three-byte character of EUC-JP, an escape sequence, the four bytes
        of gb18030), the difference is a class of its own."""
        return self.made_of(data, 0, std) and self.made_of(data, 1, ref_out)


def decode_job(args):
    """Compare the library with one reference (node, or a Python codec) over a population of byte sequences of one encoding, in replacement mode and, if
    asked, in fatal mode. The population is every sequence of n bytes (`source` an int), or the product of byte sets of a spec (`source` a string). Returns a
    dict: 'classes' {name: (count, examples)} for the replacement mode, 'composed', 'fatal' {name: (count, example)}, 'defects' and 'stats'. `shapes` says whether
    what composition does not explain is classified by shape. Run in a worker process: everything it needs is in `args`."""
    binary, enc, ref, codec, source, with_fatal, plant, shapes = args
    from encoding_diff import Ours  # imported here: encoding_diff.py is also the main program
    ours = Ours(binary, plant)
    witness_planted = 0
    if plant == "mb-witness":
        # A control: the standard's data no longer sides with this library about one pointer of jis0208 (the model reads it from the same file). It is put
        # in place here, in the process that reads the model (a parent's change is not seen by a worker that was started, not forked), and counted.
        M.indexes()["jis0208"][100] = 0x4E00
        witness_planted = 1 if M.indexes()["jis0208"][100] == 0x4E00 else 0
    elif plant == "cpp-model-witness":
        witness_planted = 1
    sets = parse_spec(source) if isinstance(source, str) else None
    n = len(sets) if sets else source
    total = 1
    for s_ in (sets or [range(256)] * n):
        total *= len(s_)
    step = total if sets else 1 << 20  # a product is enumerated whole
    classes = Classes()
    composed = 0
    composed_sig = 0
    fatal_count = defaultdict(int)
    fatal_sig = defaultdict(int)
    fatal_example = {}
    defects = []
    stats = {"compared": 0, "diverged": 0, "fatal_compared": 0, "fatal_diverged": 0, "checked_against_model": 0,
             "fatal_checked_against_model": 0, "checked_against_cpp_model": 0,
             "fatal_checked_against_cpp_model": 0, "witness_planted": witness_planted}

    def fetch(who, fatal, start, count):
        if sets:
            spec = source
            if who == "ours":
                return records_from(ours.enumx(hx(enc.encode()), "f" if fatal else "-", spec))
            if ref == "node":
                return records_from(R.enum_stream(R.node_command("enumx", hx(enc.encode()), "fi" if fatal else "i", spec)))
            return records_from(R.enum_stream(R.python_command("enumx", codec, "strict" if fatal else "replace", spec)))
        if who == "ours":
            return ours_records(ours, enc, n, fatal, start, count)
        if ref == "node":
            return node_records(enc, n, fatal, start, count)
        return python_records(codec, n, fatal, start, count)

    # The pieces for the composition of longer sequences: the product sets of runs of consecutive positions (for the sequences of n bytes, the runs of up to
    # two bytes: the three-byte population is the one whose pieces would be the 16 million).
    piece_sets = sets or [list(range(256))] * n
    pieces = Pieces(ours, enc, ref, codec, piece_sets, 2 if not sets else 5) if n >= 2 else None

    def replacement_class(data, std, b):
        """('composed', None) or ('shape', key) for a difference in replacement mode."""
        if pieces and pieces.explain(data, std, b):
            return "composed", None
        # The long stratified populations (`shapes` false) count what the pieces do not explain as one class: its variety is the reference's state, and a name
        # for each of the hundreds of combinations of a few classes of bytes would say less than the count does.
        return "shape", shape(enc, data, std, b) if shapes else "uncomposed (the reference's decode depends on the bytes around)"

    iterator = itertools.product(*sets) if sets else None
    for start in range(0, total, step):
        count = min(step, total - start)
        if sets:
            datas = [bytes(t) for t in itertools.islice(iterator, count)]
        else:
            datas = [(start + k).to_bytes(n, "big") for k in range(count)]
        o_rep = fetch("ours", False, start, count)
        if plant == "ours-extra-char" and n == 1 and start == 0:
            o_rep[0x41] = o_rep[0x41] + b"\xc3\xa9"  # a control: the library's answer for 'A' gains a character
        elif plant == "ours-lost-char" and n == 1 and start == 0:
            o_rep[0x41] = b""  # ... or loses it
        r_rep = fetch("ref", False, start, count)
        if len(o_rep) != count or len(r_rep) != count:
            defects.append("%s %s: %d / %d records for %d sequences" % (enc, ref, len(o_rep), len(r_rep), count))
            break
        if ref == "node":
            # ... and against the C++ model, the second reading, as whole record streams (one per population and encoding, like the check below).
            cpp = cpp_model_records(binary, enc, source, n, False, start, count, plant)
            if len(cpp) != count:
                defects.append("%s: the C++ model gave %d records for %d sequences" % (enc, len(cpp), count))
            else:
                for k in range(count):
                    if o_rep[k] != cpp[k]:
                        defects.append("%s: %s -> %r, the C++ model says %r" % (enc, datas[k].hex(), o_rep[k], cpp[k]))
                stats["checked_against_cpp_model"] += count
        if n <= 2 or ref == "node":
            # Every sequence of the population is also checked against the standard (the model), whatever the reference says: once for each population and
            # encoding (the job against Node; the jobs against the Python codecs are the same sequences again), in replacement mode here and in fatal mode below.
            for k in range(count):
                std, failed = model_output(enc, datas[k])
                if o_rep[k] != std:
                    defects.append("%s: %s -> %r, the standard (the model) says %r" % (enc, datas[k].hex(), o_rep[k], std))
                stats["checked_against_model"] += 1
        stats["compared"] += count
        del defects[200:]  # (a library with a broken rule would otherwise list millions)
        if o_rep != r_rep:
            for k in range(count):
                a, b = o_rep[k], r_rep[k]
                if a == b:
                    continue
                stats["diverged"] += 1
                data = datas[k]
                std, _ = model_output(enc, data)
                if a != std:
                    defects.append("%s %s: %s -> %r, the standard (the model) says %r" % (enc, ref, data.hex(), a, std))
                    continue
                b = b or b""
                kind, key = replacement_class(data, std, b)
                fp = fingerprint(data, std, b)
                if kind == "composed":
                    composed += 1
                    composed_sig = (composed_sig + fp) & 0xFFFFFFFF
                else:
                    classes.add(key, data, "%s: standard %r, %s %r" % (data.hex(), std.decode("utf-8", "replace"), codec or ref, b.decode("utf-8", "replace")), fp)
        if not with_fatal:
            continue
        o_fat = fetch("ours", True, start, count)
        r_fat = fetch("ref", True, start, count)
        if len(o_fat) != count or len(r_fat) != count:
            defects.append("%s %s fatal: %d / %d records for %d sequences" % (enc, ref, len(o_fat), len(r_fat), count))
            break
        stats["fatal_compared"] += count
        if ref == "node":
            cpp = cpp_model_records(binary, enc, source, n, True, start, count, plant)
            if len(cpp) != count:
                defects.append("%s fatal: the C++ model gave %d records for %d sequences" % (enc, len(cpp), count))
            else:
                for k in range(count):
                    if o_fat[k] != cpp[k]:
                        defects.append("%s fatal: %s -> %r, the C++ model says %r" % (enc, datas[k].hex(), o_fat[k], cpp[k]))
                stats["fatal_checked_against_cpp_model"] += count
        if n <= 2 or ref == "node":
            for k in range(count):
                std, failed = model_output(enc, datas[k])
                expect_ours = None if failed else std
                if o_fat[k] != expect_ours:
                    defects.append("%s fatal: %s -> %r, the standard (the model) says %r" % (enc, datas[k].hex(), o_fat[k], expect_ours))
                stats["fatal_checked_against_model"] += 1
        if o_fat == r_fat:
            continue
        for k in range(count):
            a, b = o_fat[k], r_fat[k]
            if a == b:
                continue
            stats["fatal_diverged"] += 1
            data = datas[k]
            std, failed = model_output(enc, data)
            expect_ours = None if failed else std
            if a != expect_ours:
                defects.append("%s %s fatal: %s -> %r, the standard (the model) says %r" % (enc, ref, data.hex(), a, expect_ours))
                continue
            # What the difference is: the standard fails and the reference does not, the reverse, or both succeed with different text.
            if failed and b is not None:
                what = "the standard fails, the reference decodes"
            elif not failed and b is None:
                what = "the standard decodes, the reference fails"
            else:
                what = "both decode, to different text"
            if o_rep[k] == r_rep[k]:
                what += " (but not in replacement mode: a difference of the fatal path only)"
            key = what
            fatal_count[key] += 1
            fatal_sig[key] = (fatal_sig[key] + fingerprint(data, a, b)) & 0xFFFFFFFF
            fatal_example.setdefault(key, "%s: standard %s, %s %s" % (
                data.hex(), "error" if failed else repr(std.decode("utf-8", "replace")), codec or ref,
                "error" if b is None else repr(b.decode("utf-8", "replace"))))
    return {"classes": classes.named(), "composed": composed, "composed_sig": composed_sig,
            "fatal": {"%s %s" % (k, signature(fatal_sig[k])): (v, fatal_example[k]) for k, v in fatal_count.items()},
            "defects": defects, "stats": stats}



# ---- the witnesses of the aggregate classes -------------------------------------------------------------------------------------------

WITNESSES = os.path.join(HERE, "encoding_mb_witnesses.txt")


def read_witnesses(path=WITNESSES):
    """[dict] from the witness file: reference, who (an encoding for Node, a codec for Python), mode, bytes, standard, reference, kind, derivation."""
    out = []
    with open(path, encoding="utf-8") as fh:
        for number, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 8:
                raise SystemExit("%s:%d: not eight tab-separated fields (reference, who, mode, bytes, standard, reference's output, kind, derivation)" % (path, number))
            out.append(dict(zip(("reference", "who", "mode", "bytes", "standard", "ref_out", "kind", "derivation"), parts)))
    return out


def fatal_kind(failed, ref_failed, same_in_replacement):
    what = ("the standard fails, the reference decodes" if failed and not ref_failed else
            "the standard decodes, the reference fails" if not failed and ref_failed else "both decode, to different text")
    return what


def check_witnesses(binary, encodings, plant=None):
    """Every witness of the aggregate classes (`composed`, `uncomposed`, the three kinds of fatal-mode difference) for the encodings that ran: a short sequence worked
    out by hand from the standard, with what the reference is seen to do. The run asks the library, the Python model and the reference about it again, and the
    class the sequence falls in is computed as the populations compute it: the aggregates are templated, and what they stand for is shown here.
    Returns (defects, number checked)."""
    from encoding_diff import Ours
    ours = Ours(binary, plant)
    defects = []
    checked = 0
    for w in (read_witnesses() if plant != "stale-witness" else [dict(read_witnesses()[0], standard="00")]):
        enc = ENCODING_NAME[w["who"]] if w["reference"] == "python" else w["who"]
        if enc not in encodings:
            continue
        data = bytes.fromhex(w["bytes"])
        fatal = w["mode"] == "fatal"
        spec = ":".join("%02X" % b for b in data)
        want_std = None if w["standard"] == "!" else bytes.fromhex(w["standard"].replace("-", ""))
        want_ref = None if w["ref_out"] == "!" else bytes.fromhex(w["ref_out"].replace("-", ""))
        label = hx(enc.encode())
        lib = records_from(ours.enumx(label, "f" if fatal else "-", spec))[0]
        if w["reference"] == "node":
            ref = records_from(R.enum_stream(R.node_command("enumx", label, "fi" if fatal else "i", spec)))[0]
        else:
            ref = records_from(R.enum_stream(R.python_command("enumx", w["who"], "strict" if fatal else "replace", spec)))[0]
        std, failed = model_output(enc, data)
        model = None if (fatal and failed) else std
        name = "witness %s %s %s %s" % (w["reference"], w["who"], w["mode"], w["bytes"])
        if lib != want_std:
            defects.append("%s: the library says %r, the witness (worked out from the standard) %r" % (name, lib, want_std))
        if model != want_std:
            defects.append("%s: the model says %r, the witness %r" % (name, model, want_std))
        if ref != want_ref:
            defects.append("%s: the reference says %r now, the witness %r" % (name, ref, want_ref))
        if fatal:
            kind = fatal_kind(failed, ref is None, False)
        else:
            sets = [[b] for b in data]
            pieces = Pieces(ours, enc, w["reference"], w["who"] if w["reference"] == "python" else None, sets, 5)
            kind = "composed" if pieces.explain(data, std, ref or b"") else "uncomposed"
        if kind != w["kind"]:
            defects.append("%s: it falls in the class %r, the witness is of %r" % (name, kind, w["kind"]))
        checked += 1
    return defects, checked


# ---- encoders ---------------------------------------------------------------------------------------------------------------------

def cp_ranges(cps):
    """A fingerprint of a set of code points: how many, in how many ranges, from where to where (a name for a class that cannot list them)."""
    cps = sorted(cps)
    runs = 0
    prev = None
    for c in cps:
        if prev is None or c != prev + 1:
            runs += 1
        prev = c
    if runs <= 4:
        out = []
        a = b = None
        for c in cps:
            if a is None:
                a = b = c
            elif c == b + 1:
                b = c
            else:
                out.append((a, b))
                a = b = c
        out.append((a, b))
        return ",".join("U+%04X" % a if a == b else "U+%04X-%04X" % (a, b) for a, b in out)
    return "%d code points in %d ranges, U+%04X to U+%04X" % (len(cps), runs, cps[0], cps[-1])


def encode_records(stream):
    """[None or (bytes, flag)] from an encenum stream: a length byte (255 for a failure), the bytes, and one byte more, which is 1 if the encoding's decoder reads
    the bytes back to the code point, 0 if to something else, 2 if it cannot. Both the library's runner and encoding.py write this, and nothing else."""
    out = []
    i = 0
    n = len(stream)
    while i < n:
        length = stream[i]
        i += 1
        if length == 255:
            out.append(None)
        else:
            data = stream[i:i + length]
            i += length
            if i >= n:
                raise ValueError("an encenum stream ends inside a record")
            out.append((data, stream[i]))
            i += 1
    return out


def encode_job(args):
    """Every code point (but the surrogates) through one encoder, alone, in fatal mode: the library against the standard (the model) for all of them, and against
    one Python codec's encoder; and what the library writes read by Node's decoder. Returns {'classes', 'defects', 'stats'}."""
    binary, enc, codec, node, plant = args
    from encoding_diff import Ours  # imported here: encoding_diff.py is also the main program
    ours = Ours(binary, plant)
    label = hx(enc.encode())
    mine = [r[0] if r else None for r in encode_records(ours.encenum(label, "F", 0, 0x10FFFF))]
    cps = [c for c in range(0x110000) if not 0xD800 <= c <= 0xDFFF]
    defects = []
    stats = {"code_points": len(cps), "encoded": 0, "python_differs": 0, "node_differs": 0, "model_checked": 0}
    if len(mine) != len(cps):
        return {"classes": {}, "defects": ["%s: %d records for %d code points" % (enc, len(mine), len(cps))], "stats": stats}
    # The standard: every code point.
    paths = {}
    for cp, got in zip(cps, mine):
        want, failed = M.encode(enc, [cp], "fatal")
        stats["model_checked"] += 1
        if (failed is not None) != (got is None) or (failed is None and got != want):
            defects.append("%s: U+%04X -> %r, the standard (the model) says %r%s" % (enc, cp, got, want, " (an error)" if failed else ""))
            if len(defects) > 20:
                break
        if got is not None:
            stats["encoded"] += 1
    classes = defaultdict(list)
    examples = {}
    if codec:
        theirs = encode_records(R.enum_stream(R.python_command("encenum", codec, "strict", 0, 0x10FFFF)))
        if len(theirs) != len(cps):
            defects.append("%s %s: %d records for %d code points" % (enc, codec, len(theirs), len(cps)))
        else:
            for cp, got, ref in zip(cps, mine, theirs):
                ref_bytes = ref[0] if ref else None
                if got == ref_bytes:
                    continue
                stats["python_differs"] += 1
                path = M.encode_path(enc, cp)
                if got is None:
                    kind = "the library refuses it, %s writes it" % codec
                elif ref is None:
                    kind = "the library writes it, %s refuses it" % codec
                else:
                    back = {1: "to the same code point", 0: "to another code point", 2: "to nothing"}[ref[1]]
                    kind = "both write it, to different bytes (%s's decoder reads its bytes %s)" % (codec, back)
                key = "%s; %s" % (path, kind)
                classes[key].append(cp)
                examples.setdefault(key, "U+%04X: library %s, %s %s" % (cp, got.hex() if got else "error", codec, ref_bytes.hex() if ref_bytes else "error"))
    named = {"%s [%s]" % (k, cp_ranges(v)): (len(v), [examples[k]]) for k, v in classes.items()}
    if node:
        # Node has no legacy encoder, but its decoder reads what the library writes: it must read it as the standard does.
        pairs = [(cp, got) for cp, got in zip(cps, mine) if got is not None and cp > 0x7F]
        node_classes = defaultdict(list)
        node_examples = {}
        for i in range(0, len(pairs), 20000):
            part = pairs[i:i + 20000]
            answers = R.ask_node(["D %s i %s" % (label, hx(got)) for _, got in part])
            for (cp, got), a in zip(part, answers):
                t = a.split()
                said = R.unhex(t[1]) if t[0] == "OUT" else None
                std, _ = model_output(enc, got)
                if said != std:
                    stats["node_differs"] += 1
                    path = M.encode_path(enc, cp)
                    key = "%s; Node reads the bytes as %s, the standard as %s" % (path, sig(said or b"") or "an error", sig(std))
                    node_classes[key].append(cp)
                    node_examples.setdefault(key, "U+%04X, library %s: standard %r, Node %r" % (
                        cp, got.hex(), std.decode("utf-8", "replace"), (said or b"").decode("utf-8", "replace")))
        for k, v in node_classes.items():
            named["node-reads: %s [%s]" % (k, cp_ranges(v))] = (len(v), [node_examples[k]])
    return {"classes": named, "defects": defects, "stats": stats}



# ---- the reasons --------------------------------------------------------------------------------------------------------------------

ENCODING_NAME = {"cp932": "shift_jis", "shift_jis": "shift_jis", "euc_jp": "euc-jp", "iso2022_jp": "iso-2022-jp", "euc_kr": "euc-kr", "cp949": "euc-kr", "big5": "big5",
                 "big5hkscs": "big5", "gbk": "gbk", "gb18030": "gb18030"}
ENCODING_OF = {"cp932": "Shift_JIS", "shift_jis": "Shift_JIS", "euc_jp": "EUC-JP", "iso2022_jp": "ISO-2022-JP", "euc_kr": "EUC-KR", "cp949": "EUC-KR", "big5": "Big5",
               "big5hkscs": "Big5", "gbk": "GBK", "gb18030": "gb18030", "euc-jp": "EUC-JP", "iso-2022-jp": "ISO-2022-JP", "euc-kr": "EUC-KR"}
SECTION = {
    "Shift_JIS": "12.3.1", "EUC-JP": "12.1.1", "ISO-2022-JP": "12.2.1", "EUC-KR": "13.1.1", "Big5": "11.1.1", "GBK": "10.2.1, GBK's decoder being gb18030's", "gb18030": "10.2.1"}
LETTER = {"A": "an ASCII character", "C": "a C1 control", "F": "U+FFFD", "P": "a private use code point", "U": "a character"}
CUT_PHRASE = {
    "ok": "a character",
    "err": "an error (U+FFFD)",
    "rst": "an error that restores the byte it was looking at (so that byte is read again as the start of what follows)",
}


def spell_sig(sig_text):
    """'FFA' -> 'U+FFFD, U+FFFD, an ASCII character'."""
    if sig_text in ("-", ""):
        return "nothing"
    return ", ".join(LETTER[c] for c in sig_text)


def parse_shape(name):
    """(who, fields) from a class name 'who std=.. ref=.. cut=.. rel=.. leads=.. trails=..'."""
    who, rest = name.split(" ", 1)
    fields = {}
    for part in rest.split(" "):
        if "=" in part:
            k, v = part.split("=", 1)
            fields[k] = v
    return who, fields


def reason(reference, population, cls, example):
    """The line of encoding_divergences.txt for a class of the multi-byte populations: what the reference does, what the standard says, and why the standard decides
    (the library agrees with the standard's own handler, written again in encoding_model.py over indexes.json, for every one of these sequences, which the run checks)."""
    who = cls.split(" ", 1)[0]
    ref_name = {"node": "Node (ICU)"}.get(reference, "Python's %s codec" % who)
    if population.startswith("mb-decode-") and population.endswith("-fatal"):
        what = cls.split(": ", 1)[1].rsplit(" sig=", 1)[0]
        note = ""
        if "fatal path only" in what:
            note = (" These are sequences on which the reference's replacement-mode decode agrees with the standard's and its fatal decode does not: a fault of its fatal path alone.")
        return ("In fatal mode %s and the standard differ on the sequences of this population where %s. The standard's fatal mode stops at the first error its handler returns "
                "(section 4.1, process an item: \"fatal\": \"Return result.\"), an error being exactly what the replacement-mode classes of the same population list (a U+FFFD "
                "the standard writes where the reference writes a character, or the reverse), so each of these sequences is one of those classes seen through the error mode.%s "
                "This line is an aggregate by kind: its members are fixed exactly by the signature in its name, and a witness worked out by hand is in encoding_mb_witnesses.txt."
                % (ref_name, what, note))
    if " composed" in cls:
        return ("Sequences in which %s differs from the standard and which add no new kind of difference: for each, the standard's decode of the bytes is the concatenation of its "
                "decodes of pieces of fewer bytes, and %s's decode of the bytes is the concatenation of its decodes of pieces of fewer bytes (the two ways of cutting may differ), "
                "so the difference is made of differences of the shorter populations (this is an equality of the whole outputs, checked for each sequence). This line is an aggregate "
                "by construction: its members are fixed exactly by the signature in its name, and a witness worked out by hand is in encoding_mb_witnesses.txt." % (ref_name, ref_name))
    if "uncomposed" in cls:
        return ("Sequences of this stratified population in which %s's decode is not the concatenation of its decodes of shorter pieces, so that what it does depends on the bytes "
                "around (a state: %s) and the pieces cannot account for it; the library decodes every one as the standard's handler does (checked against encoding_model.py). "
                "Not named by shape: there are too many combinations of a few kinds of byte for a name each to say more than the count; its members are fixed exactly by the "
                "signature in its name, and a witness worked out by hand is in encoding_mb_witnesses.txt." % (
                    ref_name, "ISO-2022-JP's escape handling" if "iso" in who else "a lead byte it holds, or the end of a long form"))
    if population.startswith("mb-decode-"):
        who2, f = parse_shape(cls)
        parts = population.split("-")
        n = int(parts[2]) if parts[2].isdigit() else 4
        encoding = ENCODING_OF.get(who2, who2)
        section = SECTION.get(encoding, "")
        units = " then ".join(CUT_PHRASE[c] for c in f["cut"].split(",")) if f.get("cut") and f["cut"] != "-" else "nothing"
        quote = ""
        if "rst" in f["cut"]:
            quote = " (\"If byte is an ASCII byte, then restore byte to ioQueue.\")"
        elif f["cut"] == "err":
            quote = " (\"Return error.\")"
        rel = {"c1": " The reference's character is a C1 control.", "ref-private-use": " The reference's character is in the private use area.",
               "same-after-nfkc": " The two characters are the same after NFKC: the reference maps the pointer to the compatibility form.",
               "other-character": " The two are different characters.", "std-private-use": " The standard's character is in the private use area."}.get(f["rel"], "")
        what = ("the byte%s %s" % ("s" if "," in f["leads"] or "-" in f["leads"] else "", f["leads"])) if n == 1 else (
            "the %d bytes with lead byte%s %s and last byte%s %s" % (n, "s" if "," in f["leads"] or "-" in f["leads"] else "", f["leads"],
                                                                      "s" if "," in f["trails"] or "-" in f["trails"] else "", f["trails"]))
        return ("Given %s, %s writes %s; the standard's %s decoder (section %s) reads them as %s and writes %s%s, which is what the handler and the index "
                "(indexes.json) say. The library writes the standard's, as the model does.%s" % (what, ref_name, spell_sig(f["ref"]), encoding, section, units, spell_sig(f["std"]), quote, rel))
    if population == "mb-encode":
        return ("Over every code point: %s. The library follows the standard's encoder, section 10.2.2, 11.1.2, 12.1.2, 12.2.2, 12.3.2 or 13.1.2 for the encoding "
                "(the model, which reads indexes.json, agrees on every code point)." % cls.split(" ", 1)[1])
    if population == "mb-encode-text":
        return ("The standard's ISO-2022-JP encoder leaves the Roman state only when a character needs another (\"If codePoint is an ASCII code point, and ISO-2022-JP encoder state is not "
                "ASCII, then restore codePoint to ioQueue, set ISO-2022-JP encoder state to ASCII, and return three bytes 0x1B 0x28 0x42\" applies to a code point Roman cannot write: "
                "\\ and ~); %s switches back to ASCII after each U+00A5 or U+203E. Both are valid ISO-2022-JP; the bytes differ." % ref_name)
    return None
