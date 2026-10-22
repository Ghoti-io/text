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
"""Write the seed corpus of tests/fuzz/fuzz_css_bytes.cpp.

    tools/css/make-fuzz-bytes-seeds.py [<css-parsing-tests dir>] [<out dir>]

The seeds are the cases of css-parsing-tests' stylesheet_bytes.json (CC0; the bytes of each, with
its protocol and environment encoding where the harness's four-entry tables have them), and a list
of this file's own: a stylesheet in each of the encodings the library decodes, each as a byte order
mark, a protocol label, an @charset, an environment encoding and the default would have it, the
declaration that straddles the kilobyte that decides the encoding at each end of it, ill-formed
input in each encoding, and stylesheets that are mostly shift sequences, escapes and surrogates.
Each is written with four option bytes in front (the harness says what they mean), and some of the
own list again with a tiny limit, a validity callback, a failed allocation and a text stream.

Files that are not seeds made here (a reproducer-*.seed a fuzzer found) are left alone. The output
is the same on every run.
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The harness's tables (o0 bits 0-1 and 2-3).
LABELS = {None: 0, 'windows-1251': 1, 'utf-16le': 2, 'shift_jis': 3}
ENVIRONMENTS = {None: 0, 'windows-1252': 1, 'iso-8859-2': 2, 'gb18030': 3}
# The same, spelled as css-parsing-tests spells them.
SUITE_LABELS = {'windows-1251': 'windows-1251', 'utf-16le': 'utf-16le', 'shift_jis': 'shift_jis'}
SUITE_ENV = {'ISO-8859-2': 'iso-8859-2', 'iso-8859-2': 'iso-8859-2'}

TEXT = 'a{b:c} .d > e{f:"g"} /* h */ @media x{i{j:k}} l{m:n\n} o{p:url(q r)}\n'
NON_ASCII = '.\u043f\u0440\u0438\u0432\u0435\u0442{\u0446\u0432\u0435\u0442:\u043a\u0440\u0430\u0441\u043d\u044b\u0439} /* \u043a\u043e\u043c */ a::before{content:"\u0451"} x{y:"\u0436\n} z{w:url(\u0431\u0431 ) q}\n'
CJK = '.\u65e5\u672c{x:y} a{b:"\uff71\uff72"} /*\u80fd*/ x{y:"\u8868\n}'


def own():
    """(name, bytes, label, environment) for each of the own stylesheets."""
    out = []

    def add(name, data, label=None, env=None):
        out.append((name, data, label, env))

    add('utf8', TEXT.encode('utf-8'))
    add('utf8-bom', b'\xef\xbb\xbf' + (TEXT + NON_ASCII).encode('utf-8'))
    add('utf8-bom-label', b'\xef\xbb\xbf' + b'@charset "iso-8859-1"; a{b:\xc3\xa9}', 'windows-1251')
    add('utf8-ill-formed', b'a\xc3{b:\xe2\x82}\xed\xa0\x80 c{d:e\xf0\x9f}\x80\x80 f{g:h}')
    add('cp1251-charset', b'@charset "windows-1251"; ' + NON_ASCII.encode('cp1251'))
    add('cp1251-label', NON_ASCII.encode('cp1251'), 'windows-1251')
    add('cp1251-env', NON_ASCII.encode('cp1251'), None, 'windows-1252')
    add('latin2-env', 'a{b:"\u0141\u00e9"}'.encode('iso8859_2'), None, 'iso-8859-2')
    add('utf16le-bom', b'\xff\xfe' + (TEXT + 'a{b:\u00e9}.\u00fc{c:"\U0001f600"}').encode('utf-16-le'))
    add('utf16be-bom', b'\xfe\xff' + (TEXT + 'a{b:\u00e9}.\u00fc{c:"\U0001f600"}').encode('utf-16-be'))
    add('utf16le-label', (TEXT + '\u00e9').encode('utf-16-le'), 'utf-16le')
    add('utf16le-lone-surrogate', b'\xff\xfea\x00{\x00\x00\xd8}\x00b')
    add('utf16-charset', b'@charset "utf-16le"; a{b:\xc3\xa9}')
    add('sjis', CJK.encode('shift_jis'), 'shift_jis')
    add('sjis-truncated', b'a{b:c}/*\x94', 'shift_jis')
    add('sjis-trail-brace', b'.\x93\xfa\x96{{x:y} a\x81{b:c}', 'shift_jis')
    add('gb18030', ('.\u4e2d\u6587{b:c} /*\U00020000*/ x{y:"\U00020000z\n}').encode('gb18030'), None, 'gb18030')
    add('gb18030-ill-formed', b'.\xd6\xd0\xce{b:c} /*\x95' + b'2\x82*/ x{y:\xff"}\x81', None, 'gb18030')
    jis = b'a{b:c}/*\x1b$BF|K\\8l\x1b(B*/.\x1b$BF|K\\\x1b(B{d:e}\n'
    add('iso2022jp', jis, 'shift_jis')  # the wrong label for it: the escapes are bytes
    add('iso2022jp-charset', b'@charset "iso-2022-jp"; ' + jis)
    add('iso2022jp-redundant-shifts', b'@charset "iso-2022-jp"; a\x1b(Bb\x1b(Bc{d:e}\x1b$BF|\x1b(B\x1b$BK\\\x1b(B x{y:z}')
    add('iso2022jp-trailing-shift', b'@charset "iso-2022-jp"; a{b:c}/*\x1b$BF|\x1b(B*/ @m \x1b$BK\\\x1b(B')
    add('iso2022jp-open-shift', b'@charset "iso-2022-jp"; a{b:c}/*\x1b$B', None)
    add('euckr', '.\ud55c\uad6d{x:y} /*\ud55c*/ q{r:"\ud55c\n}'.encode('euc_kr'), None, 'gb18030')
    add('big5', b'@charset "big5"; ' + '.\u4e2d\u6587{x:y} /*\u4e2d*/ q{r:"\u4e2d\n}'.encode('big5'))
    add('replacement', b'@charset "hz-gb-2312"; a{b:c}')
    add('x-user-defined', b'@charset "x-user-defined"; a{b:\x80\xff}')
    add('unknown-charset', b'@charset "kamoulox"; a{b:\xe9}', None, 'iso-8859-2')
    add('charset-2-spaces', b'@charset  "windows-1251"; a{b:\xe9}')
    add('charset-single-quotes', b"@charset 'windows-1251'; a{b:\xe9}")
    add('charset-null-label', b'@charset "windows\x00-1251"; a{b:\xe9}')
    window = lambda end: b'@charset "windows-1251' + b' ' * (end - 24) + b'"; a{b:\xef\xf0}'
    # the closing `";` ends at byte 1023, 1024, 1025 (exclusive ends: 1023, 1024, 1025)
    for end in (1023, 1024, 1025):
        add(f'charset-ends-{end}', window(end))
    big = b'@charset "windows-1251"; ' + b''.join(b'.\xef\xf0{b:\xe8 c}/*\xe2*/\n' for _ in range(120))
    add('cp1251-long', big)
    add('utf16le-long', b'\xff\xfe' + b''.join('a{b:\u00e9}'.encode('utf-16-le') for _ in range(300)))
    huge = b'@charset "windows-1251"; ' + b''.join(b'.\xef\xf0 > c { b: \xe8 c \"\xf6\" } /* \xe2 */\n' for _ in range(300))
    add('cp1251-huge', huge)
    add('utf16le-huge', b'\xff\xfe' + b''.join('a.b > c{d:\u00e9 \u4e2d}\n'.encode('utf-16-le') for _ in range(400)))
    add('bytes-0-255', bytes(range(256)))
    add('bytes-0-255-label', bytes(range(256)), 'windows-1251')
    add('bytes-sjis-sweep', b'a{b:"' + b''.join(bytes([0x81 + i % 120, 0x40 + (i * 7) % 0xbc]) for i in range(100)) + b'"}', 'shift_jis')
    add('empty', b'')
    add('bom-only', b'\xef\xbb\xbf')
    add('half-bom', b'\xef\xbb')
    add('utf16-bom-only', b'\xff\xfe')
    add('escapes', b'a{b:url(  x\\)y  );c:"s\\\n t";d:\\41 bc;e:\\0;f:\\110000 ;g:\\}h}/* c \\*/ */ @m \\@x{y{z:\\')
    add('numbers', b'a{b:+1.5e-3px -.5 1e400 1e-400 0.1% 12E+2 9007199254740993 --c:1.0 ' + b'9' * 400 + b'e-400}')
    add('nesting', b'a{b:c; &:hover{d:e} f:g; .h{ i:j } k:l; @media x{m:n; o{p:q}}}')
    return out


# Option bytes the own list is also written with: (suffix, o0 flags beyond the tables, o1, o2, o3).
OPTIONS = [
    ('', 0x00, 0x00, 0x00, 0x00),
    ('-cut-1', 0x00, 0x00, 0x00, 0x35),
    ('-tiny-depth', 0x00, 0x01 | (2 << 3), 0x00, 0x01),
    ('-tiny-tokens', 0x00, 0x02 | (7 << 3), 0x00, 0x02),
    ('-tiny-rules', 0x00, 0x03 | (1 << 3), 0x00, 0x03),
    ('-tiny-errors', 0x80, 0x05, 0x00, 0x04),
    ('-validity', 0x40, 0x00, 0x00, 0x05),
    ('-wtf8-count', 0x90, 0x00, 0x00, 0x06),
    ('-oom-3', 0x00, 0x00, 0x03, 0x07),
    ('-oom-11', 0x80, 0x00, 0x0b, 0x08),
    ('-text-stream', 0x20, 0x00, 0x00, 0x09),
]


def table_bits(label, env):
    bits = LABELS.get(label)
    if bits is None:
        bits = 0
    ebits = ENVIRONMENTS.get(env)
    if ebits is None:
        ebits = 0
    return bits | (ebits << 2)


def write(out, name, o0, o1, o2, o3, data):
    with open(os.path.join(out, name + '.seed'), 'wb') as f:
        f.write(bytes([o0, o1, o2, o3]) + data)


def main():
    suite = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'third_party', 'css', 'css-parsing-tests')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'tests', 'fuzz', 'corpus', 'css-bytes')
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith('.seed') and not f.startswith('reproducer-'):
            os.remove(os.path.join(out, f))
    n = 0
    with open(os.path.join(suite, 'stylesheet_bytes.json'), encoding='utf-8') as fh:
        data = json.load(fh)
    for k in range(0, len(data) - 1, 2):
        case = data[k]
        label = SUITE_LABELS.get((case.get('protocol_encoding') or '').strip().lower())
        env = SUITE_ENV.get(case.get('environment_encoding'))
        write(out, f'stylesheet-bytes-{k // 2:03d}', table_bits(label, env), 0, 0, k & 0xff, case['css_bytes'].encode('latin-1'))
        n += 1
    for name, raw, label, env in own():
        for suffix, flags, o1, o2, o3 in OPTIONS:
            write(out, f'own-{name}{suffix}', table_bits(label, env) | flags, o1, o2, o3, raw)
            n += 1
    print(f'make-fuzz-bytes-seeds: {n} seeds in {out}')


if __name__ == '__main__':
    main()
