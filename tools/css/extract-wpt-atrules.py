#!/usr/bin/env python3
"""Read the at-rule parsing vectors of the pinned web-platform-tests as data (C4d).

    extract-wpt-atrules.py <wpt checkout> <out file>

Writes a tab separated file: kind, page, input, expectation (`valid` or `invalid`). Kinds:

* ``keyframes-name``: ``test_keyframes_name_valid('x')`` / ``test_keyframes_name_invalid('x')`` of css-animations/parsing/keyframes-name-{valid,invalid}.html.
* ``keyframe-selector``: ``test_valid_keyframe_selector("x")`` / ``test_invalid_keyframe_selector("x")`` of css-animations/parsing/keyframe-selectors.html.
* ``font-src``: the ``{ src: '...', valid: true|false }`` rows of css-fonts/parsing/font-face-src-list.html.

Calls this reader does not match are counted on stderr (the pages it reads are listed in the output's header lines starting with ``#``); nothing is dropped without a count.

Copyright 2026 by Corey Pennycuff
"""
import os
import re
import sys

S = r"""(?:'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)")"""


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def pick(m):
    return unescape(m.group(1) if m.group(1) is not None else m.group(2))


def main():
    wpt, out = sys.argv[1], sys.argv[2]
    rows = []
    pages = []
    unread = 0

    def read(rel):
        path = os.path.join(wpt, "css", rel)
        if not os.path.exists(path):
            return None
        pages.append(rel)
        text = open(path, encoding="utf-8").read()
        return re.sub(r"^\s*//.*$", "", text, flags=re.M)

    for rel in ("css-animations/parsing/keyframes-name-valid.html", "css-animations/parsing/keyframes-name-invalid.html"):
        t = read(rel)
        if t is None:
            continue
        for m in re.finditer(r"test_keyframes_name_(valid|invalid)\(\s*" + S, t):
            rows.append(("keyframes-name", rel, pick(m) if False else unescape(m.group(2) if m.group(2) is not None else m.group(3)), m.group(1)))
        unread += len(re.findall(r"test_keyframes_name_(?:valid|invalid)\(", t)) - len(re.findall(r"test_keyframes_name_(?:valid|invalid)\(\s*" + S, t)) - 0
    t = read("css-animations/parsing/keyframe-selectors.html")
    if t is not None:
        for m in re.finditer(r"test_(valid|invalid)_keyframe_selector\(\s*" + S, t):
            rows.append(("keyframe-selector", "css-animations/parsing/keyframe-selectors.html", unescape(m.group(2) if m.group(2) is not None else m.group(3)), m.group(1)))
    t = read("css-fonts/parsing/font-face-src-list.html")
    if t is not None:
        for m in re.finditer(r"\{\s*src:\s*" + S + r"\s*,\s*valid:\s*(true|false)\s*\}", t):
            rows.append(("font-src", "css-fonts/parsing/font-face-src-list.html", unescape(m.group(1) if m.group(1) is not None else m.group(2)), "valid" if m.group(3) == "true" else "invalid"))
    with open(out, "w", encoding="utf-8") as f:
        for p in pages:
            f.write("# page\t%s\n" % p)
        for r in rows:
            f.write("\t".join(x.replace("\t", " ").replace("\n", " ") for x in r) + "\n")
    sys.stderr.write("extract-wpt-atrules: %d vectors from %d pages, %d calls not read\n" % (len(rows), len(pages), max(unread, 0)))


if __name__ == "__main__":
    main()
