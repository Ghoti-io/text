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
"""Score the CSS Syntax parser against css-parsing-tests and the WPT css-syntax tables.

    css_syntax_suite.py <css-parsing-tests dir> <wpt vectors json> <runner> <exceptions>

Every case of the files in scope is run through the runner
(css_syntax_suite.c) with the entry point its file names, and compared with
the expectation. Every case also has to round-trip: its tree, serialized and
parsed again by the same entry point, is the same tree (CSS Syntax 9). A
round trip has no exceptions.

**What css-parsing-tests' expectations are normalized for**, because the
suite's representation is the 2021 model's and this parser's is the current
draft's; none of these changes what is compared, only how it is written:

  - an item ["error", "invalid"] in a list of rules or declarations is the
    2021 parser's marker for a construct it dropped; the draft "returns
    nothing" for one, so the marker is removed from the expectation (the
    parser reports the drop as a parse error, which this gate does not
    score);
  - a rule's block is the component values of its text (the 2021 parser did
    not parse blocks); the runner writes ours that way.

Every other difference is a failure, unless tools/conformance/CSS_SYNTAX_EXCEPTIONS
lists the case with the answer the current draft gives (which the parser must
then give exactly) and the sentence of the draft that decides it. The
gate fails on a failure that is not listed, on a listed case that passes or
does not exist (a stale entry), on an entry without a quoted sentence, on a
file of the suite that is neither scored nor named as out of scope, on a
runner that crashes, and when fewer cases were scored than the floor
(CSS_SYNTAX_MIN_SCORED).
"""

import fnmatch
import json
import os
import subprocess
import sys

SCORED = {
    'component_value_list.json': 'component_values',
    'one_component_value.json': 'component_value',
    'declaration_list.json': 'declaration_list',
    'blocks_contents.json': 'block_contents',
    'one_declaration.json': 'declaration',
    'one_rule.json': 'rule',
    'rule_list.json': 'stylesheet_contents',
    'stylesheet.json': 'stylesheet',
    'An+B.json': 'anb',
}

# Files of bytes: each case is {"css_bytes", optional "protocol_encoding" and
# "environment_encoding"} and then [the rules, the name of the encoding], scored
# through the byte entry point (CSS Syntax 3.2).
BYTES_FILES = {'stylesheet_bytes.json': 'stylesheet_bytes'}

# Files the gate does not score, and why. A file in the pin that is in neither
# table fails the gate, so a raised pin cannot add a file silently.
NOT_SCORED = {
    'color_*.json': 'color values are CSS Color, scored with the value layer',
}

LIST_ENTRIES = {'declaration_list', 'block_contents', 'stylesheet_contents', 'stylesheet'}


def normalize(expected, entry):
    if entry in LIST_ENTRIES and isinstance(expected, list):
        return [x for x in expected if x != ['error', 'invalid']]
    return expected


def same(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b if isinstance(a, bool) and isinstance(b, bool) else False
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return float(a) == float(b)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def matches(pattern, actual):
    """An outline pattern: a list matches a list of the same length item by item,
    a dict matches a dict that has each of its keys (and may have more), and
    anything else matches what is equal."""
    if isinstance(pattern, list):
        return isinstance(actual, list) and len(pattern) == len(actual) and all(
            matches(p, a) for p, a in zip(pattern, actual))
    if isinstance(pattern, dict):
        return isinstance(actual, dict) and all(k in actual and matches(v, actual[k]) for k, v in pattern.items())
    return pattern == actual


def encode(s, wtf8):
    return s.encode('utf-8', 'surrogatepass' if wtf8 else 'strict')


def run(runner, requests):
    """requests: list of (entry, wtf8, text) or (entry, options, bytes): text is a
    str (encoded as UTF-8) or the bytes themselves, and options a list of strings
    in place of the WTF-8 flag. Returns (answers, crashed)."""
    lines = []
    for entry, wtf8, text in requests:
        data = (text if isinstance(text, bytes) else encode(text, wtf8)).hex() or '-'
        opts = ','.join(wtf8) if isinstance(wtf8, list) else ('wtf8' if wtf8 else '')
        lines.append(f'{entry} {opts or "-"} {data}\n')
    proc = subprocess.run([runner], input=''.join(lines).encode(), capture_output=True)
    answers = []
    cur = {}
    for raw in proc.stdout.decode('utf-8', 'surrogateescape').split('\n'):
        if not raw:
            continue
        key, _, rest = raw.partition(' ')
        if key == 'END':
            cur['end'] = rest
            answers.append(cur)
            cur = {}
        else:
            cur[key] = rest
    crashed = proc.returncode != 0 or len(answers) != len(requests)
    return answers, crashed, proc


def text_of(hexed):
    if hexed in ('-', None):
        return '' if hexed == '-' else None
    if hexed == 'FAILED':
        return None
    return bytes.fromhex(hexed).decode('utf-8', 'surrogateescape')


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    suite, wpt_path, runner, exceptions_path = sys.argv[1:]
    floor = int(os.environ.get('CSS_SYNTAX_MIN_SCORED', '400'))
    problems = []

    # The files of the pin.
    present = sorted(f for f in os.listdir(suite) if f.endswith('.json'))
    for f in present:
        if f in SCORED or f in BYTES_FILES or any(fnmatch.fnmatch(f, pat) for pat in NOT_SCORED):
            continue
        problems.append(f'{f}: a file of the suite that is neither scored nor named as out of scope')
    for f in list(SCORED) + list(BYTES_FILES):
        if f not in present:
            problems.append(f'{f}: scored, but not in the suite')

    # The cases.
    cases = []  # (id, entry, wtf8, input, check)
    for f in sorted(SCORED):
        if f not in present:
            continue
        entry = SCORED[f]
        with open(os.path.join(suite, f), encoding='utf-8') as fh:
            data = json.load(fh)
        for k in range(0, len(data) - 1, 2):
            cases.append((f'{f}#{k // 2}', entry, False, data[k],
                          {'kind': 'suite', 'expect': normalize(data[k + 1], entry)}))
    for f in sorted(BYTES_FILES):
        if f not in present:
            continue
        with open(os.path.join(suite, f), encoding='utf-8') as fh:
            data = json.load(fh)
        for k in range(0, len(data) - 1, 2):
            case = data[k]
            options = []
            for key, short in (('protocol_encoding', 'proto'), ('environment_encoding', 'env')):
                if case.get(key) is not None:
                    options.append(f'{short}={case[key].encode("ascii").hex()}')
            expect = data[k + 1]
            cases.append((f'{f}#{k // 2}', BYTES_FILES[f], options, case['css_bytes'].encode('latin-1'),
                          {'kind': 'bytes', 'expect': normalize(expect[0], 'stylesheet'), 'encoding': expect[1]}))
    not_scored = {}
    for f in present:
        for pat, why in NOT_SCORED.items():
            if fnmatch.fnmatch(f, pat):
                with open(os.path.join(suite, f), encoding='utf-8') as fh:
                    not_scored.setdefault(why, 0)
                    not_scored[why] += len(json.load(fh)) // 2
    with open(wpt_path, encoding='utf-8') as fh:
        wpt = json.load(fh)
    for v in wpt['vectors']:
        cases.append((v['id'], v['entry'], v.get('wtf8', False), v['input'], v['check']))

    exceptions = {}
    with open(exceptions_path, encoding='utf-8') as fh:
        for n, line in enumerate(fh, 1):
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) != 3:
                problems.append(f'{exceptions_path}:{n}: not <id> TAB <the draft\'s answer> TAB <reason>')
                continue
            ident, answer, reason = parts
            try:
                answer = json.loads(answer)
            except ValueError:
                problems.append(f'{exceptions_path}:{n}: {ident}: the draft\'s answer is not JSON')
                continue
            if reason.count('"') < 2:
                problems.append(f'{exceptions_path}:{n}: {ident}: the reason quotes no sentence of the draft')
            exceptions[ident] = (answer, reason)

    answers, crashed, proc = run(runner, [(c[1], c[2], c[3]) for c in cases])
    if crashed:
        print('RUNNER CRASHES', file=sys.stderr)
        print(f'  exit status {proc.returncode}, {len(answers)} answers to {len(cases)} requests', file=sys.stderr)
        print(proc.stderr.decode('utf-8', 'replace')[-2000:], file=sys.stderr)
        sys.exit(1)

    # The WPT round-trip vectors also need the serialization serialized again.
    fixed = [i for i, c in enumerate(cases) if c[4]['kind'] == 'roundtrip']
    again, crashed2, proc2 = run(runner, [('stylesheet', False, text_of(answers[i].get('SERIAL')) or '') for i in fixed])
    if crashed2:
        print('RUNNER CRASHES', file=sys.stderr)
        sys.exit(1)
    second = dict(zip(fixed, again))

    failures = []   # (id, why)
    hard = []       # failures no exception excuses
    passed = 0
    roundtrip_failures = []
    per_source = {}
    for i, (ident, entry, wtf8, inp, check) in enumerate(cases):
        a = answers[i]
        src = ident.split('#')[0]
        stats = per_source.setdefault(src, [0, 0, 0])
        stats[0] += 1
        why = None
        if a.get('end') != 'DONE':
            why = f'the parse failed: {a.get("end")}'
        else:
            if a.get('ROUNDTRIP') != 'ok':
                roundtrip_failures.append((ident, inp, a.get('ROUNDTRIP')))
            result = json.loads(a['RESULT'])
            kind = check['kind']
            if kind == 'bytes':
                # What decides the bytes' reading is not excusable by an exception (it is the whole of this
                # file's point); the rules are, as for any other case.
                got_encoding = a.get('ENCODING', '').split(' ')[0]
                if got_encoding != check['encoding'].lower():
                    hard.append((ident, f'decoded as {got_encoding!r}, the suite expects {check["encoding"].lower()!r}'))
                if a.get('STREAM') != 'ok':
                    hard.append((ident, f'fed a byte at a time, a stream gave a different answer: {a.get("STREAM")}'))
            serial = text_of(a.get('SERIAL'))
            cssom = text_of(a.get('CSSOM'))
            if kind == 'suite':
                if not same(result, check['expect']):
                    why = f'expected {json.dumps(check["expect"], ensure_ascii=False)}\n      got      {json.dumps(result, ensure_ascii=False)}'
            elif kind == 'bytes':
                if not same(result, check['expect']):
                    why = f'expected {json.dumps(check["expect"], ensure_ascii=False)}\n      got      {json.dumps(result, ensure_ascii=False)}'
            elif kind == 'anb':
                if check['expect'] is None:
                    if result is not None:
                        why = f'expected a parse error, got {result}'
                elif result is None or serial != check['expect']:
                    why = f'expected {check["expect"]!r}, got {result} serialized {serial!r}'
            elif kind == 'numeric':
                single = len(result) == 1 and isinstance(result[0], list) and result[0][0] == check['type']
                if check['equals']:
                    if not single or cssom != check['cssom']:
                        why = f'expected one {check["type"]} written {check["cssom"]!r}, got {result} written {cssom!r}'
                elif single and cssom == check['cssom']:
                    why = f'expected anything but one {check["type"]} written {check["cssom"]!r}, got {result}'
            elif kind == 'result':
                if not same(result, check['expect']):
                    why = f'expected {json.dumps(check["expect"])}\n      got      {json.dumps(result)}'
                elif 'cssom' in check and cssom != check['cssom']:
                    why = f'expected the text {check["cssom"]!r}, got {cssom!r}'
            elif kind == 'serial':
                if serial != check['expect']:
                    why = f'expected the value written {check["expect"]!r}, got {serial!r}'
            elif kind == 'outline':
                if not matches(check['expect'], result):
                    why = f'expected the outline {json.dumps(check["expect"])}\n      got      {json.dumps(result)}'
            elif kind == 'last-declaration':
                if len(result) != check['rules'] or not result:
                    why = f'expected {check["rules"]} rule(s), got {json.dumps(result)}'
                else:
                    for name, value in check['expect']:
                        last = [d for d in result[0].get('d', []) if d[0] == name]
                        if not last or last[-1][1] != value:
                            why = f'expected the last {name} to be {value!r}, got {json.dumps(result[0].get("d"))}'
                            break
            elif kind == 'roundtrip':
                b = second[i]
                if b.get('end') != 'DONE' or text_of(b.get('SERIAL')) != serial:
                    why = f'serializing again changed the text: {serial!r} then {text_of(b.get("SERIAL"))!r}'
            else:
                why = f'unknown check {kind}'
        if why is None:
            if ident in exceptions:
                failures.append((ident, 'STALE', 'listed as an exception, and passes'))
            passed += 1
            stats[1] += 1
        elif ident in exceptions:
            # The listed answer is the draft's: the parser must give exactly it, so a
            # different wrong answer to a listed case is still a failure.
            if a.get('end') == 'DONE' and same(json.loads(a['RESULT']), exceptions[ident][0]):
                stats[2] += 1
            else:
                failures.append((ident, 'FAIL', f'input {json.dumps(inp.decode('latin-1') if isinstance(inp, bytes) else inp, ensure_ascii=False)}\n      '
                                 f'listed with the draft\'s answer {json.dumps(exceptions[ident][0], ensure_ascii=False)}\n'
                                 f'      got      {a.get("RESULT", a.get("end"))}'))
        else:
            failures.append((ident, 'FAIL', f'input {json.dumps(inp.decode('latin-1') if isinstance(inp, bytes) else inp, ensure_ascii=False)}\n      {why}'))
    for ident, why in hard:
        failures.append((ident, 'FAIL', why))
    known = {c[0] for c in cases}
    for ident in exceptions:
        if ident not in known:
            failures.append((ident, 'STALE', 'listed as an exception, and there is no such case'))

    commit = subprocess.run(['git', '-C', suite, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    print(f'CSS Syntax vectors (css-parsing-tests @ {commit[:12]}, wpt @ {wpt["wpt_commit"][:12]}):')
    for src in sorted(per_source):
        n, ok, ex = per_source[src]
        print(f'  {src:48s} {ok:4d} of {n:4d} pass, {ex:3d} listed exceptions')
    for why, n in sorted(not_scored.items()):
        print(f'  not scored: {n} cases ({why})')
    total = len(cases)
    print(f'{passed} of {total} cases pass, {sum(s[2] for s in per_source.values())} are listed exceptions; '
          f'{total - len(roundtrip_failures)} of {total} round-trip')

    bad = False
    unexplained = [f for f in failures if f[1] == 'FAIL']
    stale = [f for f in failures if f[1] == 'STALE']
    if unexplained:
        bad = True
        print('UNEXPLAINED FAILURES', file=sys.stderr)
        for ident, _, why in unexplained:
            print(f'  {ident}: {why}', file=sys.stderr)
    if stale:
        bad = True
        print('STALE EXCEPTIONS', file=sys.stderr)
        for ident, _, why in stale:
            print(f'  {ident}: {why}', file=sys.stderr)
    if roundtrip_failures:
        bad = True
        print('ROUND TRIP FAILURES', file=sys.stderr)
        for ident, inp, why in roundtrip_failures:
            print(f'  {ident}: input {json.dumps(inp.decode('latin-1') if isinstance(inp, bytes) else inp, ensure_ascii=False)}: {why}', file=sys.stderr)
    if problems:
        bad = True
        print('SUITE PROBLEMS', file=sys.stderr)
        for p in problems:
            print(f'  {p}', file=sys.stderr)
    if total < floor:
        bad = True
        print(f'{total} cases scored, below the floor of {floor}', file=sys.stderr)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
