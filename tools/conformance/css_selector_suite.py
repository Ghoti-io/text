"""Score the selector layer against web-platform-tests' selector parsing tests.

    css_selector_suite.py <vectors dir> <runner> <exceptions> <unscored> [--regenerate]

The vectors are made by tools/css/extract-wpt-props.py from the pinned checkout: the calls of `test_valid_selector`, `test_valid_forgiving_selector`
and `test_invalid_selector` (selector.json) and of `test_valid_rule` and its kin (rule.json), read as data. A selector is read as the browsers read
`sheet.insertRule(selector + "{}")`: a style rule's prelude, by the rule parser and then the selector parser.

* **valid** and **forgiving** vectors: the library must read the selector, write it as the page expects (the string, or any of the permitted ones), and read
  its own writing back as the same writing, as the harness asserts. A failure is tagged by the stage that failed: `parse`, `serialize`, `roundtrip`.
* **invalid** vectors: the selector must be refused. A failure is `invalid-accepted`.

Every failure has to be listed, one line a case, in CSS_SELECTOR_EXCEPTIONS: the vector (file#n), the selector, the stage, the answer the library gives,
a class (draft-newer, wpt-tentative, needs-environment, wpt-wrong) and the reason, which quotes a sentence of the served draft that decides it (a
`wpt-tentative` case is one of a `.tentative.html` file, web-platform-tests' own mark for a behavior no served draft specifies, and needs no quote). The gate fails
on a failure that is not listed ("UNEXPLAINED FAILURES"), on a listed case that passes, does not exist, is another selector, is at another stage or has another answer
("STALE EXCEPTIONS"), on a reason that quotes no sentence or a class that is not one of the four ("SUITE PROBLEMS"), when fewer cases were scored than the floor
(CSS_SELECTOR_MIN_SCORED, "below the floor"), and on a runner that does not answer.

The vectors that are not scored are in UNSCORED_SELECTORS, one line for a file: the file, the number of its vectors and the reason (the rule vectors of at-rules
that other work scores). A file or a count that differs from the list fails ("UNSCORED VECTORS").
"""

import json
import os
import re
import subprocess
import sys

CLASSES = ('draft-newer', 'wpt-tentative', 'needs-environment', 'wpt-wrong')
STAGES = ('parse', 'serialize', 'roundtrip', 'invalid-accepted')


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def quotes_a_sentence(reason):
    return any(len(q) >= 20 and len(q.split()) >= 4 for q in re.findall(r'"([^"]*)"', reason))


def run_runner(runner, values):
    data = ''.join(escape(v) + '\n' for v in values)
    proc = subprocess.run([runner], input=data.encode('utf-8', 'surrogateescape'), stdout=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        print('SUITE PROBLEMS: the runner exited %d' % proc.returncode)
        sys.exit(1)
    lines = proc.stdout.decode('utf-8', 'surrogateescape').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    if len(lines) != len(values):
        print('SUITE PROBLEMS: the runner answered %d lines to %d requests' % (len(lines), len(values)))
        sys.exit(1)
    return lines


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


def vector_id(v):
    return '%s#%d' % (v['file'], v['n'])


def score(vectors_dir, runner):
    """[(id, selector, stage, answer)] for every failure, and the counts."""
    sel = json.load(open(os.path.join(vectors_dir, 'selector.json'), encoding='utf-8'))
    answers = run_runner(runner, [v['value'] for v in sel])
    failures = []
    scored = 0
    for v, a in zip(sel, answers):
        scored += 1
        vid = vector_id(v)
        if a.startswith('ERR'):
            failures.append((vid, v['value'], 'parse', a.replace('\t', ' ')))
            continue
        if v['sub'] == 'invalid':
            if a != 'INVALID':
                failures.append((vid, v['value'], 'invalid-accepted', unescape(a.split('\t')[1])))
            continue
        if a == 'INVALID':
            failures.append((vid, v['value'], 'parse', 'INVALID'))
            continue
        parts = a.split('\t')
        text, again = unescape(parts[1]), unescape(parts[2]) if len(parts) > 2 else ''
        want = [v['expected']] + list(v.get('alts') or [])
        if text not in want:
            failures.append((vid, v['value'], 'serialize', text))
        elif again != text:
            failures.append((vid, v['value'], 'roundtrip', again))
    return sel, failures, scored


def main():
    args = sys.argv[1:]
    regenerate = '--regenerate' in args
    args = [a for a in args if a != '--regenerate']
    vectors_dir, runner, exceptions_path, unscored_path = args[:4]
    sel, failures, scored = score(vectors_dir, runner)
    by_id = {vector_id(v): v for v in sel}
    if regenerate:
        with open(exceptions_path, 'w', encoding='utf-8') as f:
            f.write('# tools/conformance/css_selector_suite.py: the selector vectors of web-platform-tests\' css/ that fail and whose expectation does not bind this library, one line a case.\n')
            f.write('# Columns (tab separated): vector (file#n), selector, stage (parse, serialize, roundtrip, invalid-accepted), the answer the library gives, class, reason.\n')
            for vid, value, stage, answer in failures:
                v = by_id[vid]
                tentative = v['file'].endswith('.tentative.html')
                f.write('\t'.join([vid, escape(value), stage, escape(answer), 'wpt-tentative' if tentative else 'UNCLASSIFIED',
                                   'a tentative test: no served draft specifies it' if tentative else 'TODO']) + '\n')
        print('wrote %d' % len(failures))
        return 0
    exceptions, problems = load_table(exceptions_path, 6, 'not six')
    unscored, uproblems = load_table(unscored_path, 3, 'not three')
    problems += uproblems
    for vid, f in exceptions.items():
        if f[4] not in CLASSES:
            problems.append('%s: class %r is not one of %s' % (vid, f[4], ', '.join(CLASSES)))
        elif f[4] == 'wpt-tentative':
            if not vid.split('#')[0].endswith('.tentative.html'):
                problems.append('%s: class wpt-tentative on a page that is not a tentative test' % vid)
        elif not quotes_a_sentence(f[5]):
            problems.append('%s: the reason quotes no sentence of the served draft' % vid)
        if f[2] not in STAGES:
            problems.append('%s: stage %r is not one of %s' % (vid, f[2], ', '.join(STAGES)))
    unexplained, stale = [], []
    seen = set()
    for vid, value, stage, answer in failures:
        e = exceptions.get(vid)
        if not e:
            unexplained.append((vid, value, stage, answer))
            continue
        seen.add(vid)
        if unescape(e[1]) != value or e[2] != stage or unescape(e[3]) != answer:
            stale.append('%s: listed as %s %r -> %r, the library gives %s %r -> %r' % (vid, e[2], unescape(e[1]), unescape(e[3]), stage, value, answer))
    for vid in exceptions:
        if vid not in seen:
            stale.append('%s: listed, and it passes or does not exist' % vid)
    # the vectors that are not scored
    rule = json.load(open(os.path.join(vectors_dir, 'rule.json'), encoding='utf-8'))
    counts = {}
    for v in rule:
        counts[v['file']] = counts.get(v['file'], 0) + 1
    bad_unscored = []
    for file, n in counts.items():
        u = unscored.get(file)
        if not u:
            bad_unscored.append('%s: %d rule vectors are not scored and not listed' % (file, n))
        elif int(u[1]) != n:
            bad_unscored.append('%s: listed with %s vectors, the page has %d' % (file, u[1], n))
    for file in unscored:
        if file not in counts:
            bad_unscored.append('%s: listed, and it has no rule vectors' % file)
    floor = int(os.environ.get('CSS_SELECTOR_MIN_SCORED', '600'))
    out = False
    if problems:
        print('SUITE PROBLEMS:')
        print('\n'.join('  ' + p for p in problems))
        out = True
    if unexplained:
        print('UNEXPLAINED FAILURES: %d' % len(unexplained))
        for vid, value, stage, answer in unexplained[:60]:
            print('  %s [%s] %r -> %r' % (vid, stage, value, answer))
        out = True
    if stale:
        print('STALE EXCEPTIONS: %d' % len(stale))
        print('\n'.join('  ' + s for s in stale[:60]))
        out = True
    if bad_unscored:
        print('UNSCORED VECTORS: %d' % len(bad_unscored))
        print('\n'.join('  ' + s for s in bad_unscored))
        out = True
    if scored < floor:
        print('below the floor: %d scored, the floor is %d' % (scored, floor))
        out = True
    ceiling = os.environ.get('CSS_SELECTOR_MAX_EXCEPTIONS')
    if ceiling is not None and len(exceptions) > int(ceiling):
        print('above the ceiling: %d exceptions, the ceiling is %s' % (len(exceptions), ceiling))
        out = True
    if out:
        return 1
    classes = {}
    for f in exceptions.values():
        classes[f[4]] = classes.get(f[4], 0) + 1
    print('css selectors: %d scored, %d pass, %d exceptions (%s), %d rule vectors not scored in %d pages'
          % (scored, scored - len(failures), len(exceptions), ', '.join('%s %d' % kv for kv in sorted(classes.items())) or 'none',
             sum(counts.values()), len(counts)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
