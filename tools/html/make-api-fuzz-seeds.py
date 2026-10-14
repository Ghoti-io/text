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
"""Write the seed corpus for fuzz_html, the public HTML API's fuzzer.

    tools/html/make-api-fuzz-seeds.py

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else) and they are
*derived*, the same bytes each time. Most come from the tree builder's seeds (tests/fuzz/corpus/html-tree,
which tools/html/make-tree-fuzz-seeds.py made from the pinned web-platform-tests vectors and the
hand-chosen extras): every third one is kept, with a fifth option byte added. That byte is chosen by a hash
of the seed, so that adding or removing a seed changes no other file, from eight sets that between them
turn on every part of the API's options (count-only mode, a byte order mark in front, the cap on kept
errors, a UTF-8 transport label, the failure sweep without a result struct). The rest
are the seeds below, which hold what the vectors do not: a byte order mark whole, cut and half-formed,
inputs with more parse errors than the cap, the empty input, the largest runs the API's limits are about; and
the **encoding world's**: a document in each of the twenty encodings of the fuzzer's table, announced by a
certain label, a `<meta>` inside the first 1024 bytes, a `<meta>` after them (so that the parse restarts),
a tentative label that a late `<meta>` overrides, and a byte order mark (UTF-16), and each encoding's
bytes with ill-formed sequences among them.

Each seed is five option bytes and then the bytes to parse (the layout is in tests/fuzz/fuzz_html.cpp:
o0 to o3 are the tree builder's fuzzer's, o4 is the API's, and when o4's top bits are not zero three more bytes,
e0 to e2, choose the encoding world's options). Files named `reproducer-*.seed` are not
touched by this script, so a find can be kept there and replayed by `make test`
(tests/test-html-api-fuzz-seeds.cpp, which replays every `*.seed`) and by every fuzz run. There are none yet.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TREE = os.path.join(ROOT, "tests", "fuzz", "corpus", "html-tree")
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "html")
# The harness returns at once for an input of more than this many bytes in all, the five option bytes included, so a
# seed that is longer would be counted and never run.
MAX_SEED = 4096
OPTION_BYTES = 5


def _min_seeds():
    """The floor the replay test asserts (tests/test-html-api-fuzz-seeds.cpp, kMinSeeds): one number, read from there."""
    with open(os.path.join(ROOT, "tests", "test-html-api-fuzz-seeds.cpp"), encoding="utf-8") as fh:
        m = re.search(r"constexpr size_t kMinSeeds = (\d+);", fh.read())
    if not m:
        sys.exit("kMinSeeds is not in tests/test-html-api-fuzz-seeds.cpp")
    return int(m.group(1))


EXPECTED = (_min_seeds(), 1100)  # the size of the corpus; a selection that drifts out of it is a mistake

# o4: bit 0 count-only, bit 1 a byte order mark in front, bits 2-3 the cap, bit 4 a transport label,
# bit 5 no result struct in the failure sweep, bits 6-7 the world (0: UTF-8; else the encoding world, with three more bytes).
O4_SETS = (0x00, 0x02, 0x04 | 0x10, 0x08 | 0x01, 0x0A, 0x20, 0x0C | 0x02, 0x10)

# (name, o0..o3, text or bytes, o4)
HAND = [
    ("bom-whole", (0, 0, 0, 0), b"\xef\xbb\xbf<!DOCTYPE html><p>x</p>", 0x00),
    ("bom-added", (0, 0, 0, 0), b"<!DOCTYPE html><p>x</p></b>", 0x02),
    ("bom-added-transport", (0, 0, 0, 0), b"<p>x", 0x02 | 0x10),
    ("bom-twice", (0, 0, 0, 0), b"\xef\xbb\xbf\xef\xbb\xbf<p>x", 0x00),
    ("bom-prefix-one", (0, 0, 0, 0), b"\xef", 0x00),
    ("bom-prefix-two", (0, 0, 0, 0), b"\xef\xbb", 0x00),
    ("bom-prefix-then-text", (0, 0, 0, 0), b"\xef\xbb<p>x", 0x00),
    ("bom-prefix-then-a-bom", (0, 0, 0, 0), b"\xef\xbb\xef\xbb\xbf<p>x", 0x00),
    ("bom-cut-byte-at-a-time", (0, 0x00, 1, 0x03), b"\xef\xbb\xbf<!DOCTYPE html><title>t\xc3\xa9</title><p>\xf0\x9f\x98\x80</p>", 0x00),
    ("bom-in-the-middle", (0, 0, 0, 0), b"<p>a\xef\xbb\xbfb", 0x00),
    ("bom-fragment", (2 | (1 << 2), 0, 0, 0), b"\xef\xbb\xbf<p>x", 0x00),
    ("bom-fragment-added", (2, 0, 0, 0), b"<p>x", 0x00),
    ("empty-document", (0, 0, 0, 0), b"", 0x00),
    ("empty-bom", (0, 0, 0, 0), b"", 0x02),
    ("empty-fragment", (2 | (1 << 2), 0, 0, 0), b"", 0x00),
    ("errors-beyond-the-cap", (0, 0, 0, 0), b"</x>" * 400, 0x04),
    ("errors-count-only", (0, 0, 0, 0), b"</x>" * 400, 0x01),
    ("errors-cap-ten", (0, 0, 0, 0), b"<p></b></i></u>" * 60, 0x08),
    ("errors-cap-one", (0, 0, 0, 0), b"&#0;&#xD800;&bogus;&notin " * 40, 0x04),
    ("errors-fragment", (2, 0, 0, 0), b"</x>" * 300, 0x04),
    ("transport-utf-8", (0, 0, 0, 0), b"<!DOCTYPE html><p>x", 0x10),
    ("transport-utf-8-fragment", (2, 0, 0, 0), b"<b>x", 0x10),
    ("limit-depth", (0, 0x10, 0, 0), b"<div>" * 20 + b"x", 0x00),
    ("limit-nodes", (0, 0, 7, 0), b"<p>a<b>b</b><i>c</i></p>" * 20, 0x00),
    ("sticky-deep", (0, 0, 0, 0), b"<div>" * 100, 0x00),
    ("sticky-deep-fragment", (2, 0, 0, 0), b"<div>" * 100, 0x00),
    ("limit-input-one-byte", (0, 0, 0, 0), b"x", 0x00),
    ("invalid-utf-8", (0, 0, 0, 0), b"<p>\xff\xfe\xc0\x80\xed\xa0\x80\xf0\x9f\x98<b>\x00\x01\x7f</b>", 0x00),
    ("rich-document", (1, 0x22, 5, 0x01),
     b"\xef\xbb\xbf<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"u\"><html lang=en><head><title>t&amp;</title>"
     b"<script>x<y</script></head><body class=\"a b\" id=x><p>one<b>two<i>three</p><p>four</b></i>"
     b"<table><tr><td>x<td>y</table><template><tr><td>t</template><svg viewBox=\"0 0 1 1\"><foreignObject><p>f"
     b"</foreignObject><path d=M0/></svg><math><mi>x</mi></math><!-- c --><?pi x?><textarea>\n&lt;</textarea>"
     b"\xc3\xa9 \xf0\x9f\x98\x80 &#x41; &notin; </x></y></z>", 0x00),
    ("rich-document-by-parts", (0, 0x03, 9, 0x01),
     b"<!DOCTYPE html><title>t</title><p class=a id=b>caf\xc3\xa9 &amp; &lt; &#x1F600; <!--c--><br/></p>", 0x04),
    ("rich-document-sweep-bytes", (0, 0x00, 0, 0x03), b"<p>a<b>b</p>c<table><tr><td>x</table>", 0x20),
    ("rich-fragment-sweep", (2 | (1 << 2), 0x00, 0, 0x01), b"<p>x</p>y<!--c--><td>z", 0x00),
    ("bom-errors", (0, 0, 0, 0), b"\xef\xbb\xbf<p>x</b>y<!DOCTYPE html>\n<p>z", 0x00),
    ("huge-attribute", (0, 0, 0, 0), b"<a href=\"" + b"x" * 3000 + b"\">y", 0x00),
    ("huge-text", (0, 0, 0, 0), b"<p>" + b"abc &amp; " * 400, 0x00),
]


# The fuzzer's table of encodings (kEncodings in tests/fuzz/fuzz_html.cpp), with Python's codec for each and a text it can write.
ENCODINGS = (
    ("utf-8", "utf-8", "caf\u00e9 \u65e5\u672c \U0001F600"),
    ("utf-16le", "utf-16-le", "caf\u00e9 \u65e5\u672c \U0001F600"),
    ("utf-16be", "utf-16-be", "caf\u00e9 \u65e5\u672c \U0001F600"),
    ("windows-1252", "cp1252", "caf\u00e9 \u20ac \u00fc"),
    ("iso-8859-2", "iso8859-2", "\u011b\u0161\u010d\u0159"),
    ("koi8-r", "koi8_r", "\u041f\u0440\u0438\u0432\u0435\u0442"),
    ("windows-1251", "cp1251", "\u041f\u0440\u0438\u0432\u0435\u0442"),
    ("shift_jis", "cp932", "\u65e5\u672c\u8a9e\u3042\uff71"),
    ("euc-jp", "euc_jp", "\u65e5\u672c\u8a9e\u3042\uff71"),
    ("iso-2022-jp", "iso2022_jp", "\u65e5\u672c\u8a9e\u3042"),
    ("gbk", "gbk", "\u4e2d\u6587\u6c49\u5b57"),
    ("gb18030", "gb18030", "\u4e2d\u6587\U00010000"),
    ("big5", "big5", "\u4e2d\u6587\u6f22\u5b57"),
    ("euc-kr", "euc_kr", "\ud55c\uad6d\uc5b4"),
    ("x-user-defined", None, None),
    ("hz-gb-2312", None, None),
    ("iso-8859-8-i", "iso8859-8", "\u05e9\u05dc\u05d5\u05dd"),
    ("windows-874", "cp874", "\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35"),
    ("macintosh", "mac_roman", "caf\u00e9"),
    ("ibm866", "cp866", "\u041f\u0440\u0438\u0432\u0435\u0442"),
)


def encoded(index, markup):
    """The markup (a str with @ where the sample goes) in the table's encoding `index`, as bytes."""
    label, codec, sample = ENCODINGS[index]
    if codec is None:
        text = markup.replace("@", "abc")
        if label == "x-user-defined":
            return text.encode("ascii").replace(b"abc", b"\x80\xa0\xff")
        return text.encode("ascii")
    return markup.replace("@", sample).encode(codec)


def encoding_seeds():
    """(name, (o0, o1, o2, o3), o4, (e0, e1, e2), data) for the encoding world."""
    doc = "<!DOCTYPE html><title>@</title><p class=a>@<b>x</b></p><table>@<tr><td>@</td></tr></table><!--@-->"
    out = []
    for i, (label, codec, sample) in enumerate(ENCODINGS):
        body = encoded(i, doc)
        o = (0, 0x03 + (i % 5), i, 0x01 if i % 3 == 0 else 0x00)
        # a certain label
        out.append(("label", i, o, 0x40, (i, 0, 0), body))
        # a meta inside the first 1024 bytes decides (nothing else is announced)
        out.append(("meta-early", i, o, 0x80, (i | (3 << 5), i, 0), body))
        # a meta after the first 1024 bytes restarts the parse
        out.append(("meta-late", i, (0, 0x00, i, 0x03), 0xC0, (i | (3 << 5), i | (3 << 4), 0), body))
        # http-equiv, upper case, and a second meta after the first
        out.append(("meta-late-http-equiv", i, o, 0x40, ((i + 3) % 20 | (1 << 5), i | (2 << 4), 1 | 2 | 4), body))
        # a tentative label that the late meta overrides, with ignore_meta off and on
        out.append(("tentative-overridden", i, o, 0x80, ((i + 7) % 20 | (1 << 5), i | (2 << 4), 2), body))
        out.append(("ignore-meta", i, o, 0x40, (i | (2 << 5) | 128, (i + 1) % 20 | (3 << 4), 0), body))
        # ill-formed bytes among the markup
        bad = b"<p>" + bytes(range(0x80, 0x100)) + b"</p>\xff\xfe<b>\x1b$B\x24\x22\x1b(B</b>"
        out.append(("ill-formed", i, o, 0xC0, (i | (3 << 5) if i % 2 else i, 0, 0), bad))
        # the input ends inside a tag, so that the error at the end of the input has the offset of the end of the bytes
        out.append(("eof", i, o, 0x40, (i, 0, 0), encoded(i, "<p>@<b title=\"@")))
        # multi-byte text with errors in the middle of the run (a null byte, a character reference, a control character, text in a table),
        # so that the places of the characters of a run are seen
        out.append(("run-errors", i, o, 0x40, (i, 0, 0), encoded(i, "<p>@\x00@\x00@&#0;@&bogus;@\x01@</p><table>@x@\x00@</table>@")))
    # byte order marks (e1's top bits: 1 UTF-8, 2 UTF-16LE, 3 UTF-16BE), in front of a document in each of three encodings
    for k, (mark, name) in enumerate(((1, "utf-8"), (2, "utf-16le"), (3, "utf-16be"))):
        for j in (0, 3, 7):
            body = encoded(1 if mark == 2 else 2 if mark == 3 else 0, "<!DOCTYPE html><p>@</p>")
            out.append(("bom-" + name, j, (0, 0x01, j, 0), 0x40, (j | (3 << 5), mark << 6, 0), body))
    return out


def main():
    os.makedirs(CORPUS, exist_ok=True)
    for old in os.listdir(CORPUS):
        if old.endswith(".seed") and not old.startswith("reproducer-"):
            os.unlink(os.path.join(CORPUS, old))
    kept = 0
    trees = sorted(f for f in os.listdir(TREE) if f.endswith(".seed") and not f.startswith("reproducer-"))
    if not trees:
        sys.exit("%s has no seeds: run tools/html/make-tree-fuzz-seeds.py first" % TREE)
    for i, name in enumerate(trees):
        if i % 3 != 0:
            continue
        with open(os.path.join(TREE, name), "rb") as fh:
            raw = fh.read()
        if len(raw) < 4:
            continue
        if len(raw) + 1 > MAX_SEED:  # the tree builder's seed with the fifth option byte is too long for the harness
            continue
        o4 = O4_SETS[int.from_bytes(hashlib.sha256(raw).digest()[:4], "big") % len(O4_SETS)]
        with open(os.path.join(CORPUS, name), "wb") as fh:
            fh.write(raw[:4] + bytes((o4,)) + raw[4:])
        kept += 1
    for name, (o0, o1, o2, o3), data, o4 in HAND:
        if len(data) + OPTION_BYTES > MAX_SEED:
            sys.exit("the seed %s is %d bytes with its option bytes: the harness skips an input over %d" % (name, len(data) + OPTION_BYTES, MAX_SEED))
        with open(os.path.join(CORPUS, "api-%s.seed" % name), "wb") as fh:
            fh.write(bytes((o0, o1, o2, o3, o4)) + data)
        kept += 1
    for what, index, (o0, o1, o2, o3), o4, (e0, e1, e2), data in encoding_seeds():
        name = "api-enc-%s-%s.seed" % (what, ENCODINGS[index][0] if what != "bom-utf-8" and not what.startswith("bom-utf-16") else "%d" % index)
        if len(data) + OPTION_BYTES + 3 > MAX_SEED:
            sys.exit("the seed %s is too long" % name)
        with open(os.path.join(CORPUS, name), "wb") as fh:
            fh.write(bytes((o0, o1, o2, o3, o4, e0, e1, e2)) + data)
        kept += 1
    if not EXPECTED[0] <= kept <= EXPECTED[1]:
        sys.exit("%d seeds written; the documentation says %d to %d" % ((kept,) + EXPECTED))
    print("fuzz_html: %d seeds (every third tree-builder seed, %d of its own, and the encoding world's)" % (kept, len(HAND)))


if __name__ == "__main__":
    main()
