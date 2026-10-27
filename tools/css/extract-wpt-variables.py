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
"""Turn the custom property tests of web-platform-tests' css/ into vectors, reading the pages as data.

    tools/css/extract-wpt-variables.py WPT_ROOT OUT_DIR [--unextracted FILE]

The pages are css/css-variables and css/css-properties-values-api. Most of them are not data: they build a document and ask a browser for
`getComputedStyle()`, animate, transition, register through `CSS.registerProperty()` in a loop. A page this program reads is one of four kinds, and
every other test page is named, with the reason it is not read, in the list that `--unextracted` writes (tools/css/UNEXTRACTED_VARIABLES), which the
gate compares with a fresh extraction, so that a page that changes cannot change what is scored without a diff to review. **It evaluates nothing and
runs nothing**: the scripts are read with the literal reader of tools/css/extract-wpt-props.py (a call whose arguments are literals is data; anything
else is not read), and the style sheets and the markup are split by this program's own small readers.

  registration   `test_initial_value_valid(syntax, initial)` and `test_initial_value_invalid(...)`, under the names a page gives them (`let assert_valid =
                 test_initial_value_valid;`): `CSS.registerProperty({name, syntax, initialValue, inherits: false})` either returns or throws SyntaxError.
                 A `syntax` of `undefined` is a member left out (the default, `*`); `null` and an array are converted to a string as JavaScript does.
  reftest        a page with one `<link rel="match">` whose reference is a page of the same directory, both made of `<style>` elements and a body of
                 elements with an id, class and `style`: the page and its reference must compute the same style for the elements that hold text. The
                 style sheets (their text, in order) and the elements are the vector; the scorer runs the cascade of the library over them.
  cycles         `test_cycles(declarations, expected_invalid, expected_valid, description)` of variable-cycles.html: an element with the declarations as its
                 style attribute, and the custom properties that must be, and must not be, the guaranteed-invalid value.
  limits         the two pages that must not crash (variable-exponential-blowup.html, long-variable-reference-crash.html). Their scripts build the style by
                 a loop, which is not read; the recipe is written in this file, and the guard below fails the extraction when the page no longer says what the
                 recipe assumes.

Output: registration.json, reftest.json, cycles.json, limits.json in OUT_DIR, a list of vectors each, in the order of file name and then of the
source: {file, n, line, ...}; the identity of a vector is `file#n`.
"""

import html.parser
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("extract_wpt_props", os.path.join(HERE, "extract-wpt-props.py"))
props = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(props)

DIRS = ("css/css-variables", "css/css-properties-values-api")
KIND_FILES = ("registration", "reftest", "cycles", "limits")
SKIP_DIRS = {"resources", "support", "reference", "META.yml"}


# ---------------------------------------------------------------------------------------------------------------------
# The pages.

def test_pages(wpt):
    """The test pages of the two directories: every .html/.xht page that is not a reference, not under resources/support/reference."""
    out = []
    for d in DIRS:
        root = os.path.join(wpt, d)
        for base, dirs, names in os.walk(root):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
            for n in sorted(names):
                if not n.endswith((".html", ".xht", ".xhtml", ".htm")):
                    continue
                if re.search(r"-ref\d*\.(html|xht|xhtml|htm)$", n) or n.endswith(("-ref.html", "-notref.html")):
                    continue
                out.append(os.path.relpath(os.path.join(base, n), wpt).replace(os.sep, "/"))
    return sorted(out)


def read(path):
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def js_string(v):
    """A JavaScript value converted to a string, for an argument the harness passes on as text (None for a member left out)."""
    if v is props.Undefined:
        return None
    if v is None:
        return "null"
    if isinstance(v, str):
        return str(v)
    if isinstance(v, list):
        return ",".join(js_string(x) or "" for x in v)
    if v is True:
        return "true"
    if v is False:
        return "false"
    raise props.Unreadable("a value that is not a string")


# ---------------------------------------------------------------------------------------------------------------------
# registration vectors

def registration_vectors(rel, raw, scripts):
    """(vectors, unread calls) of the `test_initial_value_*` calls of a page, through the names the page gives them."""
    aliases = {"test_initial_value_valid": True, "test_initial_value_invalid": False}
    for m in re.finditer(r"\b(?:let|var|const)\s+(\w+)\s*=\s*(test_initial_value_valid|test_initial_value_invalid)\s*;", raw):
        aliases[m.group(1)] = aliases[m.group(2)]
    out, bad = [], []
    for body, first_line in scripts:
        try:
            toks = props.lex(body)
        except props.Unreadable:
            if any(a in body for a in aliases):
                bad.append((first_line, "script", "the script cannot be tokenized"))
            continue
        i = 0
        while i < len(toks):
            tok = toks[i]
            if tok[0] == "id" and tok[1] in aliases and i + 1 < len(toks) and toks[i + 1][:2] == ("p", "(") \
                    and not (i and toks[i - 1][0] == "id" and toks[i - 1][1] == "function") \
                    and not (i and toks[i - 1][0] == "p" and toks[i - 1][1] == "."):
                line = first_line + tok[2] - 1
                args, reason, nxt = props.read_call(toks, i)
                if args is None:
                    bad.append((line, tok[1], reason))
                else:
                    try:
                        if len(args) != 2:
                            raise props.Unreadable("%d arguments" % len(args))
                        syntax = js_string(args[0])
                        initial = js_string(args[1])
                        if initial is None:
                            raise props.Unreadable("an initial value left out")
                        out.append({"file": rel, "line": line, "call": tok[1], "valid": aliases[tok[1]], "syntax": syntax, "initial": initial})
                    except props.Unreadable as e:
                        bad.append((line, tok[1], str(e)))
                i = nxt
                continue
            i += 1
    return out, bad


# ---------------------------------------------------------------------------------------------------------------------
# cycles vectors

def cycles_vectors(rel, scripts):
    out, bad = [], []
    for body, first_line in scripts:
        try:
            toks = props.lex(body)
        except props.Unreadable:
            continue
        i = 0
        while i < len(toks):
            tok = toks[i]
            if tok[0] == "id" and tok[1] == "test_cycles" and i + 1 < len(toks) and toks[i + 1][:2] == ("p", "(") \
                    and not (i and toks[i - 1][0] == "id" and toks[i - 1][1] == "function"):
                line = first_line + tok[2] - 1
                args, reason, nxt = props.read_call(toks, i)
                if args is None:
                    bad.append((line, "test_cycles", reason))
                else:
                    try:
                        decl, inv, val, desc = args
                        for lst in (decl, inv, val):
                            if not isinstance(lst, list) or not all(isinstance(x, str) for x in lst):
                                raise props.Unreadable("an argument that is not a list of strings")
                        out.append({"file": rel, "line": line, "declarations": [str(x) for x in decl], "invalid": [str(x) for x in inv],
                                    "valid": [str(x) for x in val], "description": str(desc)})
                    except (props.Unreadable, ValueError) as e:
                        bad.append((line, "test_cycles", str(e)))
                i = nxt
                continue
            i += 1
    return out, bad


# ---------------------------------------------------------------------------------------------------------------------
# the limits pages: the recipe is the page's loop, written out, and the guard says what the page must still hold

LIMITS = {
    "css/css-variables/variable-exponential-blowup.html": {
        "id": "exponential-blowup",
        "guards": ['--v0: "Something really really really long";', "for (let i = 0; i < 31; ++i)", "css += `--v${i + 1}: var(--v${i}) var(--v${i});`;",
                   ":root::before { content: var(--v31); }"],
        "recipe": {"base": '"Something really really really long"', "levels": 31, "use": "content"},
        "why": "the loop's declarations are written out here",
    },
    "css/css-variables/long-variable-reference-crash.html": {
        "id": "long-reference",
        "guards": ["x.textContent = '* { border: ' + 'a'.repeat(2097152) + ' var(--b); }';"],
        "recipe": {"repeat": "a", "count": 2097152, "property": "border", "tail": " var(--b)"},
        "why": "the repeat is written out here",
    },
}


def limits_vector(rel, raw):
    spec = LIMITS[rel]
    for g in spec["guards"]:
        if g not in raw:
            raise SystemExit("extract-wpt-variables: %s no longer holds %r, which the recipe of its limit vector rests on" % (rel, g))
    v = {"file": rel, "line": 1, "id": spec["id"]}
    v.update(spec["recipe"])
    return v


# ---------------------------------------------------------------------------------------------------------------------
# reftest vectors: the markup of a page, as the elements the cascade is run over

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}


class Page(html.parser.HTMLParser):
    """A page reduced to what a computed-style comparison needs: the `<style>` texts in order, and the elements of the body."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.sheets = []
        self.els = []          # dicts: tag, id, classes, style, parent, text
        self.stack = []        # indexes of open elements of the body
        self.in_style = False
        self.style_buf = []
        self.in_head = True
        self.unreadable = []   # reasons the page is not a plain one
        self.scripts = 0
        self.links = []        # (rel, href)
        self.skip = 0          # inside <script>/<title>/<noscript>

    def handle_starttag(self, tag, attrs):
        a = {k: (v if v is not None else "") for k, v in attrs}
        if tag == "style":
            self.in_style = True
            self.style_buf = []
            if a.get("media") or a.get("type", "text/css") not in ("", "text/css"):
                self.unreadable.append("a style element with media or a type")
            return
        if tag == "link":
            self.links.append((a.get("rel", "").lower(), a.get("href", "")))
            return
        if tag in ("script", "title", "noscript", "template"):
            self.skip += 1
            if tag == "script":
                self.scripts += 1
            return
        if tag in ("html", "head", "meta", "base"):
            return
        if tag == "body":
            self.in_head = False
            if any(k in a for k in ("id", "class", "style")):
                self.unreadable.append("a body element with attributes")
            return
        if self.skip:
            return
        self.in_head = False
        parent = self.stack[-1] if self.stack else 1
        self.els.append({"tag": tag, "id": a.get("id", ""), "classes": a.get("class", "").split(), "style": a.get("style", ""), "parent": parent,
                         "text": False})
        for k in a:
            if k not in ("id", "class", "style", "title", "lang", "dir"):
                self.unreadable.append("an attribute (%s) the cascade could depend on" % k)
        if tag not in VOID:
            self.stack.append(len(self.els) + 1)  # element ids: html 0, body 1, then 2...

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID and tag not in ("style", "link", "script"):
            self.handle_endtag(tag)

    def handle_endtag(self, tag):
        if tag == "style":
            self.in_style = False
            self.sheets.append("".join(self.style_buf))
            return
        if tag in ("script", "title", "noscript", "template"):
            self.skip = max(0, self.skip - 1)
            return
        if self.skip or tag in ("html", "head", "body") or tag in VOID:
            return
        if self.stack:
            self.stack.pop()

    def handle_data(self, data):
        if self.in_style:
            self.style_buf.append(data)
            return
        if self.skip or not data.strip():
            return
        if self.stack:
            self.els[self.stack[-1] - 2]["text"] = True
        elif not self.in_head:
            self.unreadable.append("text outside any element")

    def handle_comment(self, data):
        pass


def parse_page(path):
    p = Page()
    raw = read(path)
    p.feed(raw)
    p.close()
    return p, raw


# the selectors the scorer's matcher knows: compound selectors of a type, `*`, `#id`, `.class` and `:root`, joined by a descendant or child combinator
SIMPLE_SELECTOR = re.compile(r"^\s*(?:(?:[A-Za-z][\w-]*|\*)?(?:#[\w-]+|\.[\w-]+|:root)*)(?:\s*>\s*|\s+(?:(?:[A-Za-z][\w-]*|\*)?(?:#[\w-]+|\.[\w-]+|:root)*))*\s*$")


def strip_comments(css):
    out, i, n = [], 0, len(css)
    while i < n:
        if css.startswith("/*", i):
            j = css.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif css[i] in "\"'":
            q = css[i]
            j = i + 1
            while j < n and css[j] != q:
                j += 2 if css[j] == "\\" else 1
            out.append(css[i:j + 1])
            i = j + 1
        else:
            out.append(css[i])
            i += 1
    return "".join(out)


def skip_block(text, i):
    """text[i] is `{`; returns the index after its matching `}` (strings, comments and nested brackets are stepped over)."""
    depth, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
        elif c in "{([":
            depth += 1
        elif c in "})]":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def rules_problems(text, problems):
    i, n = 0, len(text)
    while i < n:
        # a prelude runs to the next `{` or `;` outside brackets and strings
        j, depth = i, 0
        while j < n:
            c = text[j]
            if c in "\"'":
                q = c
                j += 1
                while j < n and text[j] != q:
                    j += 2 if text[j] == "\\" else 1
            elif c in "([":
                depth += 1
            elif c in ")]":
                depth -= 1
            elif depth <= 0 and c in "{;":
                break
            j += 1
        prelude = text[i:j].strip()
        if j >= n or text[j] == ";":
            if prelude:
                problems.add("an @%s statement" % re.sub(r"^@([\w-]+).*", r"\1", prelude, flags=re.S).lower() if prelude.startswith("@") else "a stray statement")
            i = j + 1
            continue
        end = skip_block(text, j)
        if prelude.startswith("@"):
            name = re.match(r"@([\w-]+)", prelude).group(1).lower()
            if name == "supports":
                rules_problems(text[j + 1:end - 1], problems)
            elif name != "property":
                problems.add("an @%s rule" % name)
        elif prelude:
            for sel in prelude.split(","):
                if not SIMPLE_SELECTOR.match(sel):
                    problems.add("a selector the scorer does not match (%s)" % re.sub(r"\s+", " ", sel.strip())[:40])
        i = end


def sheet_problems(css):
    """Why a style sheet is not one the scorer can run: at-rules it does not evaluate, selectors it does not match."""
    problems = set()
    rules_problems(strip_comments(css), problems)
    return sorted(problems)


def model(page):
    return {"sheets": page.sheets, "els": page.els}


def reftest_vector(wpt, rel, raw):
    """(vector or None, reason it is not one)."""
    page, _ = parse_page(os.path.join(wpt, rel))
    matches = [h for r, h in page.links if r == "match"]
    mismatches = [h for r, h in page.links if r == "mismatch"]
    if mismatches:
        return None, "a mismatch reftest"
    if len(matches) != 1:
        return None, "no single reference page"
    if page.scripts:
        return None, "the page has a script"
    href = matches[0]
    if href.startswith("/"):
        ref_path = os.path.join(wpt, href.lstrip("/"))
    else:
        ref_path = os.path.normpath(os.path.join(wpt, os.path.dirname(rel), href))
    if not ref_path.endswith((".html", ".htm")) or not os.path.isfile(ref_path):
        return None, "the reference is not a page of markup this reader takes (%s)" % os.path.basename(href)
    ref, ref_raw = parse_page(ref_path)
    if ref.scripts:
        return None, "the reference has a script"
    for pg, who in ((page, "the page"), (ref, "the reference")):
        if pg.unreadable:
            return None, "%s: %s" % (who, sorted(set(pg.unreadable))[0])
        for css in pg.sheets:
            pr = sheet_problems(css)
            if pr:
                return None, "%s has %s" % (who, pr[0])
    if re.search(r"<link[^>]*rel=[\"']?stylesheet", raw + ref_raw, re.I):
        return None, "an external style sheet"
    tx = [e for e in page.els if e["text"]]
    rx = [e for e in ref.els if e["text"]]
    if not tx or len(tx) != len(rx) or [e["tag"] for e in tx] != [e["tag"] for e in rx]:
        return None, "the reference does not hold the same text elements"
    if not page.els or not ref.els:
        return None, "no elements"
    return {"file": rel, "line": 1, "ref": os.path.relpath(ref_path, wpt).replace(os.sep, "/"), "page": model(page), "reference": model(ref)}, None


# ---------------------------------------------------------------------------------------------------------------------

def classify(rel, raw):
    """Why a page is not read, in a few words (the first rule that holds)."""
    if re.search(r"@keyframes|\.animate\(|getAnimations|\banimation-|\banimation\s*:|\btransition-|\btransition\s*:|animation_test|transition_test", raw):
        return "animation or transition: needs the animation model"
    if re.search(r"::?(before|after|first-line|first-letter|marker|placeholder)", raw) and "<script" not in raw:
        return "a pseudo-element: needs generated content"
    if "visited" in raw:
        return ":visited: needs history"
    if "@font-face" in raw or "ahem" in raw.lower():
        return "a font: needs the font machinery"
    if re.search(r"getComputedStyle|\.style\b|createElement|CSS\.registerProperty|CSS\.supports|\.sheet|cssText|attributeStyleMap|computedStyleMap|\.animate", raw):
        return "scripted: asks a live document through the CSSOM"
    if "crashtests" in rel or "crash" in rel:
        return "a page that must not crash, with no data to read"
    if 'rel="match"' in raw or "rel=match" in raw or "rel='match'" in raw:
        return "a reftest whose reference this reader does not take"
    if "rel=\"mismatch\"" in raw:
        return "a mismatch reftest"
    return "not data: no call of a known shape"


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    unext_out = None
    if "--unextracted" in argv:
        unext_out = argv[argv.index("--unextracted") + 1]
        args = [a for a in args if a != unext_out]
    wpt, out_dir = args[0], args[1]
    for d in DIRS:
        if not os.path.isdir(os.path.join(wpt, d)):
            print("extract-wpt-variables: %s has no %s (run tools/css/fetch.sh)" % (wpt, d), file=sys.stderr)
            return 1
    vectors = {k: [] for k in KIND_FILES}
    unextracted = []   # (file, count, why)
    taken = 0
    for rel in test_pages(wpt):
        raw = read(os.path.join(wpt, rel))
        scripts, _includes = props.scripts_of(raw)
        reg, reg_bad = registration_vectors(rel, raw, scripts)
        cyc, cyc_bad = cycles_vectors(rel, scripts)
        got = len(reg) + len(cyc)
        for n, v in enumerate(reg):
            v["n"] = n
            vectors["registration"].append(v)
        for n, v in enumerate(cyc):
            v["n"] = n
            vectors["cycles"].append(v)
        bad = reg_bad + cyc_bad
        if rel in LIMITS:
            v = limits_vector(rel, raw)
            v["n"] = 0
            vectors["limits"].append(v)
            got += 1
        if not got and rel not in LIMITS:
            ref_vec, why = reftest_vector(wpt, rel, raw) if ("rel=\"match\"" in raw or "rel=match" in raw) else (None, "")
            if ref_vec:
                ref_vec["n"] = 0
                vectors["reftest"].append(ref_vec)
                got = 1
            elif why:
                unextracted.append((rel, 1, "reftest not read: " + why))
                continue
        if bad:
            reasons = sorted({"%s: %s" % (c, r) for _, c, r in bad})
            unextracted.append((rel, len(bad), "; ".join(reasons)[:200]))
        elif not got:
            unextracted.append((rel, 1, classify(rel, raw)))
        else:
            taken += 1
    for k in KIND_FILES:
        with open(os.path.join(out_dir, k + ".json"), "w", encoding="utf-8") as f:
            json.dump(vectors[k], f, ensure_ascii=False, indent=0, separators=(",", ":"))
            f.write("\n")
    if unext_out:
        with open(unext_out, "w", encoding="utf-8") as f:
            f.write("# tools/css/extract-wpt-variables.py: the test pages of css/css-variables and css/css-properties-values-api of the pinned\n"
                    "# web-platform-tests that it does not read as data (or reads in part), and why. Columns: file, number of calls it could not\n"
                    "# read (1 for a page not read at all), the extractor's own wording. make conformance-css-variables fails on a page that is not\n"
                    "# here, on a count that differs and on a line for a page that is read in full now; the first two columns are compared.\n"
                    "# Rewrite it with: tools/css/extract-wpt-variables.py third_party/css/wpt build/css-variables-vectors --unextracted tools/css/UNEXTRACTED_VARIABLES\n")
            for rel, n, why in unextracted:
                f.write("%s\t%d\t%s\n" % (rel, n, why))
    print("extract-wpt-variables: %s; %d pages not read (or read in part)" % (", ".join("%s %d" % (k, len(vectors[k])) for k in KIND_FILES), len(unextracted)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
