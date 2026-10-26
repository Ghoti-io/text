"""Score the CSS property layer against web-platform-tests' property parsing tests.

    css_property_suite.py <vectors dir> <runner> <exceptions> <defects> <absent>
        [--unextracted LIST GOT] [--depth LEDGER --depth-floor FILE [--write-depth]]

The vectors are made by tools/css/extract-wpt-props.py from the pinned checkout: the calls of `test_valid_value`,
`test_invalid_value`, `test_shorthand_value` and their kin, read as data. This scores three kinds:

* **valid** (`test_valid_value`, `test_specified_serialization`): the library must parse the value as the value of the property,
  write it as the page expects (the string, or any of the array the page gave), and read its own writing back as the same
  writing, as the harness asserts. A failure is tagged by the stage that failed: `parse` (the value was refused, or the library
  cannot judge the property), `serialize` (it was written differently), `roundtrip` (its writing was refused or written differently
  again).
* **invalid** (`test_invalid_value`): the value must be refused. A failure is `invalid-accepted`.
* **shorthand** (`test_shorthand_value`): the value must parse as the value of the shorthand (`parse`); what the page expects of
  the longhands is the cascade's (C2a) and is counted, not scored.

Not scored in this slice, and printed with their counts: computed-value vectors (C2d), selector vectors (C3), rule vectors (C4),
the serialization of a shorthand (C2a: a valid vector whose property is a shorthand is held to `parse` and `roundtrip` only), and
the expected string of a vector the page compares with its own function (held to `parse` and `roundtrip` only).

Every failure has to be listed, one line per case, in one of two files. CSS_PROPERTY_EXCEPTIONS holds the cases where the page's
expectation does not apply: the vector, the property, the value, the stage, the answer the library gives, a class (draft-newer,
wpt-tentative, needs-environment, wpt-wrong) and the sentence of the served draft that says so. CSS_PROPERTY_DEFECTS holds the cases where
the page is right and the library is not: the same columns, an owner (the piece of work that closes it) and what the library lacks.
A defect is never an exception, the number of defects has a ceiling that only goes down (CSS_PROPERTY_MAX_DEFECTS), and it is printed with
every run. In both lists the library must give exactly the answer listed. The gate fails on a failure that
is in neither list ("UNEXPLAINED FAILURES"), on a listed case that passes, does not exist, is another property or value, is at
another stage or has another answer ("STALE EXCEPTIONS"), on a reason that quotes no sentence or a class that is not one of the four
("SUITE PROBLEMS"), when fewer cases were scored than the floor (CSS_PROPERTY_MIN_SCORED, "below the floor"), and on a runner that
does not answer. A call of the harness that the extractor cannot read is counted per file (tools/css/UNEXTRACTED, "UNEXTRACTED CALLS"), and a file or a
count that differs from the list fails. A vector for a property the pin does not have is never dropped: CSS_PROPERTY_ABSENT lists the property and the
number of its scored vectors with the reason, and stale and unlisted lines fail.

The depth ledger's `wpt` column is regenerated from the scored set: a property has 1 when at least one of its scored vectors exists
and all of them pass without an exception. A row that was 1 and is 0 is a regression and fails; a row that differs from the
scored set is out of date (`--write-depth` rewrites the column, and the floor).
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


def css_name(js):
    """The property a `style[name]` attribute names (CSSOM 6.7.1: camel-cased attribute, dashed attribute, cssFloat)."""
    if js == 'cssFloat':
        return 'float'
    if js.startswith('--') or '-' in js:
        return js.lower() if not js.startswith('--') else js
    out = re.sub(r'([A-Z])', lambda m: '-' + m.group(1).lower(), js)
    if out.startswith('webkit-') or out.startswith('moz-') or out.startswith('ms-') or out.startswith('o-'):
        out = '-' + out
    return out


def run_runner(runner, requests, nature=False):
    data = ''.join('%s\t%s\t%s\n' % (f, escape(p), escape(v)) for f, p, v in requests)
    cmd = [runner] + (['-n'] if nature else [])
    proc = subprocess.run(cmd, input=data.encode('utf-8', 'surrogateescape'), stdout=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        print('SUITE PROBLEMS: the runner exited %d' % proc.returncode)
        sys.exit(1)
    lines = proc.stdout.decode('utf-8', 'surrogateescape').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    if len(lines) != len(requests):
        print('SUITE PROBLEMS: the runner answered %d lines to %d requests' % (len(lines), len(requests)))
        sys.exit(1)
    return lines


def quotes_a_sentence(reason):
    """Whether a reason holds a quotation that is a sentence and not a word: four words and twenty characters, between double quotes."""
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


def read_ledger(path):
    rows, order = {}, []
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        c = line.split('\t')
        rows[(c[0], c[1])] = c
        order.append((c[0], c[1]))
    return rows, order


def main():
    argv = sys.argv[1:]
    flags = {a for a in argv if a in ('--write-depth',)}
    opts = {}
    pos = []
    i = 0
    while i < len(argv):
        if argv[i] in ('--depth', '--depth-floor'):
            opts[argv[i]] = argv[i + 1]
            i += 2
        elif argv[i] == '--unextracted':
            opts[argv[i]] = (argv[i + 1], argv[i + 2])
            i += 3
        elif argv[i].startswith('--'):
            i += 1
        else:
            pos.append(argv[i])
            i += 1
    vec_dir, runner, exceptions_path, defects_path, absent_path = pos[:5]

    def load(kind):
        return json.load(open(os.path.join(vec_dir, kind + '.json'), encoding='utf-8'))
    valid, invalid, shorthand = load('valid'), load('invalid'), load('shorthand')
    counts_unscored = {k: len(load(k)) for k in ('computed', 'selector', 'rule')}

    problems = []
    exceptions, p = load_table(exceptions_path, 7, 'not <id> TAB <property> TAB <value> TAB <stage> TAB <answer> TAB <class> TAB <reason>')
    problems += p
    for ident, f in exceptions.items():
        if f[3] not in STAGES:
            problems.append('%s: stage %r is not one of %s' % (ident, f[3], ', '.join(STAGES)))
        if f[5] not in CLASSES:
            problems.append('%s: class %r is not one of %s' % (ident, f[5], ', '.join(CLASSES)))
        if f[5] == 'wpt-tentative':
            # a fact the file name decides: the suite marks such a page by the word, in its path
            if 'tentative' not in ident:
                problems.append('%s: the class is wpt-tentative and the page is not named tentative' % ident)
        elif not quotes_a_sentence(f[6]):
            problems.append('%s: the reason quotes no sentence of the draft (a quotation of four words and twenty characters at least)' % ident)
    defects, p = load_table(defects_path, 7, 'not <id> TAB <property> TAB <value> TAB <stage> TAB <answer> TAB <owner> TAB <what the library lacks>')
    problems += p
    for ident, f in defects.items():
        if f[3] not in STAGES:
            problems.append('%s: stage %r is not one of %s' % (ident, f[3], ', '.join(STAGES)))
        if ident in exceptions:
            problems.append('%s: both an exception and a defect' % ident)
        if not f[5].strip() or len(f[6].strip()) < 20:
            problems.append('%s: a defect names its owner and says what the library lacks (twenty characters at least)' % ident)
    absent, p = load_table(absent_path, 3, 'not <property> TAB <count> TAB <reason>')
    problems += p

    # which properties does the registry have, and are they shorthands
    names = sorted({css_name(v['prop']) for v in valid + invalid + shorthand})
    nat = dict(zip(names, run_runner(runner, [('', n, '') for n in names], nature=True)))

    scored = []   # (ident, kind, vector, property)
    absent_seen = {}
    for kind, vs in (('valid', valid), ('invalid', invalid), ('shorthand', shorthand)):
        for v in vs:
            prop = css_name(v['prop'])
            if nat[prop] == 'UNKNOWN':
                absent_seen[prop] = absent_seen.get(prop, 0) + 1
                continue
            scored.append(('%s#%d' % (v['file'], v['n']), kind, v, prop))
    # the identity is file#n, n counting every vector of the file (of every kind), so it is unique over the three files
    ids = set()
    for ident, kind, v, prop in scored:
        if ident in ids:
            problems.append('%s: two vectors share the identity' % ident)
        ids.add(ident)
    requests = []
    for ident, kind, v, prop in scored:
        flag = 'q' if v.get('quirks') else ''
        requests.append((flag, prop, v['value']))
        if v['call'] == 'test_specified_serialization':
            requests.append((flag, prop, v['expected']))
    lines = run_runner(runner, requests)

    failures = {}
    clean = {}   # property -> True while every scored vector of it passed
    k = 0
    shorthand_ser = custom = longhand_expect = 0
    for ident, kind, v, prop in scored:
        line = lines[k]
        k += 1
        extra = None
        if v['call'] == 'test_specified_serialization':
            extra = lines[k]
            k += 1
        if line == 'BAD' or line.startswith('ERR\t'):
            problems.append('%s: the runner could not run it (%s)' % (ident, line))
            continue
        stage = answer = None
        if kind == 'valid' or kind == 'shorthand':
            if line.startswith('OK\t'):
                _, outcome, text, again = line.split('\t')
                text, again = unescape(text), unescape(again)
                if kind == 'valid':
                    is_short = nat[prop] == 'SHORTHAND'
                    is_custom = v.get('custom')
                    shorthand_ser += 1 if is_short else 0
                    custom += 1 if is_custom else 0
                    want = [v['expected']] + list(v.get('alts') or [])
                    if not is_short and not is_custom and text not in want:
                        stage, answer = 'serialize', text
                    elif again != text:
                        stage, answer = 'roundtrip', again.replace('!INVALID', 'INVALID') if again == '!INVALID' else again
                    elif extra is not None:
                        if extra.startswith('OK\t'):
                            e = extra.split('\t')
                            if unescape(e[2]) != v['expected']:
                                stage, answer = 'roundtrip', unescape(e[2])
                        else:
                            stage, answer = 'roundtrip', 'INVALID'
                else:
                    longhand_expect += len(v.get('longhands') or {})
            elif line in ('INVALID', 'UNDECIDED'):
                stage, answer = 'parse', line
            else:
                problems.append('%s: the runner answered %r, which is not one of OK, INVALID or UNDECIDED' % (ident, line))
                continue
        else:
            if line.startswith('OK\t'):
                stage, answer = 'invalid-accepted', unescape(line.split('\t')[2])
            elif line == 'UNDECIDED':
                stage, answer = 'invalid-accepted', 'UNDECIDED'
            elif line != 'INVALID':
                # only a refusal passes an invalid vector: an UNKNOWN property, an empty line or anything else is not "refused"
                problems.append('%s: the runner answered %r to an invalid vector, which is not INVALID' % (ident, line))
                continue
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
            elif f[3] != stage:
                stale.append('%s: the case fails at %s, the %s says %s' % (ident, stage, what, f[3]))
            elif unescape(f[4]) != answer:
                stale.append('%s: the library answers %r, the %s says %r' % (ident, answer, what, unescape(f[4])))
    for prop, f in absent.items():
        if prop not in absent_seen:
            stale.append('absent %s: the pin has it, or no vector names it' % prop)
        elif int(f[1]) != absent_seen[prop]:
            stale.append('absent %s: %d vectors, the list says %s' % (prop, absent_seen[prop], f[1]))
        if len(f[2]) < 20:
            problems.append('absent %s: the reason says nothing' % prop)
    unlisted_absent = {p: n for p, n in absent_seen.items() if p not in absent}

    # the calls of the harness the extractor could not read: per file, the count the list holds
    unext_bad = []
    if '--unextracted' in opts:
        want_path, got_path = opts['--unextracted']
        def read_unext(path):
            out = {}
            for line in open(path, encoding='utf-8'):
                f = line.rstrip('\n').split('\t')
                if line.startswith('#') or len(f) < 2:
                    continue
                out[f[0]] = int(f[1])
            return out
        want, got = read_unext(want_path), read_unext(got_path)
        for f in sorted(set(got) - set(want)):
            unext_bad.append('%s: %d calls the extractor cannot read and the list has no line for it' % (f, got[f]))
        for f in sorted(set(want) - set(got)):
            unext_bad.append('%s: listed (%d) and the extractor reads every call now (stale)' % (f, want[f]))
        for f in sorted(set(want) & set(got)):
            if want[f] != got[f]:
                unext_bad.append('%s: the extractor cannot read %d calls and the list says %d' % (f, got[f], want[f]))

    bad = False
    if problems:
        print('SUITE PROBLEMS:')
        for q in problems[:40]:
            print('  ' + q)
        bad = True
    if unexplained:
        print('UNEXPLAINED FAILURES: %d' % len(unexplained))
        for ident, (stage, answer, v, prop) in list(unexplained.items())[:(10**9 if os.environ.get('CSS_PROPERTY_LIST_ALL') else 40)]:
            print('  %s  %s: %s %s -> %s' % (ident, stage, v['prop'], json.dumps(v['value'], ensure_ascii=False), answer))
        bad = True
    if unext_bad:
        print('UNEXTRACTED CALLS: %d files differ from tools/css/UNEXTRACTED' % len(unext_bad))
        for q in unext_bad[:40]:
            print('  ' + q)
        bad = True
    if unlisted_absent:
        print('UNLISTED ABSENT PROPERTIES: %d (the pin has no such property; list each in CSS_PROPERTY_ABSENT)' % len(unlisted_absent))
        for pr, n in sorted(unlisted_absent.items())[:40]:
            print('  %s  %d vectors' % (pr, n))
        bad = True
    if stale:
        print('STALE EXCEPTIONS: %d' % len(stale))
        for s in stale[:40]:
            print('  ' + s)
        bad = True
    floor = int(os.environ.get('CSS_PROPERTY_MIN_SCORED', '0'))
    if len(scored) < floor:
        print('scored %d cases, below the floor of %d' % (len(scored), floor))
        bad = True
    ceiling = int(os.environ.get('CSS_PROPERTY_MAX_DEFECTS', '1000000'))
    if len(defects) > ceiling:
        print('%d defects listed, above the ceiling of %d (the ceiling only goes down)' % (len(defects), ceiling))
        bad = True
    if os.environ.get('CSS_PROPERTY_DUMP'):
        json.dump({i: [f[0], f[1], f[2]['prop'], f[2]['value']] for i, f in failures.items()}, open(os.environ['CSS_PROPERTY_DUMP'], 'w'))

    # the depth ledger's wpt column
    wpt_clean = sorted(p for p, ok in clean.items() if ok)
    if '--depth' in sys.argv:
        ledger_path = opts['--depth']
        rows, order = read_ledger(ledger_path)
        want = set(wpt_clean)
        changed = 0
        for key in order:
            if key[0] != 'property':
                continue
            row = rows[key]
            now = '1' if key[1] in want else '0'
            if row[5] != now:
                changed += 1
                if row[5] == '1' and '--write-depth' not in flags:
                    print('REGRESSION: property %s: the wpt column was 1 and its scored vectors no longer all pass' % key[1])
                    bad = True
                elif '--write-depth' not in flags:
                    print('OUT OF DATE: property %s: wpt is %s in the ledger and %s in the scored set (run with --write-depth)' % (key[1], row[5], now))
                    bad = True
                row[5] = now
        missing = want - {k[1] for k in order if k[0] == 'property'}
        for m in sorted(missing):
            print('SUITE PROBLEMS: %s has scored vectors and no ledger row' % m)
            bad = True
        total = sum(1 for k in order if k[0] == 'property' and rows[k][5] == '1')
        floor_path = opts.get('--depth-floor')
        if '--write-depth' in flags:
            with open(ledger_path, encoding='utf-8') as h:
                src = h.read().split('\n')
            out = []
            for line in src:
                if line and not line.startswith('#'):
                    c = line.split('\t')
                    c = rows[(c[0], c[1])]
                    line = '\t'.join(c)
                out.append(line)
            with open(ledger_path, 'w', encoding='utf-8', newline='\n') as h:
                h.write('\n'.join(out))
            if floor_path:
                allrows = [rows[k] for k in order]
                cols = [sum(1 for r in allrows if r[2 + i] == '1') for i in range(4)]
                with open(floor_path, 'w', encoding='utf-8', newline='\n') as h:
                    h.write('%d %d %d %d  # parses serializes computes wpt: rows of depth.tsv that have each; only goes up\n' % tuple(cols))
        elif floor_path:
            floor_vals = [int(x) for x in open(floor_path).read().split('#')[0].split()]
            if total < floor_vals[3]:
                print('BELOW THE FLOOR: %d properties have wpt and %s says %d' % (total, floor_path, floor_vals[3]))
                bad = True
        print('css property: depth ledger: %d properties have all their scored vectors passing' % total)

    npass = len(scored) - len(failures)
    print('css property: %d scored (valid %d, invalid %d, shorthand %d), %d passed, %d listed exceptions, %d listed DEFECTS of the library; '
          '%d vectors of %d absent properties listed' % (
              len(scored), sum(1 for s in scored if s[1] == 'valid'), sum(1 for s in scored if s[1] == 'invalid'),
              sum(1 for s in scored if s[1] == 'shorthand'), npass, sum(1 for i in failures if i in exceptions),
              sum(1 for i in failures if i in defects), sum(absent_seen.values()), len(absent_seen)))
    print('css property: not scored here: computed %d (C2d), selector %d (C3), rule %d (C4); of the scored, shorthand serialization %d '
          '(C2a), custom-compared %d (parse and roundtrip only), shorthand longhand expectations %d (C2a)' % (
              counts_unscored['computed'], counts_unscored['selector'], counts_unscored['rule'], shorthand_ser, custom, longhand_expect))
    by_stage = {}
    for f in failures.values():
        by_stage[f[0]] = by_stage.get(f[0], 0) + 1
    print('css property: failures by stage: ' + ', '.join('%s %d' % (s, by_stage.get(s, 0)) for s in STAGES))
    sys.exit(1 if bad else 0)


main()
