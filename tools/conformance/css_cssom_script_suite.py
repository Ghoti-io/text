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
"""Score the pages that the scoring interpreter reads (tools/css/cssom_eval.py makes the programs, css_cssom_script_suite.c runs them).

    css_cssom_script_suite.py <vectors dir> <runner> <exceptions> <layout> [--record FILE] [--replay FILE] [--served FILE] [--fixtures FILE] [--wpt DIR] [--regenerate]

``script.tsv`` is the flat program of each page the evaluator read, ``script-tests.tsv`` the name of every test of those pages and either ``model`` (it has ops) or the code of the
reason it has none. The runner gives each modeled test PASS, FAIL (with the first failed assertion's message) or LAYOUT (it read a property whose used value needs a layout: never
credited, never failed; ``UNSCORED_LAYOUT`` lists these and is checked against the run) or TIMELINE (it read a property that an animation targets and the caller gave the animation no time:
the value is the underlying one; never credited, never failed; ``UNSCORED_TIMELINE``, beside the layout list, lists these). The runner is the caller: its document timeline is at 0 and every animation
began there (the pages that depend on more are timing pages), so a TIMELINE is what the run with ``CSSOM_RUNNER_TIMELINE=none`` shows (the controls) and the list is empty.

**The browsers' record.** tools/oracle/css_page_record.py holds, for every page, what Chromium and Firefox answered when they ran the page itself. The gate refuses a record that is
not of this pin, harness, driver or page ("RECORD"), and holds the evaluator to it: a page's tests are as many as the harness reported, in the same order and with the same names, in both
engines ("COUNT"; a name the evaluator cannot compute is empty and is not compared). This is the resolution of the census's question of how many tests a generated page has.

**Classification** (the table of the spec, mechanical where the record decides): a test that passes is a pass (and, if both engines fail it, it is listed in the report "passes where
both engines fail"). A failure is classified from the recorded answers, the first failed assertion's actual value of each: both engines pass the page's expectation, or one passes it and
the other's answer is not ours, or both fail with answers that are not ours: ``defect`` (an owner is named by the table OWNERS below; a defect without one fails the gate); the engines
differ and ours is one of their answers: ``browsers-differ``; both engines fail and answer what we answer: ``wpt-wrong``. A class that rests on a sentence of the served text
(``served-text``, or a ``wpt-wrong`` that rests on a sentence) is never derived: it can only be written by hand in CSS_CSSOM_SCRIPT_SERVED, which the gate reads and requires to quote
the sentence. The exceptions file CSS_CSSOM_SCRIPT_EXCEPTIONS is exactly the derived list (test, our message, class, reason): a difference fails ("UNEXPLAINED FAILURES", "STALE EXCEPTIONS",
"CLASS"); ``--regenerate`` writes it.

**The replay** (--replay, tools/css/replay_gen.py, tools/oracle/css_page_record.py --replay): the flat program of each page, translated back to JavaScript on a real DOM and run by the
browsers in the place of the page's scripts. For every test the evaluator turned into ops the replay has to report what the page reports, in both engines ("REPLAY"); the record of
the replay is refused when it is of another program, harness, driver or generator.

**The user agent sheet.** The library has no user agent sheet; the runner supplies a small one (display, the body's margin). A failure whose first failed assertion changes when the same
program is run with no sheet, or with a fuller one (the runner's CSSOM_RUNNER_UA), may be the runner's and not the library's: it is class ``ua-default`` and not ``defect``, with both
answers in the reason. The class is derived, never written, and has a ceiling of its own.

**Served text** (--served, CSS_CSSOM_SCRIPT_SERVED): the one hand-written file. A line (test, our message, ``served-text`` or ``wpt-wrong``, reason) replaces what the record derives for
that failure, and the reason has to quote a sentence of the served draft and give the recorded answer of each engine; the gate refuses a line whose test does not fail, whose message is
not ours, or whose reason does not carry both answers. A quotation is written ``css-mixins-1: "the sentence"``: the draft's name (a file of tools/css/DRAFTS_SHA256), a colon and the
sentence in double quotation marks (four words or twenty characters at least). The gate hashes the drafts in third_party/css/drafts (``--drafts DIR``, ``--drafts-manifest FILE``;
tools/css/fetch.sh puts them there: the Bikeshed sources of a pinned csswg-drafts commit), reads their text (draft_text: the markup read as the page shows it, white space collapsed,
typographic quotation marks and dashes made plain; a quotation does not run through a cross-reference) and refuses a
sentence that does not occur in the draft it names ("QUOTE"), a draft that does not hash as pinned, or a draft it was not given.

Floors and ceilings (environment; they only move the right way): CSS_CSSOM_SCRIPT_MIN_SCORED, CSS_CSSOM_SCRIPT_MIN_PAGES, CSS_CSSOM_SCRIPT_MAX_DEFECTS, CSS_CSSOM_SCRIPT_MAX_LAYOUT,
CSS_CSSOM_SCRIPT_MAX_TIMELINE, CSS_CSSOM_SCRIPT_MAX_UA.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'oracle'))
sys.stdout.reconfigure(errors='backslashreplace')

CLASSES = ('served-text', 'wpt-wrong', 'browsers-differ', 'wpt-tentative', 'ua-default', 'defect')

# The owner of a defect: the part of the library (or of the harness around it) in which it is fixed. First match wins; a defect that matches none fails the gate.
OWNERS = [
    (r'^css/css-pseudo/parsing/marker-supported-properties-in-animation', 'owner: animation sampling and transition sampling (a layer the library does not have: the value of a property at the middle of a paused animation or a running transition on ::marker)'),
    (r'^css/css-values/tree-counting/sibling-(index|count)-keyframe-', 'owner: animation sampling (a layer the library does not have: the value of a property under an animation at a time, here with sibling-index() in its keyframes)'),
    (r'^css/(css-shapes|css-masking/clip-path)/(basic-shape-)?interpolation', 'owner: animation sampling (a layer the library does not have: the value of a property midway through an interpolation)'),
    (r'^css/css-(animations|transitions|anchor-position)/', 'owner: animation sampling and anchor positioning (a layer the library does not have: computed values under a running animation, anchor() resolution)'),
    (r'^css/css-(cascade|variables|mixins|properties-values-api)/', 'owner: src/css cascade and custom properties'),
    (r'^css/(css-grid|css-flexbox|css-align|css-sizing|css-box|css-multicol|css-logical|css-position|css-tables|css-forms)/', 'owner: src/css computed layer for the box properties'),
    (r'^css/(css-values|css-color|css-color-adjust|css-images|css-backgrounds|css-text|css-fonts|css-ui|css-content|css-lists|css-counter-styles|filter-effects|css-masking|css-transforms|fill-stroke|css-shapes|css-writing-modes|css-overflow|css-display|css-contain|css-pseudo)/',
     'owner: src/css computed value of the property'),
    (r'^css/(selectors|css-conditional|mediaqueries|cssom|cssom-view|css-nesting|css-scoping|css-syntax|css-viewport|css-env|css-page|css-gaps|css-text-decor|css-inline|css-ruby|scroll-animations|css-easing|css-shadow-parts|css-regions)/',
     'owner: src/css selector, condition and cascade layers'),
]
DEFAULT_OWNER = None


def tsv_lines(path):
    rows = []
    if path and os.path.exists(path):
        for n, raw in enumerate(open(path, 'rb'), 1):
            line = raw.decode('utf-8', 'surrogatepass').rstrip('\n')
            if line and not line.startswith('#'):
                rows.append((n, line.split('\t')))
    return rows


UNESC = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\'}


def unesc(s):
    return re.sub(r'\\(.)', lambda m: UNESC.get(m.group(1), m.group(1)), s, flags=re.S)


def esc(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\0', '\\0')


def harness_name(s):
    """The name testharness.js reports for a test whose name has an unpaired surrogate: it writes it as U+XXXX."""
    return re.sub('[\ud800-\udfff]', lambda m: 'U+%04x' % ord(m.group(0)), s)


def draft_text(path):
    """The plain text of an Editors' Draft from its Bikeshed source, as one line. Bikeshed markup is read as the page shows it: tags dropped, ``[=term=]`` and ``[=term|text=]`` as
    the term or the text, ``{{idl}}`` as idl, ``<<type>>`` as <type>, ``''value''`` and a ``'property'`` as the bare word, emphasis marks and backticks dropped, a cross-reference
    (``[[#id]]``, ``[[SPEC]]``) dropped (a quoted sentence cannot run through one, since the page fills it in from elsewhere), entities decoded, runs of white space one space,
    typographic quotation marks and dashes plain."""
    import html
    s = open(path, 'rb').read().decode('utf-8')
    lt, gt = '\ue000', '\ue001'
    s = re.sub(r'<<([^<>\n]*)>>', lambda m: lt + m.group(1) + gt, s)
    s = re.sub(r'\[\[[!?]?[^\[\]\n]*\]\]', '', s)
    s = re.sub(r'\[=([^\]=|]*)\|([^\]=]*)=\]', r'\2', s)
    s = re.sub(r'\[=([^\]=]*)=\]', r'\1', s)
    s = re.sub(r'\{\{([^{}\n|]*)(?:\|[^{}\n]*)?\}\}', r'\1', s)
    s = re.sub(r"''([^'\n]*)''", r'\1', s)
    s = re.sub(r"(?<![\w'])'([A-Za-z@:][\w/@:-]*)'(?![\w'])", r'\1', s)
    s = re.sub(r'<(script|style)(?=[\s>]).*?</\1>', ' ', s, flags=re.S)
    s = re.sub(r'<!--.*?-->', ' ', s, flags=re.S)
    s = re.sub(r'</?[A-Za-z][^<>]*>', '', s)
    s = s.replace('*', '').replace('`', '')
    s = html.unescape(s).replace(lt, '<').replace(gt, '>')
    return plain_text(s)


def plain_text(s):
    s = s.translate({0x2018: "'", 0x2019: "'", 0x201c: "'", 0x201d: "'", 0x2013: '-', 0x2014: '-', 0x2212: '-', 0xa0: ' '}).replace('"', "'")
    return re.sub(r'\s+', ' ', s).strip()


QUOTE = re.compile(r'(css-[a-z0-9-]+): "([^"]*)"')


def answer(msg):
    """The actual value of a failed assertion, as testharness writes it: the text after the last ' but got ', else the message itself without its description."""
    if msg is None:
        return ''
    m = re.search(r' but got (.*)$', msg, re.S)
    if m:
        return m.group(1).strip()
    m = re.match(r'assert_\w+: (.*)$', msg, re.S)
    return (m.group(1) if m else msg).strip()


MESSAGE_OWNERS = [
    (r'unsupported, needs (condition|container\.computed_style|sibling_index|sibling_count)', 'owner: src/css/css_markup.c, the element interface the Markup adapter provides (the callback the computed layer asks for is missing)'),
]


def owner_of(page, msg=''):
    for rx, text in MESSAGE_OWNERS:
        if re.search(rx, msg or ''):
            return text
    for rx, text in OWNERS:
        if re.search(rx, page):
            return text
    return DEFAULT_OWNER


def classify(page, ours_msg, ch, ff):
    """(class, reason) of a test that fails: ours_msg is our first failed assertion; ch, ff are the recorded (status, message) of the two engines."""
    ours = answer(ours_msg)
    cp, fp = ch[0] == 0, ff[0] == 0
    ca, fa = answer(ch[1]), answer(ff[1])
    both = 'chromium %s, firefox %s' % ('passes' if cp else 'answers %s' % ca, 'passes' if fp else 'answers %s' % fa)
    if cp and fp:
        return 'defect', both
    if cp != fp:
        failing = fa if cp else ca
        if failing == ours:
            return 'browsers-differ', '%s; the library answers %s as the engine that fails the page does' % (both, ours)
        return 'defect', '%s; the library answers %s' % (both, ours)
    if ca == fa == ours:
        return 'wpt-wrong', 'both engines answer %s, as the library does (the page expects otherwise)' % ours
    if ours in (ca, fa):
        return 'browsers-differ', '%s; the library answers %s as one of them does' % (both, ours)
    return 'defect', '%s; the library answers %s' % (both, ours)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opt = {}
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a in ('--record', '--wpt', '--served', '--replay', '--fixtures', '--drafts', '--drafts-manifest'):
            opt[a[2:]] = argv[i + 1]
    regenerate = '--regenerate' in sys.argv
    vdir, runner, exc_path, layout_path = args[0], args[1], args[2], args[3]
    wpt = opt.get('wpt') or os.environ.get('CSS_PROPERTY_WPT')
    record_path = opt.get('record')
    served_path = opt.get('served')
    drafts_dir = opt.get('drafts') or (os.path.join(os.path.dirname(wpt), 'drafts') if wpt else None)
    drafts_manifest = opt.get('drafts-manifest') or os.path.join(os.path.dirname(HERE), 'css', 'DRAFTS_SHA256')
    replay_path = opt.get('replay')
    fixtures_path = opt.get('fixtures')
    out = []
    replay_note = 'replay: no replay record was given'

    tests = {}   # (page, index) -> (name, state)
    for _, f in tsv_lines(os.path.join(vdir, 'script-tests.tsv')):
        tests[(f[0], int(f[1]))] = (unesc(f[2]), f[3])
    # The pages no browser finishes (UNRECORDED_PAGES, beside the exceptions): they have no entry in the page record, and have to have no test that is scored, so that nothing rests on an answer the
    # record does not have; a line for a page that is not a page of the program, or that has a scored test, fails.
    unrecorded_path = os.path.join(HERE, 'UNRECORDED_PAGES')   # beside this program, not beside the exceptions: a control gives the gate another exceptions file
    unrecorded = {}
    for n, f in tsv_lines(unrecorded_path):
        if len(f) != 3 or len(f[2].split()) < 5:
            out.append('SUITE PROBLEMS: %s:%d is not <page> TAB <engine> TAB <a reason of a few words>' % (os.path.basename(unrecorded_path), n))
            continue
        unrecorded[f[0]] = f
    for page, f in sorted(unrecorded.items()):
        if not any(k[0] == page for k in tests):
            out.append('UNRECORDED: %s is not a page of the program (stale)' % page)
        elif any(v[1] == 'model' for k, v in tests.items() if k[0] == page):
            out.append('UNRECORDED: %s has a test that is scored, and is held to the record' % page)
    pages = sorted({p for p, _ in tests} - set(unrecorded))
    modeled = {k for k, v in tests.items() if v[1] == 'model'}
    program = open(os.path.join(vdir, 'script.tsv'), 'rb').read()
    def run_all(ua=None):
        env = dict(os.environ)
        env.pop('CSSOM_RUNNER_UA', None)
        if ua:
            env['CSSOM_RUNNER_UA'] = ua
        proc = subprocess.run([runner], input=program, stdout=subprocess.PIPE, check=False, env=env)
        if proc.returncode != 0:
            return None, None
        res, errs = {}, []
        for line in proc.stdout.decode('utf-8', 'surrogateescape').split('\n')[:-1]:
            f = line.split('\t')
            if f[0] == 'R':
                res[(f[1], int(f[2]))] = (f[3], unesc(f[4]) if len(f) > 4 else '')
            elif f[0] == 'E':
                errs.append(line)
        return res, errs
    results, errors = run_all()
    if results is None:
        print('SUITE PROBLEMS: the runner exited with a failure')
        return 1
    if errors:
        out.append('SUITE PROBLEMS: a program could not be run as the page ran it (%d):' % len(errors))
        out.extend('  ' + e[:300] for e in errors[:20])
    if set(results) != modeled:
        out.append('SUITE PROBLEMS: %d results for %d modeled tests (%s)' % (len(results), len(modeled), ', '.join('%s#%d' % k for k in sorted(modeled ^ set(results))[:6])))

    # the runner's own fixtures: programs written by hand, each with the result its tests have to give
    if fixtures_path:
        fx = open(fixtures_path, 'rb').read()
        expect = {}
        for raw in fx.split(b'\n'):
            f = raw.decode('utf-8', 'surrogatepass').split('\t')
            if f[0] == '#expect' and len(f) == 4:
                expect[(f[1], int(f[2]))] = f[3]
        proc = subprocess.run([runner], input=fx, stdout=subprocess.PIPE, check=False)
        got = {}
        for line in proc.stdout.decode('utf-8', 'surrogateescape').split('\n')[:-1]:
            f = line.split('\t')
            if f[0] == 'R':
                got[(f[1], int(f[2]))] = f[3]
            elif f[0] == 'E':
                out.append('FIXTURE: %s' % line[:200])
        for k in sorted(set(expect) | set(got)):
            if got.get(k) != expect.get(k):
                out.append('FIXTURE: %s#%d is %s, the fixture says %s' % (k[0], k[1], got.get(k), expect.get(k)))
    else:
        out.append('FIXTURE: no fixtures were given')

    # the record, and the evaluator held to it
    ch, ff = {}, {}
    if record_path:
        import css_page_record as R
        header, entries = R.load(record_path)
        probs = R.problems(header, entries, pages, wpt)
        out.extend(probs)
        if not probs:
            for page in pages:
                e = entries[page]
                n = len([k for k in tests if k[0] == page])
                for engine, store in (('chromium', ch), ('firefox', ff)):
                    a = e[engine]
                    if 'error' in a:
                        out.append('RECORD: %s did not finish in %s: %s' % (page, engine, a['error']))
                        continue
                    if len(a['tests']) != n:
                        out.append('COUNT: %s has %d tests for the evaluator and %d for %s' % (page, n, len(a['tests']), engine))
                        continue
                    for i, (name, status, message) in enumerate(a['tests']):
                        want = harness_name(tests[(page, i)][0])
                        if want and want != name:
                            out.append('COUNT: %s#%d is named %r by the evaluator and %r by %s' % (page, i, want, name, engine))
                        store[(page, i)] = (status, message)
            if replay_path:
                sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'css'))
                import replay_gen as G
                progs = G.split_programs(program)
                rheader, rentries = R.load(replay_path)
                rprobs = R.replay_problems(rheader, rentries, {p: v[0] for p, v in progs.items()}, wpt)
                out.extend(rprobs)
                if not rprobs:
                    replayed = 0
                    for page in sorted(progs):
                        idx = {i for (pg, i) in modeled if pg == page}
                        diffs = R.replay_differences(page, entries[page], rentries[page], idx)
                        out.extend(diffs)
                        replayed += len(idx) if not diffs else 0
                    replay_note = 'replay: %d tests of %d pages equal the page in both engines' % (replayed, len(progs))
    else:
        out.append('RECORD: no page record was given')
    if not replay_path:
        out.append('REPLAY: no replay record was given')

    # the tests that read a layout, and the ones that read an animated property of an animation with no time
    layout = {k: v[1] for k, v in results.items() if v[0] == 'LAYOUT'}
    timeline = {k: v[1] for k, v in results.items() if v[0] == 'TIMELINE'}
    scored = {k: v for k, v in results.items() if v[0] not in ('LAYOUT', 'TIMELINE')}
    timeline_path = os.path.join(os.path.dirname(layout_path), 'UNSCORED_TIMELINE')
    listed = {}
    for n, f in tsv_lines(layout_path):
        if len(f) != 3:
            out.append('SUITE PROBLEMS: %s:%d has %d fields, not three' % (os.path.basename(layout_path), n, len(f)))
            continue
        k = f[0].rsplit('#', 1)
        listed[(k[0], int(k[1]))] = f[1]
    listed_timeline = {}
    for n, f in tsv_lines(timeline_path):
        if len(f) != 3:
            out.append('SUITE PROBLEMS: %s:%d has %d fields, not three' % (os.path.basename(timeline_path), n, len(f)))
            continue
        k = f[0].rsplit('#', 1)
        listed_timeline[(k[0], int(k[1]))] = f[1]
    timeline_lines = ['%s#%d\t%s\tan animation targets this property and was given no time: its value is the underlying one (gtext_cssom_computed_origin() says ABSENT_TIMELINE)' % (p, i, prop) for (p, i), prop in sorted(timeline.items())]
    layout_lines = ['%s#%d\t%s\tthe library has no layout: the used value of this property is not computed here (gtext_cssom_computed_origin() says ABSENT_LAYOUT)' % (p, i, prop) for (p, i), prop in sorted(layout.items())]

    # the classification
    derived = {}   # (page, index) -> (got, class, reason)
    report = []
    unowned = []
    for k, (state, msg) in sorted(scored.items()):
        if k not in ch or k not in ff:
            continue
        if state == 'PASS':
            if ch[k][0] != 0 and ff[k][0] != 0:
                report.append('%s#%d' % k)
            continue
        cls, reason = classify(k[0], msg, ch[k], ff[k])
        if cls == 'defect':
            own = owner_of(k[0], msg)
            if own is None:
                unowned.append('%s#%d' % k)
                own = 'owner: ?'
            reason = own + '; ' + reason
        derived[k] = (msg, cls, reason)
    # a defect whose answer moves with the runner's user agent sheet may be the runner's, not the library's
    if any(v[1] == 'defect' for v in derived.values()):
        by_ua = {}
        for ua in ('none', 'full'):
            by_ua[ua], _ = run_all(ua)
            if by_ua[ua] is None:
                out.append('SUITE PROBLEMS: the runner exited with a failure with CSSOM_RUNNER_UA=%s' % ua)
                by_ua[ua] = {}

        def say(r):
            if r is None:
                return 'is not run'
            if r[0] == 'PASS':
                return 'passes'
            if r[0] == 'TIMELINE':
                return 'needs a timeline'
            if r[0] == 'LAYOUT':
                return 'needs a layout'
            return 'answers %s' % (answer(r[1]) or r[1])
        for k, (msg, cls, reason) in sorted(derived.items()):
            if cls != 'defect':
                continue
            base = results.get(k)
            if any(by_ua[ua].get(k) not in (None, base) for ua in by_ua):
                derived[k] = (msg, 'ua-default', "owner: the user agent sheet of the runner (the library has none; the runner's is css_cssom_script_suite.c UA_SHEET), not shown to be the library's: the first failed assertion "
                              "moves with it (no sheet: %s; the runner's sheet: %s; a fuller sheet: %s); %s" % (say(by_ua['none'].get(k)), say(base), say(by_ua['full'].get(k)), reason.split('; ', 1)[-1]))
    served = {}
    drafts = {}   # draft name -> its text, for the drafts a served line names (each hashed against DRAFTS_SHA256 first)
    pins = {f[1][:-3]: (f[0], f[1]) for _, f in tsv_lines(drafts_manifest) if len(f) >= 2 and f[1].endswith('.bs')}
    used = {d for _, f in tsv_lines(served_path) if len(f) == 4 for d, _ in QUOTE.findall(f[3])}
    for d in sorted(used):
        if d not in pins:
            continue
        path = os.path.join(drafts_dir or '', pins[d][1])
        if not os.path.exists(path):
            out.append('SUITE PROBLEMS: the draft %s is not fetched (run tools/css/fetch.sh)' % d)
            continue
        import hashlib
        if hashlib.sha256(open(path, 'rb').read()).hexdigest() != pins[d][0]:
            out.append('SUITE PROBLEMS: %s does not hash as pinned in DRAFTS_SHA256 (run tools/css/fetch.sh, which keeps nothing that differs)' % path)
            continue
        drafts[d] = draft_text(path)
    for n, f in tsv_lines(served_path):
        if len(f) != 4 or f[2] not in ('served-text', 'wpt-wrong'):
            out.append('SUITE PROBLEMS: %s:%d is not (test, our message, served-text or wpt-wrong, reason)' % (os.path.basename(served_path or '?'), n))
            continue
        quotes = [(d, q) for d, q in QUOTE.findall(f[3]) if len(q) >= 20 or len(q.split()) >= 4]
        if not quotes:
            out.append('SUITE PROBLEMS: %s:%d: a %s reason quotes a sentence of the served draft (as css-NAME-N: "the sentence")' % (os.path.basename(served_path), n, f[2]))
        for d, q in quotes:
            text = drafts.get(d)
            if text is None:
                out.append('QUOTE: %s:%d names the draft %s, which is not in DRAFTS_SHA256 or is not fetched (tools/css/fetch.sh)' % (os.path.basename(served_path), n, d))
            elif plain_text(q) not in text:
                out.append('QUOTE: %s:%d quotes "%s" and %s does not say it' % (os.path.basename(served_path), n, q[:80], d))
        k = f[0].rsplit('#', 1)
        served[(k[0], int(k[1]))] = (unesc(f[1]), f[2], f[3])
    for k, v in served.items():
        if k not in derived:
            out.append('STALE EXCEPTIONS: %s#%d is in the served-text list and does not fail' % k)
            continue
        if derived[k][0] != v[0]:
            out.append('STALE EXCEPTIONS: %s#%d fails with "%s", not "%s"' % (k[0], k[1], derived[k][0][:100], v[0][:100]))
            continue
        for engine, (st, msg) in (('chromium', ch.get(k, (None, None))), ('firefox', ff.get(k, (None, None)))):
            need = 'passes' if st == 0 else answer(msg)
            if engine not in v[2] or not need or need not in v[2]:
                out.append('SUITE PROBLEMS: the %s line of %s#%d must give the recorded answer of %s (%s)' % (v[1], k[0], k[1], engine, need))
        derived[k] = v
    if unowned:
        out.append('UNOWNED DEFECTS (%d): a defect needs an owner (OWNERS in css_cssom_script_suite.py): %s' % (len(unowned), ', '.join(unowned[:10])))

    exc_lines = ['%s#%d\t%s\t%s\t%s' % (p, i, esc(m), c, r) for (p, i), (m, c, r) in sorted(derived.items())]
    if regenerate:
        with open(exc_path, 'w', encoding='utf-8', newline='\n', errors='surrogateescape') as f:
            f.write('# tools/conformance/css_cssom_script_suite.py: every failure of the pages the scoring interpreter reads, classified from the page record of the two browsers. Derived: test (page#index), our first failed assertion, class, reason. The gate fails on any difference from what it derives.\n')
            f.write('\n'.join(exc_lines) + '\n')
        with open(layout_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# tools/conformance/css_cssom_script_suite.py: the tests of the scoring interpreter that read a property whose used value needs a layout. Derived: test (page#index), property, why. They are neither passed nor failed; the ceiling falls only when a layout consumer supplies used values.\n')
            f.write('\n'.join(layout_lines) + '\n')
        with open(timeline_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# tools/conformance/css_cssom_script_suite.py: the tests of the scoring interpreter that read a property an animation targets while the caller gave the animation no time. Derived: test (page#index), property, why. They are neither passed nor failed.\n')
            f.write('\n'.join(timeline_lines) + ('\n' if timeline_lines else ''))
        print('regenerated: %d exceptions, %d layout tests, %d timeline tests' % (len(exc_lines), len(layout_lines), len(timeline_lines)))
        return 0

    have = {}
    for n, f in tsv_lines(exc_path):
        if len(f) != 4:
            out.append('SUITE PROBLEMS: %s:%d has %d fields, not four' % (os.path.basename(exc_path), n, len(f)))
            continue
        k = f[0].rsplit('#', 1)
        have[(k[0], int(k[1]))] = (unesc(f[1]), f[2], f[3], n)
        if f[2] not in CLASSES:
            out.append('SUITE PROBLEMS: %s:%d: class %r is not one of %s' % (os.path.basename(exc_path), n, f[2], ', '.join(CLASSES)))
    if ch:
        unexplained = [k for k in derived if k not in have]
        if unexplained:
            out.append('UNEXPLAINED FAILURES (%d):' % len(unexplained))
            out.extend('  %s#%d  %s' % (k[0], k[1], derived[k][0][:160]) for k in unexplained[:60])
        stale = []
        for k, (m, c, r, n) in have.items():
            if k not in results:
                stale.append('%s:%d: %s#%d does not exist' % (os.path.basename(exc_path), n, k[0], k[1]))
            elif k not in derived:
                stale.append('%s:%d: %s#%d does not fail' % (os.path.basename(exc_path), n, k[0], k[1]))
            elif derived[k][0] != m:
                stale.append('%s:%d: %s#%d fails with "%s", not "%s"' % (os.path.basename(exc_path), n, k[0], k[1], derived[k][0][:100], m[:100]))
            elif derived[k][1] != c:
                out.append('CLASS: %s#%d is %s in the list and %s by the record' % (k[0], k[1], c, derived[k][1]))
            elif derived[k][2] != r:
                out.append('CLASS: %s#%d has the reason "%s" in the list and "%s" by the record' % (k[0], k[1], r[:120], derived[k][2][:120]))
        if stale:
            out.append('STALE EXCEPTIONS:')
            out.extend('  ' + s for s in stale[:60])
    # the layout list
    for k in sorted(set(layout) | set(listed)):
        if k not in layout:
            out.append('LAYOUT: %s#%d is listed in %s and no longer reads a layout' % (k[0], k[1], os.path.basename(layout_path)))
        elif k not in listed:
            out.append('LAYOUT: %s#%d reads %s, which needs a layout, and is not listed in %s' % (k[0], k[1], layout[k], os.path.basename(layout_path)))
        elif listed[k] != layout[k]:
            out.append('LAYOUT: %s#%d reads %s, the list says %s' % (k[0], k[1], layout[k], listed[k]))
    for k in sorted(set(timeline) | set(listed_timeline)):
        if k not in timeline:
            out.append('TIMELINE: %s#%d is listed in %s and no longer reads an animated property of an animation with no time' % (k[0], k[1], os.path.basename(timeline_path)))
        elif k not in listed_timeline:
            out.append('TIMELINE: %s#%d reads %s, which needs a timeline, and is not listed in %s' % (k[0], k[1], timeline[k], os.path.basename(timeline_path)))
        elif listed_timeline[k] != timeline[k]:
            out.append('TIMELINE: %s#%d reads %s, the list says %s' % (k[0], k[1], timeline[k], listed_timeline[k]))
    n_scored = len(scored)
    n_pass = sum(1 for v in scored.values() if v[0] == 'PASS')
    defects = sum(1 for v in derived.values() if v[1] == 'defect')
    uas = sum(1 for v in derived.values() if v[1] == 'ua-default')
    scored_pages = len({k[0] for k in scored})
    for name, value, kind in (('CSS_CSSOM_SCRIPT_MIN_SCORED', n_scored, 'min'), ('CSS_CSSOM_SCRIPT_MIN_PAGES', scored_pages, 'min'), ('CSS_CSSOM_SCRIPT_MAX_DEFECTS', defects, 'max'),
                              ('CSS_CSSOM_SCRIPT_MAX_LAYOUT', len(layout), 'max'), ('CSS_CSSOM_SCRIPT_MAX_TIMELINE', len(timeline), 'max'),
                              ('CSS_CSSOM_SCRIPT_MAX_UA', uas, 'max')):
        limit = os.environ.get(name)
        if limit is None:
            continue
        if kind == 'min' and value < int(limit):
            out.append('below the floor: %d %s, the floor is %s' % (value, name.split('_')[-1].lower(), limit))
        if kind == 'max' and value > int(limit):
            out.append('above the ceiling: %d %s, the ceiling is %s' % (value, 'defects' if 'DEFECTS' in name else 'ua-default tests' if name.endswith('_UA') else 'timeline tests' if name.endswith('_TIMELINE') else 'layout tests', limit))
    if out:
        print('\n'.join(out))
        return 1
    with open(os.path.join(vdir, 'SCRIPT_SCORED'), 'w') as f:
        f.write('%d %d\n' % (n_scored, len(layout) + len(timeline)))
    classes = {}
    for v in derived.values():
        classes[v[1]] = classes.get(v[1], 0) + 1
    print('css-cssom-script: %d tests of %d pages scored (%d pass, %d exceptions: %s); %d need a layout and %d a timeline and are not credited; %d unmodeled (%d pages read)' % (
        n_scored, scored_pages, n_pass, len(derived), ', '.join('%d %s' % (v, k) for k, v in sorted(classes.items())) or 'none', len(layout), len(timeline), len(tests) - len(modeled), len(pages)))
    print('css-cssom-script: ' + replay_note)
    print('css-cssom-script: passes where both engines fail the page: %d%s' % (len(report), (' (' + ', '.join(report[:6]) + (', ...' if len(report) > 6 else '') + ')') if report else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
