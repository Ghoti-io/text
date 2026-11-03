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
"""The pinned list of the pages of web-platform-tests that touch the CSSOM and are ported by hand.

    css_cssom_handports.py WPT_DIR [--vectors DIR] [--controls]

tools/conformance/CSS_CSSOM_HANDPORTS has a row for each page (page, test program, tests ported, the sentence the page tests). The gate fails on a page that is not in the pin, a test
program that does not name the page, a missing sentence, the number of pages or of tests below the floors (CSS_CSSOM_MIN_HANDPORT_PAGES, CSS_CSSOM_MIN_HANDPORT_TESTS: they only rise)
and, with --vectors, a page that is on the list and is also scored by the op interpreter (a page is ported by hand because the interpreter cannot read it: when it can, the page leaves the list
and its tests are scored once). --controls makes each mistake in a copy of the list and requires the report.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LIST = os.path.join(HERE, "CSS_CSSOM_HANDPORTS")


def check(path, wpt, vectors, min_pages, min_tests):
    bad = []
    pages = 0
    tests = 0
    scored = set()
    if vectors:
        decl = os.path.join(vectors, "declaration.tsv")
        if os.path.exists(decl):
            for line in open(decl, encoding="utf-8", errors="replace"):
                if line.startswith("P\t"):
                    scored.add(line.split("\t")[1])
    seen = set()
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 4:
            bad.append("line %d has %d fields, not four" % (n, len(f)))
            continue
        page, prog, count, sentence = f
        if page in seen:
            bad.append("%s is listed twice" % page)
        seen.add(page)
        if not os.path.exists(os.path.join(wpt, page)):
            bad.append("%s is not in the pinned web-platform-tests" % page)
        pp = os.path.join(ROOT, "tests", prog)
        if not os.path.exists(pp):
            bad.append("%s: the program %s does not exist" % (page, prog))
        else:
            text = open(pp, encoding="utf-8").read()
            stem = os.path.basename(page)[:-5] if page.endswith(".html") else os.path.basename(page)
            keys = [stem, stem.rsplit("-", 1)[0], stem.split("-", 1)[-1]]
            if not any(k and k in text for k in keys):
                bad.append("%s: the program %s does not name it" % (page, prog))
        if not count.isdigit() or int(count) < 1:
            bad.append("%s: the count %r is not a positive number" % (page, count))
        else:
            tests += int(count)
        if len(sentence.split()) < 3:
            bad.append("%s: the sentence is missing" % page)
        if page in scored:
            bad.append("%s is scored by the op interpreter and listed as ported by hand" % page)
        pages += 1
    if pages < min_pages:
        bad.append("%d pages are listed, the floor is %d" % (pages, min_pages))
    if tests < min_tests:
        bad.append("%d tests are ported, the floor is %d" % (tests, min_tests))
    return bad, pages, tests


def main():
    args = sys.argv[1:]
    wpt = args.pop(0)
    vectors = None
    controls = False
    while args:
        a = args.pop(0)
        if a == "--vectors":
            vectors = args.pop(0)
        elif a == "--controls":
            controls = True
    min_pages = int(os.environ.get("CSS_CSSOM_MIN_HANDPORT_PAGES", "28"))
    min_tests = int(os.environ.get("CSS_CSSOM_MIN_HANDPORT_TESTS", "64"))
    bad, pages, tests = check(LIST, wpt, vectors, min_pages, min_tests)
    for b in bad:
        print("HANDPORTS " + b)
    if bad:
        return 1
    print("hand-ported pages: %d pages, %d tests (floors %d and %d)" % (pages, tests, min_pages, min_tests))
    if not controls:
        return 0
    lines = open(LIST, encoding="utf-8").read().split("\n")
    first = next(i for i, l in enumerate(lines) if l and not l.startswith("#"))
    row = lines[first].split("\t")
    cases = {
        "a page that is not in the pin": ("not in the pinned", lambda L: L + ["css/cssom/no-such-page.html\t%s\t1\tsome sentence of the served text" % row[1]]),
        "a program that does not name the page": ("does not name", lambda L: L + ["css/cssom/HTMLLinkElement-disabled-001.html\t%s\t1\tsome sentence of the served text" % row[1]]),
        "a missing sentence": ("sentence", lambda L: L[:first] + ["\t".join(row[:3] + ["x"])] + L[first + 1:]),
        "a page twice": ("twice", lambda L: L + [L[first]]),
        "a page dropped (below the floor)": ("floor", lambda L: L[:first] + L[first + 1:]),
    }
    for what, (want, edit) in cases.items():
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as t:
            t.write("\n".join(edit(lines)))
            name = t.name
        try:
            found, _, _ = check(name, wpt, vectors, min_pages, min_tests)
        finally:
            os.unlink(name)
        if not any(want in b for b in found):
            print("CONTROL %s: not reported (%r)" % (what, found[:2]))
            return 1
        print("control %s: caught" % what)
    return 0


if __name__ == "__main__":
    sys.exit(main())
