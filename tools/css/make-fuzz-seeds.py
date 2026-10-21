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
"""Write the seed corpus of tests/fuzz/fuzz_css_syntax.cpp.

    tools/css/make-fuzz-seeds.py [<css-parsing-tests dir>] [<out dir>]

The seeds are the inputs of css-parsing-tests (CC0; the files in scope of the
vector gate and stylesheet_bytes' inputs, read as bytes), each with the entry
point its file names, and a list of this file's own: the nesting shapes, the
custom property and unicode-range readings, every token, every parse error,
deep and wide inputs, and ill-formed UTF-8. Each is written with three option
bytes in front (tests/fuzz/fuzz_css_syntax.cpp says what they mean), and the
own list is written once for each of several option settings, so the tiny
limits, the validity callback, WTF-8 and a failed allocation are seeded too.

Files that are not seeds made here (a reproducer-*.seed a fuzzer found) are
left alone. The output is the same on every run.
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

ENTRY = {
    'stylesheet': 0, 'stylesheet_contents': 1, 'declaration_list': 2, 'block_contents': 3, 'rule': 4,
    'declaration': 5, 'component_value': 6, 'component_values': 7, 'comma_list': 8,
}

SUITE = {
    'component_value_list.json': 'component_values', 'one_component_value.json': 'component_value',
    'declaration_list.json': 'declaration_list', 'blocks_contents.json': 'block_contents',
    'one_declaration.json': 'declaration', 'one_rule.json': 'rule', 'rule_list.json': 'stylesheet_contents',
    'stylesheet.json': 'stylesheet', 'An+B.json': 'component_values',
}

# (name, entry, text): this file's own.
OWN = [
    ('nesting', 'stylesheet', 'a{color:red; &:hover{color:blue} b:c}'),
    ('nesting-deep', 'stylesheet', 'a{b{c{d{e:f} g:h} i{}} j:k}'),
    ('nesting-at', 'stylesheet', '@media x { a { b: c } d: e; @supports y { f: g } }'),
    ('decl-block', 'stylesheet', 'a{b:c{} d:{e} f:{g} h; i:{} !important; j:{}!}'),
    ('custom-property', 'declaration_list', '--x: {a} ; --y: a /* c */ b !important; --z:;'),
    ('custom-rule', 'stylesheet', '.a{} --x:hover { } .b { } div { --y:hover { ] } .c { } }'),
    ('unicode-range', 'declaration_list', 'unicode-range: U+0-7F, u+4??, U+10FFFF !important ; UNICODE-RANGE: u+1-2'),
    ('important', 'declaration_list', 'a: b ! IMPORTANT ; c: d !important x; e: !important'),
    ('bad-string', 'stylesheet', 'a{b:"x\n}c{d:\'e\n;f:g}'),
    ('bad-url', 'component_values', 'url(a b) url(a"b) url(a\\\nb) url(a\\)b) url(\x01) url(  a  ) url('),
    ('escapes', 'component_values', '\\30 a \\0 \\D800 \\110000 \\ \\\n #\\31 x a\\'),
    ('numbers', 'component_values', '+1 -.5e+3 1e 1. 12% 12\\% 3e-2.1 1e999 0.30000000000000004 12px 12\\65 5'),
    ('cdo-cdc', 'stylesheet', '<!-- a{} --> b<!---->{} -->x{}'),
    ('every-token', 'component_values', 'a f( @a #a #1 "s" \'s\' url(u) 1 1% 1px , : ; [ ] ( ) { } <!-- --> ! \\\n'),
    ('errors', 'stylesheet', 'a{} b c } d{) ] } e{f}} @g } h{ /* i'),
    ('declaration-list', 'declaration_list', 'a:b; c d e; f+:g; @x { y } ; } h: i } j'),
    ('rule', 'rule', '  @a b { c; d: e }  '),
    ('declaration', 'declaration', ' --x: a {b} c '),
    ('comma', 'comma_list', 'a, b(c, d), , e,,'),
    ('anb', 'component_values', '-n- 5'),
    ('anb-2', 'component_values', ' +3n - 2 '),
    ('utf8', 'component_values', 'a\xc3( \xed\xa0\x80 \xf0\x9f\x98\x80 \xff\xfe \xc0\xaf \xe2\x80\x8d'),
    ('whitespace', 'stylesheet', 'a \t\r\n\f{ b :\r\n c ;\f}'),
    ('wide', 'stylesheet', ''.join(f'r{i}{{p{i}:v{i}; q{i}{{}}}}' for i in range(60))),
    ('deep-blocks', 'component_values', '(' * 40 + '[' * 40 + ')' * 40),
    ('deep-rules', 'stylesheet', 'a{' * 30 + '}' * 30),
    ('deep-functions', 'declaration_list', 'a: ' + 'f(' * 30),
    ('speculation', 'stylesheet', 'a{' + 'b:{c} d;' * 10 + 'e:f{g:h{i:j}}}'),
    ('lone-commas', 'component_values', ', ,, a,'),
    ('lone-comma', 'comma_list', ','),
    ('anb-big', 'component_values', '2n+9007199254740993'),
    ('anb-min', 'component_values', '-9223372036854775808n-9223372036854775808'),
    ('unicode-range-adjacent', 'declaration_list', 'unicode-range: foo/**/U+25, 5/**/U+25-2f'),
    ('two-whitespace', 'stylesheet', 'a /**/ b { c: d /**/ e; --f: g /**/ h }'),
]

# (name, entry, bytes): inputs that are not text, written as they are.
RAW = [
    ('custom-nul', 'declaration_list', b'--x: a\x00b\xffc /* \x00 */ d; --y:\xed\xa0\x80;'),
    ('nul-everywhere', 'stylesheet', b'a\x00{b\x00:c\x00;--d:\x00}'),
]

# Option bytes the own list is written with: (suffix, o0 flags beyond the entry, o1, o2).
OPTIONS = [
    ('', 0x00, 0x00, 0x00),
    ('-tiny-depth', 0x00, 0x01 | (2 << 3), 0x00),
    ('-tiny-tokens', 0x00, 0x02 | (7 << 3), 0x00),
    ('-tiny-rules', 0x00, 0x03 | (1 << 3), 0x00),
    ('-tiny-errors', 0x80, 0x05 | (0 << 3), 0x00),
    ('-validity', 0x40, 0x00, 0x00),
    ('-wtf8-count', 0x30, 0x00, 0x00),
    ('-oom-3', 0x00, 0x00, 0x03),
    ('-oom-9', 0x80, 0x00, 0x09),
]


def write(out, name, entry, flags, o1, o2, data):
    with open(os.path.join(out, name + '.seed'), 'wb') as f:
        f.write(bytes([ENTRY[entry] | flags, o1, o2]) + data)


def main():
    suite = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'third_party', 'css', 'css-parsing-tests')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'tests', 'fuzz', 'corpus', 'css-syntax')
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith('.seed') and not f.startswith('reproducer-'):
            os.remove(os.path.join(out, f))
    n = 0
    for f, entry in sorted(SUITE.items()):
        with open(os.path.join(suite, f), encoding='utf-8') as fh:
            data = json.load(fh)
        stem = f[:-5].replace('+', 'plus').replace('_', '-')
        for k in range(0, len(data) - 1, 2):
            write(out, f'{stem}-{k // 2:03d}', entry, 0, 0, 0, data[k].encode('utf-8', 'surrogatepass'))
            n += 1
    with open(os.path.join(suite, 'stylesheet_bytes.json'), encoding='utf-8') as fh:
        data = json.load(fh)
    for k in range(0, len(data) - 1, 2):
        write(out, f'stylesheet-bytes-{k // 2:03d}', 'stylesheet', 0, 0, 0, data[k]['css_bytes'].encode('latin-1'))
        n += 1
    for name, entry, text in OWN:
        raw = text.encode('latin-1') if name == 'utf8' else text.encode('utf-8')
        for suffix, flags, o1, o2 in OPTIONS:
            write(out, f'own-{name}{suffix}', entry, flags, o1, o2, raw)
            n += 1
    for name, entry, raw in RAW:
        for suffix, flags, o1, o2 in OPTIONS:
            write(out, f'own-{name}{suffix}', entry, flags, o1, o2, raw)
            n += 1
    print(f'make-fuzz-seeds: {n} seeds in {out}')


if __name__ == '__main__':
    main()
