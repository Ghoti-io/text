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
"""Write the seed corpus for fuzz_html_tokenizer.

    tools/html/make-tokenizer-fuzz-seeds.py

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else), and
they are *derived*: this script reads the html5lib-tests tokenizer vectors under
third_party/html/html5lib-tests (fetch them with tools/html/fetch.sh) and writes them
again, the same bytes each time. Run it after a change to the selection and commit the
result. Files named `reproducer-*.seed` are not touched: they are the recorded finds,
each one the input that made the fuzzer fail before the fix, replayed by `make test`
(tests/test-html-tokenizer-fuzz-seeds.cpp) and by every fuzz run.

Each seed is five option bytes and then the bytes to tokenize (the layout is in
tests/fuzz/fuzz_html_tokenizer.cpp). The input is a vector's input, run in each of its
initial states: the state is in the options (o3 bits 3-5), a vector's `lastStartTag` is
the fifth option byte (the harness's table of names; a name it has not got is none), and a vector with a lone surrogate
has the WTF-8 bit set. The rest of the options are one of a few sets chosen by a hash of
the input, so that adding or removing a vector changes no other file: the defaults, tiny
limits, the allocator-failure runs, an input limit under the input's length, and the
tree builder's answer that a CDATA section is allowed.

Every vector's input is a seed except the generated entity tables: of the named-reference
vectors (4,210, each a name) every tenth, of the numeric ones (336) every third. They teach
the search one thing 4,210 times.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "conformance"))
from html_tokenizer_lib import unescape  # noqa: E402

VECTORS = os.path.join(ROOT, "third_party", "html", "html5lib-tests", "tokenizer")
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "html-tokenizer")
MAX_SEED = 4096  # libFuzzer's default -max_len

# Option bytes (o1, o2, o3 without the state, o4) for fuzz_html_tokenizer, chosen by a hash of
# the input. The state is OR-ed into o3.
OPTIONS = (
    (0x00, 0x00, 0x00, 0x00),  # default limits, none of the extra modes
    (0x00, 0x00, 0x40, 0x00),  # the allocator-failure runs
    (0x25, 0x24, 0x00, 0x03),  # tiny limits: a name of 2, a value of 4, a comment of 1, a text run of 1
    (0x2D, 0x2D, 0x44, 0x05),  # tiny limits and the allocator-failure runs
    (0x00, 0x00, 0x80, 0x11),  # an input limit under the input's length, pieces of 3
    (0x80, 0x00, 0x04, 0x02),  # the tree builder allows CDATA, errors kept: 1
    (0x00, 0x40, 0x03, 0x00),  # 2 attributes at most
)
LAST_INDEX = {None: 0, "title": 1, "textarea": 2, "style": 3, "xmp": 4, "iframe": 5, "noembed": 6,
              "noframes": 7, "noscript": 8, "script": 9, "plaintext": 10, "a": 11}
STATE_INDEX = {"Data state": 0, "RCDATA state": 1, "RAWTEXT state": 2, "Script data state": 3,
               "PLAINTEXT state": 4, "CDATA section state": 5}

EXPECTED = (2000, 3600)


def main():
    if not os.path.isdir(VECTORS):
        sys.exit("%s is not here; run tools/html/fetch.sh" % VECTORS)
    os.makedirs(CORPUS, exist_ok=True)
    for old in os.listdir(CORPUS):
        if old.endswith(".seed") and not old.startswith("reproducer-"):
            os.unlink(os.path.join(CORPUS, old))
    kept = 0
    seen = set()
    for name in sorted(os.listdir(VECTORS)):
        if not name.endswith(".test"):
            continue
        with open(os.path.join(VECTORS, name), encoding="utf-8") as fh:
            data = json.load(fh)
        stride = 10 if name == "namedEntities.test" else 3 if name == "numericEntities.test" else 1
        for index, t in enumerate(data.get("tests", data.get("xmlViolationTests", []))):
            if index % stride:
                continue
            text = t["input"]
            if t.get("doubleEscaped"):
                text = unescape(text)
            raw = text.encode("utf-8", "surrogatepass")
            if len(raw) > MAX_SEED:
                continue
            for state in t.get("initialStates", ["Data state"]):
                key = (raw, state)
                if key in seen:
                    continue
                seen.add(key)
                o1, o2, o3, o4 = OPTIONS[int.from_bytes(hashlib.sha256(raw + state.encode()).digest()[:4], "big") % len(OPTIONS)]
                if any(0xD800 <= ord(c) <= 0xDFFF for c in text):
                    o1 |= 0x40
                o3 = (o3 & ~0x38) | (STATE_INDEX[state] << 3)
                stem = os.path.splitext(name)[0]
                slug = re.sub(r"[^A-Za-z0-9]", "", state.split()[0].lower())
                path = os.path.join(CORPUS, "%s-%04d-%s.seed" % (stem, index, slug))
                with open(path, "wb") as fh:
                    fh.write(bytes((o1, o2, o3, o4, LAST_INDEX.get(t.get("lastStartTag"), 0))) + raw)
                kept += 1
    # A tag of 900 attributes, which the search would take a long while to build: the duplicate
    # table's lookups against a scan, and the work bound that tells them apart.
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    many = ("<a " + " ".join(digits[i // 36] + digits[i % 36] for i in range(900)) + ">").encode()
    with open(os.path.join(CORPUS, "many-attributes.seed"), "wb") as fh:
        fh.write(bytes((0, 0, 0, 0, 0)) + many)
    kept += 1
    if not EXPECTED[0] <= kept <= EXPECTED[1]:
        sys.exit("%d seeds written; the documentation says %d to %d" % ((kept,) + EXPECTED))
    print("fuzz_html_tokenizer: %d seeds from the vectors" % kept)


if __name__ == "__main__":
    main()
