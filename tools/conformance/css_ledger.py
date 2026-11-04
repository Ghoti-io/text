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
"""The ledger of the pages of web-platform-tests that the CSSOM gate does not read itself.

    css_ledger.py cssom --wpt W --vectors V --rulevec RV --elsewhere E --census C --codes R [--resolve] [--regenerate]
    css_ledger.py atrules --unscored A --codes R

``cssom`` checks, from the extractor's output in V (declaration.tsv, UNEXTRACTED_CSSOM) and the vectors of the other gates' extractors:

* every reason is a code of REASON_CODES ("REASON CODE");
* the census (C, tools/css/census-wpt-script-pages.py) is the set of pages the extractor accounts for, with the totals it pins ("CENSUS");
* every page's tests are in exactly one class each: the tests the gate scores plus the tests of its lines add up to the census count of the page ("PARTITION");
* a page the extractor lists as made by a helper script is ``scored-by:<gate>`` only for the vectors that gate's extractor has for it (and its own unscored list does not
  take back), up to the page's test count; the rest stays ``helper:<names>``. E (SCORED_ELSEWHERE_CSSOM) is that attribution written down, and it has to be exactly what is
  derived ("SCORED ELSEWHERE"); the attribution is a derivation, not a sentence;
* the pages and tests of each code are the ones REASON_CODES pins ("MOVED"): a class that grows or shrinks fails until the pin is changed in the same commit, and the failure
  prints the move.

``--resolve`` writes V/UNEXTRACTED_CSSOM with the attribution applied (the extractor's own lines are kept in UNEXTRACTED_CSSOM.raw), which is what the CSSOM gate compares with
UNSCORED_CSSOM. ``--regenerate`` writes E, C and the pins of R instead of checking them.

REASON_CODES: code, kind, CSSOM pages, CSSOM tests, at-rule pages, at-rule tests, what it means. Kinds: ``residue`` (not scorable here, and why), ``todo`` (an ``unmodeled:``
construct: the list of what to build next), ``helper`` (a helper script not modeled), ``scored-by`` (verified against the gate named), ``scored`` (the gate scores it).
"""
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CSS_TOOLS = os.path.join(os.path.dirname(HERE), 'css')

# The order in which the gates are credited when more than one has vectors for a page.
GATE_ORDER = ('css-property', 'css-computed', 'css-selectors', 'css-values', 'css-conditions', 'css-variables', 'css-atrules', 'css-cssom')
WHY = 'the vectors of the page are scored by the named gate (derived from its extractor, tools/conformance/css_ledger.py)'


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tsv(path):
    rows = []
    if os.path.exists(path):
        for n, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
            line = line.rstrip('\n')
            if line and not line.startswith('#'):
                rows.append((n, line.split('\t')))
    return rows


def load_codes(path):
    codes = {}
    for n, f in tsv(path):
        if len(f) != 7:
            raise SystemExit('REASON CODE: %s:%d has %d fields, not seven' % (os.path.basename(path), n, len(f)))
        if f[0] in codes:
            raise SystemExit('REASON CODE: %s:%d lists %s twice' % (os.path.basename(path), n, f[0]))
        codes[f[0]] = {'kind': f[1], 'cssom': (int(f[2]), int(f[3])), 'atrules': (int(f[4]), int(f[5])), 'what': f[6]}
    return codes


def css_name(js):
    """The property a ``style[name]`` attribute names (the same rule as css_property_suite.py)."""
    if js == 'cssFloat':
        return 'float'
    if js.startswith('--') or '-' in js:
        return js.lower() if not js.startswith('--') else js
    out = re.sub(r'([A-Z])', lambda m: '-' + m.group(1).lower(), js)
    if out.startswith(('webkit-', 'moz-', 'ms-', 'o-')):
        out = '-' + out
    return out


def read_json(path):
    return json.load(open(path, encoding='utf-8'))


def count(rows, key='file'):
    out = {}
    for r in rows:
        out[r[key]] = out.get(r[key], 0) + 1
    return out


def gate_programs(wpt, rulevec, vectors, scratch):
    """{gate: {page: vectors the gate's extractor has for it and its runner scores}}, from the extractors of the gates (nothing is guessed from a page's name)."""
    prog = {}
    absent = {f[0] for _, f in tsv(os.path.join(HERE, 'CSS_PROPERTY_ABSENT'))}
    p = {}
    for kind in ('valid', 'invalid', 'shorthand'):
        for v in read_json(os.path.join(rulevec, kind + '.json')):
            if css_name(v['prop']) not in absent:
                p[v['file']] = p.get(v['file'], 0) + 1
    prog['css-property'] = p
    c = count(read_json(os.path.join(rulevec, 'computed.json')))
    for _, f in tsv(os.path.join(HERE, 'UNSCORED_COMPUTED')):
        c[f[0]] = max(0, c.get(f[0], 0) - int(f[1]))
    prog['css-computed'] = {k: v for k, v in c.items() if v}
    prog['css-selectors'] = count(read_json(os.path.join(rulevec, 'selector.json')))
    vj = os.path.join(scratch, 'values.json')
    subprocess.run([sys.executable, '-I', os.path.join(CSS_TOOLS, 'extract-wpt-values.py'), wpt, vj], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    prog['css-values'] = count(read_json(vj))
    cd = os.path.join(scratch, 'cond')
    os.makedirs(cd, exist_ok=True)
    subprocess.run([sys.executable, '-I', os.path.join(CSS_TOOLS, 'extract-wpt-conditions.py'), wpt, cd], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    prog['css-conditions'] = count(read_json(os.path.join(cd, 'media.json')))
    vd = os.path.join(scratch, 'var')
    os.makedirs(vd, exist_ok=True)
    subprocess.run([sys.executable, '-I', os.path.join(CSS_TOOLS, 'extract-wpt-variables.py'), wpt, vd, '--unextracted', os.path.join(vd, 'UNEXTRACTED_VARIABLES')],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    var = {}
    for fn in sorted(os.listdir(vd)):
        if fn.endswith('.json'):
            for v in read_json(os.path.join(vd, fn)):
                if isinstance(v, dict) and 'file' in v:
                    var[v['file']] = var.get(v['file'], 0) + 1
    prog['css-variables'] = var
    av = os.path.join(scratch, 'atrules.tsv')
    subprocess.run([sys.executable, '-I', os.path.join(CSS_TOOLS, 'extract-wpt-atrules.py'), wpt, av], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    at = {}
    for line in open(av, encoding='utf-8'):
        if not line.startswith('#'):
            f = line.rstrip('\n').split('\t')
            if len(f) > 1:
                at['css/' + f[1]] = at.get('css/' + f[1], 0) + 1
    prog['css-atrules'] = at
    rv = {}
    for line in open(os.path.join(vectors, 'declaration.tsv'), 'rb'):
        if line.startswith(b'P\t'):
            f = line.decode('utf-8', 'replace').split('\t')
            if f[1].endswith('?rule-vectors'):
                rv[f[1].replace('?rule-vectors', '')] = int(f[3])
    prog['css-cssom'] = rv
    return prog


def read_lines(path):
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        if len(f) == 3 and not line.startswith('#'):
            out.append((f[0], int(f[1]), f[2]))
    return out


def derive(lines, prog):
    """(twin, resolved): the attribution of the helper pages and the lines with it applied."""
    twin, resolved = [], []
    for page, n, code in lines:
        if not code.startswith('helper:'):
            resolved.append((page, n, code))
            continue
        left = n
        for gate in GATE_ORDER:
            take = min(left, prog.get(gate, {}).get(page, 0))
            if take > 0:
                twin.append((page, gate, take))
                resolved.append((page, take, 'scored-by:' + gate))
                left -= take
        if left > 0:
            resolved.append((page, left, code))
    return twin, resolved


def scored_tests(vectors):
    out = {}
    for line in open(os.path.join(vectors, 'declaration.tsv'), 'rb'):
        if line.startswith(b'P\t'):
            f = line.decode('utf-8', 'replace').rstrip('\n').split('\t')
            page = f[1].replace('?rule-vectors', '')
            out[page] = out.get(page, 0) + int(f[3])
    return out


def classes(resolved, scored):
    """{code: (pages, tests)} with ``scored`` for the tests the gate runs."""
    pages, tests = {}, {}
    for page, n, code in resolved:
        pages.setdefault(code, set()).add(page)
        tests[code] = tests.get(code, 0) + n
    pages.setdefault('scored', set()).update(scored)
    tests['scored'] = sum(scored.values())
    return {c: (len(pages[c]), tests[c]) for c in pages}


def write_twin(path, twin):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# tools/conformance/css_ledger.py: the pages of the CSSOM list that another gate scores (the gate reads the vectors of the page: its extractor has them and its own unscored list does not take them back). Tab separated: page, gate, tests, why. Derived; the gate fails on any difference from what is derived.\n')
        for page, gate, n in sorted(twin):
            f.write('%s\t%s\t%d\t%s\n' % (page, gate, n, WHY))


def check_cssom(a):
    out = []
    codes = load_codes(a['codes'])
    raw = os.path.join(a['vectors'], 'UNEXTRACTED_CSSOM.raw')
    src = raw if os.path.exists(raw) else os.path.join(a['vectors'], 'UNEXTRACTED_CSSOM')
    lines = read_lines(src)
    for page, n, code in lines:
        if code not in codes:
            out.append('REASON CODE: %s gives %r, which is not in REASON_CODES' % (page, code))
    census_mod = load_module(os.path.join(CSS_TOOLS, 'census-wpt-script-pages.py'), 'census')
    with tempfile.TemporaryDirectory() as scratch:
        prog = gate_programs(a['wpt'], a['rulevec'], a['vectors'], scratch)
    twin, resolved = derive(lines, prog)
    # the twin: exactly what is derived
    have = {(f[0], f[1]): f[2] for _, f in tsv(a['elsewhere']) if len(f) == 4}
    for _, f in tsv(a['elsewhere']):
        if len(f) != 4:
            out.append('SCORED ELSEWHERE: a line of %s has %d fields, not four' % (os.path.basename(a['elsewhere']), len(f)))
        elif f[1] not in GATE_ORDER:
            out.append('SCORED ELSEWHERE: %s names the gate %r, which is not one of %s' % (f[0], f[1], ', '.join(GATE_ORDER)))
        elif prog.get(f[1], {}).get(f[0], 0) == 0:
            out.append('SCORED ELSEWHERE: %s has no program in %s (its extractor has no vector for the page)' % (f[0], f[1]))
    want = {(p, g): str(n) for p, g, n in twin}
    for k in sorted(set(want) | set(have)):
        if want.get(k) != have.get(k):
            out.append('SCORED ELSEWHERE: %s in %s: derived %s tests, listed %s' % (k[0], k[1], want.get(k), have.get(k)))
            if len([o for o in out if o.startswith('SCORED ELSEWHERE')]) > 40:
                break
    # the census and the partition
    rows, problems = census_mod.build(a['wpt'], a['vectors'])
    out.extend('CENSUS: ' + p for p in problems)
    pins, listed = census_mod.load_census(a['census'])
    live = {r[0]: r[1] for r in rows}
    commit = open(os.path.join(CSS_TOOLS, 'WPT_COMMIT')).read().strip()
    if pins.get('pin') != commit:
        out.append('CENSUS: the census is of pin %s, the pin is %s' % (pins.get('pin'), commit))
    for rel in sorted(set(live) - set(listed))[:20]:
        out.append('CENSUS: %s is touched and not in the census' % rel)
    for rel in sorted(set(listed) - set(live))[:20]:
        out.append('CENSUS: %s is in the census and not touched' % rel)
    for rel in sorted(set(live) & set(listed)):
        if live[rel] != listed[rel][0]:
            out.append('CENSUS: %s has %d tests, the census says %d' % (rel, live[rel], listed[rel][0]))
    total = sum(v[0] for v in listed.values())
    if str(total) != pins.get('tests') or str(len(listed)) != pins.get('pages'):
        out.append('CENSUS: the census has %d pages and %d tests; its header pins %s and %s' % (len(listed), total, pins.get('pages'), pins.get('tests')))
    if str(sum(1 for v in listed.values() if v[0] != v[1])) != pins.get('calls-differ-pages'):
        out.append('CENSUS: the pages where the census counts other calls are not the %s that the header pins' % pins.get('calls-differ-pages'))
    scored = scored_tests(a['vectors'])
    held = {}
    for page, n, code in resolved:
        held[page] = held.get(page, 0) + n
    bad = 0
    for page in sorted(set(listed) | set(held) | set(scored)):
        if page not in listed:
            continue
        got = held.get(page, 0) + scored.get(page, 0)
        if got != listed[page][0]:
            bad += 1
            if bad <= 20:
                out.append('PARTITION: %s has %d tests in the census and %d in its classes' % (page, listed[page][0], got))
    if bad > 20:
        out.append('PARTITION: and %d pages more' % (bad - 20))
    # the pins of the classes
    now = classes(resolved, scored)
    moves = []
    for c in sorted(set(now) | {c for c, v in codes.items() if v['cssom'] != (0, 0)}):
        pin = codes.get(c, {}).get('cssom', (0, 0))
        if now.get(c, (0, 0)) != pin:
            moves.append('  %s: %d pages %d tests, pinned %d pages %d tests (%+d tests)' % (c, now.get(c, (0, 0))[0], now.get(c, (0, 0))[1], pin[0], pin[1], now.get(c, (0, 0))[1] - pin[1]))
    if 'scored' not in codes:
        out.append('REASON CODE: REASON_CODES has no row for the class "scored"')
    if moves:
        out.append('MOVED: the tests of a class differ from the pins of REASON_CODES (a move is a change of the pin, in the commit that makes it):')
        out.extend(moves)
    if a.get('resolve') and not [o for o in out if o.startswith('REASON CODE')]:
        if not os.path.exists(raw):
            os.replace(os.path.join(a['vectors'], 'UNEXTRACTED_CSSOM'), raw)
        agg = {}
        for page, n, code in resolved:
            agg[(page, code)] = agg.get((page, code), 0) + n
        with open(os.path.join(a['vectors'], 'UNEXTRACTED_CSSOM'), 'w', encoding='utf-8', newline='\n') as f:
            for (page, code), n in sorted(agg.items()):
                f.write('%s\t%d\t%s\n' % (page, n, code))
    return out, now, twin, rows


def rewrite_pins(path, codes, key, now):
    """REASON_CODES with the pins of one registry replaced; a code that is used and not in the file is an error."""
    for c in now:
        if c not in codes:
            raise SystemExit('REASON CODE: %s is used and is not in REASON_CODES; add it, with a kind and what it means, first' % c)
    lines = []
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            lines.append(line)
            continue
        f = line.rstrip('\n').split('\t')
        p, t = now.get(f[0], (0, 0))
        if key == 'cssom':
            f[2], f[3] = str(p), str(t)
        else:
            f[4], f[5] = str(p), str(t)
        lines.append('\t'.join(f) + '\n')
    open(path, 'w', encoding='utf-8', newline='').write(''.join(lines))


def parse(argv):
    a = {}
    i = 0
    while i < len(argv):
        if argv[i].startswith('--'):
            k = argv[i][2:]
            if k in ('resolve', 'regenerate'):
                a[k] = True
                i += 1
            else:
                a[k] = argv[i + 1]
                i += 2
        else:
            i += 1
    return a


def main():
    mode = sys.argv[1]
    a = parse(sys.argv[2:])
    if mode == 'atrules':
        codes = load_codes(a['codes'])
        agg = {}
        out = []
        for page, n, code in read_lines(a['unscored']):
            if code not in codes:
                out.append('REASON CODE: %s gives %r, which is not in REASON_CODES' % (page, code))
            p, t = agg.get(code, (0, 0))
            agg[code] = (p + 1, t + n)
        if a.get('regenerate'):
            if out:
                print('\n'.join(out))
                return 1
            rewrite_pins(a['codes'], codes, 'atrules', agg)
            return 0
        for c in sorted(set(agg) | {c for c, v in codes.items() if v['atrules'] != (0, 0)}):
            pin = codes.get(c, {}).get('atrules', (0, 0))
            if agg.get(c, (0, 0)) != pin:
                out.append('MOVED: %s: %d pages %d calls, pinned %d pages %d calls' % (c, agg.get(c, (0, 0))[0], agg.get(c, (0, 0))[1], pin[0], pin[1]))
        if out:
            print('\n'.join(out))
            return 1
        print('css-atrules ledger: %d pages in %d classes, as pinned' % (sum(v[0] for v in agg.values()), len(agg)))
        return 0
    out, now, twin, rows = check_cssom(a)
    if a.get('regenerate'):
        hard = [o for o in out if o.startswith(('REASON CODE', 'CENSUS: ')) and 'pins' not in o and 'says' not in o and 'not in the census' not in o and 'not touched' not in o]
        if [o for o in out if o.startswith('REASON CODE')]:
            print('\n'.join(out))
            return 1
        write_twin(a['elsewhere'], twin)
        census_mod = load_module(os.path.join(CSS_TOOLS, 'census-wpt-script-pages.py'), 'census')
        commit = open(os.path.join(CSS_TOOLS, 'WPT_COMMIT')).read().strip()
        with open(a['census'], 'w', encoding='utf-8', newline='\n') as f:
            f.write(census_mod.render(commit, rows))
        rewrite_pins(a['codes'], load_codes(a['codes']), 'cssom', now)
        print('ledger regenerated: %d attributions, %d census rows' % (len(twin), len(rows)))
        return 0
    if out:
        print('\n'.join(out))
        return 1
    scoredby = sum(v[1] for c, v in now.items() if c.startswith('scored-by:'))
    helper = sum(v[1] for c, v in now.items() if c.startswith('helper:'))
    print('css-ledger: %d pages, %d tests partitioned (scored here %d, scored by another gate %d, helper not modeled %d); %d classes as pinned' % (
        len(rows), sum(r[1] for r in rows), now['scored'][1], scoredby, helper, len(now)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
