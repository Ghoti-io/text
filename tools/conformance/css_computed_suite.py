"""Score the computed values of this library against web-platform-tests' computed-value tests.

    css_computed_suite.py <vectors dir> <pages.json> <runner> <exceptions> <defects> <unscored>

The vectors are made by tools/css/extract-wpt-props.py (the `test_computed_value("width", "10px", "10px")` calls of the pinned pages, read as data) and the
documents they are asked in by tools/css/extract-wpt-computed-pages.py. A vector is asked of the library the way the page asks it of a browser: `#target` of the
page's document, the page's style rules that the library can match (`#target`, `#container`, `body`, `html`, `:root`, `*`, an element's name) as author rules, a
user-agent sheet of a few rules, `target.style[property] = specified`, and `getComputedStyle(target)[property]` read as the CSSOM writes a resolved value (a color
as `rgb()`, a shorthand from its longhands). The answer must be the page's `computed` (any of an array), and writing that answer back as the value must give it again,
as the harness asserts.

* A vector the library cannot be asked is **unscored**, and every one is listed (tools/conformance/UNSCORED_COMPUTED: the page, how many vectors, why): a page that
  has a rule or a script this cannot model, a call that compares with a function or measures layout, a pseudo-element, a property the pin does not have. A listed page
  that is scored or has another count, and a page that is unscored and unlisted, fail ("UNSCORED PAGES").
* A scored vector that fails has stage `compute` and is in one of two lists. CSS_COMPUTED_EXCEPTIONS holds the cases where the page's expectation does not bind: the
  vector, the property, the value, the stage, the answer the library gives, a class and the sentence of the served text that says so (a quotation of four words and
  twenty characters at least): `draft-newer` (the standard has changed since the page), `wpt-tentative` (the page is named tentative), `needs-environment` (the answer
  depends on something the library is not given), `wpt-wrong` (the page contradicts its own specification), `resolved-value` (CSSOM's resolved value is not the computed
  value: used values, layout). CSS_COMPUTED_DEFECTS holds the cases where the page is right and the library is not: an owner and what the library lacks, under a
  ceiling that only goes down (CSS_COMPUTED_MAX_DEFECTS). In both lists the library must give exactly the answer listed. A failure in neither is "UNEXPLAINED
  FAILURES"; a listed case that passes, does not exist or answers otherwise is "STALE EXCEPTIONS"; the scored count has a floor (CSS_COMPUTED_MIN_SCORED).
* The depth ledger is not changed: the `computes` column is what the classification says (check-css-compute); this gate refines nothing in it, and a property whose
  vectors all pass is printed.
"""

import json
import os
import re
import subprocess
import sys

CLASSES = ('draft-newer', 'wpt-tentative', 'needs-environment', 'wpt-wrong', 'resolved-value')
STAGES = ('compute',)


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def css_name(js):
    if js == 'cssFloat':
        return 'float'
    if js.startswith('--') or '-' in js:
        return js.lower() if not js.startswith('--') else js
    out = re.sub(r'([A-Z])', lambda m: '-' + m.group(1).lower(), js)
    if out.startswith(('webkit-', 'moz-', 'ms-', 'o-')):
        out = '-' + out
    return out


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


def sep_join(items):
    return '\\r'.join(items)


def element_lines(model):
    out = []
    for i, e in enumerate(model['elements']):
        attrs = sep_join(escape('%s=%s' % (k, v)) for k, v in e['attrs'].items())
        rules = sep_join(escape('%d,%d,%d,%d,%s' % (r[0], r[1], r[2], r[3], r[4])) for r in e['rules'])
        out.append('E\t%d\t%d\t%s\t%s\t%s\t%s\n' % (i, e['parent'], 's' if e['ns'] == 's' else 'h', escape(e['local']), attrs, rules))
    return out


def ask(runner, pages, items):
    """items: [(file, n, prop, value)] grouped by file; returns {(file, n): fields}."""
    by_file = {}
    for f, n, p, v in items:
        by_file.setdefault(f, []).append((n, p, v))
    data = []
    order = []
    for f, tests in by_file.items():
        model = pages[f]
        data.append('P\t%s\t%d\n' % (escape(f), model['target']))
        data.extend(element_lines(model))
        for n, p, v in tests:
            data.append('T\t%d\t%s\t%s\n' % (n, escape(p), escape(v)))
            order.append((f, n))
        data.append('X\n')
    proc = subprocess.run([runner], input=''.join(data).encode('utf-8', 'surrogateescape'), stdout=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        print('SUITE PROBLEMS: the runner exited %d' % proc.returncode)
        sys.exit(1)
    answers = {}
    k = 0
    for line in proc.stdout.decode('utf-8', 'surrogateescape').split('\n'):
        if not line or line == 'X':
            continue
        f = line.split('\t')
        if f[0] != 'R':
            print('SUITE PROBLEMS: the runner answered %r' % line)
            sys.exit(1)
        if k >= len(order):
            print('SUITE PROBLEMS: the runner answered more lines than there were questions')
            sys.exit(1)
        answers[order[k]] = f[2:]
        k += 1
    if k != len(order):
        print('SUITE PROBLEMS: the runner answered %d lines to %d questions' % (k, len(order)))
        sys.exit(1)
    return answers


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    vec_dir, pages_path, runner, exceptions_path, defects_path, unscored_path = argv[:6]
    vectors = json.load(open(os.path.join(vec_dir, 'computed.json'), encoding='utf-8'))
    pages = json.load(open(pages_path, encoding='utf-8'))
    problems = []

    exceptions, p = load_table(exceptions_path, 7, 'not <id> TAB <property> TAB <value> TAB <stage> TAB <answer> TAB <class> TAB <reason>')
    problems += p
    for ident, f in exceptions.items():
        if f[3] not in STAGES:
            problems.append('%s: stage %r is not compute' % (ident, f[3]))
        if f[5] not in CLASSES:
            problems.append('%s: class %r is not one of %s' % (ident, f[5], ', '.join(CLASSES)))
        if f[5] == 'wpt-tentative':
            if 'tentative' not in ident:
                problems.append('%s: the class is wpt-tentative and the page is not named tentative' % ident)
        elif not quotes_a_sentence(f[6]):
            problems.append('%s: the reason quotes no sentence of the served text (a quotation of four words and twenty characters at least)' % ident)
    defects, p = load_table(defects_path, 7, 'not <id> TAB <property> TAB <value> TAB <stage> TAB <answer> TAB <owner> TAB <what the library lacks>')
    problems += p
    for ident, f in defects.items():
        if ident in exceptions:
            problems.append('%s: both an exception and a defect' % ident)
        if not f[5].strip() or len(f[6].strip()) < 20:
            problems.append('%s: a defect names its owner and says what the library lacks (twenty characters at least)' % ident)
    listed_unscored = {}
    for n, line in enumerate(open(unscored_path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) != 3:
            problems.append('%s:%d: not file, count, reason' % (unscored_path, n))
            continue
        key = (f[0], f[2])
        if key in listed_unscored:
            problems.append('%s:%d: %s twice' % (unscored_path, n, f[0]))
        if len(f[2]) < 20:
            problems.append('%s:%d: the reason says nothing' % (unscored_path, n))
        listed_unscored[key] = int(f[1])

    # which vectors are scored
    scored, unscored = [], {}
    for v in vectors:
        ident = '%s#%d' % (v['file'], v['n'])
        page = pages[v['file']]
        reason = None
        if not page['scorable']:
            reason = 'the page cannot be modeled: ' + page['reason']
        elif v['call'] == 'testComputedValueGreaterOrLowerThan':
            reason = 'the call compares with an inequality of a number'
        elif v['call'] == 'testTransformValuesCloseTo':
            reason = 'the call compares transform matrices within an epsilon'
        elif v['call'] in ('test_computed_serialization', 'test_used_serialization'):
            reason = 'the call is about a used value'
        elif v.get('pseudo'):
            reason = 'a pseudo-element (selectors, C3)'
        elif v.get('custom'):
            reason = 'the page compares with a function of its own'
        elif not v.get('prop') or v.get('value') is None:
            reason = 'the call has no property or no value the extractor can read'
        if reason:
            unscored[(v['file'], reason)] = unscored.get((v['file'], reason), 0) + 1
        else:
            scored.append((ident, v))

    # the properties the pin does not have
    requests = []
    for ident, v in scored:
        requests.append((v['file'], v['n'], css_name(v['prop']), v['value']))
    answers = ask(runner, pages, requests)
    kept = []
    for ident, v in scored:
        a = answers[(v['file'], v['n'])]
        if a[0] == 'UNKNOWN':
            key = (v['file'], 'the pin has no property ' + css_name(v['prop']))
            unscored[key] = unscored.get(key, 0) + 1
        else:
            kept.append((ident, v, a))
    scored = kept

    failures = {}
    clean = {}
    for ident, v, a in scored:
        prop = css_name(v['prop'])
        want = [v['expected']] if v.get('expected') is not None else [v['value']]
        want += list(v.get('alts') or [])
        stage = answer = None
        if a[0] == 'OK':
            text, again = unescape(a[1]), unescape(a[2])
            if text not in want:
                stage, answer = 'compute', text
            elif text != v['value'] and again != text:
                stage, answer = 'compute', 'roundtrip: ' + again
        elif a[0] == 'INVALID':
            stage, answer = 'compute', 'INVALID'
        elif a[0] == 'UNSUPPORTED':
            stage, answer = 'compute', 'UNSUPPORTED ' + unescape(a[1] if len(a) > 1 else '')
        else:
            stage, answer = 'compute', ' '.join(a)
        if stage:
            failures[ident] = (stage, answer, v, prop)
            clean[prop] = False
        else:
            clean.setdefault(prop, True)

    unexplained = {i: f for i, f in failures.items() if i not in exceptions and i not in defects}
    stale = []
    for what, table in (('exception', exceptions), ('defect', defects)):
        for ident, f in table.items():
            if ident not in failures:
                stale.append('%s: the case passes or does not exist (%s)' % (ident, what))
                continue
            stage, answer, v, prop = failures[ident]
            if f[1] != v['prop'] or unescape(f[2]) != v['value']:
                stale.append('%s: the %s says %s %r, the vector is %s %r' % (ident, what, f[1], unescape(f[2]), v['prop'], v['value']))
            elif unescape(f[4]) != answer:
                stale.append('%s: the library answers %r, the %s says %r' % (ident, answer, what, unescape(f[4])))

    unscored_bad = []
    for key, n in sorted(unscored.items()):
        if key not in listed_unscored:
            unscored_bad.append('%s: %d vectors not scored (%s) and the list has no line for it' % (key[0], n, key[1]))
        elif listed_unscored[key] != n:
            unscored_bad.append('%s: %d vectors not scored (%s) and the list says %d' % (key[0], n, key[1], listed_unscored[key]))
    for key in sorted(set(listed_unscored) - set(unscored)):
        unscored_bad.append('%s: listed as not scored (%s) and every vector of it is scored or gone (stale)' % key)

    bad = False
    if problems:
        print('SUITE PROBLEMS:')
        for q in problems[:40]:
            print('  ' + q)
        bad = True
    if unexplained:
        print('UNEXPLAINED FAILURES: %d' % len(unexplained))
        limit = 10 ** 9 if os.environ.get('CSS_COMPUTED_LIST_ALL') else 40
        for ident, (stage, answer, v, prop) in list(unexplained.items())[:limit]:
            print('  %s  %s: %s %s -> %s  [want %s]' % (ident, stage, v['prop'], json.dumps(v['value'], ensure_ascii=False), answer,
                                                         json.dumps([v['expected']] + list(v.get('alts') or []) if v.get('expected') is not None else [v['value']], ensure_ascii=False)))
        bad = True
    if stale:
        print('STALE EXCEPTIONS: %d' % len(stale))
        for s in stale[:40]:
            print('  ' + s)
        bad = True
    if unscored_bad:
        print('UNSCORED PAGES: %d lines of tools/conformance/UNSCORED_COMPUTED differ' % len(unscored_bad))
        for s in unscored_bad[:40]:
            print('  ' + s)
        bad = True
    floor = int(os.environ.get('CSS_COMPUTED_MIN_SCORED', '0'))
    if len(scored) < floor:
        print('scored %d cases, below the floor of %d' % (len(scored), floor))
        bad = True
    ceiling = int(os.environ.get('CSS_COMPUTED_MAX_DEFECTS', '1000000'))
    if len(defects) > ceiling:
        print('%d defects listed, above the ceiling of %d (the ceiling only goes down)' % (len(defects), ceiling))
        bad = True
    if os.environ.get('CSS_COMPUTED_DUMP'):
        json.dump({i: [f[0], f[1], f[2]['prop'], f[2]['value'], f[2].get('expected')] for i, f in failures.items()}, open(os.environ['CSS_COMPUTED_DUMP'], 'w'))

    if os.environ.get('CSS_COMPUTED_UNSCORED_DUMP'):
        with open(os.environ['CSS_COMPUTED_UNSCORED_DUMP'], 'w', encoding='utf-8') as h:
            for (f, reason), n in sorted(unscored.items()):
                h.write('%s\t%d\t%s\n' % (f, n, reason))

    npass = len(scored) - len(failures)
    clean_props = sorted(p for p, ok in clean.items() if ok)
    print('css computed: %d scored, %d passed, %d listed exceptions, %d listed DEFECTS of the library; %d vectors not scored in %d pages (UNSCORED_COMPUTED)' % (
        len(scored), npass, sum(1 for i in failures if i in exceptions), sum(1 for i in failures if i in defects), sum(unscored.values()),
        len({k[0] for k in unscored})))
    print('css computed: %d properties have every scored vector passing' % len(clean_props))
    by_class = {}
    for i in failures:
        if i in exceptions:
            by_class[exceptions[i][5]] = by_class.get(exceptions[i][5], 0) + 1
    print('css computed: exceptions by class: ' + ', '.join('%s %d' % (c, by_class.get(c, 0)) for c in CLASSES))
    sys.exit(1 if bad else 0)


main()
