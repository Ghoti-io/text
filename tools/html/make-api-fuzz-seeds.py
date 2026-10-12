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
errors, a UTF-8 transport label, the failure sweep without a result struct, each refused label). The rest
are the seeds below, which hold what the vectors do not: a byte order mark whole, cut and half-formed,
inputs with more parse errors than the cap, the empty input, the largest runs the API's limits are about.

Each seed is five option bytes and then the bytes to parse (the layout is in tests/fuzz/fuzz_html.cpp:
o0 to o3 are the tree builder's fuzzer's, o4 is the API's). Files named `reproducer-*.seed` are not
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
# bit 5 no result struct in the failure sweep, bits 6-7 which label is refused.
O4_SETS = (0x00, 0x02, 0x04 | 0x10, 0x08 | 0x01, 0x0A | 0x40, 0x20 | 0x80, 0x0C | 0x02 | 0xC0, 0x10 | 0x40)

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
    ("refused-windows-1252", (0, 0, 0, 0), b"<p>x", 0x00),
    ("refused-utf-16", (0, 0, 0, 0), b"<p>x", 0x40),
    ("refused-empty", (0, 0, 0, 0), b"<p>x", 0x80),
    ("refused-utf-8x", (0, 0, 0, 0), b"<p>x", 0xC0),
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
    ("huge-attribute", (0, 0, 0, 0), b"<a href=\"" + b"x" * 3000 + b"\">y", 0x00),
    ("huge-text", (0, 0, 0, 0), b"<p>" + b"abc &amp; " * 400, 0x00),
]


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
    if not EXPECTED[0] <= kept <= EXPECTED[1]:
        sys.exit("%d seeds written; the documentation says %d to %d" % ((kept,) + EXPECTED))
    print("fuzz_html: %d seeds (every third tree-builder seed, and %d of its own)" % (kept, len(HAND)))


if __name__ == "__main__":
    main()
