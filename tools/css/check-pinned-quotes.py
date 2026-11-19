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
"""Fail when a sentence that the sources and the documentation quote from SVG 2 or CSS Transforms 1 is not in the pinned page, or is no longer where it is quoted.

    tools/css/check-pinned-quotes.py [--control]

The presentation attributes are read by rules that rest on a few sentences: how a number becomes a length or an angle, what an attribute that does not parse is, how the
transform attributes are written. Those sentences are quoted in comments and in the manual, and a quotation that is not the text is worse than none. Each entry of QUOTES names a pinned page
(third_party/css/svg2/*.html, pinned by tools/css/SVG2_SHA256; the Bikeshed sources of third_party/css/drafts, pinned by tools/css/DRAFTS_SHA256), a sentence, and the tracked files
that quote it. The check requires the sentence (its parts, where `...` stands for text left out, in order) in the page's text, and every named file to hold it as well, after the comment
markers and the line breaks of the file are read away. A page is read as the page shows it: tags dropped, entities decoded, white space and the typographic quotation marks plain.

--control shows that the check can fail: a quotation that is not in the page, a file that no longer holds its quotation, and a missing page are each reported.

Copyright 2026 by Corey Pennycuff
"""
import hashlib
import html
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

SVG2 = "third_party/css/svg2/"
TRANSFORMS = "third_party/css/drafts/css-transforms-1.bs"

HEADER = "include/ghoti.io/text/css/css_presentation.h"
READINGS = "src/css/css_readings.h"
SOURCE = "src/css/css_presentation.c"
MANUAL = "documentation/formats/css-style.md"
TESTS = "tests/test-css-style.cpp"

# (the pinned page, the sentence, the files that quote it)
QUOTES = [
    (SVG2 + "types.html", "Replace all instances of <length> in grammar with [<length> | <number>]", [HEADER]),
    (SVG2 + "types.html", "Replace all instances of <length> in grammar with [<length> | <number>]. Replace all instances of <length-percentage> in grammar with "
     "[<length-percentage> | <number>]. Replace all instances of <angle> in grammar with [<angle> | <number>].", [READINGS, MANUAL]),
    (SVG2 + "types.html", "The insertion of the <number> symbols allows for unitless length and angles to be used in presentation attribute while disallowing them in corresponding "
     "property values.", [READINGS, MANUAL]),
    (SVG2 + "types.html", "disallowing them in corresponding property values", [HEADER]),
    (SVG2 + "types.html", "When an attribute fails to parse according to the specified CSS Value Definition Syntax ... the attribute is assumed to have been specified as the given "
     "initial value", [HEADER, SOURCE, MANUAL]),
    (SVG2 + "types.html", "is assumed to have been specified as the given initial value", [TESTS]),
    (SVG2 + "types.html", "can override values that come from lower priority style sheet rules, such as those from the user agent style sheet", [HEADER, MANUAL]),
    (SVG2 + "styling.html", "Presentation attributes contribute to the author level of the cascade, following all other author-level style sheets", [HEADER]),
    (SVG2 + "styling.html", "an !important declaration within a presentation attribute will cause it to have an invalid value", [TESTS]),
    (TRANSFORMS, "the syntax of the transform, patternTransform, gradientTransform attributes differ from the syntax of the 'transform' CSS property", [HEADER, SOURCE, MANUAL]),
    (TRANSFORMS, "there is no support for additional <transform-function>s defined for the CSS 'transform' property", [HEADER]),
]


def squash(s):
    """The comparison form of a text: typographic quotes plain, quotation marks of every kind and backslashes gone, white space one space."""
    s = s.translate({0x2018: "'", 0x2019: "'", 0x201c: '"', 0x201d: '"', 0x2013: '-', 0x2014: '-', 0xa0: ' '})
    s = re.sub(r"[\"'\\]", "", s)
    return re.sub(r"\s+", " ", s).strip()


def pinned_hash(path):
    """The SHA-256 a pinned page must have, from the manifest that pins it (tools/css/SVG2_SHA256 or tools/css/DRAFTS_SHA256)."""
    name = os.path.basename(path)
    manifest = "SVG2_SHA256" if path.startswith(SVG2) else "DRAFTS_SHA256"
    for line in open(os.path.join(HERE, manifest), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        fields = line.split()
        if len(fields) >= 2 and fields[1] == name:
            return fields[0]
    return None


def page_text(path):
    """The text of a pinned page: an HTML page of SVG 2 with its tags dropped, or a Bikeshed source read as the CSS script gate reads it (draft_text)."""
    if path.endswith(".bs"):
        spec = importlib.util.spec_from_file_location("css_cssom_script_suite", os.path.join(ROOT, "tools", "conformance", "css_cssom_script_suite.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        # a Bikeshed link to an element's attribute, `<{pattern/patternTransform}>`, is the attribute's name on the page
        return squash(re.sub(r"<\{(?:[^{}/]*/)*([^{}]*)\}>", r"\1", mod.draft_text(os.path.join(ROOT, path))))
    raw = open(os.path.join(ROOT, path), "rb").read().decode("utf-8")
    raw = re.sub(r"<(script|style)(?=[\s>]).*?</\1>", " ", raw, flags=re.S)
    raw = re.sub(r"<!--.*?-->", " ", raw, flags=re.S)
    raw = re.sub(r"<[^<>]*>", "", raw)
    return squash(html.unescape(raw))


def file_text(path):
    """A tracked file as running text: the comment markers at the starts of lines gone (` * `, `//`, `///`), then as squash()."""
    raw = open(os.path.join(ROOT, path), "rb").read().decode("utf-8")
    raw = re.sub(r"(?m)^[ \t]*(?:/\*\*?|\*/|\*|///?)[ \t]?", "", raw)
    return squash(raw)


def contains_in_order(text, sentence):
    """Whether the parts of the sentence (split at ` ... `) are in the text, in order."""
    at = 0
    for part in sentence.split(" ... "):
        if not squash(part):
            return "(an empty part)"
        i = text.find(squash(part), at)
        if i < 0:
            return part
        at = i + len(squash(part))
    return None


def check(quotes):
    problems = []
    pages, files = {}, {}
    for page, sentence, where in quotes:
        if not where:
            problems.append("%r is quoted nowhere: name the files that quote it" % sentence[:40])
        if page not in pages:
            if not os.path.exists(os.path.join(ROOT, page)):
                pages[page] = None
            elif hashlib.sha256(open(os.path.join(ROOT, page), "rb").read()).hexdigest() != pinned_hash(page):
                problems.append("%s does not hash as pinned (tools/css/fetch.sh puts the pinned page there)" % page)
                pages[page] = None
            else:
                pages[page] = page_text(page)
        if pages[page] is None:
            problems.append("%s is not here (tools/css/fetch.sh puts it there)" % page)
            continue
        miss = contains_in_order(pages[page], sentence)
        if miss is not None:
            problems.append("not in %s: %r" % (page, miss))
        for f in where:
            if f not in files:
                files[f] = file_text(f)
            miss = contains_in_order(files[f], sentence)
            if miss is not None:
                problems.append("%s no longer quotes %s: %r" % (f, os.path.basename(page), miss))
    return problems


def control():
    ok = True
    first = QUOTES[0]
    cases = [
        ("a sentence that is not in the page", [(first[0], first[1] + " and the angles", first[2])]),
        ("a file that does not quote it", [(first[0], first[1], [SOURCE])]),
        ("a page that is not pinned", [(SVG2 + "no-such-page.html", first[1], first[2])]),
        ("a part left out that is not in the page", [(first[0], "Replace all instances of <length> in grammar ... with nothing at all", first[2])]),
        ("an empty part", [(first[0], "Replace all instances of <length> in grammar ...  ... with [<length> | <number>]", first[2])]),
        ("a sentence quoted nowhere", [(first[0], first[1], [])]),
        ("a Bikeshed source that does not hold it", [(TRANSFORMS, first[1], first[2])]),
        ("a Bikeshed source whose sentence is changed", [(TRANSFORMS, "there is no support for additional <transform-function>s defined for the HTML 'transform' property", [HEADER])]),
    ]
    for what, quotes in cases:
        if not check(quotes):
            sys.stderr.write("check-pinned-quotes: the check did not fail for %s\n" % what)
            ok = False
    return ok


def main(argv):
    if "--control" in argv:
        if not control():
            return 1
        print("pinned quotes: the check fails for a sentence that is not in the page, a file that does not quote it, a missing page, a missing or an empty part, a sentence quoted nowhere "
              "and a changed sentence of a Bikeshed source")
        return 0
    problems = check(QUOTES)
    for p in problems:
        sys.stderr.write("check-pinned-quotes: %s\n" % p)
    if problems:
        return 1
    print("pinned quotes: %d sentences are in the pinned pages and where they are quoted" % len(QUOTES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
