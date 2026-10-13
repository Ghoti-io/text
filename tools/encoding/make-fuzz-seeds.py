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
"""Write the seed corpus for fuzz_encoding, the Encoding module's fuzzer.

    tools/encoding/make-fuzz-seeds.py

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else) and they are *derived*, the same bytes
each time. Each is four option bytes and then the payload (the layout is in tests/fuzz/fuzz_encoding.cpp): o0 the encoding
(1 to 40 in the standard's order; 0, 41, 42 and 43 are values that are no encoding), o1 the decoder's options and cut, o2 the
encoder's mode and cut, o3 the limit, the refused sink call, the allocation sweep and the misuse of the entry points.

The payloads for the multi-byte encodings add the vectors of the standard's own suite (the decode vectors of iso-2022-jp-decoder.any.js, the gb18030 and
GBK decoders and the ones in textdecoder-mistakes.any.js, read from the pinned checkout under third_party/encoding/wpt when it is there, bytes of the
legacy-mb-* pages' character files) and sequences written for the rules of sections 10 to 13: escape sequences of every kind, a lead byte at the end of
the input, an ASCII trail byte after an invalid pair (the byte restored), the four-byte forms of gb18030 and its ends, Big5's four two-code-point pairs,
Shift_JIS's private use pointers, EUC-JP's three-byte form, the 0x80 of each.

The payloads are what the codecs are about: text in each of UTF-8's lengths, ill-formed UTF-8 of every kind in the Unicode
standard's table, every byte value, UTF-16 with pairs and lone surrogates and an odd tail, byte order marks whole and cut,
the bytes that three of the ISO-8859 encodings leave without a character, text with characters outside a legacy encoding,
and runs longer than the 4 KiB of one output chunk. The ten encodings that matter most get all of them and the others a tenth of them, with option bytes taken from a hash of
the pair, so that adding or removing a payload changes no other file; a few seeds are written by hand for the option bits that
a hash reaches rarely (the allocation sweep, the misuse of every entry point, each limit).

Files named `reproducer-*.seed` are not touched by this script, so a find can be kept there and replayed by `make test`
(tests/test-encoding-fuzz-seeds.cpp, which replays every `*.seed`) and by every fuzz run.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SUITE = os.path.join(ROOT, "third_party", "encoding", "wpt", "encoding")
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "encoding")
NAMES = ["utf-8", "ibm866", "iso-8859-2", "iso-8859-3", "iso-8859-4", "iso-8859-5", "iso-8859-6", "iso-8859-7",
         "iso-8859-8", "iso-8859-8-i", "iso-8859-10", "iso-8859-13", "iso-8859-14", "iso-8859-15", "iso-8859-16",
         "koi8-r", "koi8-u", "macintosh", "windows-874", "windows-1250", "windows-1251", "windows-1252", "windows-1253",
         "windows-1254", "windows-1255", "windows-1256", "windows-1257", "windows-1258", "x-mac-cyrillic", "gbk",
         "gb18030", "big5", "euc-jp", "iso-2022-jp", "shift_jis", "euc-kr", "replacement", "utf-16be", "utf-16le",
         "x-user-defined"]


def u16(cps, big):
    out = b""
    for cp in cps:
        out += cp.to_bytes(2, "big" if big else "little")
    return out


def payloads():
    p = {}
    p["empty"] = b""
    p["ascii"] = b"Hello, world! 0123456789 <p>text</p>\n\t\x00\x7f"
    p["all-bytes"] = bytes(range(256))
    p["all-bytes-reversed"] = bytes(range(255, -1, -1))
    p["utf8-valid"] = "a\u00e9\u20ac\U0001f4a9z\u0100\u07ff\u0800\uffff\U00010000\U0010ffff".encode("utf-8")
    p["utf8-two-byte-text"] = "caf\u00e9 na\u00efve \u0153uvre \u00fcber \u00df".encode("utf-8")
    p["utf8-euro-emoji"] = "price \u20ac5 \U0001f4a9 \u2603".encode("utf-8")
    p["utf8-cyrillic-greek"] = "\u041f\u0440\u0438\u0432\u0435\u0442 \u03b3\u03b5\u03b9\u03ac \u05e9\u05dc\u05d5\u05dd \u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35".encode("utf-8")
    p["utf8-cjk"] = "\u65e5\u672c\u8a9e \u4e2d\u6587 \ud55c\uad6d\uc5b4".encode("utf-8")
    p["utf8-ill-formed-table"] = bytes([0x61, 0xF1, 0x80, 0x80, 0xE1, 0x80, 0xC2, 0x62, 0x80, 0x63, 0x80, 0xBF, 0x64])
    p["utf8-overlong-and-surrogates"] = bytes([0xC0, 0xAF, 0xE0, 0x80, 0xBF, 0xF0, 0x81, 0x82, 0x41, 0xED, 0xA0, 0x80, 0xED, 0xBF, 0xBF])
    p["utf8-above-10ffff"] = bytes([0xF4, 0x90, 0x80, 0x80, 0xF5, 0x80, 0x80, 0x80, 0xF8, 0x88, 0x80, 0x80, 0x80, 0xFF, 0xFE])
    p["utf8-truncated-tails"] = bytes([0x41, 0xE2, 0x82, 0x41, 0xF0, 0x9F, 0x92, 0x41, 0xC3, 0xE2, 0x82])
    p["utf8-truncated-at-end"] = bytes([0x41, 0xF0, 0x9F, 0x92])
    p["utf8-continuation-bytes"] = bytes([0x80, 0x81, 0xBF, 0x80, 0x41])
    p["utf16le-valid"] = u16([0x41, 0x20AC, 0xD83D, 0xDCA9, 0x4F60, 0xFFFE, 0xFFFF, 0x0], False)
    p["utf16be-valid"] = u16([0x41, 0x20AC, 0xD83D, 0xDCA9, 0x4F60, 0xFFFE, 0xFFFF, 0x0], True)
    p["utf16le-lone-surrogates"] = u16([0x61, 0x62, 0xD800, 0x77, 0x78, 0xDC00, 0xD800, 0xD800, 0xDC00, 0xDFFF, 0xD800], False)
    p["utf16be-lone-surrogates"] = u16([0x61, 0x62, 0xD800, 0x77, 0x78, 0xDC00, 0xD800, 0xD800, 0xDC00, 0xDFFF, 0xD800], True)
    p["utf16le-odd-tail"] = u16([0x41, 0xD83D], False) + b"\x42"
    p["utf16be-odd-tail"] = u16([0x41, 0xD83D], True) + b"\x42"
    p["utf16le-boundary-pairs"] = u16([0xD7FF, 0xD800, 0xDC00, 0xDBFF, 0xDFFF, 0xE000, 0xDBFF, 0xDFFF], False)
    p["utf16be-boundary-pairs"] = u16([0xD7FF, 0xD800, 0xDC00, 0xDBFF, 0xDFFF, 0xE000, 0xDBFF, 0xDFFF], True)
    p["utf16-single-byte"] = b"\x00"
    p["bom-utf8"] = b"\xEF\xBB\xBFhello \xC3\xA9"
    p["bom-utf8-twice"] = b"\xEF\xBB\xBF\xEF\xBB\xBFhello"
    p["bom-utf16le"] = b"\xFF\xFE" + u16([0x68, 0x69, 0xD83D, 0xDCA9], False)
    p["bom-utf16be"] = b"\xFE\xFF" + u16([0x68, 0x69, 0xD83D, 0xDCA9], True)
    p["bom-prefix-ef-bb"] = b"\xEF\xBB"
    p["bom-prefix-then-text"] = b"\xEF\xBBabc"
    p["bom-prefix-fe"] = b"\xFE"
    p["bom-prefix-ff-then-text"] = b"\xFFabc"
    p["bom-in-the-middle"] = b"abc\xEF\xBB\xBFdef"
    p["bom-after-error"] = b"\xFF\xEF\xBB\xBF"
    p["latin-high-bytes"] = bytes([0x80, 0x81, 0x8D, 0x8F, 0x90, 0x9D, 0xA0, 0xA4, 0xE9, 0xFF])
    p["unmapped-in-some-iso"] = bytes([0xA5, 0xAE, 0xBE, 0xC3, 0xD0, 0xE3, 0xF0, 0xA1, 0xDB, 0xFF, 0xFC, 0xFD, 0xFE, 0xAF, 0xD2])
    p["run-4k-ascii"] = b"x" * 4100
    p["run-8k-three-byte"] = "\u20ac".encode("utf-8") * 2800
    p["run-high-bytes"] = bytes([0x80 + (i % 128) for i in range(5000)])
    p["run-errors"] = bytes([0xF0, 0x9F, 0x92] * 1500)
    p["run-lone-leads"] = u16([0xD800] * 2500, False)
    p["run-emoji-utf8"] = "\U0001f4a9".encode("utf-8") * 1200
    return p


MULTI = ("gbk", "gb18030", "big5", "euc-jp", "iso-2022-jp", "shift_jis", "euc-kr")


def multi_byte_payloads():
    """Payloads for the multi-byte encodings: [(name, bytes, encodings it is for)]."""
    out = []
    # written for the rules
    out.append(("mb-lead-at-end", b"ab\x81", MULTI))
    out.append(("mb-lead-lead-trail", b"\x81\x81\x40", MULTI))
    out.append(("mb-invalid-pair-ascii-trail", b"\x81\x20\x81\x7f\x8e\x41\xa1\x30", MULTI))
    out.append(("mb-0x80-and-ff", b"\x80\xff\xfe\xfd\xa0", MULTI))
    out.append(("mb-high-run", bytes(range(0x80, 0x100)) * 3, MULTI))
    out.append(("gb-four-byte", b"\x81\x30\x81\x30 \x84\x31\xa4\x39 \x84\x31\xa5\x30 \x90\x30\x81\x30 \xe3\x32\x9a\x35 \xe3\x32\x9a\x36", ("gb18030", "gbk")))
    out.append(("gb-four-byte-broken", b"\x81\x30\x81\x41\x81\x30\xfe\x81\x30\x81\x30\x81\x30\x20\x81\x31\xff", ("gb18030", "gbk")))
    out.append(("gb-e7c7-and-side", b"\x81\x35\xf4\x37\x81\x35\xf4\x36\xa6\xd9\xfe\x59\xa3\xa0\xa2\xe3\x80", ("gb18030", "gbk")))
    out.append(("big5-pairs", b"\x88\x62\x88\x64\x88\xa3\x88\xa5\x88\x63\xa4\x40\xf9\xf9\x81\x40\x87\x40", ("big5",)))
    out.append(("sjis-text", b"\x82\xa0\x82\xa2\x83\x41\xb1\xb2\x81\x40\x5c\x7e\xf0\x40\xf9\xfc\x85\x40\xfa\x40", ("shift_jis",)))
    out.append(("sjis-invalid", b"\x81\x20\x81\xfd\x82\x40\xa0\xfd\xfe\xff\x80\x86\xff", ("shift_jis",)))
    out.append(("eucjp-text", b"\xa4\xa2\xa4\xa4\x8e\xb1\x8f\xb0\xa1\x8f\xb0\x8f\xa2\xaf\x41\x8e\xe0", ("euc-jp",)))
    out.append(("eucjp-three-byte-broken", b"\x8f\xa1\x20\x8f\x20\x8f\xa1\xa1\x8f\xa1\xff\x8e\xa0", ("euc-jp",)))
    out.append(("euckr-text", b"\xb0\xa1\xb0\xa2\x81\x41\xc9\xfe\xc6\xfe\x8e\x41\xa1\xa1\xff", ("euc-kr",)))
    out.append(("iso-escapes", b"\x1b$B$\"\x1b(B\x1b(J\\~\x1b(I1_\x1b$@!!\x1b(B", ("iso-2022-jp",)))
    out.append(("iso-escape-errors", b"\x1b\x1b$\x1b$x\x1b(\x1b(x\x1b$B\x1b(B\x1b(B\x1b$B!\x1b(Bx\x0e\x0f", ("iso-2022-jp",)))
    out.append(("iso-escape-cut", b"\x1b$B$\"\x1b$B$\"\x1b(", ("iso-2022-jp",)))
    out.append(("iso-roman-katakana", b"\x1b(J\x5c\x7e\x1b(I\x21\x5f\x60\x1b(B\x5c\x7e", ("iso-2022-jp",)))
    out.append(("iso-text-utf8", "A~\u00a5\u203e\u3042\u30a2\uff71\u4e9c\u00e9\u2212\U0001f4a9\x0e\x1b".encode("utf-8"), ("iso-2022-jp",)))
    out.append(("iso-text-ends-in-kanji", "abc \u3042\u4e9c".encode("utf-8"), ("iso-2022-jp",)))
    out.append(("iso-text-ends-in-roman", "abc\u00a5\u203e".encode("utf-8"), ("iso-2022-jp",)))
    out.append(("iso-text-kanji-then-reference", "\u3042\u00e9\u3044\u00e9".encode("utf-8"), ("iso-2022-jp",)))
    out.append(("cjk-utf8-all", "\u3042\u30a2\uff71\u4e9c\ud55c\uac00\u4e2d\u6587\u20ac\u00a5\u203e\u2212\ue5e5\U00020000\U0010ffff\u0080".encode("utf-8"), MULTI))
    # the suite's own vectors
    def numbers(text):
        return bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{1,2}", text))
    def read(name):
        path = os.path.join(SUITE, name)
        if not os.path.exists(path):
            return ""
        with open(path, encoding="latin-1") as fh:  # (the character files are in their encodings; only their markup is read)
            return fh.read()
    for n, m in enumerate(re.finditer(r"decode\(\[([^\]]*)\]", read("iso-2022-jp-decoder.any.js"))):
        out.append(("wpt-iso2022jp-%02d" % n, numbers(m.group(1)), ("iso-2022-jp",)))
    for n, m in enumerate(re.finditer(r"decode\(\[([^\]]*)\]", read("legacy-mb-schinese/gb18030/gb18030-decoder.any.js"))):
        out.append(("wpt-gb18030-%02d" % n, numbers(m.group(1)), ("gb18030", "gbk")))
    for n, m in enumerate(re.finditer(r"check\(\[([^\]]*)\]", read("textdecoder-mistakes.any.js"))):
        out.append(("wpt-mistakes-gb-%02d" % n, numbers(m.group(1)), ("gb18030", "gbk")))
    mistakes = read("textdecoder-mistakes.any.js")
    for n, m in enumerate(re.finditer(r"\[\[((?:0x[0-9a-fA-F]{2},?\s*)+)\],", mistakes)):
        for e in ("big5", "iso-2022-jp"):
            out.append(("wpt-mistakes-%s-%02d" % (e, n), numbers(m.group(1)), (e,)))
    # bytes of the character files of the legacy-mb pages: one payload per encoding of evenly spaced characters, and the first and last of each
    pages = {"shift_jis": "legacy-mb-japanese/shift_jis/sjis_chars.html", "euc-jp": "legacy-mb-japanese/euc-jp/eucjp_chars.html",
             "iso-2022-jp": "legacy-mb-japanese/iso-2022-jp/iso2022jp_chars.html", "euc-kr": "legacy-mb-korean/euc-kr/euckr_chars.html",
             "big5": "legacy-mb-tchinese/big5/big5_chars.html"}
    for enc, page in pages.items():
        found = re.findall(r'data-bytes="([0-9A-F ]+)"', read(page))
        if not found:
            continue
        step = max(1, len(found) // 60)
        picked = found[::step][:60]
        out.append(("wpt-chars-%s-sample" % enc, b"".join(bytes.fromhex(x.replace(" ", "")) for x in picked), (enc,)))
        out.append(("wpt-chars-%s-ends" % enc, b"".join(bytes.fromhex(x.replace(" ", "")) for x in (found[:3] + found[-3:])), (enc,)))
    return out


# Every payload for these; the small set for the others.
KEY = ("utf-8", "windows-1252", "iso-8859-2", "utf-16le", "utf-16be", "replacement", "x-user-defined", "iso-8859-3", "koi8-r",
       "shift_jis", "gbk", "gb18030", "big5", "euc-jp", "iso-2022-jp", "euc-kr")
SMALL = ("empty", "ascii", "all-bytes", "utf8-euro-emoji", "utf8-ill-formed-table", "latin-high-bytes", "unmapped-in-some-iso",
         "bom-utf8", "utf16le-lone-surrogates", "utf8-cyrillic-greek")

# o1: bit 0 fatal, bit 1 sniffing, bit 2 removal, bits 4-7 fineness. o2: bits 0-1 mode, bits 4-7 fineness.
O1 = [0x00, 0x01, 0x02, 0x04, 0x03, 0x05, 0x06, 0x20, 0x31, 0x52, 0x74, 0x10, 0x87, 0xF1, 0xA2, 0xC4]
O2 = [0x00, 0x01, 0x02, 0x10, 0x21, 0x32, 0x41, 0x52, 0x80, 0xF1, 0x62, 0x70]
# o3: bits 0-2 limit, 3-5 sink call, 6 allocation sweep, 7 misuse.
O3 = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x10, 0x1A, 0x2B, 0x40, 0x41, 0x42, 0x48, 0x80, 0x82, 0xC0, 0x21]


def pick(table, key, salt):
    h = hashlib.sha256((key + "/" + salt).encode()).digest()
    return table[h[0] % len(table)]


def write(name, o0, o1, o2, o3, payload, written):
    path = os.path.join(CORPUS, name + ".seed")
    with open(path, "wb") as fh:
        fh.write(bytes([o0, o1, o2, o3]) + payload)
    written.add(os.path.basename(path))


def main():
    os.makedirs(CORPUS, exist_ok=True)
    written = set()
    pl = payloads()
    for index, enc in enumerate(NAMES):
        for pname, data in pl.items():
            if enc not in KEY and pname not in SMALL:
                continue
            key = "%s/%s" % (enc, pname)
            write("%s-%s" % (enc, pname), index + 1, pick(O1, key, "o1"), pick(O2, key, "o2"), pick(O3, key, "o3"), data, written)
    # The multi-byte encodings' payloads, each with option bytes taken from a hash of the pair, and the same with the bits that a hash reaches rarely.
    for pname, data, encs in multi_byte_payloads():
        for enc in encs:
            index = NAMES.index(enc) + 1
            key = "%s/%s" % (enc, pname)
            write("%s-%s" % (enc, pname), index, pick(O1, key, "o1"), pick(O2, key, "o2"), pick(O3, key, "o3"), data, written)
            for label, o1, o2, o3 in (("fatal", 0x01, 0x01, 0x04), ("bytewise-limit", 0x01, 0x10, 0x05), ("alloc", 0x00, 0x02, 0x40), ("sniff", 0x02, 0x00, 0x00)):
                if hashlib.sha256((key + label).encode()).digest()[0] < 96:
                    write("%s-%s-%s" % (enc, pname, label), index, o1, o2, o3, data, written)
    # Values that are no encoding, with the misuse bit and without.
    for o0 in (0, 41, 42, 43):
        write("not-an-encoding-%d" % o0, o0, 0, 0, 0x80, b"abc\xff", written)
        write("not-an-encoding-%d-plain" % o0, o0, 0, 0, 0, b"", written)
    # The option bits that a hash reaches rarely, for the encodings that matter most.
    for enc in ("utf-8", "windows-1252", "iso-8859-2", "utf-16le", "x-user-defined", "replacement", "shift_jis", "gb18030", "big5", "euc-jp", "iso-2022-jp", "euc-kr", "gbk"):
        index = NAMES.index(enc) + 1
        for pname in ("utf8-euro-emoji", "utf8-ill-formed-table", "run-8k-three-byte", "all-bytes"):
            for label, o1, o2, o3 in (("alloc", 0x00, 0x01, 0x40), ("alloc-fatal", 0x01, 0x00, 0x40), ("misuse", 0x00, 0x00, 0x80),
                                      ("limit-short", 0x00, 0x01, 0x02), ("limit-half", 0x11, 0x11, 0x03), ("limit-one", 0x00, 0x02, 0x04),
                                      ("sink-refuses", 0x00, 0x01, 0x18), ("sniff-removal", 0x06, 0x02, 0x00), ("bytewise", 0x01, 0x10, 0x00)):
                write("%s-%s-%s" % (enc, pname, label), index, o1, o2, o3, pl[pname], written)
    # Remove seeds a previous run wrote and this one does not (a reproducer is kept).
    for f in sorted(os.listdir(CORPUS)):
        if f.endswith(".seed") and not f.startswith("reproducer-") and f not in written:
            os.remove(os.path.join(CORPUS, f))
    print("%d seeds" % len(written))
    # The count the replay test holds the corpus to (tests/test-encoding-fuzz-seeds.cpp): a seed that is deleted, or that the generator no longer writes, is a change to read.
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "FUZZ_SEED_COUNT"), "w") as fh:
        fh.write("%d\n" % len(written))


if __name__ == "__main__":
    main()
