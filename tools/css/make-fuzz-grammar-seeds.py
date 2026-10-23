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
"""Write the seed corpus of tests/fuzz/fuzz_css_grammar.cpp: tests/fuzz/corpus/css-grammar/.

    tools/css/make-fuzz-grammar-seeds.py

A seed is three option bytes, a grammar, a NUL and the CSS to match (see the harness). The seeds are the grammars of
tools/css/grammar-corpus.txt that name only native types and the types the harness's resolver has (t, lp, f(), the
property p), each with every witness and counter-witness it lists; a list of the notation's corner cases; and the
same inputs with the options that turn on a tiny definition limit and with an allocation to fail.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "tests", "fuzz", "corpus", "css-grammar")
KNOWN = {"t", "lp", "f()"}
NATIVE = set("""integer number percentage dimension length angle time frequency resolution flex ratio zero string url ident
custom-ident dashed-ident any-value declaration-value urange an+b declaration-list rule-list ident-token function-token
at-keyword-token hash-token string-token bad-string-token url-token delim-token number-token percentage-token
dimension-token whitespace-token CDO-token CDC-token colon-token semicolon-token comma-token [-token ]-token (-token
)-token {-token }-token""".split())

OWN = [
    ("a b c", "a b c"), ("a | b | c", "b"), ("a && b && c", "c a b"), ("a || b || c", "b"), ("[ a | b ]*", "a b b a"),
    ("a? b* c+", "c"), ("a#", "a, a"), ("a{2,3}", "a a a"), ("[ a? b? ]!", "b"), ("<number [0,1]>", "0.5"),
    ("<length [0in,1in]>", "96px"), ("foo( a | b )", "foo(b)"), ("{ <declaration-list> }", "{ a: b }"),
    ("( <number> )", "(1)"), ("a , b", "a, b"), ("a / b", "a / b"), ("<t>", "a a a"), ("<lp>#", "1px, 2%"),
    ("<f()>", "f(1, 2)"), ("<'p'>", "1px 2%"), ("<length>", "calc(1px + 2px)"), ("<length>", "var(--a)"),
    ("<any-value>", "a (b) [c]"), ("<ratio>", "16 / 9"), ("<url>", "url(a) "), ("<an+b>", "2n+1"),
    ("[ [ a? b? ]* ]+", "a a a a a a a a"), ("a* a* a* b", "a a a a b"), ("[ a | a a ]+ b", "a a a a a"),
    ("<x>", "a"), ("<t> <t>", "a"), ("a b |", "a b"), ("[", ""), ("<number [1,0]>", "1"), ("a{0}", ""), ("a b c d e f g h", "a b c d e f g h"),
]


def seed(name, o0, o1, o2, grammar, css):
    data = bytes([o0, o1, o2]) + grammar.encode("utf-8") + b"\0" + css.encode("utf-8")
    with open(os.path.join(OUT, name + ".seed"), "wb") as f:
        f.write(data)


def refs_ok(text):
    for m in re.finditer(r"<('?)([^<>\s\[]+)'?[^<>]*>", text):
        quoted, name = m.group(1), m.group(2)
        if quoted:
            if name != "p":
                return False
        elif name not in KNOWN and name not in NATIVE:
            return False
    return True


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".seed") and not f.startswith("reproducer-"):
            os.remove(os.path.join(OUT, f))
    count = 0
    entries = []
    grammar = None
    for line in open(os.path.join(HERE, "grammar-corpus.txt"), encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("grammar "):
            grammar = line[8:].strip()
        elif line[:1] in "+-" and grammar is not None:
            body = line[1:].split(" => ")[0].strip()
            if body.startswith("@decl "):
                body = body[6:].split(": ", 1)[-1]
            if refs_ok(grammar):
                entries.append((grammar, body))
    entries += OWN
    for i, (g, c) in enumerate(entries):
        seed("corpus-%04d" % i, 0, 0, 0, g, c)
        count += 1
        if i % 3 == 0:
            seed("oom-%04d" % i, 0, 0, 1 + (i * 7) % 40, g, c)
            seed("quirk-%04d" % i, 1, 0, 0, g, c)
            count += 2
        if i % 5 == 0:
            seed("limit-%04d" % i, 2, 1 | (4 << 2), 0, g, c)
            seed("limit-depth-%04d" % i, 2, 2 | (2 << 2), 0, g, c)
            seed("limit-nodes-%04d" % i, 2, 3 | (6 << 2), 0, g, c)
            count += 3
    # named seeds the planted defects of tools/css/plant-fuzz.py are replayed from
    seed("own-completes-under-the-fault-allocator", 0, 0, 250, "[ a | b c ]+ foo( <number>#{1,2} ) <lp>?", "a b c a foo(1, 2) 1px")
    seed("own-bounds", 0, 0, 0, "[ <lp> ]* | [ a | a b ]+ c", "a b a b c")
    seed("own-hook", 4, 0, 0, "<lp>#", "1px, 2%")
    count += 3
    print("%d seeds in %s" % (count, OUT))


if __name__ == "__main__":
    main()
