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
"""Score custom properties, arbitrary substitution and `@property` against web-platform-tests' css-variables and css-properties-values-api.

    css_variables_suite.py <vectors dir> <runner> <exceptions> <defects> [--unextracted <listed> <fresh>]

The vectors are made by tools/css/extract-wpt-variables.py from the pinned checkout (registration, reftest, cycles, limits); the runner is
tools/conformance/css_variables_suite.c, which runs the library and answers; this program compares. What a vector asks:

  registration  `CSS.registerProperty({name, syntax, initialValue, inherits: false})` returns (valid) or throws SyntaxError (invalid): the library
                registers the syntax and the initial value, or refuses them. Stages: `register` (a valid one refused), `invalid-accepted`.
  reftest       the page and its reference compute the same style for the elements that hold text, the cascade run over the page's style sheets and
                elements (inline `style`, selectors of a type, id, class and `:root`, `@supports`, `@property`). Stage `style`; the answer is the first
                property that differs.
  cycles        the custom properties that must be the guaranteed-invalid value are (they have no computed value) and the others have one, and
                `--sanity: valid` computes. Stage `cycle`.
  limits        the pages that must not crash: the computation ends with status OK, no substituted value is longer than the budget allows and the
                value the budget stopped is the guaranteed-invalid value. Stage `limit`.

Every failure has to be listed, one line per case, in one of two files. CSS_VARIABLES_EXCEPTIONS holds the cases where the page's expectation does
not bind: the vector, a summary, the stage, the answer the library gives, a class (draft-newer, wpt-tentative, needs-environment, wpt-wrong) and the
sentence of the served draft that says so, quoted. CSS_VARIABLES_DEFECTS holds the cases where the page is right and the library is not: the same
columns with an owner and what the library lacks; the number of defects has a ceiling that only goes down (CSS_VARIABLES_MAX_DEFECTS). In both lists
the library must give exactly the answer listed. The gate fails on a failure that is in neither list ("UNEXPLAINED FAILURES"), on a listed case that
passes, does not exist or has another stage or answer ("STALE EXCEPTIONS"), on a reason that quotes no sentence or a class that is not one of the four
("SUITE PROBLEMS"), on fewer scored cases than the floor (CSS_VARIABLES_MIN_SCORED), and on a page the extractor does not read in full that tools/css/
UNEXTRACTED_VARIABLES does not list, or lists with another count ("UNEXTRACTED PAGES").
"""

import json
import os
import re
import subprocess
import sys

CLASSES = ('draft-newer', 'wpt-tentative', 'needs-environment', 'wpt-wrong')
STAGES = ('register', 'invalid-accepted', 'style', 'cycle', 'limit')
BUDGET = 1048576


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


class Runner:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.buf = b''

    def send(self, text):
        self.p.stdin.write(text.encode('utf-8', 'surrogateescape'))
        self.p.stdin.flush()

    def line(self):
        while b'\n' not in self.buf:
            chunk = self.p.stdout.read1(1 << 16) if hasattr(self.p.stdout, 'read1') else os.read(self.p.stdout.fileno(), 1 << 16)
            if not chunk:
                print('SUITE PROBLEMS: the runner stopped answering (exit %s)' % self.p.poll())
                sys.exit(1)
            self.buf += chunk
        raw, self.buf = self.buf.split(b'\n', 1)
        return raw.decode('utf-8', 'surrogateescape')

    def register(self, syntax, initial):
        flags = 'n' if syntax is None else ''
        self.send('REG\t%s\t%s\t%s\n' % (flags, escape(syntax or ''), escape(initial)))
        return self.line()

    def page(self, sheets, els):
        """els: (id, parent, tag, element id, classes, style) in order. Returns {id: {'status', 'counters', 'P': {...}, 'C': {...}}} or an ERR string."""
        out = ['PAGE\n']
        for css in sheets:
            out.append('SHEET\t%s\n' % escape(css))
        for e in els:
            out.append('EL\t%d\t%d\t%s\t%s\t%s\t%s\n' % (e[0], e[1], escape(e[2]), escape(e[3]), escape(' '.join(e[4])), escape(e[5])))
        out.append('END\n')
        self.send(''.join(out))
        result, cur = {}, None
        while True:
            line = self.line()
            if line == '.':
                return result
            f = line.split('\t')
            if f[0] == 'ERR':
                self.line()  # the closing dot
                return unescape(f[1])
            if f[0] == 'E':
                cur = {'status': unescape(f[2]), 'counters': f[3], 'P': {}, 'C': {}}
                result[int(f[1])] = cur
            elif f[0] in ('P', 'C') and cur is not None:
                cur[f[0]][unescape(f[1])] = unescape(f[2])
            else:
                print('SUITE PROBLEMS: the runner answered %r' % line)
                sys.exit(1)

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def quotes_a_sentence(reason):
    return any(len(q) >= 20 and len(q.split()) >= 4 for q in re.findall(r'"([^"]*)"', reason))


def load_table(path, columns, what):
    entries, problems = {}, []
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) != columns:
            problems.append('%s:%d: %d columns, %s' % (path, n, len(f), what))
            continue
        if f[0] in entries:
            problems.append('%s:%d: %s twice' % (path, n, f[0]))
        entries[f[0]] = f
    return entries, problems


def counters(c):
    return dict(kv.split('=') for kv in c.split())


def els_of(model):
    """The elements of a page model with the implicit html (0) and body (1): (id, parent, tag, element id, classes, style)."""
    out = [(0, -1, 'html', '', [], ''), (1, 0, 'body', '', [], '')]
    for i, e in enumerate(model['els']):
        out.append((i + 2, e['parent'], e['tag'], e['id'], e['classes'], e['style']))
    return out


def text_ids(model):
    return [i + 2 for i, e in enumerate(model['els']) if e['text']]


def first_difference(a, b):
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            return '%s: %s != %s' % (k, a.get(k, '(none)'), b.get(k, '(none)'))
    return None


def score_registration(runner, v):
    answer = runner.register(v['syntax'], v['initial'])
    if v['valid']:
        if answer != 'OK':
            return 'register', answer.replace('\t', ' ')
    elif answer == 'OK':
        return 'invalid-accepted', 'OK'
    elif not answer.startswith('REFUSED\t'):
        return 'register', 'the runner answered %r' % answer
    return None


def score_reftest(runner, v):
    a = runner.page(v['page']['sheets'], els_of(v['page']))
    b = runner.page(v['reference']['sheets'], els_of(v['reference']))
    for who, r in (('page', a), ('reference', b)):
        if isinstance(r, str):
            return 'style', '%s: %s' % (who, r)
        for i, e in r.items():
            if e['status'] != 'OK':
                return 'style', '%s element %d: %s' % (who, i, e['status'])
    for ia, ib in zip(text_ids(v['page']), text_ids(v['reference'])):
        d = first_difference(a[ia]['P'], b[ib]['P'])
        if d:
            return 'style', d
    return None


def score_cycles(runner, v):
    style = ';'.join(v['declarations'] + ['--sanity:valid'])
    r = runner.page([], [(0, -1, 'html', '', [], ''), (1, 0, 'body', '', [], ''), (2, 1, 'div', '', [], style)])
    if isinstance(r, str):
        return 'cycle', r
    e = r[2]
    if e['status'] != 'OK':
        return 'cycle', e['status']
    for name in v['invalid']:
        if name in e['C']:
            return 'cycle', '%s is %r, not the guaranteed-invalid value' % (name, e['C'][name])
    for name in v['valid']:
        if not e['C'].get(name):
            return 'cycle', '%s is the guaranteed-invalid value' % name
    if e['C'].get('--sanity') != 'valid':
        return 'cycle', '--sanity is %r' % e['C'].get('--sanity')
    return None


def limits_sheet(v):
    if v['id'] == 'exponential-blowup':
        decl = '--v0: %s;' % v['base'] + ''.join('--v%d: var(--v%d) var(--v%d);' % (i + 1, i, i) for i in range(v['levels']))
        return ':root { %s content: var(--v%d); }' % (decl, v['levels'])
    if v['id'] == 'long-reference':
        return '* { %s: %s%s; }' % (v['property'], v['repeat'] * v['count'], v['tail'])
    raise SystemExit('no recipe for the limit vector %s' % v['id'])


def score_limits(runner, v):
    r = runner.page([limits_sheet(v)], [(0, -1, 'html', '', [], ''), (1, 0, 'body', '', [], '')])
    if isinstance(r, str):
        return 'limit', r
    for i, e in r.items():
        if e['status'] != 'OK':
            return 'limit', 'element %d: %s' % (i, e['status'])
        c = counters(e['counters'])
        if int(c['peak']) > 2 * BUDGET:
            return 'limit', 'a substituted value of %s tokens' % c['peak']
    if v['id'] == 'exponential-blowup':
        got = r[0]['P'].get('content')
        if got != 'normal':
            return 'limit', 'content is %r, not the value the budget leaves (the guaranteed-invalid value, so unset)' % got
        if int(counters(r[0]['counters'])['limited']) < 1:
            return 'limit', 'no value was stopped by the budget'
    return None


def main():
    argv = sys.argv[1:]
    unext = None
    pos = []
    i = 0
    while i < len(argv):
        if argv[i] == '--unextracted':
            unext = (argv[i + 1], argv[i + 2])
            i += 3
        else:
            pos.append(argv[i])
            i += 1
    vec_dir, runner_path, exceptions_path, defects_path = pos[:4]

    def load(kind):
        return json.load(open(os.path.join(vec_dir, kind + '.json'), encoding='utf-8'))

    problems = []
    exceptions, p = load_table(exceptions_path, 6, 'not <id> TAB <summary> TAB <stage> TAB <answer> TAB <class> TAB <reason>')
    problems += p
    for ident, f in exceptions.items():
        if f[2] not in STAGES:
            problems.append('%s: stage %r is not one of %s' % (ident, f[2], ', '.join(STAGES)))
        if f[4] not in CLASSES:
            problems.append('%s: class %r is not one of %s' % (ident, f[4], ', '.join(CLASSES)))
        if f[4] == 'wpt-tentative':
            if 'tentative' not in ident:
                problems.append('%s: the class is wpt-tentative and the page is not named tentative' % ident)
        elif not quotes_a_sentence(f[5]):
            problems.append('%s: the reason quotes no sentence of the draft (a quotation of four words and twenty characters at least)' % ident)
    defects, p = load_table(defects_path, 6, 'not <id> TAB <summary> TAB <stage> TAB <answer> TAB <owner> TAB <what the library lacks>')
    problems += p
    for ident, f in defects.items():
        if f[2] not in STAGES:
            problems.append('%s: stage %r is not one of %s' % (ident, f[2], ', '.join(STAGES)))
        if ident in exceptions:
            problems.append('%s: both an exception and a defect' % ident)
        if not f[4].strip() or len(f[5].strip()) < 20:
            problems.append('%s: a defect names its owner and says what the library lacks (twenty characters at least)' % ident)

    runner = Runner(runner_path)
    scored, failures = [], {}
    scorers = (('registration', score_registration), ('reftest', score_reftest), ('cycles', score_cycles), ('limits', score_limits))
    seen = set()
    for kind, fn in scorers:
        for v in load(kind):
            ident = '%s#%s' % (v['file'], v['n'] if kind != 'limits' else v['id'])
            if kind == 'registration' or kind == 'cycles':
                ident = '%s#%d' % (v['file'], v['n'])
            if ident in seen:
                problems.append('%s: two vectors share the identity' % ident)
            seen.add(ident)
            scored.append((ident, kind))
            res = fn(runner, v)
            if res:
                failures[ident] = (res[0], res[1], kind, v)
    runner.close()

    summary = {}
    for ident, f in list(exceptions.items()) + list(defects.items()):
        pass
    unexplained = {i: f for i, f in failures.items() if i not in exceptions and i not in defects}
    stale = []
    for what, table, ans_col in (('exception', exceptions, 3), ('defect', defects, 3)):
        for ident, f in table.items():
            if ident not in failures:
                stale.append('%s: the case passes or does not exist (%s)' % (ident, what))
                continue
            stage, answer = failures[ident][0], failures[ident][1]
            if f[2] != stage:
                stale.append('%s: the case fails at %s, the %s says %s' % (ident, stage, what, f[2]))
            elif unescape(f[3]) != answer:
                stale.append('%s: the library answers %r, the %s says %r' % (ident, answer, what, unescape(f[3])))

    unext_bad = []
    if unext:
        def read_unext(path):
            out = {}
            for line in open(path, encoding='utf-8'):
                f = line.rstrip('\n').split('\t')
                if line.startswith('#') or len(f) < 2:
                    continue
                out[f[0]] = int(f[1])
            return out
        want, got = read_unext(unext[0]), read_unext(unext[1])
        for f in sorted(set(got) - set(want)):
            unext_bad.append('%s: not read (%d) and the list has no line for it' % (f, got[f]))
        for f in sorted(set(want) - set(got)):
            unext_bad.append('%s: listed (%d) and the extractor reads it in full now (stale)' % (f, want[f]))
        for f in sorted(set(want) & set(got)):
            if want[f] != got[f]:
                unext_bad.append('%s: the extractor leaves %d unread and the list says %d' % (f, got[f], want[f]))

    bad = False
    if problems:
        print('SUITE PROBLEMS:')
        for q in problems[:40]:
            print('  ' + q)
        bad = True
    if unexplained:
        print('UNEXPLAINED FAILURES: %d' % len(unexplained))
        for ident, (stage, answer, kind, v) in list(unexplained.items())[:(10**9 if os.environ.get('CSS_VARIABLES_LIST_ALL') else 60)]:
            print('  %s  %s: %s' % (ident, stage, answer[:200]))
        bad = True
    if unext_bad:
        print('UNEXTRACTED PAGES: %d differ from tools/css/UNEXTRACTED_VARIABLES' % len(unext_bad))
        for q in unext_bad[:40]:
            print('  ' + q)
        bad = True
    if stale:
        print('STALE EXCEPTIONS: %d' % len(stale))
        for s in stale[:40]:
            print('  ' + s)
        bad = True
    floor = int(os.environ.get('CSS_VARIABLES_MIN_SCORED', '0'))
    if len(scored) < floor:
        print('scored %d cases, below the floor of %d' % (len(scored), floor))
        bad = True
    ceiling = int(os.environ.get('CSS_VARIABLES_MAX_DEFECTS', '1000000'))
    if len(defects) > ceiling:
        print('%d defects listed, above the ceiling of %d (the ceiling only goes down)' % (len(defects), ceiling))
        bad = True
    if os.environ.get('CSS_VARIABLES_DUMP'):
        json.dump({i: [f[0], f[1]] for i, f in failures.items()}, open(os.environ['CSS_VARIABLES_DUMP'], 'w'))
    by_kind = {}
    for ident, kind in scored:
        by_kind[kind] = by_kind.get(kind, 0) + 1
    print('css variables: %d scored (%s), %d passed, %d listed exceptions, %d listed DEFECTS of the library' % (
        len(scored), ', '.join('%s %d' % (k, by_kind.get(k, 0)) for k, _ in scorers), len(scored) - len(failures),
        sum(1 for i in failures if i in exceptions), sum(1 for i in failures if i in defects)))
    by_stage = {}
    for f in failures.values():
        by_stage[f[0]] = by_stage.get(f[0], 0) + 1
    print('css variables: failures by stage: ' + ', '.join('%s %d' % (s, by_stage.get(s, 0)) for s in STAGES))
    sys.exit(1 if bad else 0)


main()
