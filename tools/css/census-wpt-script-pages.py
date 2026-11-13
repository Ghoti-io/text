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
"""The census of the pages of the pinned web-platform-tests that touch the CSSOM IDL.

    census-wpt-script-pages.py <wpt checkout> <vectors dir> <census file>          check the census file against the pages
    census-wpt-script-pages.py <wpt checkout> <vectors dir> <census file> --write  write it
    census-wpt-script-pages.py <wpt checkout> <vectors dir> --screen               print the screen of the script pages (nothing is written)

<vectors dir> is the output of tools/css/extract-wpt-cssom.py (``declaration.tsv`` and ``UNEXTRACTED_CSSOM``). The census has a row for every page the extractor touches, and for
the pages whose rule vectors the extractor runs as a program (those pages do not touch the IDL by the extractor's own test; they are in the census so that the tests the gate scores
are all in it), with

* ``tests``: the number of tests the extractor gives the page (the tests it scored and the ones it listed, each once);
* ``calls``: the number of test calls of the page's source, counted here without the extractor's code (``test(``, ``promise_test(``, ``async_test(``, a call of a
  ``test_<name>(`` helper that the page does not define, 1 for a reference test or a page with no harness call). Where the two numbers differ the page is listed in the file's
  header count, so that the difference is a number that is pinned and not a surprise; the extractor's number is the one the registries partition;
* ``markers``: what the page's own markup and inline scripts mention (a screen, not a classification; the helper scripts a page loads are not read): ``timing``,
  ``layout-geometry``, ``input-driver``, ``frame-or-popup``, ``typed-om``, ``register-property``, ``view-transition``, ``custom-element``, ``fonts``, ``computed``,
  ``sheets``, ``media``, ``shadow``, ``loop``, ``function``, ``reftest``, ``crash``, ``manual`` and ``rule-vectors``;
* ``helpers``: the scripts the page loads other than the harness.

The file's header pins the WPT commit, the number of pages and the number of tests. The check fails when the pages the extractor touches are not the pages of the file, when a
page's ``tests`` differ, and when the totals differ without a change of the pin; nothing is rewritten by the check.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = ('testharness.js', 'testharnessreport.js', 'testdriver.js', 'testdriver-vendor.js')

MARKERS = [
    ('timing', r'\bpromise_test\b|\basync_test\b|requestAnimationFrame|setTimeout|\bawait\b|transition(?:run|start|end|cancel)|animation(?:start|end|iteration|cancel)|getAnimations|\.animate\(|document\.fonts'),
    ('layout-geometry', r'getBoundingClientRect|getClientRects|offset(?:Width|Height|Left|Top|Parent)|client(?:Width|Height|Left|Top)|scroll(?:Width|Height|Top|Left)\b|:hover|:focus|\.focus\(\)'),
    ('input-driver', r'test_driver|testdriver'),
    ('frame-or-popup', r'<iframe|window\.open|popover|<dialog|showModal|requestFullscreen|fullscreen'),
    ('typed-om', r'computedStyleMap|attributeStyleMap|CSSStyleValue|CSSUnitValue'),
    ('register-property', r'registerProperty'),
    ('view-transition', r'startViewTransition|view-transition'),
    ('custom-element', r'customElements|attachInternals'),
    ('fonts', r'\bFontFace\b|document\.fonts|@font-face'),
    ('computed', r'getComputedStyle'),
    ('sheets', r'cssRules|insertRule|deleteRule|styleSheets|CSSStyleSheet|\.sheet\b|adoptedStyleSheets|replaceSync'),
    ('media', r'matchMedia|MediaQueryList|CSS\.supports'),
    ('shadow', r'attachShadow|shadowRoot|shadowrootmode'),
    ('loop', r'\bfor\s*\(|\bwhile\s*\(|forEach|\.map\('),
    ('function', r'\bfunction\b|=>'),
]
MARKER_RE = [(n, re.compile(p)) for n, p in MARKERS]
SCRIPT_RE = re.compile(r'<script\b([^>]*)>(.*?)</script\s*>', re.S | re.I)
SRC_RE = re.compile(r'\bsrc\s*=\s*["\']?([^"\'\s>]+)', re.I)
CALL_RE = re.compile(r'(?<![\w.$])(?:test|promise_test|async_test)\s*\(')
HELPER_CALL_RE = re.compile(r'(?<![\w.$])(?<!function )test_\w+\s*\(')
HELPER_DEF_RE = re.compile(r'\bfunction\s+(test_\w+)\s*\(|\b(?:const|let|var)\s+(test_\w+)\s*=')
# The tests a call of a modeled helper makes (tools/css/cssom_eval.py MODELED_HELPERS: resources/matchmedia-utils.js): `query_should_be_unknown` makes two, the others one. The census counts them
# by the page's own text, so that its count of a page that calls them is not the evaluator's.
HELPER_TESTS = {'query_should_be_css_parseable': 1, 'query_should_not_be_css_parseable': 1, 'query_should_be_js_parseable': 1, 'query_should_not_be_js_parseable': 1,
                'query_should_be_known': 1, 'query_should_be_unknown': 2}
MODELED_HELPER_CALL_RE = re.compile(r'(?<![\w.$])(' + '|'.join(HELPER_TESTS) + r')\s*\(')
REFTEST_RE = re.compile(r'rel=["\']?(?:match|mismatch)')


def load_extractor():
    spec = importlib.util.spec_from_file_location('extract_wpt_cssom', os.path.join(HERE, 'extract-wpt-cssom.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_text(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def vectors_by_page(vdir):
    """(scored tests, listed tests) by page, from the extractor's output. A ``?rule-vectors`` program is credited to its page."""
    scored = {}
    for line in open(os.path.join(vdir, 'declaration.tsv'), 'rb'):
        if line.startswith(b'P\t'):
            f = line.decode('utf-8', 'replace').rstrip('\n').split('\t')
            page = f[1].replace('?rule-vectors', '')
            scored[page] = scored.get(page, 0) + int(f[3])
    # the tests of the scoring interpreter's programs (script.tsv) are scored tests of their page too
    script = os.path.join(vdir, 'script.tsv')
    if os.path.exists(script):
        for line in open(script, 'rb'):
            if line.startswith(b'P\t'):
                f = line.decode('utf-8', 'replace').rstrip('\n').split('\t')
                scored[f[1]] = scored.get(f[1], 0) + int(f[3])
    listed = {}
    for line in open(os.path.join(vdir, 'UNEXTRACTED_CSSOM'), encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        listed[f[0]] = listed.get(f[0], 0) + int(f[1])
    return scored, listed


def rule_vector_pages(vdir):
    out = set()
    for line in open(os.path.join(vdir, 'declaration.tsv'), 'rb'):
        if line.startswith(b'P\t'):
            f = line.decode('utf-8', 'replace').split('\t')
            if f[1].endswith('?rule-vectors'):
                out.add(f[1].replace('?rule-vectors', ''))
    return out


def features(wpt, rel, rulevec):
    """(calls, markers, helpers) of one page, from its own source."""
    text = read_text(os.path.join(wpt, rel))
    scripts = []
    helpers = []
    for attrs, body in SCRIPT_RE.findall(text):
        m = SRC_RE.search(attrs)
        if m:
            name = m.group(1)
            if os.path.basename(name) not in HARNESS:
                helpers.append(name)
        else:
            scripts.append(body)
    own = '\n'.join(scripts) if not rel.endswith('.js') else text
    markup = SCRIPT_RE.sub('', text) if not rel.endswith('.js') else ''
    where = own + '\n' + markup
    marks = [n for n, rx in MARKER_RE if rx.search(where)]
    if 'input-driver' not in marks and any('testdriver' in src for src in SRC_RE.findall(text)):
        marks.append('input-driver')
    reftest = bool(REFTEST_RE.search(text))
    harness = 'testharness.js' in text or rel.endswith(('.window.js', '.any.js', '.worker.js'))
    if reftest:
        marks.append('reftest')
    elif not harness:
        marks.append('manual' if re.search(r'manual|\.xht$', rel) and 'crash' not in rel else 'crash')
    if rulevec:
        marks.append('rule-vectors')
    defined = {a or b for a, b in HELPER_DEF_RE.findall(own)}
    calls = len(CALL_RE.findall(own)) + len([c for c in HELPER_CALL_RE.findall(own) if c.split('(')[0].strip() not in defined])
    if any(os.path.basename(h) == 'matchmedia-utils.js' for h in helpers):
        defined_here = set(re.findall(r'\bfunction\s+(\w+)\s*\(', own))
        calls += sum(HELPER_TESTS[m.group(1)] for m in MODELED_HELPER_CALL_RE.finditer(own) if m.group(1) not in defined_here)
    if reftest or not harness or calls == 0:
        calls = 1 if not rulevec else calls
    return calls, sorted(set(marks)), sorted(set(helpers))


def build(wpt, vdir):
    ext = load_extractor()
    scored, listed = vectors_by_page(vdir)
    rvpages = rule_vector_pages(vdir)
    touched = []
    for rel in ext.walk(wpt):
        try:
            text = read_text(os.path.join(wpt, rel))
        except OSError:
            continue
        if ext.TOUCH.search(text):
            touched.append(rel)
    extractor_pages = set(listed) | {p for p in scored if p not in rvpages or p in listed}
    problems = []
    if set(touched) - extractor_pages:
        problems.append('pages the census touches and the extractor does not account for: %s' % sorted(set(touched) - extractor_pages)[:10])
    rows = []
    for rel in sorted(set(touched) | rvpages):
        tests = scored.get(rel, 0) + listed.get(rel, 0)
        calls, marks, helpers = features(wpt, rel, rel in rvpages and rel not in touched)
        rows.append((rel, tests, calls, marks, helpers))
    extra = extractor_pages - set(touched)
    if extra:
        problems.append('pages the extractor accounts for and the census does not touch: %s' % sorted(extra)[:10])
    return rows, problems


def header(wpt_commit, rows):
    differ = sum(1 for r in rows if r[1] != r[2])
    return [
        '# tools/css/census-wpt-script-pages.py: the pages of the pinned web-platform-tests css/ that touch the CSSOM IDL (and the pages whose rule vectors the extractor runs), with the',
        '# tests the extractor gives each, the test calls the census counts on its own, the markers of the page and the helper scripts it loads. See the script for the columns.',
        '# pin\t%s' % wpt_commit,
        '# pages\t%d' % len(rows),
        '# tests\t%d' % sum(r[1] for r in rows),
        '# calls\t%d' % sum(r[2] for r in rows),
        '# calls-differ-pages\t%d' % differ,
    ]


def render(wpt_commit, rows):
    out = header(wpt_commit, rows)
    for rel, tests, calls, marks, helpers in rows:
        out.append('%s\t%d\t%d\t%s\t%s' % (rel, tests, calls, ','.join(marks), ','.join(helpers)))
    return '\n'.join(out) + '\n'


def load_census(path):
    """(pins, rows) of a census file; rows are {page: (tests, calls, markers, helpers)}."""
    pins = {}
    rows = {}
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if line.startswith('# ') and '\t' in line:
            k, v = line[2:].split('\t', 1)
            pins[k] = v
        elif line and not line.startswith('#'):
            f = line.split('\t')
            if len(f) != 5:
                raise SystemExit('CENSUS: %s:%d has %d fields, not five' % (os.path.basename(path), n, len(f)))
            if f[0] in rows:
                raise SystemExit('CENSUS: %s:%d lists %s twice' % (os.path.basename(path), n, f[0]))
            rows[f[0]] = (int(f[1]), int(f[2]), f[3], f[4])
    return pins, rows


def screen(wpt, vdir):
    """The screen of the script pages: pages and tests by the first marker of the ladder timing, layout-geometry, input-driver, frame-or-popup, else none."""
    rows, _ = build(wpt, vdir)
    ladder = ('timing', 'layout-geometry', 'input-driver', 'frame-or-popup')
    scripted = {}
    for line in open(os.path.join(vdir, 'UNEXTRACTED_CSSOM'), encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        reason = f[2]
        if reason.startswith(('unmodeled:', 'timing')):
            scripted[f[0]] = scripted.get(f[0], 0) + int(f[1])
    by = {}
    for rel, tests, calls, marks, helpers in rows:
        if rel not in scripted:
            continue
        key = next((m for m in ladder if m in marks), 'none')
        p, t = by.get(key, (0, 0))
        by[key] = (p + 1, t + scripted[rel])
    print('script pages: %d pages, %d tests' % (len(scripted), sum(scripted.values())))
    for k in ladder + ('none',):
        print('  %-16s %5d pages %6d tests' % k_fmt(k, by.get(k, (0, 0))))


def k_fmt(k, v):
    return (k, v[0], v[1])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    wpt, vdir = args[0], args[1]
    if '--screen' in sys.argv:
        screen(wpt, vdir)
        return 0
    path = args[2]
    commit = open(os.path.join(HERE, 'WPT_COMMIT')).read().strip()
    rows, problems = build(wpt, vdir)
    if '--write' in sys.argv:
        if problems:
            print('\n'.join('CENSUS: ' + p for p in problems))
            return 1
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(render(commit, rows))
        print('census: %d pages, %d tests (%d pages where the census counts other calls)' % (len(rows), sum(r[1] for r in rows), sum(1 for r in rows if r[1] != r[2])))
        return 0
    out = ['CENSUS: ' + p for p in problems]
    pins, have = load_census(path)
    live = {r[0]: r[1:] for r in rows}
    if pins.get('pin') != commit:
        out.append('CENSUS: the census is of pin %s, the pin is %s' % (pins.get('pin'), commit))
    for rel in sorted(set(live) - set(have))[:20]:
        out.append('CENSUS: %s is touched and not in the census' % rel)
    for rel in sorted(set(have) - set(live))[:20]:
        out.append('CENSUS: %s is in the census and not touched' % rel)
    for rel in sorted(set(live) & set(have)):
        if live[rel][0] != have[rel][0]:
            out.append('CENSUS: %s has %d tests, the census says %d' % (rel, live[rel][0], have[rel][0]))
    total = sum(v[0] for v in live.values())
    if str(total) != pins.get('tests') or str(len(live)) != pins.get('pages'):
        out.append('CENSUS: %d pages and %d tests; the census pins %s and %s, and the pin has not changed' % (len(live), total, pins.get('pages'), pins.get('tests')))
    if out:
        print('\n'.join(out))
        return 1
    print('census: %d pages, %d tests, as pinned' % (len(live), total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
