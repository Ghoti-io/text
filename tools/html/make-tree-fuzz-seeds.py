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
"""Write the seed corpus for fuzz_html_tree.

    tools/html/make-tree-fuzz-seeds.py

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else) and they
are *derived*: this script reads the web-platform-tests tree-construction vectors under
third_party/html/wpt (fetch them with tools/html/fetch.sh), the hand-chosen cases of
tools/conformance/html_tree_extra.dat and the lists below, and writes them again, the same
bytes each time. Run it after a change to the selection and commit the result. Files named
`reproducer-*.seed` are not touched: they are the recorded finds, replayed by `make test`
(tests/test-html-tree-fuzz-seeds.cpp) and by every fuzz run.

Each seed is four option bytes and then the bytes to parse (the layout is in
tests/fuzz/fuzz_html_tree.cpp): o0 bit 0 is the scripting flag, o1 the size of the pieces
(low bits) and the open-element limit (high bits), o2 the seed of the random cut, o3 the cut
and the allocator-failure modes. The vectors that stop at a construct of a later stage are
seeds too (the harness checks that they stop cleanly); the option bytes are one of a few sets
chosen by a hash of the input, so that adding or removing a vector changes no other file.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "conformance"))
import html_tree_suite as suite  # noqa: E402

WPT = os.path.join(ROOT, "third_party", "html", "wpt", "html", "syntax", "parsing", "resources")
EXTRA = os.path.join(ROOT, "tools", "conformance", "html_tree_extra.dat")
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "html-tree")
MAX_SEED = 4096
EXPECTED = (1500, 2100)  # the documented size of the corpus; a selection that drifts out of it is a mistake

OPTIONS = (
    (0x00, 0x00, 0x00, 0x00),  # scripting off, 64 open, whole, no failure modes
    (0x01, 0x21, 0x05, 0x01),  # scripting on, pieces of 2, tiny depth
    (0x00, 0x53, 0x11, 0x02),  # a byte at a time in the failure sweep
    (0x01, 0x07, 0x22, 0x03),
    (0x00, 0xA5, 0x33, 0x00),
)

EXTRAS = [
    # A deep run of divs and one of formatting elements: the limits and the work bound.
    ("deep-div", "<div>" * 120 + "x"),
    ("deep-formatting", "<b><i>" * 80 + "x</b>y"),
    ("many-attributes", "<div " + " ".join("a%d=v" % i for i in range(300)) + ">x"),
    ("html-attributes", "<html" + "".join(" h%d=v" % i for i in range(100)) + ">" + "<html a=b>" * 50),
    ("many-paragraphs", "<span>" * 60 + "<p>x" * 200),
    ("formatting-noahs-ark", "<p><i a=1><i a=1><i a=1><i a=1><i a=2></p>x" * 20),
    ("adoption-nine-blocks", "<a>" + "<div>" * 9 + "x</a>y"),
    ("misnested-everything", "<b><i><u><s><em><strong><small><big><font><tt><code>x</b></i></u></s></em>y</p>z"),
    ("text-in-pieces", "<p>" + "a</x>" * 300),
    ("frameset-then-more", "<frameset><frame>x<frameset><noframes>n</noframes></frameset> </frameset><!--c--></html>q"),
    ("odd-names", "<a$b><x:y c\"d=1 e<f=2 \u00e9=3><\u00e9>\x01\ufffe<!--\x01-->\ufdd0<!DOCTYPE a$b \u00e9>x"),
    ("custom-elements", "".join("<c-%d>%s" % (i % 230, "</c-%d>" % (i % 7) if i % 5 == 0 else "x") for i in range(400))),
    ("stray-end-tags", "</p></div></b></html></body></head>x</br>" * 5),
]


def main():
    if not os.path.isdir(WPT):
        sys.exit("%s is not here; run tools/html/fetch.sh" % WPT)
    os.makedirs(CORPUS, exist_ok=True)
    for old in os.listdir(CORPUS):
        if old.endswith(".seed") and not old.startswith("reproducer-"):
            os.unlink(os.path.join(CORPUS, old))
    vectors, _, _ = suite.load(WPT)
    vectors += suite.parse_dat(EXTRA, "html_tree_extra.dat")
    kept = 0
    seen = set()
    for v in vectors:
        if v["fragment"]:
            continue
        raw = v["data"].encode("utf-8", "surrogatepass")
        if len(raw) > MAX_SEED or raw in seen:
            continue
        seen.add(raw)
        o0, o1, o2, o3 = OPTIONS[int.from_bytes(hashlib.sha256(raw).digest()[:4], "big") % len(OPTIONS)]
        if v["scripting"]:
            o0 |= 1
        stem, n = v["id"].split("#")
        path = os.path.join(CORPUS, "%s-%04d.seed" % (os.path.splitext(stem)[0], int(n)))
        with open(path, "wb") as fh:
            fh.write(bytes((o0, o1, o2, o3)) + raw)
        kept += 1
    for name, text in EXTRAS:
        if len(text.encode()) > MAX_SEED:
            sys.exit("the extra seed %s is %d bytes: the harness skips an input over %d" % (name, len(text.encode()), MAX_SEED))
        with open(os.path.join(CORPUS, "%s.seed" % name), "wb") as fh:
            fh.write(bytes((0, 0, 0, 0)) + text.encode())
        kept += 1
    if not EXPECTED[0] <= kept <= EXPECTED[1]:
        sys.exit("%d seeds written; the documentation says %d to %d" % ((kept,) + EXPECTED))
    print("fuzz_html_tree: %d seeds from the vectors" % kept)


if __name__ == "__main__":
    main()
