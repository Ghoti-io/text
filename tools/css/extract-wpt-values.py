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
"""Turn the value tests of web-platform-tests' css/css-values and css/css-color/parsing into vectors.

    tools/css/extract-wpt-values.py WPT_ROOT OUT.json

The pages' own scripts are run, in a small interpreter of the part of JavaScript they use, with the test helpers replaced by recorders: what
is recorded is what the page asks, not what a browser answers. (A gate that needs a JavaScript engine cannot run in the build image, which has none; an earlier
node program did the same, and the output of this one was compared with it byte for byte on the pinned pages when it replaced it.
The interpreter has the part of JavaScript those pages use and no more.) Only the
specified-value stage is kept (the computed and used stages belong to the cascade). A vector is

  {file, n, kind, prop, value, expected, approx, epsilon}

kind "valid": `value` must be accepted by the grammar of `prop` and be written as `expected`;
kind "invalid": `value` must not be accepted;
kind "equiv": `value` and `expected` (test_math_specified) must both be accepted and be written alike (within `approx` when it is given).

A script that needs the browser itself stops where the browser would be needed (a call of a helper that is not recorded is a
ReferenceError, as it was in the sandbox of the node program): what it recorded before stays, and the script blocks after it still run.
A script that uses a construct of JavaScript the interpreter does not have is skipped only if it never names a recorder (it cannot
record anything then); one that does name a recorder stops the program, which names the construct, so that a page that changes cannot
change the vectors silently.

This file reads the pinned pages and runs their scripts only to learn which values they test: it never reads another implementation's code.
"""

import json
import math
import os
import re
import sys
from decimal import Decimal


class Unsupported(Exception):
    """A construct of JavaScript this interpreter does not have."""


class JSError(Exception):
    """A JavaScript exception (what a try/catch, or the end of a script, takes)."""

    def __init__(self, kind, message=""):
        Exception.__init__(self, "%s: %s" % (kind, message))
        self.kind = kind


class _Undefined:
    def __repr__(self):
        return "undefined"


class _Null:
    def __repr__(self):
        return "null"


UNDEF = _Undefined()
NULL = _Null()


class Return(Exception):
    def __init__(self, value):
        self.value = value


class Break(Exception):
    pass


class Continue(Exception):
    pass


# ---- lexer -------------------------------------------------------------------------------------------------------------------------

PUNCT = ["...", "===", "!==", "**=", "<<=", ">>=", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=",
         "**", "<<", ">>", "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "?", ":", "=", ".", "@"]
KEYWORDS = {"var", "let", "const", "function", "return", "if", "else", "for", "while", "of", "in", "try", "catch", "finally", "throw", "break",
            "continue", "true", "false", "null", "undefined", "typeof", "void", "new", "this", "do", "switch", "class", "delete", "instanceof"}
UNSUPPORTED_KEYWORDS = {"new", "this", "do", "switch", "class", "delete", "instanceof", "void", "throw"}
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "v": "\v", "0": "\0"}


class Tok:
    __slots__ = ("kind", "value", "nl", "pos")

    def __init__(self, kind, value, nl, pos):
        self.kind, self.value, self.nl, self.pos = kind, value, nl, pos

    def __repr__(self):
        return "%s(%r)" % (self.kind, self.value)


def read_string(src, i):
    """A quoted string starting at src[i]; returns (text, index after)."""
    q = src[i]
    i += 1
    out = []
    while True:
        if i >= len(src):
            raise Unsupported("unterminated string")
        c = src[i]
        if c == q:
            return "".join(out), i + 1
        if c == "\\":
            i += 1
            e = src[i]
            if e in ESCAPES and not (e == "0" and i + 1 < len(src) and src[i + 1].isdigit()):
                out.append(ESCAPES[e])
                i += 1
            elif e == "x":
                out.append(chr(int(src[i + 1:i + 3], 16)))
                i += 3
            elif e == "u":
                if src[i + 1] == "{":
                    j = src.index("}", i)
                    out.append(chr(int(src[i + 2:j], 16)))
                    i = j + 1
                else:
                    out.append(chr(int(src[i + 1:i + 5], 16)))
                    i += 5
            elif e == "\n":
                i += 1
            else:
                out.append(e)
                i += 1
        else:
            out.append(c)
            i += 1


def read_template(src, i):
    """A template literal starting at the backtick at src[i]; returns (parts, index after): parts alternate strings and expression sources."""
    i += 1
    parts = []
    cur = []
    while True:
        if i >= len(src):
            raise Unsupported("unterminated template")
        c = src[i]
        if c == "`":
            parts.append("".join(cur))
            return parts, i + 1
        if c == "\\":
            i += 1
            e = src[i]
            if e in ESCAPES:
                cur.append(ESCAPES[e])
                i += 1
            elif e == "u":
                cur.append(chr(int(src[i + 1:i + 5], 16)))
                i += 5
            else:
                cur.append(e)
                i += 1
        elif c == "$" and src[i + 1:i + 2] == "{":
            parts.append("".join(cur))
            cur = []
            depth = 1
            j = i + 2
            while depth:
                if j >= len(src):
                    raise Unsupported("unterminated ${")
                d = src[j]
                if d == "{":
                    depth += 1
                elif d == "}":
                    depth -= 1
                    if depth == 0:
                        break
                elif d in "'\"":
                    _, j = read_string(src, j)
                    continue
                elif d == "`":
                    _, j = read_template(src, j)
                    continue
                j += 1
            parts.append(Expr(src[i + 2:j]))
            i = j + 1
        else:
            cur.append(c)
            i += 1


class Expr:
    """The source of an expression inside a template literal."""

    def __init__(self, src):
        self.src = src


def lex(src):
    toks = []
    i, n = 0, len(src)
    nl = False
    while i < n:
        c = src[i]
        if c in " \t\r\f\v ﻿":
            i += 1
            continue
        if c == "\n":
            nl = True
            i += 1
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            if "\n" in src[i:j if j >= 0 else n]:
                nl = True
            i = n if j < 0 else j + 2
            continue
        if c.isalpha() or c in "_$":
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] in "_$"):
                j += 1
            toks.append(Tok("name", src[i:j], nl, i))
            i = j
        elif c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            m = re.compile(r"0[xX][0-9a-fA-F]+|(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?").match(src, i)
            text = m.group(0)
            toks.append(Tok("num", float(int(text, 16)) if text[:2] in ("0x", "0X") else float(text), nl, i))
            i = m.end()
        elif c in "'\"":
            s, i2 = read_string(src, i)
            toks.append(Tok("str", s, nl, i))
            i = i2
        elif c == "`":
            parts, i2 = read_template(src, i)
            toks.append(Tok("tmpl", parts, nl, i))
            i = i2
        elif c == "/" and regex_allowed(toks):
            j = i + 1
            in_class = False
            while True:
                d = src[j]
                if d == "\\":
                    j += 2
                    continue
                if d == "[":
                    in_class = True
                elif d == "]":
                    in_class = False
                elif d == "/" and not in_class:
                    break
                j += 1
            k = j + 1
            while k < n and src[k].isalpha():
                k += 1
            toks.append(Tok("regex", (src[i + 1:j], src[j + 1:k]), nl, i))
            i = k
        else:
            for p in PUNCT:
                if src.startswith(p, i):
                    toks.append(Tok("p", p, nl, i))
                    i += len(p)
                    break
            else:
                raise Unsupported("character %r" % c)
        nl = False
    toks.append(Tok("eof", None, nl, n))
    return toks


def regex_allowed(toks):
    if not toks:
        return True
    t = toks[-1]
    if t.kind in ("num", "str", "tmpl", "regex"):
        return False
    if t.kind == "name":
        return t.value in ("return", "typeof", "of", "in", "else")
    return t.value not in (")", "]", "}")


# ---- parser ---------------------------------------------------------------------------------------------------------------------------

class Parser:
    def __init__(self, src):
        self.toks = lex(src)
        self.i = 0

    @property
    def tok(self):
        return self.toks[self.i]

    def peek(self, k=1):
        return self.toks[min(self.i + k, len(self.toks) - 1)]

    def next(self):
        t = self.toks[self.i]
        self.i += 1
        return t

    def is_p(self, v):
        return self.tok.kind == "p" and self.tok.value == v

    def is_name(self, v):
        return self.tok.kind == "name" and self.tok.value == v

    def eat_p(self, v):
        if self.is_p(v):
            self.i += 1
            return True
        return False

    def expect_p(self, v):
        if not self.eat_p(v):
            raise Unsupported("expected %r, found %r near %r" % (v, self.tok, self.context()))

    def context(self):
        return " ".join(str(t.value) for t in self.toks[max(0, self.i - 6):self.i + 6])

    def semicolon(self):
        if self.eat_p(";"):
            return
        if self.is_p("}") or self.tok.kind == "eof" or self.tok.nl:
            return
        raise Unsupported("expected ; near %r" % self.context())

    # statements

    def program(self):
        out = []
        while self.tok.kind != "eof":
            out.append(self.statement())
        return out

    def block(self):
        self.expect_p("{")
        out = []
        while not self.is_p("}"):
            out.append(self.statement())
        self.expect_p("}")
        return ("block", out)

    def statement(self):
        t = self.tok
        if t.kind == "p":
            if t.value == "{":
                return self.block()
            if t.value == ";":
                self.i += 1
                return ("empty",)
        if t.kind == "name":
            v = t.value
            if v in UNSUPPORTED_KEYWORDS:
                raise Unsupported("keyword %s near %r" % (v, self.context()))
            if v in ("var", "let", "const"):
                d = self.declaration()
                self.semicolon()
                return d
            if v == "function":
                return self.function(declaration=True)
            if v == "return":
                self.i += 1
                e = None
                if not (self.is_p(";") or self.is_p("}") or self.tok.nl or self.tok.kind == "eof"):
                    e = self.expression()
                self.semicolon()
                return ("return", e)
            if v == "if":
                self.i += 1
                self.expect_p("(")
                c = self.expression()
                self.expect_p(")")
                a = self.statement()
                b = None
                if self.is_name("else"):
                    self.i += 1
                    b = self.statement()
                return ("if", c, a, b)
            if v == "for":
                return self.for_statement()
            if v == "while":
                self.i += 1
                self.expect_p("(")
                c = self.expression()
                self.expect_p(")")
                return ("while", c, self.statement())
            if v == "break":
                self.i += 1
                self.semicolon()
                return ("break",)
            if v == "continue":
                self.i += 1
                self.semicolon()
                return ("continue",)
            if v == "try":
                self.i += 1
                body = self.block()
                param = handler = final = None
                if self.is_name("catch"):
                    self.i += 1
                    if self.eat_p("("):
                        param = self.binding_target()
                        self.expect_p(")")
                    handler = self.block()
                if self.is_name("finally"):
                    self.i += 1
                    final = self.block()
                return ("try", body, param, handler, final)
        e = self.expression()
        self.semicolon()
        return ("expr", e)

    def declaration(self):
        kind = self.next().value
        decls = []
        while True:
            target = self.binding_target()
            init = None
            if self.eat_p("="):
                init = self.assignment()
            decls.append((target, init))
            if not self.eat_p(","):
                break
        return ("decl", kind, decls)

    def binding_target(self):
        """A name, or an array or object pattern."""
        if self.is_p("["):
            self.i += 1
            items = []
            while not self.is_p("]"):
                if self.is_p(","):
                    self.i += 1
                    items.append(None)
                    continue
                if self.eat_p("..."):
                    items.append(("rest", self.binding_target()))
                else:
                    t = self.binding_target()
                    d = self.assignment() if self.eat_p("=") else None
                    items.append(("item", t, d))
                if not self.eat_p(","):
                    break
            self.expect_p("]")
            return ("arrpat", items)
        if self.is_p("{"):
            self.i += 1
            items = []
            while not self.is_p("}"):
                if self.eat_p("..."):
                    items.append(("rest", self.binding_target()))
                else:
                    k = self.next()
                    if k.kind == "p" and k.value == "[":
                        raise Unsupported("computed key in a pattern")
                    key = k.value
                    if self.eat_p(":"):
                        t = self.binding_target()
                    else:
                        t = ("name", key)
                    d = self.assignment() if self.eat_p("=") else None
                    items.append(("prop", key, t, d))
                if not self.eat_p(","):
                    break
            self.expect_p("}")
            return ("objpat", items)
        t = self.next()
        if t.kind != "name":
            raise Unsupported("binding %r near %r" % (t, self.context()))
        return ("name", t.value)

    def for_statement(self):
        self.i += 1
        self.expect_p("(")
        init = None
        if self.is_p(";"):
            pass
        elif self.tok.kind == "name" and self.tok.value in ("var", "let", "const"):
            save = self.i
            kind = self.next().value
            target = self.binding_target()
            if self.is_name("of") or self.is_name("in"):
                which = self.next().value
                it = self.expression()
                self.expect_p(")")
                return ("forin" if which == "in" else "forof", kind, target, it, self.statement())
            self.i = save
            init = self.declaration()
        else:
            e = self.expression()
            if self.is_name("of") or self.is_name("in"):
                which = self.next().value
                it = self.expression()
                self.expect_p(")")
                return ("forin" if which == "in" else "forof", None, ("expr", e), it, self.statement())
            init = ("expr", e)
        self.expect_p(";")
        cond = None if self.is_p(";") else self.expression()
        self.expect_p(";")
        step = None if self.is_p(")") else self.expression()
        self.expect_p(")")
        return ("for", init, cond, step, self.statement())

    def function(self, declaration=False):
        self.i += 1  # function
        if self.is_p("*"):
            raise Unsupported("generator")
        name = None
        if self.tok.kind == "name" and not self.is_p("("):
            name = self.next().value
        params = self.params()
        body = self.block()
        node = ("func", name, params, body, False)
        if declaration:
            return ("funcdecl", name, node)
        return node

    def params(self):
        self.expect_p("(")
        out = []
        while not self.is_p(")"):
            if self.eat_p("..."):
                out.append(("rest", self.binding_target()))
            else:
                t = self.binding_target()
                d = self.assignment() if self.eat_p("=") else None
                out.append(("item", t, d))
            if not self.eat_p(","):
                break
        self.expect_p(")")
        return out

    # expressions

    def expression(self):
        e = self.assignment()
        if self.is_p(","):
            items = [e]
            while self.eat_p(","):
                items.append(self.assignment())
            return ("seq", items)
        return e

    def arrow_ahead(self):
        """Whether the tokens at the cursor start an arrow function."""
        t = self.tok
        if t.kind == "name" and self.peek().kind == "p" and self.peek().value == "=>":
            return True
        if t.kind == "p" and t.value == "(":
            depth = 0
            j = self.i
            while True:
                x = self.toks[j]
                if x.kind == "eof":
                    return False
                if x.kind == "p" and x.value in "([{":
                    depth += 1
                elif x.kind == "p" and x.value in ")]}":
                    depth -= 1
                    if depth == 0:
                        nxt = self.toks[j + 1]
                        return nxt.kind == "p" and nxt.value == "=>"
                j += 1
        return False

    def arrow(self):
        if self.tok.kind == "name":
            params = [("item", ("name", self.next().value), None)]
        else:
            params = self.params()
        self.expect_p("=>")
        if self.is_p("{"):
            body = self.block()
        else:
            body = ("block", [("return", self.assignment())])
        return ("func", None, params, body, True)

    def assignment(self):
        if self.arrow_ahead():
            return self.arrow()
        left = self.conditional()
        t = self.tok
        if t.kind == "p" and t.value in ("=", "+=", "-=", "*=", "/=", "%=", "**="):
            self.i += 1
            right = self.assignment()
            if left[0] in ("array", "object") and t.value == "=":
                raise Unsupported("destructuring assignment")
            return ("assign", t.value, left, right)
        return left

    def conditional(self):
        c = self.binary(0)
        if self.eat_p("?"):
            a = self.assignment()
            self.expect_p(":")
            b = self.assignment()
            return ("cond", c, a, b)
        return c

    LEVELS = [["??"], ["||"], ["&&"], ["|"], ["^"], ["&"], ["==", "!=", "===", "!=="], ["<", ">", "<=", ">="], ["<<", ">>"], ["+", "-"], ["*", "/", "%"], ["**"]]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        ops = self.LEVELS[level]
        left = self.binary(level + 1)
        while self.tok.kind == "p" and self.tok.value in ops:
            op = self.next().value
            right = self.binary(level + 1)
            left = ("bin", op, left, right)
        return left

    def unary(self):
        t = self.tok
        if t.kind == "p" and t.value in ("!", "-", "+", "~"):
            self.i += 1
            return ("unary", t.value, self.unary())
        if t.kind == "p" and t.value in ("++", "--"):
            self.i += 1
            return ("update", t.value, True, self.unary())
        if t.kind == "name" and t.value == "typeof":
            self.i += 1
            return ("typeof", self.unary())
        e = self.postfix()
        return e

    def postfix(self):
        e = self.call_member()
        if self.tok.kind == "p" and self.tok.value in ("++", "--") and not self.tok.nl:
            op = self.next().value
            return ("update", op, False, e)
        return e

    def call_member(self):
        e = self.primary()
        while True:
            t = self.tok
            if t.kind == "p" and t.value == ".":
                self.i += 1
                name = self.next()
                e = ("member", e, ("str", name.value))
            elif t.kind == "p" and t.value == "?.":
                raise Unsupported("optional chaining")
            elif t.kind == "p" and t.value == "[":
                self.i += 1
                idx = self.expression()
                self.expect_p("]")
                e = ("member", e, idx)
            elif t.kind == "p" and t.value == "(":
                e = ("call", e, self.arguments())
            elif t.kind == "tmpl":
                raise Unsupported("tagged template")
            else:
                return e

    def arguments(self):
        self.expect_p("(")
        out = []
        while not self.is_p(")"):
            if self.eat_p("..."):
                out.append(("spread", self.assignment()))
            else:
                out.append(self.assignment())
            if not self.eat_p(","):
                break
        self.expect_p(")")
        return out

    def primary(self):
        t = self.next()
        if t.kind == "num":
            return ("num", t.value)
        if t.kind == "str":
            return ("str", t.value)
        if t.kind == "tmpl":
            parts = []
            for p in t.value:
                if isinstance(p, Expr):
                    sub = Parser(p.src)
                    parts.append(sub.expression())
                    if sub.tok.kind != "eof":
                        raise Unsupported("template expression %r" % p.src)
                else:
                    parts.append(("str", p))
            return ("tmpl", parts)
        if t.kind == "regex":
            return ("regex", t.value[0], t.value[1])
        if t.kind == "name":
            v = t.value
            if v in UNSUPPORTED_KEYWORDS:
                raise Unsupported("keyword %s near %r" % (v, self.context()))
            if v == "function":
                self.i -= 1
                return self.function()
            if v == "true":
                return ("lit", True)
            if v == "false":
                return ("lit", False)
            if v == "null":
                return ("lit", NULL)
            return ("name", v)
        if t.kind == "p":
            if t.value == "(":
                e = self.expression()
                self.expect_p(")")
                return e
            if t.value == "[":
                items = []
                while not self.is_p("]"):
                    if self.is_p(","):
                        self.i += 1
                        items.append(("lit", UNDEF))
                        continue
                    if self.eat_p("..."):
                        items.append(("spread", self.assignment()))
                    else:
                        items.append(self.assignment())
                    if not self.eat_p(","):
                        break
                self.expect_p("]")
                return ("array", items)
            if t.value == "{":
                props = []
                while not self.is_p("}"):
                    if self.eat_p("..."):
                        props.append(("spread", self.assignment()))
                    else:
                        k = self.next()
                        if k.kind == "p" and k.value == "[":
                            key = self.assignment()
                            self.expect_p("]")
                        elif k.kind in ("name", "str"):
                            key = ("str", k.value)
                        elif k.kind == "num":
                            key = ("str", num_to_str(k.value))
                        else:
                            raise Unsupported("object key %r" % (k,))
                        if self.eat_p(":"):
                            props.append(("prop", key, self.assignment()))
                        elif self.is_p("("):
                            raise Unsupported("method shorthand")
                        else:
                            props.append(("prop", key, ("name", k.value)))
                    if not self.eat_p(","):
                        break
                self.expect_p("}")
                return ("object", props)
        raise Unsupported("unexpected %r near %r" % (t, self.context()))


# ---- values ---------------------------------------------------------------------------------------------------------------------------

class JSArray(list):
    pass


class JSObject(dict):
    pass


class JSFunction:
    def __init__(self, node, scope, interp):
        self.node, self.scope, self.interp = node, scope, interp
        self.props = JSObject()


class Native:
    def __init__(self, fn, name="native"):
        self.fn, self.name = fn, name


class JSRegExp:
    def __init__(self, source, flags):
        if flags.replace("g", ""):
            raise Unsupported("regex flags %r" % flags)
        self.source, self.flags = source, flags
        self.re = re.compile(source)
        self.glob = "g" in flags


class Stub:
    """An element or a computed style of the sandbox: any property is another stub, or '' for a style."""

    def __init__(self, style=False):
        self.style = style


def num_to_str(x):
    if x != x:
        return "NaN"
    if x == math.inf:
        return "Infinity"
    if x == -math.inf:
        return "-Infinity"
    if x == 0:
        return "0"
    if x < 0:
        return "-" + num_to_str(-x)
    d = Decimal(repr(x))
    sign, digits, exp = d.as_tuple()
    digits = "".join(str(c) for c in digits).rstrip("0") or "0"
    # value = 0.digits * 10^n
    n = len(digits) + (exp + (len(d.as_tuple().digits) - len(digits.rstrip("0")) if False else 0))
    # recompute from the full tuple: the digits as given, exponent exp, so the value is int(all digits) * 10^exp
    all_digits = "".join(str(c) for c in d.as_tuple().digits)
    n = len(all_digits) + exp
    stripped = all_digits.rstrip("0")
    k = len(stripped)
    s = stripped
    if k <= n <= 21:
        return s + "0" * (n - k)
    if 0 < n <= 21:
        return s[:n] + "." + s[n:]
    if -6 < n <= 0:
        return "0." + "0" * (-n) + s
    e = n - 1
    es = ("+" if e >= 0 else "-") + str(abs(e))
    if k == 1:
        return s + "e" + es
    return s[0] + "." + s[1:] + "e" + es


def to_str(v):
    if isinstance(v, str):
        return v
    if v is UNDEF:
        return "undefined"
    if v is NULL:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, float):
        return num_to_str(v)
    if isinstance(v, JSArray):
        return ",".join("" if (x is UNDEF or x is NULL) else to_str(x) for x in v)
    if isinstance(v, JSObject):
        return "[object Object]"
    if isinstance(v, (JSFunction, Native)):
        return "function"
    if isinstance(v, JSRegExp):
        return "/%s/%s" % (v.source, v.flags)
    return "[object Object]"


def to_num(v):
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    if isinstance(v, float):
        return v
    if v is UNDEF:
        return math.nan
    if v is NULL:
        return 0.0
    if isinstance(v, str):
        s = v.strip()
        if s == "":
            return 0.0
        try:
            if re.fullmatch(r"[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?", s):
                return float(s)
            if re.fullmatch(r"0[xX][0-9a-fA-F]+", s):
                return float(int(s, 16))
            if s in ("Infinity", "+Infinity"):
                return math.inf
            if s == "-Infinity":
                return -math.inf
        except ValueError:
            pass
        return math.nan
    if isinstance(v, JSArray):
        return to_num(to_str(v))
    return math.nan


def truthy(v):
    if v is UNDEF or v is NULL or v is False:
        return False
    if isinstance(v, float):
        return not (v == 0 or v != v)
    if isinstance(v, str):
        return v != ""
    return True


def to_prim(v):
    if isinstance(v, (JSArray, JSObject)):
        return to_str(v)
    return v


def loose_eq(a, b):
    if (a is UNDEF or a is NULL) and (b is UNDEF or b is NULL):
        return True
    if a is UNDEF or a is NULL or b is UNDEF or b is NULL:
        return False
    if type(a) == type(b) and not isinstance(a, (float,)):
        return a == b if isinstance(a, (str, bool)) else a is b
    if isinstance(a, float) and isinstance(b, float):
        return a == b
    if isinstance(a, (str, float, bool)) and isinstance(b, (str, float, bool)):
        return to_num(a) == to_num(b)
    return strict_eq(to_prim(a), to_prim(b)) if isinstance(a, (JSArray, JSObject)) or isinstance(b, (JSArray, JSObject)) else a is b


def strict_eq(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, float) and isinstance(b, float):
        return a == b
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    return a is b


class Scope:
    def __init__(self, parent=None):
        self.vars = {}
        self.parent = parent

    def lookup(self, name):
        s = self
        while s is not None:
            if name in s.vars:
                return s
            s = s.parent
        return None

    def get(self, name):
        s = self.lookup(name)
        if s is None:
            raise JSError("ReferenceError", "%s is not defined" % name)
        return s.vars[name]

    def set(self, name, value):
        s = self.lookup(name)
        if s is None:
            raise JSError("ReferenceError", "%s is not defined" % name)
        s.vars[name] = value


# ---- the interpreter ----------------------------------------------------------------------------------------------------------------

class Interp:
    def __init__(self, globals_):
        self.root = Scope()
        self.root.vars.update(globals_)
        self.steps = 0

    def run_program(self, src):
        stmts = Parser(src).program()
        self.hoist(stmts, self.root, function_level=True)
        for s in stmts:
            self.exec(s, self.root)

    # hoisting

    def hoist(self, stmts, scope, function_level):
        for s in stmts:
            if s[0] == "funcdecl":
                scope.vars[s[1]] = JSFunction(s[2], scope, self)
            elif function_level:
                self.hoist_vars(s, scope)

    def hoist_vars(self, s, scope):
        kind = s[0]
        if kind == "decl" and s[1] == "var":
            for target, _ in s[2]:
                for name in pattern_names(target):
                    scope.vars.setdefault(name, UNDEF)
        elif kind == "block":
            for x in s[1]:
                self.hoist_vars(x, scope)
        elif kind == "if":
            self.hoist_vars(s[2], scope)
            if s[3]:
                self.hoist_vars(s[3], scope)
        elif kind in ("for",):
            if s[1]:
                self.hoist_vars(s[1], scope)
            self.hoist_vars(s[4], scope)
        elif kind in ("forof", "forin"):
            if s[1] == "var":
                for name in pattern_names(s[2]):
                    scope.vars.setdefault(name, UNDEF)
            self.hoist_vars(s[4], scope)
        elif kind == "while":
            self.hoist_vars(s[2], scope)
        elif kind == "try":
            for part in (s[1], s[3], s[4]):
                if part:
                    self.hoist_vars(part, scope)

    # statements

    def exec(self, s, scope):
        self.steps += 1
        if self.steps > 50_000_000:
            raise JSError("Error", "script took too long")
        k = s[0]
        if k == "expr":
            self.eval(s[1], scope)
        elif k == "decl":
            for target, init in s[2]:
                if init is None:
                    if s[1] == "var":
                        if scope.lookup(target[1]) is None:
                            scope.vars[target[1]] = UNDEF
                        continue
                    value = UNDEF
                else:
                    value = self.eval(init, scope)
                    if init[0] == "func" and target[0] == "name" and init[1] is None:
                        pass
                self.bind(target, value, scope, s[1])
        elif k == "funcdecl":
            scope.vars[s[1]] = JSFunction(s[2], scope, self)
        elif k == "block":
            inner = Scope(scope)
            self.hoist(s[1], inner, False)
            for x in s[1]:
                self.exec(x, inner)
        elif k == "if":
            if truthy(self.eval(s[1], scope)):
                self.exec(s[2], scope)
            elif s[3] is not None:
                self.exec(s[3], scope)
        elif k == "return":
            raise Return(UNDEF if s[1] is None else self.eval(s[1], scope))
        elif k == "empty":
            pass
        elif k == "for":
            inner = Scope(scope)
            if s[1] is not None:
                self.exec(s[1], inner)
            while s[2] is None or truthy(self.eval(s[2], inner)):
                try:
                    self.exec(s[4], inner)
                except Break:
                    break
                except Continue:
                    pass
                if s[3] is not None:
                    self.eval(s[3], inner)
        elif k == "forof" or k == "forin":
            it = self.eval(s[3], scope)
            if k == "forof":
                if isinstance(it, str):
                    seq = list(it)
                elif isinstance(it, JSArray):
                    seq = list(it)
                else:
                    raise JSError("TypeError", "not iterable")
            else:
                if isinstance(it, JSArray):
                    seq = [num_to_str(float(i)) for i in range(len(it))]
                elif isinstance(it, JSObject):
                    seq = list(it.keys())
                else:
                    seq = []
            for item in seq:
                inner = Scope(scope)
                if s[1] is None:
                    self.assign_to(s[2][1], item, inner)
                else:
                    self.bind(s[2], item, inner, s[1])
                try:
                    self.exec(s[4], inner)
                except Break:
                    break
                except Continue:
                    continue
        elif k == "while":
            while truthy(self.eval(s[1], scope)):
                try:
                    self.exec(s[2], scope)
                except Break:
                    break
                except Continue:
                    continue
        elif k == "break":
            raise Break()
        elif k == "continue":
            raise Continue()
        elif k == "try":
            try:
                try:
                    self.exec(s[1], scope)
                except JSError as e:
                    if s[3] is None:
                        raise
                    inner = Scope(scope)
                    if s[2] is not None:
                        self.bind(s[2], JSObject({"message": str(e), "name": e.kind}), inner, "let")
                    self.exec(s[3], inner)
            finally:
                if s[4] is not None:
                    self.exec(s[4], scope)
        else:
            raise Unsupported("statement " + k)

    # bindings

    def bind(self, target, value, scope, kind):
        k = target[0]
        if k == "name":
            if kind == "var":
                s = scope.lookup(target[1])
                if s is not None:
                    s.vars[target[1]] = value
                    return
            scope.vars[target[1]] = value
        elif k == "arrpat":
            if value is UNDEF or value is NULL:
                raise JSError("TypeError", "not iterable")
            seq = list(value) if isinstance(value, (JSArray, str)) else None
            if seq is None:
                raise JSError("TypeError", "not iterable")
            for idx, item in enumerate(target[1]):
                if item is None:
                    continue
                if item[0] == "rest":
                    self.bind(item[1], JSArray(seq[idx:]), scope, kind)
                    break
                v = seq[idx] if idx < len(seq) else UNDEF
                if v is UNDEF and item[2] is not None:
                    v = self.eval(item[2], scope)
                self.bind(item[1], v, scope, kind)
        elif k == "objpat":
            if value is UNDEF or value is NULL:
                raise JSError("TypeError", "cannot destructure")
            for item in target[1]:
                if item[0] == "rest":
                    raise Unsupported("object rest")
                v = self.get_prop(value, item[1])
                if v is UNDEF and item[3] is not None:
                    v = self.eval(item[3], scope)
                self.bind(item[2], v, scope, kind)
        else:
            raise Unsupported("binding " + k)

    def assign_to(self, target, value, scope):
        k = target[0]
        if k == "name":
            s = scope.lookup(target[1])
            if s is None:
                self.root.vars[target[1]] = value  # an implicit global
            else:
                s.vars[target[1]] = value
        elif k == "member":
            obj = self.eval(target[1], scope)
            key = self.prop_key(self.eval(target[2], scope))
            self.set_prop(obj, key, value)
        else:
            raise Unsupported("assignment target " + k)

    # expressions

    def eval(self, e, scope):
        k = e[0]
        if k == "num":
            return e[1]
        if k == "str":
            return e[1]
        if k == "lit":
            return e[1]
        if k == "name":
            if e[1] == "undefined":
                return UNDEF
            return scope.get(e[1])
        if k == "tmpl":
            out = []
            for p in e[1]:
                v = self.eval(p, scope)
                out.append(v if isinstance(v, str) else to_str(to_prim(v)))
            return "".join(out)
        if k == "array":
            out = JSArray()
            for it in e[1]:
                if it[0] == "spread":
                    v = self.eval(it[1], scope)
                    out.extend(list(v) if isinstance(v, (JSArray, str)) else [])
                else:
                    out.append(self.eval(it, scope))
            return out
        if k == "object":
            out = JSObject()
            for p in e[1]:
                if p[0] == "spread":
                    v = self.eval(p[1], scope)
                    if isinstance(v, JSObject):
                        out.update(v)
                else:
                    out[self.prop_key(self.eval(p[1], scope))] = self.eval(p[2], scope)
            return out
        if k == "regex":
            return JSRegExp(e[1], e[2])
        if k == "func":
            return JSFunction(e, scope, self)
        if k == "member":
            obj = self.eval(e[1], scope)
            return self.get_prop(obj, self.prop_key(self.eval(e[2], scope)))
        if k == "call":
            return self.call_expr(e, scope)
        if k == "unary":
            v = self.eval(e[2], scope)
            op = e[1]
            if op == "!":
                return not truthy(v)
            if op == "-":
                return -to_num(v)
            if op == "+":
                return to_num(v)
            raise Unsupported("unary " + op)
        if k == "typeof":
            if e[1][0] == "name" and scope.lookup(e[1][1]) is None and e[1][1] != "undefined":
                return "undefined"
            v = self.eval(e[1], scope)
            if v is UNDEF:
                return "undefined"
            if v is NULL:
                return "object"
            if isinstance(v, bool):
                return "boolean"
            if isinstance(v, float):
                return "number"
            if isinstance(v, str):
                return "string"
            if isinstance(v, (JSFunction, Native)):
                return "function"
            return "object"
        if k == "bin":
            return self.binary(e, scope)
        if k == "cond":
            return self.eval(e[2] if truthy(self.eval(e[1], scope)) else e[3], scope)
        if k == "assign":
            return self.assign(e, scope)
        if k == "update":
            target = e[3]
            old = to_num(self.eval(target, scope))
            new = old + (1.0 if e[1] == "++" else -1.0)
            self.assign_to(target, new, scope)
            return new if e[2] else old
        if k == "seq":
            v = UNDEF
            for x in e[1]:
                v = self.eval(x, scope)
            return v
        raise Unsupported("expression " + k)

    def prop_key(self, v):
        if isinstance(v, float):
            return num_to_str(v)
        return to_str(v)

    def binary(self, e, scope):
        op = e[1]
        if op == "&&":
            a = self.eval(e[2], scope)
            return self.eval(e[3], scope) if truthy(a) else a
        if op == "||":
            a = self.eval(e[2], scope)
            return a if truthy(a) else self.eval(e[3], scope)
        if op == "??":
            a = self.eval(e[2], scope)
            return self.eval(e[3], scope) if (a is UNDEF or a is NULL) else a
        a = self.eval(e[2], scope)
        b = self.eval(e[3], scope)
        if op == "+":
            a, b = to_prim(a), to_prim(b)
            if isinstance(a, str) or isinstance(b, str):
                return to_str(a) + to_str(b)
            return to_num(a) + to_num(b)
        if op in ("-", "*", "/", "%", "**"):
            x, y = to_num(a), to_num(b)
            if op == "-":
                return x - y
            if op == "*":
                return x * y
            if op == "/":
                if y == 0:
                    return math.nan if (x == 0 or x != x) else math.copysign(math.inf, x) * math.copysign(1.0, y)
                return x / y
            if op == "%":
                return math.nan if (y == 0 or x != x or y != y or math.isinf(x)) else math.fmod(x, y)
            try:
                return x ** y
            except (OverflowError, ZeroDivisionError):
                return math.inf
        if op in ("<", ">", "<=", ">="):
            a, b = to_prim(a), to_prim(b)
            if isinstance(a, str) and isinstance(b, str):
                x, y = a, b
            else:
                x, y = to_num(a), to_num(b)
                if x != x or y != y:
                    return False
            return {"<": x < y, ">": x > y, "<=": x <= y, ">=": x >= y}[op]
        if op == "===":
            return strict_eq(a, b)
        if op == "!==":
            return not strict_eq(a, b)
        if op == "==":
            return loose_eq(a, b)
        if op == "!=":
            return not loose_eq(a, b)
        raise Unsupported("operator " + op)

    def assign(self, e, scope):
        op, target = e[1], e[2]
        if op == "=":
            value = self.eval(e[3], scope)
        else:
            cur = self.eval(target, scope)
            rhs = self.eval(e[3], scope)
            value = self.binary(("bin", op[:-1], ("lit", cur), ("lit", rhs)), scope)
        self.assign_to(target, value, scope)
        return value

    # properties

    def get_prop(self, obj, key):
        if obj is UNDEF or obj is NULL:
            raise JSError("TypeError", "cannot read property %r of %s" % (key, to_str(obj)))
        if isinstance(obj, Stub):
            if obj.style:
                return ""
            if key == "style":
                return Stub(style=True)
            if key == "id":
                return "target"
            return Native(lambda this, args: Stub(), "stub")
        if isinstance(obj, str):
            if key == "length":
                return float(len(obj))
            if key.isdigit():
                i = int(key)
                return obj[i] if i < len(obj) else UNDEF
            return self.string_method(obj, key)
        if isinstance(obj, JSArray):
            if key == "length":
                return float(len(obj))
            if key.isdigit():
                i = int(key)
                return obj[i] if i < len(obj) else UNDEF
            return self.array_method(obj, key)
        if isinstance(obj, JSObject):
            return obj.get(key, UNDEF)
        if isinstance(obj, (JSFunction, Native)):
            return UNDEF
        return UNDEF

    def set_prop(self, obj, key, value):
        if isinstance(obj, Stub):
            return
        if isinstance(obj, JSObject):
            obj[key] = value
        elif isinstance(obj, JSArray):
            if key.isdigit():
                i = int(key)
                while len(obj) <= i:
                    obj.append(UNDEF)
                obj[i] = value
            elif key == "length":
                raise Unsupported("array length assignment")
        elif obj is UNDEF or obj is NULL:
            raise JSError("TypeError", "cannot set property of " + to_str(obj))

    def string_method(self, s, name):
        def replace(this, args):
            pat, rep = args[0], args[1] if len(args) > 1 else UNDEF
            if not isinstance(rep, str):
                raise Unsupported("replace with a function")
            if "$" in rep:
                raise Unsupported("replace pattern with $")
            if isinstance(pat, JSRegExp):
                return pat.re.sub(lambda m: rep, this, count=0 if pat.glob else 1)
            return this.replace(to_str(pat), rep, 1)

        table = {
            "replace": replace,
            "toString": lambda this, args: this,
            "toLowerCase": lambda this, args: this.lower(),
            "toUpperCase": lambda this, args: this.upper(),
            "trim": lambda this, args: this.strip(),
            "indexOf": lambda this, args: float(this.find(to_str(args[0]))),
            "includes": lambda this, args: to_str(args[0]) in this,
            "startsWith": lambda this, args: this.startswith(to_str(args[0])),
            "endsWith": lambda this, args: this.endswith(to_str(args[0])),
            "slice": lambda this, args: this[slice_index(args, len(this))],
            "repeat": lambda this, args: this * int(to_num(args[0])),
            "split": lambda this, args: JSArray(this.split(to_str(args[0])) if to_str(args[0]) != "" else list(this)),
            "concat": lambda this, args: this + "".join(to_str(a) for a in args),
        }
        if name in table:
            return Native(table[name], name)
        raise Unsupported("string method %s" % name)

    def array_method(self, a, name):
        interp = self

        def push(this, args):
            this.extend(args)
            return float(len(this))

        def join(this, args):
            sep = "," if not args or args[0] is UNDEF else to_str(args[0])
            return sep.join("" if (x is UNDEF or x is NULL) else to_str(x) for x in this)

        def for_each(this, args):
            for i, x in enumerate(list(this)):
                interp.call(args[0], UNDEF, [x, float(i), this])
            return UNDEF

        def map_(this, args):
            return JSArray(interp.call(args[0], UNDEF, [x, float(i), this]) for i, x in enumerate(list(this)))

        def filter_(this, args):
            return JSArray(x for i, x in enumerate(list(this)) if truthy(interp.call(args[0], UNDEF, [x, float(i), this])))

        def concat(this, args):
            out = JSArray(this)
            for x in args:
                if isinstance(x, JSArray):
                    out.extend(x)
                else:
                    out.append(x)
            return out

        table = {
            "push": push, "join": join, "forEach": for_each, "map": map_, "filter": filter_, "concat": concat,
            "slice": lambda this, args: JSArray(this[slice_index(args, len(this))]),
            "toString": lambda this, args: join(this, []),
            "includes": lambda this, args: any(strict_eq(x, args[0]) for x in this),
            "indexOf": lambda this, args: float(next((i for i, x in enumerate(this) if strict_eq(x, args[0])), -1)),
        }
        if name in table:
            return Native(table[name], name)
        raise Unsupported("array method %s" % name)

    # calls

    def call_expr(self, e, scope):
        callee = e[1]
        this = UNDEF
        if callee[0] == "member":
            this = self.eval(callee[1], scope)
            fn = self.get_prop(this, self.prop_key(self.eval(callee[2], scope)))
        else:
            fn = self.eval(callee, scope)
        args = []
        for a in e[2]:
            if a[0] == "spread":
                v = self.eval(a[1], scope)
                args.extend(list(v) if isinstance(v, (JSArray, str)) else [])
            else:
                args.append(self.eval(a, scope))
        return self.call(fn, this, args)

    def call(self, fn, this, args):
        if isinstance(fn, Native):
            return fn.fn(this, args)
        if not isinstance(fn, JSFunction):
            raise JSError("TypeError", "%s is not a function" % to_str(fn))
        node = fn.node
        scope = Scope(fn.scope)
        scope.vars["arguments"] = JSArray(args)
        if node[1] is not None and not node[4]:
            scope.vars.setdefault(node[1], fn)
        for idx, p in enumerate(node[2]):
            if p[0] == "rest":
                self.bind(p[1], JSArray(args[idx:]), scope, "let")
                break
            v = args[idx] if idx < len(args) else UNDEF
            if v is UNDEF and p[2] is not None:
                v = self.eval(p[2], scope)
            self.bind(p[1], v, scope, "let")
        body = node[3][1]
        self.hoist(body, scope, True)
        try:
            for s in body:
                self.exec(s, scope)
        except Return as r:
            return r.value
        return UNDEF


def pattern_names(t):
    if t[0] == "name":
        return [t[1]]
    out = []
    if t[0] == "arrpat":
        for it in t[1]:
            if it is None:
                continue
            out += pattern_names(it[1])
    elif t[0] == "objpat":
        for it in t[1]:
            out += pattern_names(it[1] if it[0] == "rest" else it[2])
    return out


def slice_index(args, n):
    a = 0 if not args or args[0] is UNDEF else int(to_num(args[0]))
    b = n if len(args) < 2 or args[1] is UNDEF else int(to_num(args[1]))
    return slice(a, b)


# ---- the sandbox and the vectors -------------------------------------------------------------------------------------------------------

RECORDERS = re.compile(r"\b(test_valid_value|test_invalid_value|test_specified_serialization|test_math_specified|fuzzy_test_valid_color)\b")
MATH_PROP = {"number": "scale", "integer": "z-index", "length": "flex-basis", "angle": "rotate", "time": "transition-delay",
             "resolution": "image-resolution", "flex": "grid-template-rows"}


def files(root):
    out = []
    vals = os.path.join(root, "css/css-values")
    for f in sorted(os.listdir(vals)):
        if f.endswith(".html") and not f.endswith("-ref.html"):
            out.append(os.path.join(vals, f))
    col = os.path.join(root, "css/css-color/parsing")
    for f in sorted(os.listdir(col)):
        if f.endswith(".html") and re.search(r"valid|alpha-color-parsing", f):
            out.append(os.path.join(col, f))
    return out


def js_get(o, key, default=UNDEF):
    return o.get(key, default) if isinstance(o, JSObject) else default


def make_globals(record, interp_holder):
    noop = Native(lambda this, args: UNDEF, "noop")

    def test(this, args):
        try:
            interp_holder[0].call(args[0], UNDEF, [])
        except JSError:
            pass  # page code that needs a browser
        return UNDEF

    def test_valid_value(this, args):
        prop, value = args[0], args[1]
        exp = to_str(value) if len(args) < 3 else args[2]
        alts = []
        if isinstance(exp, JSArray):
            alts = list(exp[1:])
            exp = exp[0] if exp else UNDEF
        rec = {"kind": "valid", "prop": prop, "value": to_str(value), "expected": exp, "alts": "\u0001".join(to_str(a) for a in alts)}
        record(rec)
        return UNDEF

    def test_invalid_value(this, args):
        record({"kind": "invalid", "prop": args[0] if args else UNDEF, "value": to_str(args[1]) if len(args) > 1 else "undefined"})
        return UNDEF

    def test_specified_serialization(this, args):
        record({"kind": "valid", "prop": args[0], "value": to_str(args[1]), "expected": args[2] if len(args) > 2 else UNDEF})
        return UNDEF

    def test_math_specified(this, args):
        t = args[0] if args else UNDEF
        e = args[1] if len(args) > 1 else UNDEF
        opts = args[2] if len(args) > 2 and truthy(args[2]) else JSObject()
        typ = js_get(opts, "type")
        typ = "length" if typ is UNDEF else typ
        prop = js_get(opts, "prop")
        if not truthy(prop):
            prop = MATH_PROP.get(typ, UNDEF) if isinstance(typ, str) else UNDEF
        if not truthy(prop):
            return UNDEF
        approx = js_get(opts, "approx")
        record({"kind": "equiv", "prop": prop, "value": to_str(t), "expected": to_str(e), "approx": approx if truthy(approx) else 0.0})
        return UNDEF

    def fuzzy_test_valid_color(this, args):
        specified = args[0]
        serialized = args[1] if len(args) > 1 else UNDEF
        epsilon = args[2] if len(args) > 2 else UNDEF
        if not truthy(serialized):
            serialized = specified
        record({"kind": "valid", "prop": "color", "value": to_str(specified), "expected": serialized, "epsilon": epsilon if truthy(epsilon) else 0.01})
        return UNDEF

    doc = Stub()
    g = {
        "document": doc, "window": JSObject(), "console": JSObject({"log": noop, "error": noop, "warn": noop}),
        "CSS": JSObject({"supports": Native(lambda this, args: True, "supports"), "registerProperty": noop}),
        "getComputedStyle": Native(lambda this, args: Stub(style=True), "getComputedStyle"),
        "test": Native(test, "test"), "promise_test": noop, "async_test": noop, "setup": noop, "done": noop, "add_completion_callback": noop,
        "assert_equals": noop, "assert_not_equals": noop, "assert_true": noop, "assert_false": noop, "assert_array_equals": noop,
        "assert_approx_equals": noop, "assert_throws_js": noop, "assert_in_array": noop, "assert_array_approx_equals": noop,
        "requestAnimationFrame": noop, "setTimeout": noop, "Exception": noop,
        "test_valid_value": Native(test_valid_value), "test_invalid_value": Native(test_invalid_value),
        "test_specified_serialization": Native(test_specified_serialization), "test_math_specified": Native(test_math_specified),
        "fuzzy_test_valid_color": Native(fuzzy_test_valid_color),
        "test_math_used": noop, "test_math_computed": noop, "test_computed_value": noop, "fuzzy_test_computed_color": noop,
    }
    return g


def to_json_value(v):
    if v is UNDEF:
        return None
    if isinstance(v, bool):
        return v
    if isinstance(v, float):
        if v != v or math.isinf(v):
            return None
        return int(v) if v == int(v) and abs(v) < 1e21 else v
    if isinstance(v, (str, int)):
        return v
    if v is NULL:
        return None
    if isinstance(v, JSArray):
        return [to_json_value(x) for x in v]
    if isinstance(v, JSObject):
        return {k: to_json_value(x) for k, x in v.items() if x is not UNDEF}
    return None


def dump(v):
    """JSON.stringify: a number is written as JavaScript writes it (0.000001, not 1e-06)."""
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, float):
        return num_to_str(v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    if isinstance(v, list):
        return "[" + ",".join(dump(x) for x in v) + "]"
    return "{" + ",".join(json.dumps(k, ensure_ascii=False) + ":" + dump(x) for k, x in v.items()) + "}"


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: extract-wpt-values.py WPT_ROOT OUT.json\n")
        return 2
    root, out_file = argv[1], argv[2]
    vectors = []
    state = {"current": None, "counter": 0}

    def record(v):
        entry = {"file": state["current"], "n": state["counter"]}
        state["counter"] += 1
        for k, x in v.items():
            if x is not UNDEF:
                entry[k] = to_json_value(x)
        vectors.append(entry)

    pages = files(root)
    for path in pages:
        state["current"] = os.path.relpath(path, root)
        state["counter"] = 0
        with open(path, encoding="utf-8") as fh:
            html = fh.read()
        holder = [None]
        interp = Interp(make_globals(record, holder))
        holder[0] = interp
        for m in re.finditer(r"<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)</script>", html):
            try:
                interp.run_program(m.group(1))
            except JSError:
                pass  # a script that needs the browser itself stops here; what it recorded before stays
            except (Return, Break, Continue):
                pass
            except Unsupported as e:
                # A script that never names a recorder cannot record anything, whatever its syntax is: it is skipped. One that names a recorder
                # and cannot be run would change the vectors, so it is an error.
                if RECORDERS.search(m.group(1)):
                    sys.stderr.write("%s: JavaScript this interpreter does not have: %s\n" % (state["current"], e))
                    return 1
            except RecursionError:
                sys.stderr.write("%s: the script nests too deeply for the interpreter\n" % state["current"])
                return 1
    with open(out_file, "w", encoding="utf-8") as fh:
        fh.write(dump(vectors) + "\n")
    print("%d vectors from %d pages" % (len(vectors), len(pages)))
    return 0


if __name__ == "__main__":
    sys.setrecursionlimit(20000)
    sys.exit(main(sys.argv))
