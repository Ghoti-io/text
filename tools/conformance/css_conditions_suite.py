"""Score the media query layer against the media query vectors of the pinned web-platform-tests.

    css_conditions_suite.py <vectors dir> <runner> <exceptions> <unscored> [--regenerate]

The vectors are made by tools/css/extract-wpt-conditions.py: ``serialize`` vectors (what `matchMedia(q).media` is) and ``reftest`` vectors (an @media rule of a
reference test that paints green where the query must apply and red where it must not, in the 800 by 600 screen the reftest harness gives a page).

Every failure has to be listed, one line a case, in CSS_CONDITIONS_EXCEPTIONS: the vector (file#line), the query, the answer the library gives, a class
(draft-newer, wpt-tentative, needs-environment, wpt-wrong) and the reason, which for draft-newer and wpt-wrong quotes a sentence of the served draft. The gate fails on a
failure that is not listed ("UNEXPLAINED FAILURES"), on a listed case that passes, does not exist, is another query or has another answer ("STALE EXCEPTIONS"), on a
reason without a quote where one is needed or a class outside the four ("SUITE PROBLEMS"), when fewer cases were scored than the floor (CSS_CONDITIONS_MIN_SCORED,
"below the floor"), and on a runner that does not answer.

The pages that are not read are in UNSCORED_CONDITIONS, one line for a page: the file, the number of its calls and the reason. A page or a count that differs fails
("UNSCORED VECTORS").
"""
import json
import os
import re
import subprocess
import sys

CLASSES = ('draft-newer', 'wpt-tentative', 'needs-environment', 'wpt-wrong')


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def quotes_a_sentence(reason):
    return any(len(q) >= 20 and len(q.split()) >= 4 for q in re.findall(r'"([^"]*)"', reason))


def load_tsv(path):
    rows = []
    if os.path.exists(path):
        for n, line in enumerate(open(path, encoding='utf-8'), 1):
            line = line.rstrip('\n')
            if line and not line.startswith('#'):
                rows.append((n, line.split('\t')))
    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    regenerate = '--regenerate' in sys.argv
    vdir, runner, exc_path, unscored_path = args[:4]
    vectors = json.load(open(os.path.join(vdir, 'media.json'), encoding='utf-8'))
    asks = []
    for v in vectors:
        if v['kind'] == 'serialize':
            asks.append(('serialize', v['query']))
        else:
            for b in v['blocks']:
                asks.append(('match', b[0]))
    data = ''.join(escape(k) + '\t' + escape(q) + '\n' for k, q in asks)
    proc = subprocess.run([runner], input=data.encode('utf-8', 'surrogateescape'), stdout=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        print('SUITE PROBLEMS: the runner exited %d' % proc.returncode)
        return 1
    answers = proc.stdout.decode('utf-8', 'surrogateescape').split('\n')[:-1]
    if len(answers) != len(asks):
        print('SUITE PROBLEMS: %d answers for %d questions' % (len(answers), len(asks)))
        return 1
    failures = {}
    it = iter(answers)
    for v in vectors:
        if v['kind'] == 'serialize':
            a = next(it)
            got = unescape(a.split(':', 1)[1]) if ':' in a else a
            if not (a.startswith('ser:') and got == v['expected']):
                failures['%s#%d' % (v['file'], v['line']) + '\t' + v['query']] = (v, a)
        else:
            final = v['base']
            errs = []
            for b in v['blocks']:
                a = next(it)
                if a == 'match:1':
                    final = b[1]
                elif a != 'match:0':
                    errs.append(a)
            if errs or final != 'green':
                failures['%s#%d' % (v['file'], v['line']) + '\t' + v['query']] = (v, errs[0] if errs else 'final:%s' % final)
    scored = len(vectors)
    if regenerate:
        for k, (v, a) in sorted(failures.items()):
            print('%s\t%s\t%s\tTODO\tTODO' % (k.split('\t')[0], v['query'], a))
        return 0
    problems = []
    exceptions = {}
    for n, f in load_tsv(exc_path):
        if len(f) != 5:
            problems.append('SUITE PROBLEMS: %s:%d has %d fields, not five' % (os.path.basename(exc_path), n, len(f)))
            continue
        vec, query, got, cls, reason = f
        if cls not in CLASSES:
            problems.append('SUITE PROBLEMS: %s:%d: class %r is not one of %s' % (os.path.basename(exc_path), n, cls, ', '.join(CLASSES)))
        elif cls in ('draft-newer', 'wpt-wrong') and not quotes_a_sentence(reason):
            problems.append('SUITE PROBLEMS: %s:%d: a %s reason quotes a sentence of the served draft' % (os.path.basename(exc_path), n, cls))
        exceptions[vec + '\t' + query] = (got, n)
    unexplained = [k for k in failures if k not in exceptions]
    stale = []
    for k, (got, n) in exceptions.items():
        if k not in failures:
            stale.append('%s:%d: %s passes or does not exist' % (os.path.basename(exc_path), n, k.replace('\t', ' ')))
        elif failures[k][1] != got:
            stale.append('%s:%d: %s answers %s, not %s' % (os.path.basename(exc_path), n, k.replace('\t', ' '), failures[k][1], got))
    out = list(problems)
    if unexplained:
        out.append('UNEXPLAINED FAILURES (%d):' % len(unexplained))
        for k in unexplained[:60]:
            v, a = failures[k]
            out.append('  %s  %r expected %r, got %s%s' % (k.split('\t')[0], v['query'], v['expected'], a, '  blocks=%s base=%s' % (v['blocks'], v['base']) if v['kind'] == 'reftest' else ''))
    if stale:
        out.append('STALE EXCEPTIONS:')
        out.extend('  ' + s for s in stale)
    # The pages not read.
    have = {}
    for line in open(os.path.join(vdir, 'UNEXTRACTED_CONDITIONS'), encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        have[f[0]] = f[1]
    listed = {}
    for n, f in load_tsv(unscored_path):
        if len(f) != 3:
            out.append('SUITE PROBLEMS: %s:%d has %d fields, not three' % (os.path.basename(unscored_path), n, len(f)))
            continue
        listed[f[0]] = f[1]
    if have != listed:
        out.append('UNSCORED VECTORS: the pages not read differ from %s' % os.path.basename(unscored_path))
        for k in sorted(set(have) | set(listed)):
            if have.get(k) != listed.get(k):
                out.append('  %s: read %s, listed %s' % (k, have.get(k), listed.get(k)))
    floor = int(os.environ.get('CSS_CONDITIONS_MIN_SCORED', '0'))
    if scored < floor:
        out.append('below the floor: %d scored, the floor is %d' % (scored, floor))
    if out:
        print('\n'.join(out))
        return 1
    print('css-conditions: %d vectors scored, %d pass, %d are exceptions; %d pages not read' % (scored, scored - len(failures), len(failures), len(have)))
    return 0


sys.exit(main())
