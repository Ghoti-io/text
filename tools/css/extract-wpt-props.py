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
"""Turn the property tests of web-platform-tests' css/ into vectors, reading the calls as data.

    tools/css/extract-wpt-props.py WPT_ROOT OUT_DIR [--unextracted FILE]

WPT's parsing, computed-value and serialization tests are calls of a small harness (css/support/parsing-testcommon.js and
its siblings): `test_valid_value("margin-top", "calc(2em + 3ex)")`. A call is data. This program finds the calls in the
pages' scripts with a lexer that knows comments, strings, template literals and regular-expression literals, and reads
each argument with a restricted literal reader: a string or template literal without a substitution, a number, `undefined`,
`null`, `true`/`false`, an array of those, an object whose values are those or a function (kept only as the fact that the
vector is custom-compared), and `+` between string literals (a long value written over several lines). **It evaluates
nothing and runs nothing.** A call whose argument is anything else (a loop variable, a computed string, a spread) cannot be
read; it is counted, per file, and the count has to equal the line in tools/css/UNEXTRACTED, which is how a page that
changes cannot change the vectors silently. A page that includes the harness and yields no call at all is counted too.

The harness's own semantics, from the pinned css/support/ files:

* `test_valid_value(prop, value, serialized, options)`: `serialized` defaults to `value` only when the call has fewer than
  three arguments; an array means any of its members; `options.comparisonFunction` replaces the equality (the vector is
  marked `custom`, and its expected string is only what the page passed to that function).
* `test_invalid_value(prop, value)`.
* `test_shorthand_value(prop, value, {longhand: expected, ...})` (shorthand-testcommon.js).
* `test_computed_value(prop, specified, computed, titleExtra, options)`, `testComputedValueGreaterOrLowerThan`,
  `testTransformValuesCloseTo`, `test_pseudo_computed_value`, `test_computed_serialization` and `test_used_serialization`
  (serialize-testcommon.js): computed-value vectors.
* `test_specified_serialization(prop, t, e)`: a valid vector whose serialization is `e` (it also requires `e` to read back as `e`).
* `test_valid_selector`, `test_valid_forgiving_selector`, `test_invalid_selector`: selector vectors.
* `test_valid_rule`, `test_invalid_rule`, `test_keyframes_name_valid`, `test_keyframes_name_invalid`: rule vectors.

Output: one JSON file per kind in OUT_DIR (valid.json, invalid.json, shorthand.json, computed.json, selector.json,
rule.json), a list of vectors each, in the order of file name and then of the source:

  {file, n, line, call, prop, value, expected, alts, custom, nested, links, ...}

`file` is relative to the css/ directory's parent (`css/css-grid/parsing/x.html`), `n` counts the file's vectors from zero
(the identity is `file#n`), `line` is the call's line in the file, `alts` the accepted serializations when an array was
given, `nested` is true for a call inside a function or block (the call is read as it is written; whether the page runs
it is the page's business), `quirks` is true when the page has no doctype, and `links` are the page's `<link rel="help">` targets (the spec sentence a vector rests on).

The reader never evaluates a program and never reads another implementation's code.
"""

import json
import os
import re
import sys

HARNESS_FILES = (
    "parsing-testcommon.js", "computed-testcommon.js", "serialize-testcommon.js", "shorthand-testcommon.js",
    "inheritance-testcommon.js",
)

# call name -> (kind, argument names)
CALLS = {
    "test_valid_value": ("valid", ("prop", "value", "expected", "options")),
    "test_invalid_value": ("invalid", ("prop", "value")),
    "test_shorthand_value": ("shorthand", ("prop", "value", "longhands")),
    "test_specified_serialization": ("valid", ("prop", "value", "expected")),
    "test_computed_value": ("computed", ("prop", "value", "expected", "title", "options")),
    "testComputedValueGreaterOrLowerThan": ("computed", ("prop", "value", "expected", "title")),
    "testTransformValuesCloseTo": ("computed", ("value", "epsilon", "expected", "title")),
    "test_pseudo_computed_value": ("computed", ("pseudo", "prop", "value", "expected", "title")),
    "test_computed_serialization": ("computed", ("prop", "value", "expected")),
    "test_used_serialization": ("computed", ("prop", "value", "expected")),
    "test_valid_selector": ("selector", ("value", "expected", "options")),
    "test_valid_forgiving_selector": ("selector", ("value", "expected")),
    "test_invalid_selector": ("selector", ("value",)),
    "test_valid_rule": ("rule", ("value", "expected")),
    "test_invalid_rule": ("rule", ("value",)),
    "test_keyframes_name_valid": ("rule", ("value",)),
    "test_keyframes_name_invalid": ("rule", ("value",)),
}
KIND_FILES = ("valid", "invalid", "shorthand", "computed", "selector", "rule")
# the call decides which of two sub-kinds a vector of a kind is
SUBKIND = {
    "test_valid_value": "valid", "test_specified_serialization": "valid", "test_invalid_value": "invalid",
    "test_valid_selector": "valid", "test_valid_forgiving_selector": "forgiving", "test_invalid_selector": "invalid",
    "test_valid_rule": "valid", "test_invalid_rule": "invalid", "test_keyframes_name_valid": "keyframes-valid",
    "test_keyframes_name_invalid": "keyframes-invalid",
}


class Unreadable(Exception):
    """An argument that is not a literal."""


# ---------------------------------------------------------------------------------------------------------------------
# The lexer. Tokens: ("str", value), ("tpl", value), ("tplx", None) (a template with a substitution), ("num", text),
# ("id", name), ("p", punctuator), ("re", None); each with its line.

PUNCT3 = ("===", "!==", "...", "<<=", ">>=", ">>>", "**=", "&&=", "||=", "??=")
PUNCT2 = ("=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "**")
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "v": "\v", "0": "\0"}


def read_escape(src, i):
    """src[i] is the character after a backslash; returns (text, next index)."""
    c = src[i]
    if c in "nrtbfv":
        return ESCAPES[c], i + 1
    if c == "0" and not (i + 1 < len(src) and src[i + 1].isdigit()):
        return "\0", i + 1
    if c == "x":
        return chr(int(src[i + 1:i + 3], 16)), i + 3
    if c == "u":
        if src[i + 1] == "{":
            j = src.index("}", i)
            return chr(int(src[i + 2:j], 16)), j + 1
        v = int(src[i + 1:i + 5], 16)
        i += 5
        # a surrogate pair is one character
        if 0xD800 <= v < 0xDC00 and src[i:i + 2] == "\\u":
            w = int(src[i + 2:i + 6], 16)
            if 0xDC00 <= w < 0xE000:
                return chr(0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00)), i + 6
        return chr(v), i
    if c == "\r":
        return "", i + (2 if src[i + 1:i + 2] == "\n" else 1)
    if c == "\n" or c == " " or c == " ":
        return "", i + 1
    return c, i + 1


REGEX_PRECEDERS = set("(,=:[!&|?{};+-*%<>~^")


def lex(src):
    toks = []
    i, n, line = 0, len(src), 1
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r\f\v﻿ ":
            i += 1
        elif src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += src.count("\n", i, j)
            i = j
        elif c == "'" or c == '"':
            q, out, start = c, [], line
            i += 1
            while i < n and src[i] != q:
                if src[i] == "\\":
                    t, i2 = read_escape(src, i + 1)
                    line += src.count("\n", i, i2)
                    out.append(t)
                    i = i2
                elif src[i] == "\n":
                    raise Unreadable("unterminated string at line %d" % line)
                else:
                    out.append(src[i])
                    i += 1
            i += 1
            toks.append(("str", "".join(out), start))
        elif c == "`":
            out, start, subst = [], line, False
            i += 1
            depth = 0
            while i < n and not (src[i] == "`" and depth == 0):
                if src[i] == "\\":
                    t, i2 = read_escape(src, i + 1)
                    line += src.count("\n", i, i2)
                    out.append(t)
                    i = i2
                elif src.startswith("${", i):
                    subst = True
                    depth += 1
                    i += 2
                elif depth and src[i] == "}":
                    depth -= 1
                    i += 1
                else:
                    if src[i] == "\n":
                        line += 1
                    out.append(src[i])
                    i += 1
            i += 1
            toks.append(("tplx" if subst else "tpl", None if subst else "".join(out).replace("\r\n", "\n"), start))
        elif c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            m = re.compile(r"0[xX][0-9a-fA-F_]+n?|0[bB][01_]+n?|0[oO][0-7_]+n?|(?:\d[\d_]*\.?[\d_]*|\.\d[\d_]*)(?:[eE][+-]?\d+)?n?").match(src, i)
            toks.append(("num", m.group(0), line))
            i = m.end()
        elif c.isalpha() or c in "_$" or ord(c) > 127:
            m = re.compile(r"[\w$\u0080-\U0010ffff]+").match(src, i)
            toks.append(("id", m.group(0), line))
            i = m.end()
        elif c == "/" and (not toks or toks[-1][0] == "p" and toks[-1][1] in REGEX_PRECEDERS or toks[-1][0] == "id" and toks[-1][1] in ("return", "typeof", "in", "of")):
            j, in_class = i + 1, False
            while j < n and (src[j] != "/" or in_class):
                if src[j] == "\\":
                    j += 1
                elif src[j] == "[":
                    in_class = True
                elif src[j] == "]":
                    in_class = False
                elif src[j] == "\n":
                    break
                j += 1
            j += 1
            while j < n and src[j].isalpha():
                j += 1
            toks.append(("re", None, line))
            i = j
        else:
            for p in PUNCT3:
                if src.startswith(p, i):
                    toks.append(("p", p, line))
                    i += 3
                    break
            else:
                for p in PUNCT2:
                    if src.startswith(p, i):
                        toks.append(("p", p, line))
                        i += 2
                        break
                else:
                    toks.append(("p", c, line))
                    i += 1
    return toks


# ---------------------------------------------------------------------------------------------------------------------
# The restricted literal reader.

class Reader:
    def __init__(self, toks, pos):
        self.t, self.i = toks, pos

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else ("eof", None, 0)

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def punct(self, p):
        tok = self.peek()
        return tok[0] == "p" and tok[1] == p

    def skip_balanced(self):
        """At an opening bracket: step over it and what it holds (a function body, a nested construct)."""
        pairs = {"(": ")", "[": "]", "{": "}"}
        stack = [pairs[self.take()[1]]]
        while stack:
            tok = self.take()
            if tok[0] == "eof":
                raise Unreadable("unbalanced")
            if tok[0] == "p":
                if tok[1] in pairs:
                    stack.append(pairs[tok[1]])
                elif tok[1] == stack[-1]:
                    stack.pop()

    def value(self):
        """One argument: a literal, or Unreadable. Returns a python value; a function is the string FUNCTION."""
        tok = self.peek()
        if tok[0] in ("str", "tpl"):
            self.take()
            s = tok[1]
            while self.punct("+"):  # string literal + string literal
                save = self.i
                self.take()
                nxt = self.peek()
                if nxt[0] in ("str", "tpl"):
                    self.take()
                    s += nxt[1]
                else:
                    self.i = save
                    raise Unreadable("concatenation with a non-literal")
            return s
        if tok[0] == "tplx":
            raise Unreadable("template with a substitution")
        if tok[0] == "num":
            self.take()
            if re.fullmatch(r"\d+", tok[1]) and (tok[1] == "0" or not tok[1].startswith("0")):
                return Number(tok[1])
            if re.fullmatch(r"(0|[1-9]\d*)\.\d+", tok[1]) and not tok[1].endswith("0"):
                return Number(tok[1])
            raise Unreadable("number %s" % tok[1])
        if tok[0] == "id" and tok[1] in ("undefined", "null", "true", "false"):
            self.take()
            return {"undefined": Undefined, "null": None, "true": True, "false": False}[tok[1]]
        if tok[0] == "p" and tok[1] == "[":
            self.take()
            out = []
            while not self.punct("]"):
                out.append(self.value())
                if self.punct(","):
                    self.take()
                elif not self.punct("]"):
                    raise Unreadable("array element")
            self.take()
            return out
        if tok[0] == "p" and tok[1] == "{":
            self.take()
            out = {}
            while not self.punct("}"):
                key = self.take()
                if key[0] not in ("id", "str"):
                    raise Unreadable("object key")
                if self.punct(":"):
                    self.take()
                    if self.peek()[0] == "id" and self.peek()[1] == "function" or self.peek()[0] == "id" and self.peek()[1] == "async":
                        out[key[1]] = Function
                        self.skip_function()
                    elif self.is_arrow():
                        out[key[1]] = Function
                        self.skip_arrow()
                    else:
                        out[key[1]] = self.value()
                elif self.punct("("):  # a method
                    self.skip_balanced()
                    if not self.punct("{"):
                        raise Unreadable("method")
                    self.skip_balanced()
                    out[key[1]] = Function
                else:
                    raise Unreadable("shorthand property")
                if self.punct(","):
                    self.take()
                elif not self.punct("}"):
                    raise Unreadable("object member")
            self.take()
            return out
        if tok[0] == "id" and tok[1] == "function":
            self.skip_function()
            return Function
        if self.is_arrow():
            self.skip_arrow()
            return Function
        raise Unreadable("%s %r" % (tok[0], tok[1]))

    def is_arrow(self):
        tok = self.peek()
        if tok[0] == "id" and self.i + 1 < len(self.t) and self.t[self.i + 1][:2] == ("p", "=>"):
            return True
        if tok[0] == "p" and tok[1] == "(":
            depth, j = 0, self.i
            while j < len(self.t):
                if self.t[j][0] == "p" and self.t[j][1] == "(":
                    depth += 1
                elif self.t[j][0] == "p" and self.t[j][1] == ")":
                    depth -= 1
                    if depth == 0:
                        return j + 1 < len(self.t) and self.t[j + 1][:2] == ("p", "=>")
                j += 1
        return False

    def skip_arrow(self):
        if self.punct("("):
            self.skip_balanced()
        else:
            self.take()
        self.take()  # =>
        if self.punct("{"):
            self.skip_balanced()
        else:  # an expression body ends at the next top-level , ) ] }
            depth = 0
            while True:
                tok = self.peek()
                if tok[0] == "eof":
                    raise Unreadable("arrow body")
                if tok[0] == "p" and tok[1] in "([{":
                    depth += 1
                elif tok[0] == "p" and tok[1] in ")]}":
                    if depth == 0:
                        break
                    depth -= 1
                elif tok[0] == "p" and tok[1] == "," and depth == 0:
                    break
                self.take()

    def skip_function(self):
        if self.peek()[1] == "async":
            self.take()
        self.take()  # function
        if self.peek()[0] == "id":
            self.take()
        if not self.punct("("):
            raise Unreadable("function")
        self.skip_balanced()
        if not self.punct("{"):
            raise Unreadable("function body")
        self.skip_balanced()


class Number(str):
    """A number literal in canonical decimal text (what JavaScript converts it back to)."""


class _Undefined:
    def __repr__(self):
        return "undefined"


Undefined = _Undefined()
Function = object()


def read_call(toks, i):
    """toks[i] is the identifier, toks[i+1] the parenthesis. Returns (args or None, reason, index after the call)."""
    r = Reader(toks, i + 2)
    args = []
    try:
        while not r.punct(")"):
            if r.punct("..."):
                raise Unreadable("spread")
            args.append(r.value())
            if r.punct(","):
                r.take()
            elif not r.punct(")"):
                raise Unreadable("%s %r after an argument" % (r.peek()[0], r.peek()[1]))
        r.take()
        return args, None, r.i
    except Unreadable as e:
        # step to the call's end for the next search
        r2 = Reader(toks, i + 1)
        try:
            r2.skip_balanced()
            nxt = r2.i
        except Unreadable:
            nxt = i + 2
        return None, str(e), nxt
    except (IndexError, ValueError):
        return None, "unreadable", i + 2


# ---------------------------------------------------------------------------------------------------------------------

SCRIPT = re.compile(r"<script\b([^>]*)>(.*?)</script\s*>", re.S | re.I)
LINK_HELP = re.compile(r"<link\b[^>]*\brel\s*=\s*[\"']?help[\"']?[^>]*>", re.S | re.I)
HREF = re.compile(r"\bhref\s*=\s*(?:\"([^\"]*)\"|'([^']*)')", re.I)
SRC = re.compile(r"\bsrc\s*=\s*(?:\"([^\"]*)\"|'([^']*)')", re.I)


def scripts_of(text):
    """(source, first line, includes) for each inline script, plus the list of external script names."""
    out, includes = [], []
    for m in SCRIPT.finditer(text):
        attrs = m.group(1)
        sm = SRC.search(attrs)
        if sm:
            includes.append(os.path.basename(sm.group(1) or sm.group(2)))
            continue
        body = m.group(2)
        if "<![CDATA[" in body:
            body = body.replace("<![CDATA[", "         ").replace("]]>", "   ")
        line = text.count("\n", 0, m.start(2)) + 1
        out.append((body, line))
    return out, includes


def clean_links(text):
    links = []
    for m in LINK_HELP.finditer(text):
        h = HREF.search(m.group(0))
        if h:
            links.append((h.group(1) or h.group(2)).strip())
    return links


def string_of(v):
    """A JavaScript value's string conversion, for an argument the harness uses as a CSS text."""
    if isinstance(v, str):
        return str(v)
    raise Unreadable("a non-string where a string is needed")


def make_vector(name, args, file, line, links, nested, quirks):
    kind, names = CALLS[name]
    a = dict(zip(names, args))  # an argument past the harness function's parameters is ignored, as JavaScript ignores it
    v = {"file": file, "line": line, "call": name, "kind": kind, "sub": SUBKIND.get(name, ""), "links": links, "nested": nested,
         "quirks": quirks}
    if "prop" in a:
        v["prop"] = string_of(a["prop"])
    if "pseudo" in a:
        v["pseudo"] = string_of(a["pseudo"])
    def text(x):
        if isinstance(x, (str,)):
            return str(x)
        if x is Undefined:
            return None
        raise Unreadable("a value that is not a string")
    v["value"] = text(a["value"]) if "value" in a else None
    if v["value"] is None:
        raise Unreadable("a missing value")
    # `expected`, with the harness's defaulting (parsing-testcommon.js): test_valid_value and test_specified_serialization
    # take the value when fewer than three arguments are given, test_valid_selector when fewer than two, test_valid_rule
    # when it is undefined, test_valid_forgiving_selector when it is undefined or null.
    def serialization(e):
        if isinstance(e, list):
            v["alts"] = [text(x) for x in e]
            v["expected"] = v["alts"][0] if v["alts"] else ""
        elif isinstance(e, str):
            v["expected"] = str(e)
            v["alts"] = []
        else:
            raise Unreadable("an expectation that is not a string")
    if name in ("test_valid_value", "test_specified_serialization"):
        if len(args) < 3:
            v["expected"], v["alts"] = v["value"], []
        else:
            serialization(a["expected"])
    elif name == "test_valid_selector":
        if len(args) < 2:
            v["expected"], v["alts"] = v["value"], []
        else:
            serialization(a["expected"])
    elif name == "test_valid_rule":
        if len(args) < 2 or a["expected"] is Undefined:
            v["expected"], v["alts"] = v["value"], []
        else:
            serialization(a["expected"])
    elif name == "test_valid_forgiving_selector":
        e = a.get("expected", Undefined)
        if e is Undefined or e is None or e == "":
            v["expected"], v["alts"] = v["value"], []
        else:
            serialization(e)
    elif "expected" in a:
        serialization(a["expected"])
    opts = a.get("options")
    v["custom"] = bool(isinstance(opts, dict) and opts.get("comparisonFunction") is Function)
    if name == "test_shorthand_value":
        lh = a["longhands"]
        if not isinstance(lh, dict) or not all(isinstance(x, str) for x in lh.values()):
            raise Unreadable("longhands")
        v["longhands"] = {k: str(x) for k, x in lh.items()}
    if name == "testTransformValuesCloseTo":
        v["prop"] = "transform"
        v["epsilon"] = str(a["epsilon"]) if isinstance(a.get("epsilon"), str) else None
    if name == "testComputedValueGreaterOrLowerThan":
        v["custom"] = True
    return v


def extract_file(path, rel, harness_names):
    """Returns (vectors, unreadable [(line, call, reason)], harness included?)."""
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    links = clean_links(text)
    # A page without a doctype is in quirks mode (an XML page never is): the parsing of a unitless length depends on it.
    quirks = not rel.endswith((".xht", ".xhtml")) and not re.search(r"<!doctype", text[:4096], re.I)
    scripts, includes = scripts_of(text)
    uses_harness = any(h in includes for h in HARNESS_FILES) or any(i in harness_names for i in includes)
    vectors, bad = [], []
    for body, first_line in scripts:
        try:
            toks = lex(body)
        except Unreadable as e:
            if any(c in body for c in CALLS):
                bad.append((first_line, "script", "the script cannot be tokenized (%s)" % e))
            continue
        depth_stack = []  # "{" opened by a function or block; we only need depth
        depth = 0
        i = 0
        while i < len(toks):
            tok = toks[i]
            if tok[0] == "p" and tok[1] == "{":
                depth += 1
            elif tok[0] == "p" and tok[1] == "}":
                depth -= 1
            elif tok[0] == "id" and tok[1] in CALLS and i + 1 < len(toks) and toks[i + 1][:2] == ("p", "(") \
                    and not (i and toks[i - 1][0] == "id" and toks[i - 1][1] == "function") \
                    and not (i and toks[i - 1][0] == "p" and toks[i - 1][1] == "."):
                line = first_line + tok[2] - 1
                args, reason, nxt = read_call(toks, i)
                if args is None:
                    bad.append((line, tok[1], reason))
                else:
                    try:
                        vectors.append(make_vector(tok[1], args, rel, line, links, depth > 0, quirks))
                    except Unreadable as e:
                        bad.append((line, tok[1], str(e)))
                i = nxt
                continue
            i += 1
    return vectors, bad, uses_harness


def walk(root):
    out = []
    for d, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            if n.endswith((".html", ".xht", ".xhtml", ".htm")):
                out.append(os.path.join(d, n))
    return out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    unext_out = None
    if "--unextracted" in argv:
        unext_out = argv[argv.index("--unextracted") + 1]
        args = [a for a in args if a != unext_out]
    wpt, out_dir = args[0], args[1]
    css = os.path.join(wpt, "css")
    if not os.path.isdir(os.path.join(css, "support")):
        print("extract-wpt-props: %s has no css/support (run tools/css/fetch.sh)" % wpt, file=sys.stderr)
        return 1
    harness_names = set()
    vectors = {k: [] for k in KIND_FILES}
    unextracted = []  # (file, count, reasons)
    empty_harness = []
    for path in walk(css):
        rel = os.path.relpath(path, wpt).replace(os.sep, "/")
        # a file is only worth reading when it mentions the harness: a cheap test before the lexer
        try:
            raw = open(path, encoding="utf-8", errors="surrogateescape").read()
        except OSError:
            continue
        if not any(c in raw for c in CALLS):
            continue
        vs, bad, uses = extract_file(path, rel, harness_names)
        if not vs and not bad:
            # the names appear but never as a call (a definition, a comment)
            if uses:
                empty_harness.append(rel)
            continue
        for n, v in enumerate(vs):
            v["n"] = n
            vectors[v["kind"]].append(v)
        if bad:
            reasons = sorted({"%s: %s" % (c, r) for _, c, r in bad})
            unextracted.append((rel, len(bad), "; ".join(reasons)[:200]))
    for k in KIND_FILES:
        with open(os.path.join(out_dir, k + ".json"), "w", encoding="utf-8") as f:
            json.dump(vectors[k], f, ensure_ascii=False, indent=0, separators=(",", ":"))
            f.write("\n")
    if unext_out:
        with open(unext_out, "w", encoding="utf-8") as f:
            f.write("# tools/css/extract-wpt-props.py: the calls of the property test harness it cannot read as data, per file of the pinned\n"
                    "# web-platform-tests (a loop variable, a template with a substitution, a value computed by the page), and the pages that\n"
                    "# include the harness and call none of its functions with a name it knows. Columns: file, number of calls, what stopped it.\n"
                    "# make conformance-css-property fails on a file that is not here, on a count that differs and on a line for a file that is\n"
                    "# read in full now; the first column and the count are compared, the third is the extractor's own wording.\n"
                    "# Rewrite it with: tools/css/extract-wpt-props.py third_party/css/wpt build/css-property-vectors --unextracted tools/css/UNEXTRACTED\n")
            for rel, n, why in unextracted:
                f.write("%s\t%d\t%s\n" % (rel, n, why))
            for rel in empty_harness:
                f.write("%s\t0\tthe page includes the harness and calls none of its functions with a name this reader knows\n" % rel)
    total = sum(len(v) for v in vectors.values())
    print("extract-wpt-props: %d vectors (%s), %d calls not read in %d files, %d harness pages with no call" % (
        total, ", ".join("%s %d" % (k, len(vectors[k])) for k in KIND_FILES), sum(n for _, n, _ in unextracted), len(unextracted),
        len(empty_harness)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
