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
"""What the HTML tokenizer's two drivers share: the runner's protocol.

The runner (html_tokenizer_suite.c) takes one request per line and prints the
tokens and parse errors of the tokenizer; this module frames requests, runs the
runner over many of them in a process, and reads the answers into tuples that
compare with `==`. Used by html_tokenizer_suite.py (the vectors) and by
tools/oracle/html_tokenizer_diff.py (the references), so that the two read the
runner one way.

Copyright 2026 by Corey Pennycuff
"""
import re
import subprocess

STATES = {
    'Data state': 'data',
    'PLAINTEXT state': 'plaintext',
    'RCDATA state': 'rcdata',
    'RAWTEXT state': 'rawtext',
    'Script data state': 'script',
    'CDATA section state': 'cdata',
}


def unescape(s):
    """The vectors' second round of unescaping: \\uHHHH into a code point."""
    return re.sub(r'\\u([0-9a-fA-F]{4})', lambda m: chr(int(m.group(1), 16)), s)


def enc(s):
    return s.encode('utf-8', 'surrogatepass')


def hexs(b):
    return 'x' + b.hex()


def unhex(field):
    assert field[0] == 'x', field
    return bytes.fromhex(field[1:])


def parse_answer(lines, ordered=False):
    """The runner's lines for one request -> (tokens, errors, dropped, status).

    A start tag's attributes are a frozenset of (name, value) unless `ordered`,
    when they are a tuple in source order."""
    tokens, errors, dropped, status = [], [], 0, None
    for line in lines:
        f = line.split(' ')
        k = f[0]
        if k == 'C':
            tokens.append(('C', unhex(f[1])))
        elif k in ('S', 'E'):
            n = int(f[3])
            pairs = [(unhex(f[4 + 2 * i]), unhex(f[5 + 2 * i])) for i in range(n)]
            if k == 'E':
                tokens.append(('E', unhex(f[1])))
            else:
                tokens.append(('S', unhex(f[1]), tuple(pairs) if ordered else frozenset(pairs), n,
                               f[2] == '1'))
        elif k == 'M':
            tokens.append(('M', unhex(f[1])))
        elif k == 'D':
            fl = f[1]
            tokens.append(('D', fl[0] == 'N', unhex(f[2]), fl[1] == 'P', unhex(f[3]),
                           fl[2] == 'S', unhex(f[4]), fl[3] == 'Q'))
        elif k == 'P':
            tokens.append(('P', unhex(f[1]), unhex(f[2])))
        elif k == 'F':
            tokens.append(('F',))
        elif k == 'X':
            errors.append((f[1], int(f[2]), int(f[3]), int(f[4])))
        elif k == 'DROPPED':
            dropped = int(f[1])
        elif k == 'END':
            status = f[1]
    return tokens, errors, dropped, status


def run_batch(runner, requests):
    """requests: list of request lines. Returns a list of answer-line lists, or
    None if the runner died."""
    data = ('\n'.join(requests) + '\n').encode('ascii')
    try:
        done = subprocess.run([runner], input=data, capture_output=True, timeout=600)
    except subprocess.TimeoutExpired:
        return None
    if done.returncode != 0:
        return None
    answers, cur = [], []
    for line in done.stdout.decode('ascii').split('\n'):
        if not line:
            continue
        cur.append(line)
        if line.startswith('END '):
            answers.append(cur)
            cur = []
    if len(answers) != len(requests):
        return None
    return answers


def run_all(runner, requests):
    """Answers for all requests; a request that kills the runner is isolated."""
    answers = [None] * len(requests)
    crashed = []
    step = 400
    for lo in range(0, len(requests), step):
        part = requests[lo:lo + step]
        got = run_batch(runner, part)
        if got is not None:
            answers[lo:lo + len(part)] = got
            continue
        for i, r in enumerate(part):
            one = run_batch(runner, [r])
            if one is None:
                crashed.append(lo + i)
            else:
                answers[lo + i] = one[0]
    return answers, crashed


def coalesce(tokens):
    out = []
    for t in tokens:
        if t[0] == 'C' and out and out[-1][0] == 'C':
            out[-1] = ('C', out[-1][1] + t[1])
        else:
            out.append(t)
    return out


