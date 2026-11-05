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
"""Hold the properties that src/css/css_pseudo_apply.c says apply to ::first-line, ::first-letter and ::marker to what the two browsers recorded.

    check-pseudo-apply.py <runner> [--controls]

<runner> is the runner of the scoring interpreter (build/css-cssom-script-runner). For every case of tools/conformance/css_pseudo_answers.jsonl (written by
tools/oracle/css_pseudo_record.py) and each of the three pseudo-elements, the library is asked what `getComputedStyle(element, pseudo)` answers for the property with the case's
declaration on the pseudo-element and without it; the property *applies* when the two differ, as it is recorded for the browsers. Where Chromium and Firefox agree, the library has
to agree, unless the line of tools/conformance/CSS_PSEUDO_SERVED says the served text decides otherwise (it quotes the sentence, which the gate looks up in the pinned draft). Where
they differ, the served text decides and the library may do either; a case on which the library does not parse the value at all is a fault of the case, not an answer.

--controls shows the gate fail: a record answer turned over, a served line removed, a served line that quotes a sentence the draft does not say, a served line for a property the
browsers and the library agree on.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CONF = os.path.join(ROOT, 'tools', 'conformance')
PSEUDOS = ('::first-line', '::first-letter', '::marker')
sys.path.insert(0, CONF)


def esc(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '\\r')


def load_record(path):
    rows = []
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        rows.append(json.loads(line))
    return rows


def load_served(path):
    """(pseudo, property) -> the quoted reason. Columns: pseudo, property, reason (which quotes the served draft as css-NAME: "sentence")."""
    out = {}
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        if line.startswith('#') or not line.strip():
            continue
        f = line.rstrip('\n').split('\t')
        if len(f) != 3 or f[0] not in PSEUDOS:
            raise SystemExit('%s:%d: three columns (pseudo, property, reason) are expected' % (os.path.basename(path), n))
        out[(f[0], f[1])] = f[2]
    return out


def camel(prop):
    return re.sub(r'-([a-z])', lambda m: m.group(1).upper(), prop)


def library_answers(runner, rows):
    """For every (case index, pseudo): (with the rule, without it, the element itself with the declaration, the element without), each as the runner reports it.

    The runner reports the first failed assertion of a test only, so each read is a test of its own."""
    out = {}
    # one program per read position, so that the first failed assertion is that read
    answers = {}
    for t in range(4):
        prog = single(rows, t)
        p = subprocess.run([runner], input=prog.encode('utf-8'), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0:
            raise SystemExit('check-pseudo-apply: the runner exited %d' % p.returncode)
        for line in p.stdout.decode('utf-8', 'replace').splitlines():
            f = line.split('\t')
            if f[0] == 'R':
                answers[(int(f[2]), t)] = (f[3], f[4] if len(f) > 4 else '')
    for k in range(len(rows)):
        for j, ps in enumerate(PSEUDOS):
            i = k * 3 + j
            out[(k, ps)] = tuple(value_of(answers.get((i, t))) for t in range(4))
    return out


def value_of(a):
    """The value a read gave, from the message of the failed assertion (`expected "@@@" but got "static"`)."""
    if a is None:
        return None
    status, msg = a
    if status == 'LAYOUT':
        return '<layout %s>' % msg
    m = re.search(r' but got (.*)$', msg)
    if not m:
        return '<%s %s>' % (status, msg)
    got = m.group(1)
    return json.loads(got) if got.startswith('"') else got


def single(rows, t):
    """The program of one read: position t of (pseudo with the rule, pseudo without, the element with the declaration, the element without)."""
    css = []
    body = []
    for k, r in enumerate(rows):
        for j, ps in enumerate(PSEUDOS):
            i = k * 3 + j
            css.append('#e%d%s { %s: %s }' % (i, ps, r['property'], r['value']))
            css.append('#v%d { %s: %s }' % (i, r['property'], r['value']))
            tagname = 'li' if ps == '::marker' else 'div'
            for tag in ('e', 'b', 'v', 'w'):
                body.append('<%s id="%s%d">text</%s>' % (tagname, tag, i, tagname))
    html = '<!DOCTYPE html><html><head><title>t</title><style>%s</style></head><body><ul>%s</ul></body></html>' % ('\n'.join(css), ''.join(body))
    ops = ['O\tscript\t%d' % len(html), 'O\tdoc\t%d' % len(html)]
    n = 0
    for k, r in enumerate(rows):
        for j, ps in enumerate(PSEUDOS):
            i = k * 3 + j
            prop = camel(r['property'])
            tag, pse = [('e', ps), ('b', ps), ('v', ''), ('w', '')][t]
            n += 1
            ops += ['O\ttest\t%d\t%s' % (i, esc('%s %s' % (ps, r['property']))), 'O\telem\tx%d\tsel\tdoc\t#%s%d' % (n, tag, i), 'O\tgcs\ts%d\tx%d\t%s' % (n, n, pse),
                    'O\tsattr\tr%d\ts%d\t%s' % (n, n, prop), 'O\tassert_equals\tr:r%d\ts:@@@\t' % n, 'O\tcleanup', 'O\tend']
    return 'P\tpseudo.html\t0\t%d\nH\t%s\n%s\n' % (len(rows) * 3, esc(html), '\n'.join(ops))


def applies(rec, engine, ps):
    d = rec['engines'][engine].get(ps)
    return None if d is None else d['pseudo'] != d['base']


def check(rows, lib, served, drafts_text, layout=None):
    """The faults, as lines. `layout` collects the properties a read cannot show here because their computed value needs a layout."""
    layout = set() if layout is None else layout
    out = []
    used = set()
    for k, r in enumerate(rows):
        for ps in PSEUDOS:
            c, f = applies(r, 'chromium', ps), applies(r, 'firefox', ps)
            if c is None or f is None:
                out.append('RECORD: %s %s has no answer of %s' % (ps, r['property'], 'chromium' if c is None else 'firefox'))
                continue
            with_rule, without, element_with, element_without = lib[(k, ps)]
            if str(element_with).startswith('<layout'):
                layout.add(r['property'])   # a used value: the library answers it from a layout, which a read of the pseudo-element does not have either
                continue
            if element_with == element_without:
                out.append('CASE: the library does not take the value of %s: %s (the element itself does not change)' % (r['property'], r['value']))
                continue
            ours = with_rule != without
            key = (ps, r['property'])
            if c != f:
                if key in served:
                    out.append('SERVED: %s %s is listed and the browsers differ (the line is for a property they agree on)' % key)
                continue
            if ours == c and key in served:
                out.append('SERVED: %s %s is listed and the library agrees with both browsers (a stale line)' % key)
            elif ours != c:
                if key in served:
                    used.add(key)
                else:
                    out.append('DISAGREE: %s %s: both browsers say %s, the library says %s' % (ps, r['property'], 'applies' if c else 'does not apply', 'applies' if ours else 'does not apply'))
    for key in served:
        if key not in used and not any(key == (ps, r['property']) for r in rows for ps in PSEUDOS):
            out.append('SERVED: %s %s is not a case of the record' % key)
    for key, reason in served.items():
        for d, q in re.findall(r'(css-[a-z0-9-]+): "([^"]*)"', reason):
            text = drafts_text.get(d)
            if text is None:
                out.append('QUOTE: %s %s names the draft %s, which is not pinned or not fetched' % (key + (d,)))
            elif q_plain(q) not in text:
                out.append('QUOTE: %s %s quotes "%s" and %s does not say it' % (key + (q[:60], d)))
        if not re.search(r'css-[a-z0-9-]+: "[^"]{20,}"', reason):
            out.append('QUOTE: %s %s quotes no sentence of a pinned draft' % key)
    return out


def q_plain(q):
    import css_cssom_script_suite as s
    return s.plain_text(q)


def drafts_for(served):
    import css_cssom_script_suite as s
    manifest = os.path.join(ROOT, 'tools', 'css', 'DRAFTS_SHA256')
    pins = {}
    for line in open(manifest, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 2 and not line.startswith('#') and f[1].endswith('.bs'):
            pins[f[1][:-3]] = (f[0], f[1])
    out = {}
    for reason in served.values():
        for d, _ in re.findall(r'(css-[a-z0-9-]+): "([^"]*)"', reason):
            if d in out or d not in pins:
                continue
            path = os.path.join(ROOT, 'third_party', 'css', 'drafts', pins[d][1])
            if not os.path.exists(path):
                raise SystemExit('check-pseudo-apply: the draft %s is not fetched (tools/css/fetch.sh)' % d)
            if hashlib.sha256(open(path, 'rb').read()).hexdigest() != pins[d][0]:
                raise SystemExit('check-pseudo-apply: %s does not hash as pinned' % path)
            out[d] = s.draft_text(path)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    controls = '--controls' in sys.argv
    if len(args) != 1:
        raise SystemExit(__doc__)
    runner = args[0]
    rows = load_record(os.path.join(CONF, 'css_pseudo_answers.jsonl'))
    served = load_served(os.path.join(CONF, 'CSS_PSEUDO_SERVED'))
    lib = library_answers(runner, rows)
    drafts = drafts_for(served)
    layout = set()
    faults = check(rows, lib, served, drafts, layout)
    if not controls:
        if faults:
            print('check-pseudo-apply: %d faults' % len(faults))
            for f in faults[:40]:
                print('  ' + f)
            return 1
        print('check-pseudo-apply: %d properties on %d pseudo-elements; the library agrees with both browsers wherever they agree (%d served-text exceptions); %d are layout-bound and not read here (%s)' % (len(rows), len(PSEUDOS), len(served), len(layout), ', '.join(sorted(layout))))
        return 0
    if faults:
        print('check-pseudo-apply: the gate does not pass unedited:')
        for f in faults[:20]:
            print('  ' + f)
        return 1
    bad = 0

    def expect(name, want, fs):
        nonlocal bad
        if not any(want in x for x in fs):
            print('control %s: the gate did not say %r (%r)' % (name, want, fs[:3]))
            bad = 1
        else:
            print('control %s: caught' % name)

    # a record answer turned over: the first property both browsers agree does not apply on ::first-line
    flipped = json.loads(json.dumps(rows))
    for r in flipped:
        if r['property'] == 'position':
            for e in r['engines'].values():
                d = e['::first-line']
                d['pseudo'] = d['base'] + ' flipped'
    expect('record-answer-flipped', 'DISAGREE: ::first-line position', check(flipped, lib, served, drafts))
    # a served line removed
    if served:
        key = sorted(served)[0]
        less = dict(served)
        del less[key]
        expect('served-line-removed', 'DISAGREE: %s %s' % key, check(rows, lib, less, drafts))
    # a served line that quotes what the draft does not say
    wrong = dict(served)
    wrong[('::first-line', 'word-break')] = 'css-pseudo-4: "a sentence that the draft of pseudo-elements does not contain anywhere"'
    expect('quote-not-in-draft', 'does not say it', check(rows, lib, wrong, drafts_for(wrong)))
    # a served line for a property the browsers and the library agree on
    agree = dict(served)
    agree[('::first-line', 'position')] = 'css-pseudo-4: "The following CSS properties apply to a ::first-line pseudo-element"'
    expect('stale-served-line', 'a stale line', check(rows, lib, agree, drafts_for(agree)))
    return bad


if __name__ == '__main__':
    sys.exit(main())
