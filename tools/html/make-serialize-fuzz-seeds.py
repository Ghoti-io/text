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
"""Write the seed corpus for fuzz_html_serialize, the HTML serializer's fuzzer.

    tools/html/make-serialize-fuzz-seeds.py

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else) and they are *derived*, the same bytes
each time. Most come from the tree builder's seeds (tests/fuzz/corpus/html-tree, which tools/html/make-tree-fuzz-seeds.py made
from the pinned web-platform-tests vectors and the hand-chosen extras): every third one is kept, with the document or fragment
choice and the scripting flag of its first byte, and with the other five option bytes taken from one of fourteen profiles chosen
by a hash of the seed, so that adding or removing a seed changes no other file. The profiles between them turn on every option
of the serializer, each entry point of the failure sweep, the sink that fails, both limits, the outer scope, and the two option
combinations that are refused, and four of the eighteen write in an output encoding (o2's top bits, with o4's bits 3 to 7 choosing which of
the twenty). The rest are the seeds below, which hold what the vectors do not: a no-break space in text and
attributes, a text of thousands of ampersands (the sink fails in the middle of a walk), templates inside templates, foreign
attributes, raw text elements, a tree of every nesting the limits are about.

Each seed is six option bytes and then the bytes to parse (the layout is in tests/fuzz/fuzz_html_serialize.cpp). Files named
`reproducer-*.seed` are not touched by this script, so a find can be kept there and replayed by `make test`
(tests/test-html-serialize-fuzz-seeds.cpp, which replays every `*.seed`) and by every fuzz run. There are none yet.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TREE = os.path.join(ROOT, "tests", "fuzz", "corpus", "html-tree")
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "html-serialize")
MAX_SEED = 4096
OPTION_BYTES = 6


def _min_seeds():
    with open(os.path.join(ROOT, "tests", "test-html-serialize-fuzz-seeds.cpp"), encoding="utf-8") as fh:
        m = re.search(r"constexpr size_t kMinSeeds = (\d+);", fh.read())
    if not m:
        sys.exit("kMinSeeds is not in tests/test-html-serialize-fuzz-seeds.cpp")
    return int(m.group(1))


EXPECTED = (_min_seeds(), 1100)

# (o1, o2, o3, o4, o5)
#   o1: omit 1, when-needed 2, best 4, solidus 8, space 16, minimize 32, alphabetical 64, strip 128
#   o2: escape_rcdata 1, doctype ids 2, inject 4, outer 8, quote char (default 0, `"` 16, `'` 32), an output encoding (bits 6-7)
#   o3: the sink call that fails (low nibble), the size of the pieces of text for the token stream (high nibble)
#   o4: failure sweeps: sink 1, buffer 2, token stream 4; bits 3-7: which output encoding
#   o5: the output limit (bit 7 on; bits 0-2: the length, one less, half, one more, small ones)
PROFILES = (
    (0x00, 0x00, 0x00, 0x00, 0x00),
    (0x01, 0x00, 0x30, 0x00, 0x00),                   # optional tags, text fed in pieces
    (0x02 | 0x04, 0x00, 0x00, 0x00, 0x00),            # quotes when needed, the best one
    (0x08 | 0x10, 0x00, 0x00, 0x00, 0x00),            # <br />
    (0x08 | 0x02, 0x00, 0x00, 0x00, 0x00),            # <br/> and a value that must be quoted before it
    (0x20 | 0x40, 0x00, 0x00, 0x00, 0x00),            # minimized booleans, sorted attributes
    (0x80, 0x00, 0x50, 0x00, 0x00),                   # white space collapsed
    (0x00, 0x01 | 0x02, 0x00, 0x00, 0x00),            # raw text escaped, identifiers of the document type
    (0x01 | 0x40, 0x04, 0x00, 0x00, 0x00),            # inject a meta charset under the optional tags
    (0x00, 0x08, 0x00, 0x00, 0x00),                   # the first child itself
    (0xFF & ~0x10, 0x07, 0x21, 0x07, 0x00),           # everything on, every failure sweep, a failing sink
    (0x00, 0x00, 0x00, 0x00, 0x80),                   # the output limit at the length
    (0x00, 0x00, 0x00, 0x00, 0x81),                   # ... and one less
    (0x02, 0x20, 0x01, 0x03, 0x83),                   # the quote character, a failing sink, the sweeps, the output limit
    (0x01, 0x40 | 0x04, 0x00, 0x10, 0x00),            # an output encoding, the optional tags and the meta it declares
    (0x00, 0x80, 0x31, 0x20 | 0x07, 0x00),            # another, the sweeps, text fed in pieces
    (0x08, 0xC0, 0x00, 0x30, 0x83),                   # another, <br/>, the output limit
    (0x00, 0x40 | 0x08, 0x00, 0x40 | 0x01, 0x00),     # another, the first child itself, a failing sink
)

# The output encodings (kOutEncodings in tests/fuzz/fuzz_html_serialize.cpp) in the order o4 >> 3 and o2 >> 6 choose them.
OUT = ("iso-8859-2", "windows-1252", "shift_jis", "euc-jp", "iso-2022-jp", "gbk", "gb18030", "big5", "euc-kr", "utf-16le", "utf-16be",
       "koi8-r", "windows-1251", "x-user-defined", "hz-gb-2312", "windows-874", "ibm866", "macintosh", "iso-8859-8", "utf-8")

# (name, o0, o1..o5, data)
HAND = [
    ("plain-defaults", 0, (0, 0, 0, 0, 0), b"<p class=a title='x\"y'>one &amp; two<br><b>three</p>"),
    ("nbsp", 0, (0, 0, 0, 0, 0), b"<p>a&nbsp;b<a title=\"&nbsp;\">x</a></p>"),
    ("one-character-of-text", 0, (0, 0, 0, 0, 0), b"<p>x</p>"),
    ("ampersands-sink-fails-midway", 0, (0, 0, 0x01, 0, 0), b"<p>" + b"& " * 1000),
    ("template", 0, (0, 0, 0, 0, 0), b"<template><p>a<template><b>x</template></template>"),
    ("svg-xlink", 0, (0, 0, 0, 0, 0), b"<svg xmlns:xlink=\"http://www.w3.org/1999/xlink\"><a xlink:href=u xml:lang=en></a></svg>"),
    ("raw-text", 0, (0, 0, 0, 0, 0), b"<script>a<b&c</script><style>x>y</style><xmp>&</xmp><textarea>&lt;</textarea>"),
    ("noscript-scripting", 1, (0, 0, 0, 0, 0), b"<noscript><p>a&lt;</p></noscript>"),
    ("noscript-no-scripting", 0, (0, 0, 0, 0, 0), b"<noscript><p>a&lt;</p></noscript>"),
    ("nested", 0, (0, 0, 0, 0, 0x81), b"<div><div><div>x</div></div></div>"),
    ("limits-exact", 0, (0, 0, 0, 0, 0x80), b"<div><p>a</p><p>b</p></div>"),
    ("alloc-sweep-all-forms", 0, (0x01 | 0x40 | 0x20, 0x04, 0, 0x07, 0),
     b"<!DOCTYPE html><html><head><meta name=x><title>t</title></head><body><table><tr><td>a<td b=1 a=2>c</table><ul><li>x<li>y</ul></body></html>"),
    ("refused-space-without-solidus", 0, (0x10, 0, 0, 0, 0), b"<p>x"),
    ("refused-best-with-quote-char", 0, (0x04, 0x10, 0, 0, 0), b"<p title=x>x"),
    ("optional-tags", 0, (0x01, 0, 0, 0, 0), b"<!DOCTYPE html><html><head><title>t</title></head><body><ul><li>a<li>b</ul><p>c<p>d<table><caption>x<colgroup><col><tbody><tr><td>1<td>2</table>"),
    ("inject", 0, (0, 0x04, 0, 0, 0), b"<head><meta charset=ascii><title>t</title></head>"),
    ("inject-none", 0, (0, 0x04, 0, 0, 0), b"<head><title>t</title></head>"),
    ("outer", 0, (0, 0x08, 0, 0, 0), b"<div class=a><br>t</div>"),
    ("fragment-td", 2 | (1 << 2), (0, 0, 0, 0, 0), b"<b>x<td>y"),
    ("fragment-table", 2 | (3 << 2), (0x01, 0, 0, 0, 0), b"<tbody><tr><td>a<td>b<tr><td>c"),
    ("fragment-select", 2 | (4 << 2), (0x01, 0, 0, 0, 0), b"<option>a<option>b<optgroup label=g><option>c"),
    ("fragment-template", 2 | (9 << 2), (0, 0, 0, 0, 0), b"<p>x<template><i>y</template>"),
    ("fragment-svg", 2 | (12 << 2), (0, 0, 0, 0, 0), b"<a xlink:href=x>t</a><style>a<b</style>"),
    ("pre-newline", 0, (0, 0, 0, 0, 0), b"<pre>\n\nx</pre><textarea>\n\ny</textarea><listing>\n\nz</listing>"),
    ("doctype-identifiers", 0, (0, 0x02, 0, 0, 0), b"<!DOCTYPE html PUBLIC \"a\" \"b\"><p>"),
    ("comments", 0, (0, 0, 0, 0, 0), b"<!--a--!><!-- -- --><!---><p><!--x-->"),
    ("deep", 0, (0, 0, 0, 0, 0x82), b"<div>" * 300 + b"x"),
    ("white-space", 0, (0x80, 0, 0x30, 0, 0), b"<p>  a \n b <pre>  x </pre><textarea> y  z </textarea><script> s  t </script>"),
    ("unquoted-before-a-solidus", 0, (0x02 | 0x08, 0, 0, 0, 0), b"<br class=a><img src=b alt=\"\"><input disabled>"),
    ("many-attributes-sorted", 0, (0x40, 0, 0, 0x01, 0),
     b"<div " + b" ".join(b"a%d=%d" % (i, i) for i in range(30, 0, -1)) + b">x</div>"),
    ("boolean-attributes", 0, (0x20, 0, 0, 0, 0), b"<input checked=checked disabled=x readonly><button disabled=disabled>"),
    ("quote-apostrophe", 0, (0, 0x20, 0, 0, 0), b"<p title=\"it's\" lang='a\"b'>x"),
    ("best-quote", 0, (0x04, 0, 0, 0, 0), b"<p title=\"it's\" lang='a\"b' x='a\"b&#39;c'>x"),
    ("escape-rcdata", 0, (0, 0x01, 0, 0, 0), b"<script>a<b&c</script><plaintext>x<y"),
    ("plaintext", 0, (0, 0, 0, 0, 0), b"<plaintext>a<b&c"),
    ("foreign-names", 0, (0, 0, 0, 0, 0), b"<svg><foreignObject><p>x</foreignObject><clipPath viewBox=1></clipPath></svg><math><mi definitionURL=x>y</mi></math>"),
    ("empty", 0, (0, 0, 0, 0, 0), b""),
    ("only-text", 0, (0x01, 0, 0, 0, 0), b"just text"),
]

# The encoded seeds: (name, encoding index, o1, extra o2 bits, data). o2's top bits are 01; o4's bits 3-7 are (index - 1) so that
# (o4 >> 3) + (o2 >> 6) names the index.
ENC_DOCS = {
    "references": "<p title=\"\u20ac \u65e5\u672c\">\u20ac \u65e5\u672c \U0001F600 &amp; \u00e9</p><table>\u20ac<tr><td>\u65e5</td></tr></table><textarea>\u20ac</textarea>",
    "errors-in-raw-text": "<p>ok</p><script>var s = '\u20ac';</script><p>after</p>",
    "errors-in-a-comment": "<p>\u65e5</p><!-- \u20ac --><p>x</p>",
    "errors-in-a-name": "<p>x</p><\u65e5-tag class=a>y</\u65e5-tag>",
    "errors-in-an-attribute-name": "<p \u20ac=1>x</p>",
    "errors-in-a-doctype": "<!DOCTYPE \u20ac><p>x",
    "representable": "<p class=a>caf\u00e9</p>",
    "iso-2022-jp": "<p>\u65e5\u672c\u20ac\u65e5</p><b>x</b>\u65e5",
    "style-and-xmp": "<style>a { content: '\u65e5' }</style><xmp>\u20ac</xmp><iframe>\u00e9</iframe>",
}


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
        if i % 3 != 1:
            continue
        with open(os.path.join(TREE, name), "rb") as fh:
            raw = fh.read()
        if len(raw) < 4 or len(raw) - 4 + OPTION_BYTES > MAX_SEED:
            continue
        profile = PROFILES[int.from_bytes(hashlib.sha256(raw).digest()[:4], "big") % len(PROFILES)]
        # the tree builder's first byte: bit 0 scripting, bit 1 fragment, bits 2-7 the context (it has 64, this has 16)
        o0 = (raw[0] & 3) | (((raw[0] >> 2) & 15) << 2)
        with open(os.path.join(CORPUS, name), "wb") as fh:
            fh.write(bytes((o0,) + profile) + raw[4:])
        kept += 1
    for name, o0, opts, data in HAND:
        if len(data) + OPTION_BYTES > MAX_SEED:
            sys.exit("the seed %s is %d bytes with its option bytes: the harness skips an input over %d" % (name, len(data) + OPTION_BYTES, MAX_SEED))
        with open(os.path.join(CORPUS, "ser-%s.seed" % name), "wb") as fh:
            fh.write(bytes((o0,) + opts) + data)
        kept += 1
    for what, text in sorted(ENC_DOCS.items()):
        for index, label in enumerate(OUT):
            if label == "utf-8" and what != "references":
                continue
            o2 = 0x40 | (0x04 if index % 5 == 0 else 0)
            o4 = ((index - 1) % 20) << 3
            o1 = (0x01 if index % 4 == 0 else 0) | (0x02 if index % 3 == 0 else 0)
            o3 = 0x01 if index % 7 == 0 else 0x00
            o5 = 0x80 | (index % 5) if index % 6 == 0 else 0
            name = "ser-enc-%s-%s.seed" % (what, label)
            data = text.encode("utf-8")
            if len(data) + OPTION_BYTES > MAX_SEED:
                sys.exit("the seed %s is too long" % name)
            with open(os.path.join(CORPUS, name), "wb") as fh:
                fh.write(bytes((0, o1, o2, o3, o4, o5)) + data)
            kept += 1
    if not EXPECTED[0] <= kept <= EXPECTED[1]:
        sys.exit("%d seeds written; the documentation says %d to %d" % ((kept,) + EXPECTED))
    print("fuzz_html_serialize: %d seeds (every third tree-builder seed, and %d of its own)" % (kept, len(HAND)))


if __name__ == "__main__":
    main()
