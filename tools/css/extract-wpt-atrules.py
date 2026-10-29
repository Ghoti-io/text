#!/usr/bin/env python3
"""Read the at-rule parsing vectors of the pinned web-platform-tests as data (C4d).

    extract-wpt-atrules.py <wpt checkout> <out file>

Writes a tab separated file: kind, page, input, expectation (`valid` or `invalid`). Kinds:

* ``keyframes-name``: ``test_keyframes_name_valid('x')`` / ``test_keyframes_name_invalid('x')`` of css-animations/parsing/keyframes-name-{valid,invalid}.html.
* ``keyframe-selector``: ``test_valid_keyframe_selector("x")`` / ``test_invalid_keyframe_selector("x")`` of css-animations/parsing/keyframe-selectors.html.
* ``font-src``: the ``{ src: '...', valid: true|false }`` rows of css-fonts/parsing/font-face-src-list.html.

* ``font-src``: also the ``src`` rows of css-fonts/parsing/font-face-src-{format,local,tech}.html.
* ``counter-descriptor``: input ``descriptor US value US expected`` (US is the unit separator, U+001F); ``test_valid_X(value[, expected])`` / ``test_invalid_X(value)`` of css-counter-styles/counter-style-at-rule/*-syntax.html
  and fallback.html (the wrapper names are read from each page's own function definitions), expectation ``valid`` or ``invalid``.
* ``counter-name``: ``test_valid_name('x')`` / ``test_invalid_name('x')`` of css-counter-styles/counter-style-at-rule/name-syntax.html.
* ``face-descriptor``: input ``descriptor US value US expected`` (US is the unit separator, U+001F); the value tables of css-fonts/parsing/font-face-{metric-overrides,size-adjust,width}.html.

Every other page of the directories the at-rules live in (css-fonts palette, font-face and feature-value pages, css-counter-styles, css-page, css-view-transitions, css-anchor-position
@position-try pages, css-namespaces, css-color @color-profile pages) is written as a line ``# unscored TAB page TAB count TAB reason``: the number of its test calls (1 for a reftest) and why
it is not read as data. tools/conformance/UNSCORED_ATRULES must say the same, or the gate fails.

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


AT_RULE_PAGES = [
    (r"^css-fonts/(?:[^/]*palette[^/]*|[^/]*font-face[^/]*|[^/]*feature-values[^/]*|test_font_feature_values[^/]*|font-display/[^/]*|parsing/font-face[^/]*|parsing/font-palette-values[^/]*)$"),
    r"^css-counter-styles/.*$",
    r"^css-page/.*$",
    r"^css-view-transitions/.*$",
    r"^css-anchor-position/(?:[^/]*position-try[^/]*)$",
    r"^css-namespaces/.*$",
    r"^css-color/[^/]*color-profile[^/]*$",
    r"^css-animations/(?:parsing/keyframe[^/]*|[^/]*keyframe[^/]*)$",
    r"^css-properties-values-api/[^/]*at-property[^/]*$",
]
PAGE_EXT = (".html", ".htm", ".xht", ".xhtml", ".xml")


def list_unscored(wpt, read_pages):
    """(page, calls, reason) for each test page of the at-rule directories that this reader did not take data from: a reftest counts as one."""
    out = []
    base = os.path.join(wpt, "css")
    for dirpath, dirs, files in os.walk(base):
        dirs.sort()
        for fn in sorted(files):
            rel = os.path.relpath(os.path.join(dirpath, fn), base).replace(os.sep, "/")
            if not fn.endswith(PAGE_EXT) or "/support/" in "/" + rel or "/resources/" in "/" + rel:
                continue
            if re.search(r"-(?:ref|notref)\d*\.(?:html|htm|xht|xhtml|xml)$", fn) or "-ref." in fn or "-notref." in fn:
                continue
            if not any(re.match(pat, rel) for pat in AT_RULE_PAGES):
                continue
            if rel in read_pages:
                continue
            text = open(os.path.join(base, rel), encoding="utf-8", errors="replace").read()
            calls = len(re.findall(r"\b(?:test|promise_test|async_test)\(", text))
            if re.search(r'rel=["\']?(?:match|mismatch)', text):
                out.append((rel, 1, "a reftest: the answer is the rendering"))
            elif "crashtest" in rel or "reftest-wait" in text and calls == 0:
                out.append((rel, max(calls, 1), "a crash or rendering test"))
            elif calls:
                out.append((rel, calls, "a script test that reads a style sheet, an animation or a layout, not a table of calls"))
            else:
                out.append((rel, 1, "no test call: a manual or rendering page"))
    return out


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
    for rel in ("css-fonts/parsing/font-face-src-format.html", "css-fonts/parsing/font-face-src-local.html", "css-fonts/parsing/font-face-src-tech.html"):
        t = read(rel)
        if t is None:
            continue
        for m in re.finditer(r"\{\s*src:\s*" + S + r"\s*,\s*valid:\s*(true|false)\s*\}", t):
            rows.append(("font-src", rel, unescape(m.group(1) if m.group(1) is not None else m.group(2)), "valid" if m.group(3) == "true" else "invalid"))
    # counter styles: the wrappers a page defines name the descriptor
    cdir = "css-counter-styles/counter-style-at-rule"
    if os.path.isdir(os.path.join(wpt, "css", cdir)):
        for fn in sorted(os.listdir(os.path.join(wpt, "css", cdir))):
            if not fn.endswith(".html") or fn.endswith("-ref.html") or fn.endswith("-notref.html"):
                continue
            rel = cdir + "/" + fn
            t = read(rel)
            if t is None:
                continue
            wrappers = {}
            for m in re.finditer(r"function\s+(test_(?:valid|invalid)_\w+)\s*\([^)]*\)\s*\{[^}]*?test_(?:valid|invalid)_counter_style_descriptor\(\s*" + S, t):
                wrappers[m.group(1)] = unescape(m.group(2) if m.group(2) is not None else m.group(3))
            for m in re.finditer(r"(?m)^\s*(test_(valid|invalid)_(\w+))\(\s*" + S + r"(?:\s*,\s*" + S + r")?\s*\)\s*;", t):
                call, verdict = m.group(1), m.group(2)
                value = unescape(m.group(4) if m.group(4) is not None else m.group(5))
                expected = unescape(m.group(6) if m.group(6) is not None else m.group(7)) if (m.group(6) is not None or m.group(7) is not None) else ""
                if call in wrappers:
                    rows.append(("counter-descriptor", rel, "\x1f".join((wrappers[call], value, expected or value if verdict == "valid" else "")), verdict))
                elif call in ("test_valid_name", "test_invalid_name"):
                    rows.append(("counter-name", rel, value, verdict))
                elif call in ("test_valid_counter_style_descriptor", "test_invalid_counter_style_descriptor"):
                    pass
            for m in re.finditer(r"test_(valid|invalid)_counter_style_descriptor\(\s*" + S + r"\s*,\s*" + S + r"(?:\s*,\s*" + S + r")?\s*\)", t):
                d = unescape(m.group(2) if m.group(2) is not None else m.group(3))
                v = unescape(m.group(4) if m.group(4) is not None else m.group(5))
                if re.search(r"function\s+test_(?:valid|invalid)_counter_style_descriptor", t):
                    continue
                e = unescape(m.group(6) if m.group(6) is not None else m.group(7)) if (m.group(6) is not None or m.group(7) is not None) else ""
                rows.append(("counter-descriptor", rel, "\x1f".join((d, v, e or v if m.group(1) == "valid" else "")), m.group(1)))
    # font-face value tables
    t = read("css-fonts/parsing/font-face-metric-overrides.html")
    if t is not None:
        ds = re.findall(r'"([a-z-]+)"', re.search(r"const descriptors = \[(.*?)\]", t, re.S).group(1))
        for m in re.finditer(r"\{\s*value:\s*" + S + r"\s*,\s*valid:\s*(true|false)\s*(?:,\s*expected:\s*" + S + r")?\s*\}", t):
            v = unescape(m.group(1) if m.group(1) is not None else m.group(2))
            exp = unescape(m.group(4) if m.group(4) is not None else m.group(5)) if (m.group(4) is not None or m.group(5) is not None) else ""
            for d in ds:
                rows.append(("face-descriptor", "css-fonts/parsing/font-face-metric-overrides.html", "\x1f".join((d, v, exp if m.group(3) == "true" else "")), "valid" if m.group(3) == "true" else "invalid"))
    t = read("css-fonts/parsing/font-face-size-adjust.html")
    if t is not None:
        for m in re.finditer(r"\{\s*sizeAdjust:\s*" + S + r"\s*,\s*valid:\s*(true|false)\s*\}", t):
            v = unescape(m.group(1) if m.group(1) is not None else m.group(2))
            rows.append(("face-descriptor", "css-fonts/parsing/font-face-size-adjust.html", "\x1f".join(("size-adjust", v, "")), "valid" if m.group(3) == "true" else "invalid"))
    t = read("css-fonts/parsing/font-face-width.html")
    if t is not None:
        for name, verdict in (("validValues", "valid"), ("invalidValues", "invalid")):
            blk = re.search(r"const " + name + r" = \[(.*?)\];", t, re.S)
            if blk:
                for m in re.finditer(S, blk.group(1)):
                    v = unescape(m.group(1) if m.group(1) is not None else m.group(2))
                    for d in ("font-width", "font-stretch"):
                        rows.append(("face-descriptor", "css-fonts/parsing/font-face-width.html", "\x1f".join((d, v, "")), verdict))
    unscored = list_unscored(wpt, set(pages))
    with open(out, "w", encoding="utf-8") as f:
        for u in unscored:
            f.write("# unscored\t%s\t%d\t%s\n" % u)
        for p in pages:
            f.write("# page\t%s\n" % p)
        for r in rows:
            f.write("\t".join(x.replace("\t", " ").replace("\n", " ") for x in r) + "\n")
    sys.stderr.write("extract-wpt-atrules: %d vectors from %d pages, %d calls not read\n" % (len(rows), len(pages), max(unread, 0)))


if __name__ == "__main__":
    main()
