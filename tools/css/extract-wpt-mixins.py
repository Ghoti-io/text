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
"""Turn the `<template>` tests of web-platform-tests' css/css-mixins into vectors, reading the pages as data.

    tools/css/extract-wpt-mixins.py WPT_ROOT OUT_DIR [--unextracted FILE]

A page of css/css-mixins that is made of templates says, in a script of two lines, "each `<template data-name="...">` is a test: append its content to `#main`, and the
computed values of two things must be equal". Two scripts do it:

  custom    `test_all_templates()` of css-mixins/resources/utils.js: `getComputedStyle(#target)` has the same `--actual` and `--expected`.
  elements  functions/function-attr.html: `getComputedStyle(#actual)` and `getComputedStyle(#expected)` have the same `list-style-image` and `width`.

The vector is the whole document the test builds: the page's own markup and style sheets with the template's content in `<div id=main>`, as `main.append(template.content.
cloneNode(true))` does. **It evaluates nothing and runs nothing**; the scripts are checked for the exact text they must have (a page that is read as a `custom` page loads
utils.js and has `test_all_templates();`), and a page that no longer says so is a page this program does not read and the gate says why.

Output: `templates.json`, a list of {file, n, name, kind, html, props}, in the order of the file name and then of the source; the identity of a vector is `file#n`. With
`--unextracted FILE`, the other pages of css/css-mixins (every page, in every directory) with the number of tests they hold and the reason they are not read, which
tools/css/UNEXTRACTED_MIXINS holds and the gate compares with a fresh extraction.
"""

import html
import json
import os
import re
import sys

KIND_CUSTOM = "custom"
KIND_ELEMENTS = "elements"
ELEMENT_PROPS = ["list-style-image", "width"]

MAIN = re.compile(r'<div\s+id=(?:"main"|\'main\'|main)\s*>\s*</div>', re.I)
TEMPLATE = re.compile(r'<template\b([^>]*)>(.*?)</template>', re.S | re.I)
SCRIPT = re.compile(r'<script\b[^>]*>.*?</script>|<script\b[^>]*/>', re.S | re.I)
LINK_OR_META = re.compile(r'<(?:link|meta)\b[^>]*>', re.I)
TITLE = re.compile(r'<title>.*?</title>', re.S | re.I)
DATA_NAME = re.compile(r'\bdata-name\s*=\s*(?:"([^"]*)"|\'([^\']*)\')', re.S)
TEST_CALL = re.compile(r'(?<![\w.])(?:test|promise_test|async_test)\s*\(')
SHADOW = re.compile(r'shadowrootmode|attachShadow|attachInternals', re.I)


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def pages(root):
    base = os.path.join(root, "css", "css-mixins")
    out = []
    for d, _, files in os.walk(base):
        for f in sorted(files):
            if f.endswith(".html"):
                out.append(os.path.relpath(os.path.join(d, f), root))
    return sorted(out)


def count_tests(text):
    """The tests a page holds: its templates and data-name hosts (one each), else the calls of test(), promise_test() and async_test() in its scripts."""
    scripts = "\n".join(re.findall(r'<script\b[^>]*>(.*?)</script>', text, re.S | re.I))
    named = len(DATA_NAME.findall(text))
    if named:
        return named
    return max(1, len(TEST_CALL.findall(scripts)))


def classify(rel, text):
    """(kind, why): the kind of a page this program reads in full, else None and the reason it does not."""
    scripts = "\n".join(re.findall(r'<script\b[^>]*>(.*?)</script>', text, re.S | re.I))
    uses_utils = re.search(r'<script\s+src=["\'](?:\.\./|)resources/utils\.js["\']', text) is not None
    if uses_utils and "test_all_templates();" in scripts:
        if SHADOW.search(TEMPLATE.sub("", text)) or re.search(r'<template[^>]*shadowrootmode', text):
            return None, "declarative shadow trees: the pages' tests run inside shadow roots, which the library's HTML parser does not build"
        if not MAIN.search(text) or "id=target" not in text and 'id="target"' not in text:
            return None, "a page of templates whose document has no `#main` or no `#target` of the shape the reader expects"
        return KIND_CUSTOM, ""
    if rel.endswith("functions/function-attr.html") and "template.content.cloneNode(true)" in scripts and "relevant_properties" in scripts:
        if "let relevant_properties = ['list-style-image', 'width'];" not in scripts:
            return None, "function-attr.html compares other properties than the reader expects"
        return KIND_ELEMENTS, ""
    if uses_utils and "test_all_shadows();" in scripts:
        return None, "declarative shadow trees: the tests run inside shadow roots (test_all_shadows), which the library's HTML parser does not build"
    if "shadowrootmode" in text or "attachShadow" in text:
        return None, "shadow trees: the tests build or read shadow roots"
    if "/mixins/" in rel or "/private/" in rel:
        return None, "script tests of mixins and private properties (CSSOM and `@apply`): the CSSOM script gate reads the ones it models"
    return None, "script tests (CSSOM objects, dynamic changes, containers, media queries): the CSSOM script gate reads the ones it models"


def page_document(text, template_html):
    """The document a test builds: the page without its templates, scripts, links, meta and title, with the template's content in `#main`."""
    body = TEMPLATE.sub("", text)
    body = SCRIPT.sub("", body)
    body = LINK_OR_META.sub("", body)
    body = TITLE.sub("", body)
    body = re.sub(r"<!DOCTYPE[^>]*>", "", body, flags=re.I)
    body = MAIN.sub(lambda m: '<div id="main">' + template_html + '</div>', body, count=1)
    return "<!DOCTYPE html><html><head></head><body>" + body + "</body></html>"


def extract(root):
    vectors, unextracted = [], []
    for rel in pages(root):
        text = read(os.path.join(root, rel))
        kind, why = classify(rel, text)
        if kind is None:
            unextracted.append((rel, count_tests(text), why))
            continue
        n = 0
        for m in TEMPLATE.finditer(text):
            attrs, content = m.group(1), m.group(2)
            nm = DATA_NAME.search(attrs)
            name = html.unescape(nm.group(1) if nm and nm.group(1) is not None else (nm.group(2) if nm else ""))
            vectors.append({"file": rel, "n": n, "name": name, "kind": kind, "html": page_document(text, content),
                            "props": ELEMENT_PROPS if kind == KIND_ELEMENTS else []})
            n += 1
        if n == 0:
            sys.exit("%s: read as a page of templates and has none" % rel)
    return vectors, unextracted


def main(argv):
    args = [a for a in argv[1:]]
    unext = None
    if "--unextracted" in args:
        i = args.index("--unextracted")
        unext = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    root, out = args
    vectors, unextracted = extract(root)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "templates.json"), "w", encoding="utf-8") as fh:
        json.dump(vectors, fh)
    if unext:
        with open(unext, "w", encoding="utf-8") as fh:
            fh.write("# tools/css/extract-wpt-mixins.py: the test pages of css/css-mixins of the pinned web-platform-tests that it does not read as data, and why. Columns: file, number of\n"
                     "# tests the page holds, the extractor's own wording. make conformance-css-mixins fails on a page that is not here, on a count that differs and on a line for a page that is\n"
                     "# read in full now; the first two columns are compared.\n"
                     "# Rewrite it with: tools/css/extract-wpt-mixins.py third_party/css/wpt build/css-mixins-vectors --unextracted tools/css/UNEXTRACTED_MIXINS\n")
            for rel, n, why in unextracted:
                fh.write("%s\t%d\t%s\n" % (rel, n, why))
    print("%d templates of %d pages; %d pages not read" % (len(vectors), len({v["file"] for v in vectors}), len(unextracted)))


if __name__ == "__main__":
    main(sys.argv)
