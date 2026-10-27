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
"""Fail if a source shifts an integer that may have 32 bits by 32 or more places.

    tools/check-size-shifts.py [DIR...]    check src, include and tests (or the directories given); then show the gate fails

`(size_t)1 << 40` is undefined where size_t has 32 bits: the compiler refuses the build for i686 (-Wshift-count-overflow under -Werror), but the
64-bit build, which is the one run most, accepts it. A test once did this twice and nothing noticed until the 32-bit gate ran.

The source is read as tokens (comments, strings and character constants removed; a statement may run over lines). For every `<<` and `<<=` whose count is a
constant of 32 or more (a literal, a parenthesized sum or product of literals, or a name that the file defines as one with #define, enum, constexpr or
const), the type of the left operand is worked out:

  * a literal: wide with `ll`/`LL` or when it does not fit in 32 bits; otherwise narrow (`1`, `1u`, `1UL` and `0x1u` all are on a 32-bit target);
  * a cast (`(size_t)1`, `static_cast<size_t>(1)`, `(unsigned long)x`) is the type it names; `UINT64_C(1)`, `INT64_C(1)` are wide and `UINT32_C`/`SIZE_C`
    narrow; `sizeof` is size_t;
  * a parenthesized expression, or a product, sum or bitwise operation: wide if any operand is wide (the usual conversions), else narrow if any is
    narrow, else unknown;
  * a name: the type of its declaration in the file (`size_t one = 1;`, a parameter, a member), or unknown.

A narrow left operand is refused. An operand of a type the gate cannot see (the result of a call, a member, a name it finds no declaration of) is not: a
stream is shifted by a number just as often, and the i686 gate (suite/tools/xarch) is the compiler that sees the rest. The narrow types are size_t, unsigned, int, long,
unsigned long, short, char, the 8, 16 and 32-bit fixed-width types, uintptr_t, intptr_t, ptrdiff_t and ssize_t; the wide ones are the 64-bit fixed-width
types, long long, unsigned long long, uintmax_t, intmax_t and the C++ types that are one of them.

The gate is then shown to fail: each form that was once missed (`1UL << 40`, `1u << 32`, a cast, `SIZE_C`, a hex literal, a variable, a shift over two
lines, a count that is a #define) is planted and must be refused, a compiler failure is not a pass (there is no compiler in it), and a 64-bit shift,
a shift in a comment or in a string, and a variable shifted by a variable count must be accepted.
"""

import os
import re
import sys
import tempfile

NARROW = {"size_t", "unsigned", "int", "long", "short", "char", "signed", "uintptr_t", "intptr_t", "ptrdiff_t", "ssize_t", "uint8_t", "int8_t",
          "uint16_t", "int16_t", "uint32_t", "int32_t", "uint_least32_t", "int_least32_t", "uint_fast32_t", "int_fast32_t", "wchar_t", "off_t"}
WIDE = {"uint64_t", "int64_t", "uintmax_t", "intmax_t", "uint_least64_t", "int_least64_t", "uint_fast64_t", "int_fast64_t", "double", "float"}
TYPE_WORDS = NARROW | WIDE | {"unsigned", "signed", "const", "volatile", "static", "register", "auto", "constexpr", "struct", "enum", "inline"}


def strip(text):
    """The text with comments, strings and character constants blanked (newlines kept), and the lines that carry a `wide` marker."""
    out = []
    marked = set()
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            if re.search(r"\bwide\b", text[i:j]):
                marked.add(line)
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            body = text[i:j]
            if re.search(r"\bwide\b", body):
                marked.add(line)
            out.append("\n" * body.count("\n"))
            line += body.count("\n")
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append('0' if c == "'" else '""')
            i = j + 1
        else:
            if c == "\n":
                line += 1
            out.append(c)
            i += 1
    return "".join(out), marked


TOKEN = re.compile(r"0[xX][0-9a-fA-F]+[uUlL]*|\d+\.?\d*(?:[eE][+-]?\d+)?[uUlLfF]*|[A-Za-z_]\w*|<<=|<<|>>=|>>|->|&&|\|\||[-+*/%&|^~!<>=?:;,.()\[\]{}#]|\S")


def tokenize(text):
    toks = []
    line = 1
    pos = 0
    for m in TOKEN.finditer(text):
        line += text.count("\n", pos, m.start())
        pos = m.start()
        toks.append((m.group(0), line))
    return toks


def literal(tok):
    """(value, wide) of an integer literal token, or None."""
    m = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)([uUlL]*)", tok)
    if not m:
        return None
    body, suffix = m.groups()
    try:
        value = int(body, 16) if body[:2] in ("0x", "0X") else int(body, 8) if len(body) > 1 and body[0] == "0" else int(body)
    except ValueError:
        return None
    wide = "ll" in suffix.lower() or value > 0xFFFFFFFF
    return value, wide


def defined_constants(toks):
    """name -> integer, for `#define NAME n`, `enum { NAME = n }` and `const|constexpr [type] NAME = n;`, each defined once in the file (a name that is
    defined twice, or that is not a constant anywhere, may be another thing in another scope and is not used)."""
    consts = {}
    counts = {}
    n = len(toks)
    for i in range(n - 2):
        t = toks[i][0]
        if t == "#" and toks[i + 1][0] == "define" and i + 3 < n and toks[i + 2][1] == toks[i + 3][1]:
            name = toks[i + 2][0]
            counts[name] = counts.get(name, 0) + 1
            v = constant(toks, i + 3, consts, same_line=toks[i + 2][1])
            if v is not None:
                consts[name] = v[0]
        if re.fullmatch(r"[A-Za-z_]\w*", t) and toks[i + 1][0] == "=" and i > 0:
            # `const`/`constexpr` within the declaration, or an enumerator
            j = i - 1
            const = False
            while j >= 0 and toks[j][0] not in (";", "{", "}", "(", ")") and i - j < 8:
                const = const or toks[j][0] in ("const", "constexpr")
                j -= 1
            enumerator = toks[i - 1][0] in ("{", ",") and enum_body(toks, i)
            if const or enumerator:
                counts[t] = counts.get(t, 0) + 1
                v = constant(toks, i + 2, consts)
                if v is not None:
                    consts[t] = v[0]
    return {k: v for k, v in consts.items() if counts.get(k) == 1}


def enum_body(toks, i):
    """Whether toks[i] is inside the braces of an enum."""
    depth = 0
    j = i
    while j >= 0:
        if toks[j][0] == "}":
            depth += 1
        elif toks[j][0] == "{":
            if depth == 0:
                return j >= 1 and (toks[j - 1][0] == "enum" or (j >= 2 and toks[j - 2][0] == "enum"))
            depth -= 1
        j -= 1
    return False


def constant(toks, i, consts, same_line=None):
    """Evaluate a constant expression of integer literals, known names, + - * ( ) and << starting at toks[i]. (value, next index) or None."""
    expr = []
    depth = 0
    j = i
    while j < len(toks):
        t, ln = toks[j]
        if same_line is not None and ln != same_line:
            break
        if t == "(":
            depth += 1
        elif t == ")":
            if depth == 0:
                break
            depth -= 1
        elif t in (";", ",", "}", "{", "?", ":") and depth == 0:
            break
        elif t in ("<<", ">>") and depth == 0 and False:
            break
        expr.append(t)
        j += 1
    if not expr:
        return None
    py = []
    for t in expr:
        lit = literal(t)
        if lit is not None:
            py.append(str(lit[0]))
        elif t in consts:
            py.append(str(consts[t]))
        elif t in ("+", "-", "*", "(", ")", "<<", ">>", "|", "&"):
            py.append(t)
        else:
            return None
    try:
        return eval("".join(py), {"__builtins__": {}}, {}), j   # only digits and the operators above reach here
    except (SyntaxError, ValueError, TypeError, OverflowError, MemoryError, ZeroDivisionError):
        return None


def type_of_words(words):
    """'narrow', 'wide' or None for the words of a type name."""
    ws = [w for w in words if w not in ("const", "volatile", "static", "register", "struct", "enum", "inline", "constexpr", "auto", "*", "&")]
    if not ws:
        return None
    if ws.count("long") >= 2 or any(w in WIDE for w in ws):
        return "wide"
    if any(w in NARROW for w in ws):
        return "narrow"
    return None


def declared_types(toks):
    """name -> 'narrow'/'wide' from declarations `type name` (a variable, a parameter, a member), the last wins within the file."""
    types = {}
    n = len(toks)
    for i in range(1, n):
        name = toks[i][0]
        if not re.fullmatch(r"[A-Za-z_]\w*", name) or name in TYPE_WORDS:
            continue
        if toks[i - 1][0] in TYPE_WORDS or toks[i - 1][0] in ("*", "&"):
            j = i - 1
            words = []
            while j >= 0 and (toks[j][0] in TYPE_WORDS or toks[j][0] in ("*", "&")):
                words.append(toks[j][0])
                j -= 1
            if "*" in words or "&" in words and False:
                continue
            k = type_of_words(list(reversed(words)))
            nxt = toks[i + 1][0] if i + 1 < n else ""
            if k and nxt in ("=", ";", ",", ")", "[", "{", ":"):
                types[name] = k
    return types


def merge(a, b):
    if a == "wide" or b == "wide":
        return "wide"
    if a == "narrow" or b == "narrow":
        return "narrow"
    return None


def left_operand(toks, end, types):
    """The type of the operand that ends just before toks[end] (the `<<`): 'narrow', 'wide' or None."""
    i = end - 1
    if i < 0:
        return None
    t = toks[i][0]
    lit = literal(t)
    if lit is not None:
        # a cast before the literal is the type of the operand
        cast = cast_before(toks, i)
        return cast if cast else ("wide" if lit[1] else "narrow")
    if t == ")":
        depth = 0
        j = i
        while j >= 0:
            if toks[j][0] == ")":
                depth += 1
            elif toks[j][0] == "(":
                depth -= 1
                if depth == 0:
                    break
            j -= 1
        if j < 0:
            return None
        before = toks[j - 1][0] if j > 0 else ""
        inner = [x[0] for x in toks[j + 1:i]]
        if before == ">":
            # static_cast<T>(...): the type between the matching < and >
            k = j - 2
            words = []
            while k >= 0 and toks[k][0] != "<":
                words.append(toks[k][0])
                k -= 1
            return type_of_words(list(reversed(words))) or None
        if before in ("UINT64_C", "INT64_C", "UINTMAX_C", "INTMAX_C"):
            return "wide"
        if before in ("UINT32_C", "INT32_C", "SIZE_C", "UINT16_C", "UINT8_C", "INT16_C", "INT8_C"):
            return "narrow"
        if before == "sizeof":
            return "narrow"
        if before not in ("", "(", "[", "{", ",", ";", "=", "return", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "<", ">", "!", "~", "?", ":", "&&", "||", "==", "!=") and re.fullmatch(r"[A-Za-z_]\w*", before):
            return None   # a call, or a macro: its type is not seen here
        # an expression in parentheses: wide if any operand is, else narrow if any is
        found = None
        for p, tok in enumerate(inner):
            lt = literal(tok)
            if lt is not None:
                found = merge(found, "wide" if lt[1] else "narrow")
            elif tok in types:
                found = merge(found, types[tok])
            elif tok == "sizeof":
                found = merge(found, "narrow")
        words = inner
        if words and all(w in TYPE_WORDS or w in ("*",) for w in words):
            return None   # `(type)` alone: a cast, handled by the operand after it
        return found
    if t == "}":
        # T{...}: the type named before the brace
        depth = 0
        j = i
        while j >= 0:
            if toks[j][0] == "}":
                depth += 1
            elif toks[j][0] == "{":
                depth -= 1
                if depth == 0:
                    break
            j -= 1
        words = []
        k = j - 1
        while k >= 0 and (toks[k][0] in TYPE_WORDS):
            words.append(toks[k][0])
            k -= 1
        return type_of_words(list(reversed(words))) if j > 0 else None
    if re.fullmatch(r"[A-Za-z_]\w*", t):
        cast = cast_before(toks, i)
        if cast:
            return cast
        if i >= 1 and toks[i - 1][0] in (".", "->", "::"):
            return None   # a member: its declaration is not necessarily this one's
        return types.get(t)
    return None


def cast_before(toks, i):
    """If toks[i] is preceded by `( type )`, the type it names."""
    if i >= 2 and toks[i - 1][0] == ")":
        j = i - 2
        words = []
        while j >= 0 and toks[j][0] != "(":
            words.append(toks[j][0])
            j -= 1
        words.reverse()
        if j >= 0 and words and all(w in TYPE_WORDS or w == "*" for w in words):
            return type_of_words(words)
    return None


def check_text(text):
    """The findings in a source text: (line, shifted operand description, reason)."""
    stripped, marked = strip(text)
    toks = tokenize(stripped)
    consts = defined_constants(toks)
    types = declared_types(toks)
    found = []
    for i, (t, line) in enumerate(toks):
        if t not in ("<<", "<<="):
            continue
        c = constant(toks, i + 1, consts)
        if c is None or c[0] < 32:
            continue
        kind = left_operand(toks, i, types)
        if kind != "narrow":
            continue   # wide, or a type the gate cannot see (a stream is shifted by a number too): the 32-bit build sees the rest
        found.append((line, " ".join(x[0] for x in toks[max(0, i - 4):i + 3]), "shift by %d of an operand that may have 32 bits" % c[0]))
    return found


def check_dirs(dirs):
    bad = 0
    for d in dirs:
        for dirpath, _, files in os.walk(d):
            for name in sorted(files):
                if not name.endswith((".c", ".cpp", ".h", ".hpp")):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, encoding="utf-8", errors="replace") as handle:
                    findings = check_text(handle.read())
                for line, where, why in findings:
                    sys.stderr.write("%s:%d: %s  [%s]\n" % (path, line, why, where))
                    bad += 1
    return bad


MUST_REFUSE = [
    ("(size_t)1 << 40", "static unsigned long f(void) { return (size_t)1 << 40; }"),
    ("static_cast<size_t>(1) << 33", "static unsigned long f() { return static_cast<size_t>(1) << 33; }"),
    ("1UL << 40", "static unsigned long f(void) { return 1UL << 40; }"),
    ("1u << 32", "static unsigned f(void) { return 1u << 32; }"),
    ("0x1u << 36", "static unsigned f(void) { return 0x1u << 36; }"),
    ("(unsigned long)x << 35", "static unsigned long f(unsigned long x) { return (unsigned long)x << 35; }"),
    ("SIZE_C(1) << 40", "static unsigned long f(void) { return SIZE_C(1) << 40; }"),
    ("sizeof(int) << 33", "static unsigned long f(void) { return sizeof(int) << 33; }"),
    ("a variable", "static unsigned long f(void) { size_t one = 1; return one << 40; }"),
    ("a parameter", "static unsigned long f(size_t one) { return one << 40; }"),
    ("a compound shift", "static void f(size_t one) { one <<= 40; }"),
    ("two lines", "static unsigned long f(void) {\n  return (size_t)1\n      << 40;\n}"),
    ("a #define count", "#define BIG 40\nstatic unsigned long f(void) { return (size_t)1 << BIG; }"),
    ("a sum for the count", "static unsigned long f(void) { return (size_t)1 << (20 + 20); }"),
    ("an enum count", "enum { BITS = 36 };\nstatic unsigned long f(void) { return 1UL << BITS; }"),
    ("a parenthesized product", "static unsigned long f(size_t a) { return (a * 2) << 40; }"),
]

MUST_ACCEPT = [
    ("a 64-bit cast", "static unsigned long long f(void) { return (uint64_t)1 << 40; }"),
    ("an ull literal", "static unsigned long long f(void) { return 1ULL << 40; }"),
    ("a long long cast", "static unsigned long long f(void) { return (unsigned long long)1 << 40; }"),
    ("static_cast to uint64_t", "static unsigned long long f() { return static_cast<uint64_t>(1) << 40; }"),
    ("UINT64_C", "static unsigned long long f(void) { return UINT64_C(1) << 40; }"),
    ("a literal too big for 32 bits", "static unsigned long long f(void) { return 0x100000000 << 8; }"),
    ("a 64-bit variable", "static unsigned long long f(uint64_t x) { return x << 40; }"),
    ("a comment", "/* (size_t)1 << 40 */ // 1UL << 40\nstatic int f(void) { return 0; }"),
    ("a string", "static const char * f(void) { return \"1u << 40\"; }"),
    ("a count below 32", "static unsigned f(void) { return 1u << 31; }"),
    ("a count that is a variable", "static unsigned f(unsigned n) { return 1u << n; }"),
    ("an operand the gate cannot see", "static unsigned long long f(void) { return g() << 40; }"),
    ("a stream", "static void f(void) { std::cout << \"x\" << 40; out << n << 60000; }"),
    ("a constant used as a variable elsewhere", "static unsigned f(void) { for (size_t n = 77; n < 80; n++) { g(); } size_t n = 5; return 1u << (8 * n); }"),
    ("a 64-bit operand of a sum", "static unsigned long long f(uint64_t a) { return (a + 1) << 40; }"),
]


def controls():
    """The gate is shown to fail: every form it once missed is refused, and what is fine is accepted."""
    bad = 0
    for what, src in MUST_REFUSE:
        if not check_text(src):
            sys.stderr.write("the shift gate did not refuse %s\n" % what)
            bad += 1
    for what, src in MUST_ACCEPT:
        found = check_text(src)
        if found:
            sys.stderr.write("the shift gate refused %s: %s\n" % (what, found))
            bad += 1
    return bad


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dirs = argv[1:] or [os.path.join(root, d) for d in ("src", "include", "tests")]
    if controls():
        return 1
    for d in dirs:
        if not os.path.isdir(d):
            sys.stderr.write("%s is not a directory\n" % d)
            return 1
    if check_dirs(dirs):
        sys.stderr.write("use SIZE_MAX / 2, a uint64_t, a width check (sizeof(size_t) > 4 ? ... : ...), or mark a 64-bit operand /* wide */ instead.\n")
        return 1
    print("check-size-shifts: no shift by 32 or more of an operand that may have 32 bits; the gate refused %d planted forms and accepted %d fine ones" %
          (len(MUST_REFUSE), len(MUST_ACCEPT)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
