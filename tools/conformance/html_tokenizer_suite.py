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
"""Score the HTML tokenizer against the pinned html5lib-tests tokenizer vectors.

Usage: html_tokenizer_suite.py <html5lib-tests/tokenizer dir> <runner> [exceptions-file]

Every `*.test` file there is read: the `tests` of each, and the
`xmlViolationTests` of xmlViolation.test. A case is run once for each of its
`initialStates` (Data state if there are none) with its `lastStartTag`, and
`doubleEscaped` applied to the input and every string of the output, as the
vectors' README says.

The runner (html_tokenizer_suite.c) takes one request per line and prints the
tokens and the parse errors; `README.md` of the vectors is the format the
expectation is in. What is compared:

  tokens   the whole list, with adjacent characters already coalesced, by
           kind and every field (a start tag's attributes as a set, since a
           JSON object has no order; the order is checked by the differential).
  errors   code, line and column, in order. A vector without an "errors" entry
           expects none: the vectors written before errors were recorded say
           nothing, and "nothing" is checked, so a tokenizer that reports an
           error on a clean input fails them.

Every case is also run with the input cut a byte at a time and in pieces of
1 to 9 bytes, and each must give *exactly* the whole-input run's output - the
same tokens, errors, positions and byte offsets. That is not scored against
the vectors and has no exceptions list: a case where cutting changes the
answer is a defect in the tokenizer.

A scored case that fails is a failure of the gate unless it is listed in the
exceptions file as `<id><TAB><reason>`. An exception that no longer fails is
also a failure, and so is one that is not in the suite. The id is
`<file>#<index>` (the position in the file's list, from 0) and `@<state>` if it
is run in a state other than the data state. The exit status is the verdict;
it is non-zero if anything fails unexplained, if any exception is stale, if the
runner crashed, or if fewer than HTML_TOKENIZER_MIN_SCORED runs were scored.
"""
import collections
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from html_tokenizer_lib import (STATES, coalesce, enc, hexs, parse_answer,  # noqa: E402
                                run_all, unescape, unhex)

SUITE = sys.argv[1]
RUNNER = sys.argv[2]
EXCEPTIONS = sys.argv[3] if len(sys.argv) > 3 else None
# A floor on the runs scored, so that a truncated corpus cannot pass by being small. The
# environment can raise or lower it (the gate's own control does); it cannot be skipped.
DEFAULT_MIN_SCORED = 7000

def load_cases():
    cases = []
    for path in sorted(glob.glob(os.path.join(SUITE, '*.test'))):
        name = os.path.basename(path)
        with open(path, encoding='utf-8') as fh:
            data = json.load(fh)
        listing = data.get('tests', data.get('xmlViolationTests'))
        if listing is None:
            sys.exit('%s: neither "tests" nor "xmlViolationTests"' % name)
        for index, t in enumerate(listing):
            esc = bool(t.get('doubleEscaped'))
            fix = unescape if esc else (lambda s: s)
            states = t.get('initialStates', ['Data state'])
            for st in states:
                if st not in STATES:
                    sys.exit('%s#%d: unknown initial state %r' % (name, index, st))
                ident = '%s#%d' % (name, index)
                if st != 'Data state':
                    ident += '@' + STATES[st]
                cases.append({
                    'id': ident,
                    'file': name,
                    'description': t['description'],
                    'input': enc(fix(t['input'])),
                    'state': STATES[st],
                    'last': t.get('lastStartTag'),
                    'output': [expected_token(tok, fix) for tok in t['output']],
                    'errors': [(e['code'], e['line'], e['col']) for e in t.get('errors', [])],
                })
    return cases


def expected_token(tok, fix):
    kind = tok[0]
    if kind == 'DOCTYPE':
        name, public, system, correct = tok[1:5]
        return ('D', name is not None, enc(fix(name)) if name is not None else b'',
                public is not None, enc(fix(public)) if public is not None else b'',
                system is not None, enc(fix(system)) if system is not None else b'',
                not correct)
    if kind in ('StartTag', 'EndTag'):
        if kind == 'EndTag':
            # The vectors' end tag carries a name and nothing else, though the
            # standard's has attributes (an error) and a flag.
            return ('E', enc(fix(tok[1])))
        attrs = tok[2]
        selfclosing = bool(len(tok) > 3 and tok[3])
        return ('S', enc(fix(tok[1])),
                frozenset((enc(fix(k)), enc(fix(v))) for k, v in attrs.items()),
                len(attrs), selfclosing)
    if kind == 'Comment':
        return ('M', enc(fix(tok[1])))
    if kind == 'ProcessingInstruction':
        return ('P', enc(fix(tok[1])), enc(fix(tok[2])))
    if kind == 'Character':
        return ('C', enc(fix(tok[1])))
    sys.exit('unknown expected token %r' % (tok,))


def load_exceptions():
    table = {}
    if EXCEPTIONS and os.path.exists(EXCEPTIONS):
        with open(EXCEPTIONS, encoding='utf-8') as fh:
            for raw in fh:
                raw = raw.rstrip('\n')
                if not raw.strip() or raw.startswith('#'):
                    continue
                ident, _, reason = raw.partition('\t')
                if not reason.strip():
                    sys.exit('exception %r has no reason' % ident)
                table[ident] = reason.strip()
    return table


def show(tok):
    return repr(tok)


cases = load_cases()
if not cases:
    sys.exit('no vectors in %s' % SUITE)
exceptions = load_exceptions()

requests = []
slot = {}  # (case index, kind) -> index into requests
STRICT_OK = 'strict'
for i, c in enumerate(cases):
    last = '-' if c['last'] is None else hexs(enc(c['last']))
    base = '%s %s ' % (c['state'], last) + hexs(c['input'])
    for kind, head, tail in (('whole', 'whole ', ''), ('bytes', 'bytes ', ''),
                             ('chunks', 'chunks:%d ' % (i + 1), ''), ('limits', 'limits:%d ' % (i + 1), '')):
        slot[(i, kind)] = len(requests)
        requests.append(head + base + tail)
    # The strict UTF-8 path is scored on the inputs that are UTF-8: those with no lone surrogate.
    try:
        c['input'].decode('utf-8')
        slot[(i, STRICT_OK)] = len(requests)
        requests.append('whole ' + base + ' strict')
    except UnicodeDecodeError:
        pass

answers, crashed = run_all(RUNNER, requests)

passed = collections.Counter()
failed_by_file = collections.Counter()
total_by_file = collections.Counter()
failures, explained, cut_differs = [], [], []
status_bad = []
limit_bad = []
strict_runs = 0
for i, c in enumerate(cases):
    whole, by_bytes, chunks = answers[slot[(i, 'whole')]], answers[slot[(i, 'bytes')]], answers[slot[(i, 'chunks')]]
    limited = answers[slot[(i, 'limits')]]
    if whole is None or by_bytes is None or chunks is None or limited is None:
        continue
    if (i, STRICT_OK) in slot:
        strict = answers[slot[(i, STRICT_OK)]]
        strict_runs += 1
        if strict is None or strict != whole:
            cut_differs.append((c['id'], 'strict UTF-8', whole, strict or ['(no answer)']))
    # Under tiny limits chosen by a seed, the run stops at a limit or finishes; what it delivered
    # before it stopped is the beginning of what the roomy run delivered, errors included, and
    # a run that finished kept the first errors the roomy run raised and counted the rest.
    l_tokens, l_errors, l_dropped, l_status = parse_answer(limited)
    w_tokens, w_errors, _w_dropped, w_status = parse_answer(whole)
    w_tokens = [t for t in coalesce(w_tokens) if t[0] != 'F']
    l_tokens = [t for t in coalesce(l_tokens) if t[0] != 'F']
    if l_status == 'DONE' or l_status.startswith('E_LIMIT'):
        if l_tokens != w_tokens[:len(l_tokens)] or (l_status == 'DONE' and l_tokens != w_tokens):
            limit_bad.append((c['id'], 'tokens', l_status))
        elif [e[:3] for e in l_errors] != [e[:3] for e in w_errors[:len(l_errors)]]:
            limit_bad.append((c['id'], 'errors', l_status))
        elif l_status == 'DONE' and l_dropped != len(w_errors) - len(l_errors):
            limit_bad.append((c['id'], 'dropped', l_status))
    else:
        limit_bad.append((c['id'], 'status', l_status))
    total_by_file[c['file']] += 1
    if by_bytes != whole:
        cut_differs.append((c['id'], 'bytes', whole, by_bytes))
    if chunks != whole:
        cut_differs.append((c['id'], 'chunks', whole, chunks))
    tokens, errors, dropped, status = parse_answer(whole)
    tokens = coalesce(tokens)
    if status != 'DONE':
        status_bad.append((c['id'], status))
        continue
    want_tokens = coalesce(c['output'])
    # The vectors do not list the end-of-file token.
    got_tokens = [t for t in tokens if t[0] != 'F']
    problems = []
    if got_tokens != want_tokens:
        problems.append('tokens\n        want %s\n        got  %s' % (
            '\n             '.join(map(show, want_tokens)),
            '\n             '.join(map(show, got_tokens))))
    got_errors = [(e[0], e[1], e[2]) for e in errors]
    if got_errors != c['errors']:
        problems.append('errors\n        want %s\n        got  %s' % (c['errors'], got_errors))
    if not problems:
        passed[c['file']] += 1
        if c['id'] in exceptions:
            pass  # reported as stale below
        continue
    failed_by_file[c['file']] += 1
    detail = '%s: %s\n      input %r' % (c['id'], c['description'], c['input'])
    if c['id'] in exceptions:
        explained.append((c['id'], exceptions[c['id']]))
    else:
        failures.append((c['id'], detail, problems))

total = sum(total_by_file.values())
total_pass = sum(passed.values())
print('html5lib-tests tokenizer vectors: %d runs (%d cases over %d files, each in each of its initial states)' % (
    len(cases), len({(c['file'], c['id'].split('@')[0]) for c in cases}), len(total_by_file)))
print()
for f in sorted(total_by_file):
    print('  %-30s %5d of %5d' % (f, passed[f], total_by_file[f]))
print('  %-30s %5d of %5d' % ('total', total_pass, total))
print()
print('cut a byte at a time and in pieces of 1 to 9 bytes, and (for the %d inputs with no lone surrogate) read as strict UTF-8, against the whole-input run: %d runs, %d differ' % (
    strict_runs, 2 * len(cases) + strict_runs, len(cut_differs)))
print('under tiny limits chosen by a seed (a name of 1 to 12 bytes, 1 to 4 attributes, 1 to 6 errors kept ...): %d runs, %d break the prefix property' % (
    len(cases), len(limit_bad)))
print('exceptions (runs that fail, each with its reason): %d' % len(explained))
for r, v in collections.Counter(r for _, r in explained).most_common():
    print('  %4d  %s' % (v, r))

status = 0
if failures:
    status = 1
    print()
    print('UNEXPLAINED FAILURES: %d' % len(failures))
    for ident, detail, problems in failures[:60]:
        print('  ' + detail)
        for p in problems:
            print('    ' + p)
if limit_bad:
    status = 1
    print()
    print('A RUN UNDER TINY LIMITS IS NOT THE BEGINNING OF THE ROOMY RUN: %d' % len(limit_bad))
    for ident, what, st in limit_bad[:20]:
        print('  %s: %s (%s)' % (ident, what, st))
if cut_differs:
    status = 1
    print()
    print('THE ANSWER DEPENDS ON HOW THE INPUT WAS CUT: %d' % len(cut_differs))
    for ident, how, a, b in cut_differs[:20]:
        print('  %s (%s)' % (ident, how))
        print('      whole: %s' % ' | '.join(a))
        print('      cut:   %s' % ' | '.join(b))
if status_bad:
    status = 1
    print()
    print('THE TOKENIZER STOPPED BEFORE THE END: %d' % len(status_bad))
    for ident, st in status_bad[:20]:
        print('  %s: %s' % (ident, st))
if crashed:
    status = 1
    print()
    print('RUNNER CRASHES / TIMEOUTS: %d' % len(crashed))
    for i in crashed[:20]:
        print('  %s' % requests[i][:120])
stale = [i for i in exceptions if i not in {e[0] for e in explained}]
if stale:
    status = 1
    print()
    print('STALE EXCEPTIONS (listed, but the run no longer fails, or is not in the suite): %d' % len(stale))
    for ident in stale[:50]:
        print('  %s: %s' % (ident, exceptions[ident]))
floor = os.environ.get('HTML_TOKENIZER_MIN_SCORED', str(DEFAULT_MIN_SCORED))
if total < int(floor):
    status = 1
    print()
    print('conformance: only %d runs were scored, below the floor of %s' % (total, floor))
if status == 0:
    print()
    print('conformance: every scored run passes or is an explained exception')
sys.exit(status)
