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
"""Extract the tables of web-platform-tests' css/css-syntax into syntax vectors.

    tools/css/extract-wpt.py <wpt checkout> <out.json>

The tests are browser tests: each reads its table through an API above
syntax (a selector, a property, a custom property's value). This turns each
table into what the syntax layer must do for the assertion to hold, and
nothing more; the translation of each file is below, beside its regular
expressions, so it can be argued with. The files read:

  anb-parsing.html, anb-serialization.html   (An+B)
      testANB(input, expected): the input, read as a list of component
      values (it is the contents of `:nth-child()`), is an <an-plus-b> whose
      serialization (9.1) is `expected`, or is not one ("parse error").

  decimal-points-in-numbers.html   (decimals)
      roundTripNumber / roundTripDimension: `line-height` and `width` take
      one number or one dimension. Equal: the input is one numeric token
      whose CSSOM serialization is the expected text. Not equal: the input's
      CSSOM serialization is not the expected text.

  input-preprocessing.html   (preprocessing)
      testParsing(input, expected) through `selectorText`: the input is one
      ident token (a type selector) whose value, and whose serialization as
      an identifier, is `expected`. A lone surrogate in the JavaScript string
      is given to the parser as WTF-8, with the option that reads it as one.

  non-ascii-codepoints.html   (non-ASCII)
      testValidRanges: each valid code point makes "f" cp "oo" one ident
      token; each invalid one makes it three tokens, ident "f", the code
      point (a delim) and ident "oo". The file's list has a missing comma
      after `[0x203f, 0x2040]`, so in a browser the expression indexes that
      array and yields undefined, and the harness throws at that entry before
      any invalid code point is tested. The ranges are taken as the source
      writes them, which is the list of the standard's definition.

  escaped-eof.html   (escapes)
      The `#123` / `#foo\\` pair: their hash tokens' type flags; and each of
      the four stylesheets: the serialization of the first declaration's
      value is the expected `getPropertyValue("--foo")`.

  declarations-trim-whitespace.html   (whitespace)
      Each custom property of the rule, in a rule of its own: its value
      serialized is the canonical "bar" (no white space is kept around it).

  custom-property-rule-ambiguity.html, trailing-braces.html,
  var-with-blocks.html   (nesting: declarations and rules in one block)
      The structure of each style sheet, as far as the assertion reads it:
      the rules (by prelude) and, for a property the test reads, the last
      declaration of that name and its value (the declared value CSSOM gives
      is the last valid one; a declaration the syntax drops is not there).
      stylesheet4 of the first file, and invalid-nested-rules.html, need a
      grammar to call a declaration or a selector invalid, which is above
      syntax; they are held by the unit tests with a validity callback.

  serialize-escape-identifiers.html   (escapes, serialization)
      The stylesheet of the test: parse, serialize and parse gives the same
      tree, and serializing again gives the same text.

Every vector names its file and its position in the file. The output records
the WPT commit it was read from.
"""

import json
import re
import subprocess
import sys


def js_string(lit):
    """The value of a JavaScript string literal's body (escapes \\xHH, \\uHHHH, \\\\, \\", \\')."""
    out = []
    i = 0
    while i < len(lit):
        c = lit[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        n = lit[i + 1]
        if n == 'x':
            out.append(chr(int(lit[i + 2:i + 4], 16)))
            i += 4
        elif n == 'u':
            out.append(chr(int(lit[i + 2:i + 6], 16)))
            i += 6
        elif n == 'n':
            out.append('\n')
            i += 2
        else:
            out.append(n)
            i += 2
    return ''.join(out)


def has_surrogate(s):
    return any(0xD800 <= ord(c) <= 0xDFFF for c in s)


def read(root, name):
    with open(f'{root}/css/css-syntax/{name}', encoding='utf-8') as f:
        return f.read()


def anb(root, name, vectors):
    text = read(root, name)
    for k, m in enumerate(re.finditer(r'testANB\("((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\);', text)):
        expected = js_string(m.group(2))
        vectors.append({
            'id': f'wpt:{name}#{k}',
            'entry': 'anb',
            'input': js_string(m.group(1)),
            'check': {'kind': 'anb', 'expect': None if expected == 'parse error' else expected},
        })


def decimals(root, vectors):
    name = 'decimal-points-in-numbers.html'
    text = read(root, name)
    pat = r'assert_(not_)?equals\(roundTrip(Number|Dimension)\("((?:[^"\\]|\\.)*)"\), "((?:[^"\\]|\\.)*)"\);'
    for k, m in enumerate(re.finditer(pat, text)):
        vectors.append({
            'id': f'wpt:{name}#{k}',
            'entry': 'component_values',
            'input': js_string(m.group(3)),
            'check': {
                'kind': 'numeric',
                'type': 'number' if m.group(2) == 'Number' else 'dimension',
                'equals': m.group(1) is None,
                'cssom': js_string(m.group(4)),
            },
        })


def preprocessing(root, vectors):
    name = 'input-preprocessing.html'
    text = read(root, name)
    for k, m in enumerate(re.finditer(r'testParsing\("((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\);', text)):
        inp = js_string(m.group(1))
        expected = js_string(m.group(2))
        vectors.append({
            'id': f'wpt:{name}#{k}',
            'entry': 'component_values',
            'input': inp,
            'wtf8': has_surrogate(inp),
            'check': {'kind': 'result', 'expect': [['ident', expected]], 'cssom': expected},
        })


def non_ascii(root, vectors):
    name = 'non-ascii-codepoints.html'
    text = read(root, name)
    m = re.search(r'testValidRanges\(\[(.*?)\]\);', text, re.S)
    if not m:
        raise SystemExit(f'{name}: testValidRanges([...]) not found')
    body = m.group(1)
    ranges = []
    pos = 0
    item = re.compile(r'\s*(?:\[\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\]|(0x[0-9a-fA-F]+))\s*,?')
    while pos < len(body.rstrip()):
        im = item.match(body, pos)
        if not im:
            raise SystemExit(f'{name}: cannot read the range list at {body[pos:pos + 20]!r}')
        if im.group(3):
            ranges.append(int(im.group(3), 16))
        else:
            ranges.append((int(im.group(1), 16), int(im.group(2), 16)))
        pos = im.end()
    # The harness's own two loops (testValidRanges), with testInvalid's skip above 0x1ffff.
    valid, invalid = [], []
    for r in ranges:
        if isinstance(r, int):
            valid.append(r)
            continue
        valid.append(r[0])
        if r[1] - r[0] > 1:
            valid.append((r[0] + r[1]) // 2)
        valid.append(r[1])

    def test_invalid(cp):
        if cp <= 0x1ffff:
            invalid.append(cp)

    test_invalid(0x80)
    last = 0x80
    for r in ranges:
        if isinstance(r, int):
            if r - 1 != last:
                test_invalid(r - 1)
            test_invalid(r + 1)
            last = r + 1
            continue
        if r[0] - 1 != last:
            test_invalid(r[0] - 1)
        test_invalid(r[1] + 1)
        last = r[1] + 1
    k = 0
    for cp in valid:
        s = 'f' + chr(cp) + 'oo'
        vectors.append({
            'id': f'wpt:{name}#{k}', 'entry': 'component_values', 'input': s, 'wtf8': has_surrogate(s),
            'check': {'kind': 'result', 'expect': [['ident', s]]},
        })
        k += 1
    for cp in invalid:
        s = 'f' + chr(cp) + 'oo'
        vectors.append({
            'id': f'wpt:{name}#{k}', 'entry': 'component_values', 'input': s, 'wtf8': has_surrogate(s),
            'check': {'kind': 'result', 'expect': [['ident', 'f'], chr(cp), ['ident', 'oo']]},
        })
        k += 1


def escaped_eof(root, vectors):
    name = 'escaped-eof.html'
    text = read(root, name)
    if 'document.querySelector("#123")' not in text or 'document.querySelector("#foo\\\\")' not in text:
        raise SystemExit(f'{name}: the hash assertions are not where they were')
    vectors.append({'id': f'wpt:{name}#hash-0', 'entry': 'component_values', 'input': '#123',
                    'check': {'kind': 'result', 'expect': [['hash', '123', 'unrestricted']]}})
    vectors.append({'id': f'wpt:{name}#hash-1', 'entry': 'component_values', 'input': '#foo\\',
                    'check': {'kind': 'result', 'expect': [['hash', 'foo�', 'id']]}})
    sheets = re.findall(r'<style>(.*?)</style>', text, re.S)
    expects = re.findall(r'assert_equals\((.*?), val\);', text)
    if len(sheets) != 4 or len(expects) != 4:
        raise SystemExit(f'{name}: expected four style sheets and four values, read {len(sheets)} and {len(expects)}')
    for k, (sheet, exp) in enumerate(zip(sheets, expects)):
        exp = exp.strip()
        if exp.startswith('`') and exp.endswith('`'):
            value = exp[1:-1]
        elif exp.startswith('"') and exp.endswith('"'):
            value = js_string(exp[1:-1])
        else:
            raise SystemExit(f'{name}: cannot read {exp!r}')
        vectors.append({'id': f'wpt:{name}#{k}', 'entry': 'declaration_value', 'input': sheet,
                        'check': {'kind': 'serial', 'expect': value}})


def escape_identifiers(root, vectors):
    name = 'serialize-escape-identifiers.html'
    text = read(root, name)
    m = re.search(r'<style id="sheet">(.*?)</style>', text, re.S)
    if not m:
        raise SystemExit(f'{name}: the style sheet is not where it was')
    vectors.append({'id': f'wpt:{name}#0', 'entry': 'stylesheet', 'input': m.group(1),
                    'check': {'kind': 'roundtrip'}})
    # And each rule on its own, so a failure names it.
    for k, line in enumerate(l.strip() for l in m.group(1).splitlines() if l.strip()):
        vectors.append({'id': f'wpt:{name}#{k + 1}', 'entry': 'stylesheet', 'input': line,
                        'check': {'kind': 'roundtrip'}})


def trim_whitespace(root, vectors):
    name = 'declarations-trim-whitespace.html'
    text = read(root, name)
    m = re.search(r'#foo \{(.*?)\n\s*\}', text, re.S)
    canonical = re.search(r'let canonical = "(.*?)";', text)
    if not m or not canonical:
        raise SystemExit(f'{name}: the style rule or the canonical value is not where it was')
    lines = [l.strip() for l in m.group(1).splitlines() if l.strip()]
    for k, line in enumerate(lines):
        vectors.append({'id': f'wpt:{name}#{k}', 'entry': 'declaration_value', 'input': '#foo {\n' + line + '\n}',
                        'check': {'kind': 'serial', 'expect': canonical.group(1)}})


def styles(root, name):
    text = read(root, name)
    return text, dict(re.findall(r'<style id=["\']?([a-z_0-9]+)["\']?>(.*?)</style>', text, re.S))


def custom_property_ambiguity(root, vectors):
    name = 'custom-property-rule-ambiguity.html'
    text, sheets = styles(root, name)
    want = ['stylesheet1', 'stylesheet2', 'stylesheet3']
    if any(w not in sheets for w in want) or "'hover { }\\n    .b { }'" not in text:
        raise SystemExit(f'{name}: the style sheets or the --x value are not where they were')
    # stylesheet1 and 2: the rule that looks like a custom property is dropped.
    for k in (0, 1):
        vectors.append({'id': f'wpt:{name}#{k}', 'entry': 'outline', 'input': sheets[want[k]],
                        'check': {'kind': 'outline', 'expect': [{'q': '.a'}, {'q': '.b'}]}})
    # stylesheet3: nested, it is a custom property declaration that runs to the end of the block.
    vectors.append({'id': f'wpt:{name}#2', 'entry': 'outline', 'input': sheets['stylesheet3'],
                    'check': {'kind': 'outline', 'expect': [
                        {'q': 'div', 'r': [{'q': '.a'}, {'n': [['--x', 'hover { }\n    .b { }', False]]}]}]}})


def trailing_braces(root, vectors):
    name = 'trailing-braces.html'
    text, sheets = styles(root, name)
    if 'style' not in sheets or "rules[0].style.color, 'green'" not in text:
        raise SystemExit(f'{name}: the style sheet is not where it was')
    vectors.append({'id': f'wpt:{name}#0', 'entry': 'outline', 'input': sheets['style'],
                    'check': {'kind': 'last-declaration', 'rules': 1, 'expect': [['color', 'green']]}})


def var_with_blocks(root, vectors):
    name = 'var-with-blocks.html'
    text, sheets = styles(root, name)
    k = 0
    for m in re.finditer(r'let rules = ([a-z_0-9]+)\.sheet\.rules;(.*?)\}, ', text, re.S):
        sheet, body = m.group(1), m.group(2)
        expect = []
        for a in re.finditer(r"assert_equals\(declarations\.(color|backgroundColor), '(.*?)'\);", body):
            expect.append(['color' if a.group(1) == 'color' else 'background-color', a.group(2)])
        for a in re.finditer(r"assert_equals\(rules\[0\]\.style\.getPropertyValue\('(--[a-z]+)'\), '(.*?)'\);", body):
            expect.append([a.group(1), a.group(2)])
        if sheet not in sheets or not expect:
            raise SystemExit(f'{name}: cannot read the test of {sheet}')
        vectors.append({'id': f'wpt:{name}#{k}', 'entry': 'outline', 'input': sheets[sheet],
                        'check': {'kind': 'last-declaration', 'rules': 1, 'expect': expect}})
        k += 1
    if k != 14:
        raise SystemExit(f'{name}: read {k} tests, the file had 14')


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    root, out = sys.argv[1], sys.argv[2]
    commit = subprocess.run(['git', '-C', root, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    vectors = []
    anb(root, 'anb-parsing.html', vectors)
    anb(root, 'anb-serialization.html', vectors)
    decimals(root, vectors)
    preprocessing(root, vectors)
    non_ascii(root, vectors)
    escaped_eof(root, vectors)
    escape_identifiers(root, vectors)
    trim_whitespace(root, vectors)
    custom_property_ambiguity(root, vectors)
    trailing_braces(root, vectors)
    var_with_blocks(root, vectors)
    with open(out, 'w', encoding='utf-8') as f:
        json.dump({'wpt_commit': commit, 'vectors': vectors}, f, ensure_ascii=True, indent=0)
    print(f'extract-wpt: {len(vectors)} vectors from css/css-syntax @ {commit}')


if __name__ == '__main__':
    main()
