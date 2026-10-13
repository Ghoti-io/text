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
"""Check the transcription of the multi-byte handlers against the standard's text (make check-encoding-transcription).

    tools/encoding/check-transcription.py [--show]

The handlers of src/encoding/encoding_cjk.c are written by hand from the standard's algorithms, and the models that the tests compare them with
(tests/encoding_cjk_model.h, tools/oracle/encoding_model.py) were written from the same reading, so a constant misread in all three would pass every comparison
between them. This check reads the constants from the standard's source text instead (third_party/encoding/encoding.bs, the Bikeshed source at the pinned commit,
verified by tools/encoding/fetch.sh against ENCODING_BS_SHA256) and compares them with the constants in the code of each encoding, section by section:

  * every number the standard's algorithm names (a byte, a code point, a pointer, a multiplier: `0x81`, `U+20AC`, `188`) must appear in the code of that encoding;
  * every number in the code of that encoding must appear in the standard's text of it, or be listed in tools/encoding/TRANSCRIPTION_ALLOWED with the reason
    it is the implementation's and not the standard's (a table size, a buffer length, a bit mask);
  * the escape sequences of ISO-2022-JP, `0x1B` and two bytes, are the same set in the standard and in the calls of iso_escape();
  * the lists are checked for staleness: an allowed number that is no longer in the code, or a section that has no text, is a failure.

What it does not check is the logic around the numbers (the models and the differentials do), or that a constant is used in the right place: a number moved to the
wrong branch passes here and fails there. The two checks cover each other's gap.

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BS = os.path.join(ROOT, "third_party", "encoding", "encoding.bs")
CODE = os.path.join(ROOT, "src", "encoding", "encoding_cjk.c")
# The index operations that the tables carry (the exclusions, the last pointer of six code points, the ranges' two functions) are in the generator and in the
# lookup of the internal header, not in the handlers: their numbers are looked for there too.
ELSEWHERE = ["src/encoding/encoding_internal.h", "tools/encoding/gen_tables.py"]
ALLOWED = os.path.join(HERE, "TRANSCRIPTION_ALLOWED")

# The standard's sections (ids of the Bikeshed headings and, for the index operations, of the `<dfn>`s) for each region of the code, which begins at the banner
# comment of that name in encoding_cjk.c.
REGIONS = [
    ("gb18030 and GBK", "gb18030 and GBK (section 10)", ["gbk-decoder", "gbk-encoder", "gb18030-decoder", "gb18030-encoder"], ["index gb18030 ranges code point", "index gb18030 ranges pointer"]),
    ("Big5", "Big5 (section 11)", ["big5-decoder", "big5-encoder"], ["index Big5 pointer"]),
    ("EUC-JP", "EUC-JP (section 12.1)", ["euc-jp-decoder", "euc-jp-encoder"], []),
    ("ISO-2022-JP", "ISO-2022-JP (section 12.2)", ["iso-2022-jp-decoder", "iso-2022-jp-encoder"], []),
    ("Shift_JIS", "Shift_JIS (section 12.3)", ["shift_jis-decoder", "shift_jis-encoder"], ["index Shift_JIS pointer"]),
    ("EUC-KR", "EUC-KR (section 13)", ["euc-kr-decoder", "euc-kr-encoder"], []),
]


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def section_text(bs, heading_id):
    """The text of the section whose heading has this id, up to the next heading, with the markup taken out."""
    m = re.search(r"^<h[2-6] id=%s[ >].*$" % re.escape(heading_id), bs, re.M)
    if not m:
        raise SystemExit("the standard has no section %r (is third_party/encoding/encoding.bs the pinned one? tools/encoding/fetch.sh)" % heading_id)
    nxt = re.search(r"^<h[2-6] ", bs[m.end():], re.M)
    return strip(bs[m.end():m.end() + nxt.start()] if nxt else bs[m.end():])


def definition_text(bs, name):
    """The `<div algorithm>` that defines `name` (an index operation of the indexes section), as text."""
    m = re.search(r"<dfn>%s</dfn>" % re.escape(name), bs)
    if not m:
        raise SystemExit("the standard has no definition of %r" % name)
    return strip(bs[m.start():bs.index("</div>", m.end())])


def strip(text):
    text = re.sub(r"<!--.*?-->", " ", text, flags=re.S)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
    return re.sub(r"\s+", " ", text)


def numbers(text, standard):
    """{value: spelling} of the numbers in a text. In the standard: `0x81`, `U+20AC`, and decimals of two digits or more outside dates and section numbers."""
    out = {}
    for m in re.finditer(r"0[xX]([0-9A-Fa-f]+)", text):
        out.setdefault(int(m.group(1), 16), m.group(0))
    if standard:
        for m in re.finditer(r"U\+([0-9A-F]{4,6})", text):
            out.setdefault(int(m.group(1), 16), m.group(0))
        for m in re.finditer(r"(?<![\w.+])(\d{2,})(?![\w.])", text):
            out.setdefault(int(m.group(1)), m.group(1))
    else:
        for m in re.finditer(r"(?<![\w.])(\d{2,})[uU]?(?![\w.])", text):
            out.setdefault(int(m.group(1)), m.group(1))
        for m in re.finditer(r"'(.)'", text):
            out.setdefault(ord(m.group(1)), m.group(0))
    return out


def code_without_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)


def code_regions(code):
    """{banner: text of the code under it}."""
    marks = [(m.start(), m.group(1)) for m in re.finditer(r"^/\* ---- (.+?) -+ \*/$", code, re.M)]
    out = {}
    for i, (pos, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(code)
        out[name] = code[pos:end]
    return out


def read_allowed():
    out = {}
    with open(ALLOWED, encoding="utf-8") as fh:
        for number, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 4 or not parts[1].strip() or not parts[3].strip():
                raise SystemExit("%s:%d: not region, number, where, reason: %r" % (ALLOWED, number, line[:60]))
            out[(parts[0], int(parts[1], 0))] = (parts[2], parts[3])
    return out


def evaluate(bs, regions, allowed, elsewhere_text):
    """(failures, used allowed entries, total standard numbers, total code numbers) for the code of each region."""
    failures = []
    used_allowed = set()
    total_std = total_code = 0
    for name, banner, headings, definitions in REGIONS:
        if banner not in regions:
            failures.append("%s: no region %r in encoding_cjk.c" % (name, banner))
            continue
        served = " ".join([section_text(bs, h) for h in headings] + [definition_text(bs, d) for d in definitions])
        std = numbers(served, True)
        std.pop(0, None)  # "set it to 0x00": the code's 0 is not a literal of two digits
        mine = numbers(code_without_comments(regions[banner]), False)
        total_std += len(std)
        total_code += len(mine)
        if not std:
            failures.append("%s: the standard's text has no numbers (the extraction is broken)" % name)
        # A number of the standard that the handlers do not use is allowed when it is a note's (`note`), or when it is used in the file the entry names (a
        # number of an index operation that the tables carry); a number of the code that the standard's text does not name, when the entry says `code`.
        missing = sorted(v for v in std if v not in mine)
        extra = sorted(v for v in mine if v not in std)
        ok_missing = []
        for v in missing:
            where = allowed.get((name, v), (None,))[0]
            if where == "note" or (where in elsewhere_text and v in numbers(elsewhere_text[where], False)):
                used_allowed.add((name, v))
                ok_missing.append(v)
        ok_extra = []
        for v in extra:
            if allowed.get((name, v), (None,))[0] == "code":
                used_allowed.add((name, v))
                ok_extra.append(v)
        missing = [v for v in missing if v not in ok_missing]
        extra = [v for v in extra if v not in ok_extra]
        if missing:
            failures.append("%s: the standard names %s and the code does not use them" % (name, ", ".join(std[v] for v in missing)))
        if extra:
            failures.append("%s: the code uses %s, which the standard's text of it does not name (add to TRANSCRIPTION_ALLOWED with the reason if it is the implementation's)" % (
                name, ", ".join(mine[v] for v in extra)))
        if name == "ISO-2022-JP":
            std_escapes = set(re.findall(r"0x1B 0x([0-9A-F]{2}) 0x([0-9A-F]{2})", served))
            code_escapes = set((a.upper(), b.upper()) for a, b in re.findall(r"iso_escape\(out, 0x([0-9A-Fa-f]{2}), 0x([0-9A-Fa-f]{2})\)", regions[banner]))
            if std_escapes != code_escapes or len(std_escapes) != 3:
                failures.append("ISO-2022-JP: the standard's escape sequences (encoder) are %s, the calls of iso_escape() write %s" % (sorted(std_escapes), sorted(code_escapes)))
    return failures, used_allowed, total_std, total_code


def misread(text, value):
    """The text with every literal of `value` (hex, or decimal of two digits or more) read as value + 1: a constant misread consistently."""
    def swap(m):
        spelling = m.group(0)
        if spelling.lower().startswith("0x"):
            body = spelling[2:].rstrip("uU")
            width = len(body)
            return "0x%0*X" % (width, value + 1) + ("u" if spelling.lower().endswith("u") else "")
        return str(value + 1) + ("u" if spelling.lower().endswith("u") else "")
    return re.sub(r"(?<![\w.])(0[xX][0-9A-Fa-f]+[uU]?|\d{2,}[uU]?)(?![\w.])", lambda m: swap(m) if int(m.group(1).rstrip("uU"), 0 if m.group(1).lower().startswith("0x") else 10) == value else m.group(0), text)


def main(argv):
    show = "--show" in argv
    if not os.path.exists(BS):
        sys.stderr.write("%s is not here: run tools/encoding/fetch.sh\n" % BS)
        return 2
    bs = read(BS)
    code = read(CODE)
    regions = code_regions(code)
    allowed = read_allowed()
    elsewhere_text = {path: code_without_comments(read(os.path.join(ROOT, path))) for path in ELSEWHERE}
    failures, used_allowed, total_std, total_code = evaluate(bs, regions, allowed, elsewhere_text)
    for name, banner, headings, definitions in REGIONS:
        if show:
            print("%-16s checked" % name)
    for region, v in sorted(k for k in allowed if k not in used_allowed):
        failures.append("TRANSCRIPTION_ALLOWED lists %s %s, which the code and the standard no longer differ on" % (region, hex(v)))
    if failures:
        sys.stderr.write("\033[0;31m\n### The transcription of the multi-byte handlers does not match the standard's text ###\033[0m\n  %s\n" % "\n  ".join(failures))
        return 3
    # The controls: every number the code and the standard share, misread by one in the whole of its region, must be found.
    tried = undetected = 0
    for name, banner, headings, definitions in REGIONS:
        served = " ".join([section_text(bs, h) for h in headings] + [definition_text(bs, d) for d in definitions])
        shared = sorted(set(numbers(served, True)) & set(numbers(code_without_comments(regions[banner]), False)) - {0})
        for v in shared:
            if (name, v) in allowed:
                continue
            mutated = dict(regions)
            mutated[banner] = misread(regions[banner], v)
            tried += 1
            if mutated[banner] == regions[banner]:
                undetected += 1
                sys.stderr.write("control %s %s: the number was not found to change\n" % (name, hex(v)))
                continue
            found, _, _, _ = evaluate(bs, mutated, allowed, elsewhere_text)
            if not found:
                undetected += 1
                sys.stderr.write("control %s %s: misread by one, and the check passed\n" % (name, hex(v)))
    if undetected:
        return 3
    print("transcription: %d numbers of the standard's text and %d of the code, region by region, agree (%d allowed as the implementation's); %d misreadings planted, all found" % (
        total_std, total_code, len(allowed), tried))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
