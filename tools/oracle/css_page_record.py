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
"""The page record: what Chromium and Firefox answer when they run a page of web-platform-tests under its own testharness.js.

    css_page_record.py --record [--pages FILE] [--out FILE] [--batch N]      ask the browsers (needs the pinned image and a container engine)
    css_page_record.py --check  [--pages FILE] [--record-file FILE]          verify the committed record against the checkout (no browser, no network)

The classification table of the scoring interpreter (the spec's "How a result is classified") needs, for each test it scores, the status and the message of the page *as the page
ran it*, in both browsers. The record holds, for each page the interpreter reads, the sha-256 of the page and, for each engine, the tests in the order the harness made them:
name, status (0 PASS, 1 FAIL, 2 TIMEOUT, 3 NOTRUN, 4 PRECONDITION_FAILED) and message. Its first line names the pin of the suite (the commit and the sha-256 of testharness.js, the
harness that ran), the engines and the image (tools/oracle/containers/IMAGES), and the sha-256 of the mounted page driver (containers/cssprop/page_driver.mjs).

The default gate never needs a browser: it reads the record and refuses it (``--check``) when the pin, the harness or the driver differ from the repository's, when a page of the
program has no entry, or when a page's hash differs from the one recorded (the page changed: the record is stale). The container gate records again and compares.

**The replay** (tools/css/replay_gen.py) is the same question asked of the flat program instead of the page: the program of each page, translated back into JavaScript on the real DOM, is
put where the page's scripts were and run by the same driver under the same harness. ``--replay`` records it (tools/conformance/css_cssom_replayrecord.jsonl: for each page the sha-256
of its program and, for each engine, the tests the replay reported), and the CSSOM gate requires, for every test the evaluator turned into ops, that the replay's status (and, for a
failure, its actual value) is the page's in both engines: when it is not, the evaluator's reading of the page is not the page. ``--replay-controls`` mistranslates pages on purpose and
requires the comparison to refuse each.

Pages are a file of paths, one a line (build/.../script-pages.txt, written by the gate); the browsers see them at /<path> of a loopback server over the checkout.

**Stability.** A page that reads layout, or whose result depends on when the parser yields to a frame (one of the pages asks `rlh` of the root, and Chromium answers 36px instead of 146px
when a frame was drawn between its `<style>` and its `<script>`), can answer differently from one run to the next when the host is loaded: the browsers' own timing, which no driver
setting removes. The driver therefore fixes what it can (the viewport, 1280x800 at device pixel ratio 1, is set before every page; a page that does not finish in 15 s is an error,
not a wait) and this module decides what is an answer. Recording from nothing asks every page twice, in two fresh sessions, and a page whose two answers differ is asked again, alone,
in a session of its own, until two consecutive answers agree (five tries at most; a page that never does is recorded as an error, and the record is refused). Asking against a
committed record (``--against``) asks every page once, and a page that differs is asked again, alone: when that answer is the committed one the page is *unstable* (listed, with both
answers, and counted: more than UNSTABLE_LIMIT of them means the host is too loaded to be trusted and the run is refused), when it is not the answer stays and the comparison fails.
A change in a page, in a browser or in the harness is answered the same way by the second try, so it cannot hide there; the one thing a retry can hide is a page whose answer
really does alternate between the recorded one and another, which is what *unstable* names and counts.

**Declared flaky pages.** For one page the alternation is the browser's, is measured, and does not go away with a quiet host: Chromium answers css/css-values/rlh-on-root-lengths.html
"36px" instead of "146px" in about one load in thirty (one in six inside a long session), because a style recalculation that runs between the page's ``<style>`` and its
``<script>`` computes the root's ``rlh`` before the root's own ``line-height`` (no frame is drawn: a ``requestAnimationFrame`` callback made first has not run). A retry alone is
a fresh session, which is the slowest one, so "differs, then differs again" is not rare enough to be a verdict. Such a page is declared in FLAKY (tools/oracle/css_page_flaky.json:
page, engine, the exact alternate answers, and why). The rule for a declared page, and only for it: the page is asked FLAKY_ASKS times in one session; an answer that is neither the
recorded one nor a declared alternate is a change (the comparison fails); and when none of the answers is the recorded one the comparison fails too (a browser that has come to answer
the alternate every time is a change, not noise). Anything else is *unstable*, listed and counted like any other. A declaration excuses its own page, its own engine and its own
answers; the controls (``--policy-controls``) show that each of those limits holds.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

DRIVER = os.path.join(HERE, 'containers', 'cssprop', 'page_driver.mjs')
RECORD = os.path.join(ROOT, 'tools', 'conformance', 'css_cssom_pagerecord.jsonl')
REPLAY_RECORD = os.path.join(ROOT, 'tools', 'conformance', 'css_cssom_replayrecord.jsonl')
GENERATOR = os.path.join(ROOT, 'tools', 'css', 'replay_gen.py')
REPLAY_ROOT = os.path.join(ROOT, 'build', 'css-replay-root')
WPT = os.environ.get('CSS_PROPERTY_WPT') or os.path.join(ROOT, 'third_party', 'css', 'wpt')
ENGINES = ('chromium', 'firefox')


def sha256_file(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def pin():
    return open(os.path.join(ROOT, 'tools', 'css', 'WPT_COMMIT')).read().strip()


def read_pages(path):
    return [l.rstrip('\n') for l in open(path, encoding='utf-8') if l.strip()]


def normalize(text):
    """A message that names the loopback server names its port, which is the system's to choose: the record writes PORT."""
    return None if text is None else re.sub(r'127\.0\.0\.1:\d+', '127.0.0.1:PORT', text)


def ask(engine, pages, wpt):
    import oracle_env
    inner = ['sh', '-c', 'cd /opt/cssprop && exec node "%s" %s "%s"' % (DRIVER, engine, wpt)]
    cmd = oracle_env.command('cssprop', inner)
    data = ''.join(json.dumps(p) + '\n' for p in pages).encode('utf-8')
    p = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if p.returncode != 0:
        raise SystemExit('css_page_record: %s exited %d:\n%s' % (engine, p.returncode, oracle_env.reference_stderr(p.stderr.decode('utf-8', 'replace'))[-1500:]))
    lines = p.stdout.decode('utf-8', 'surrogateescape').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    header = json.loads(lines[0][1:])
    rows = [json.loads(x) for x in lines[1:]]
    if len(rows) != len(pages):
        raise SystemExit('css_page_record: %s answered %d pages to %d' % (engine, len(rows), len(pages)))
    return header, rows


UNSTABLE_LIMIT = 25
STABLE_TRIES = 5
FLAKY_ASKS = 16
FLAKY_FILE = os.path.join(HERE, 'css_page_flaky.json')


def load_flaky(path=None):
    """{(page, engine): [the alternate entries]} of the declared flaky pages."""
    out = {}
    with open(path or FLAKY_FILE, encoding='utf-8') as f:
        for page, engines in json.load(f).items():
            for engine, decl in engines.items():
                if engine not in ENGINES or not decl.get('reason') or not decl.get('alternates'):
                    raise SystemExit('css_page_record: %s: %s/%s needs an engine, a reason and alternates' % (path or FLAKY_FILE, page, engine))
                out[(page, engine)] = decl['alternates']
    return out


FLAKY = load_flaky()


def ask_many(engine, page, count, wpt):
    """The entries of `count` loads of one page in one session."""
    _, rows = ask(engine, [page] * count, wpt)
    return [entry_of(r) for r in rows]


def settle_flaky(engine, page, seen, reference, wpt):
    """(entry to keep, unstable answers or None) for a declared flaky page, `seen` being the answers already had. With a reference the entry is the reference when every answer is the
    reference or a declared alternate and the reference was among them; otherwise the first answer that is neither (or, when only alternates came, the first alternate), so that the
    comparison fails. Without one the entry is the one answer that is not an alternate, or an error when there are several."""
    alts = FLAKY[(page, engine)]
    seen = list(seen) + ask_many(engine, page, FLAKY_ASKS, wpt)
    others = [x for x in seen if x not in alts and x != reference]
    if reference is not None:
        if others:
            return others[0], None
        if reference not in seen:
            return seen[0], None
        return reference, [x for x in seen[:2]]
    plain = []
    for x in seen:
        if x not in alts and x not in plain:
            plain.append(x)
    if len(plain) > 1:
        return {'error': 'unstable: %d different answers that are not declared alternates' % len(plain)}, seen[:2]
    if not plain:
        return seen[0], None
    return plain[0], (seen[:2] if any(x in alts for x in seen) else None)


def entry_of(a):
    """What the record keeps of a driver answer: the status and the tests with the port written as PORT, or the error."""
    if 'error' in a:
        return {'error': normalize(a['error'])}
    return {'status': a['status'], 'tests': [[normalize(t['name']), t['status'], normalize(t['message'])] for t in a['tests']]}


def one_pass(engine, pages, wpt, batch, label):
    """One session per batch of pages: (header of the engine, {page: entry}, slowest page in ms)."""
    entries = {}
    header = None
    slowest = 0
    for i in range(0, len(pages), batch):
        part = pages[i:i + batch]
        head, rows = ask(engine, part, wpt)
        header = {'browser': head['browser'], 'image': head['image']}
        for r in rows:
            if r['page'] not in part:
                raise SystemExit('css_page_record: %s answered a page that was not asked: %s' % (engine, r['page']))
            entries[r['page']] = entry_of(r)
            slowest = max(slowest, r.get('ms', 0))
        if len(pages) > 1:
            print('css_page_record: %s %s %d of %d pages' % (engine, label, min(i + batch, len(pages)), len(pages)), file=sys.stderr)
    return header, entries, slowest


def settle(engine, pages, wpt, batch, reference=None, label=''):
    """(header, {page: entry}, {page: [the entries it answered]} for the pages that answered more than one thing).

    reference None: two passes over every page, and a page whose answers differ is asked alone until two in a row agree. reference {page: entry}: one pass, and a page that differs from
    the reference is asked once more alone; the entry kept is that second one, and the page is unstable when it is the reference's."""
    header, first, slowest = one_pass(engine, pages, wpt, batch, label + 'pass 1')
    entries = dict(first)
    unstable = {}
    if reference is not None:
        for p in pages:
            if first[p] != reference.get(p) and (p, engine) in FLAKY:
                entries[p], seen = settle_flaky(engine, p, [first[p]], reference.get(p), wpt)
                if seen is not None:
                    unstable[p] = seen
            elif first[p] != reference.get(p):
                _, again, _ = one_pass(engine, [p], wpt, 1, label + 'retry')
                entries[p] = again[p]
                if again[p] == reference.get(p):
                    unstable[p] = [first[p], again[p]]
    else:
        _, second, _ = one_pass(engine, pages, wpt, batch, label + 'pass 2')
        for p in pages:
            if (p, engine) in FLAKY:
                entries[p], seen = settle_flaky(engine, p, [first[p], second[p]], None, wpt)
                if seen is not None:
                    unstable[p] = seen
                continue
            if first[p] == second[p]:
                continue
            seen = [first[p], second[p]]
            while len(seen) < STABLE_TRIES and seen[-1] != seen[-2]:
                _, again, _ = one_pass(engine, [p], wpt, 1, label + 'retry')
                seen.append(again[p])
            unstable[p] = seen
            entries[p] = seen[-1] if seen[-1] == seen[-2] else {'error': 'unstable: %d answers, no two in a row alike' % len(seen)}
    print('css_page_record: %s: slowest page %d ms' % (engine, slowest), file=sys.stderr)
    return header, entries, unstable


def settle_engines(pages, wpt, batch, references=None, label='', jobs=2):
    """settle() for both engines (at the same time when jobs > 1): (headers, {engine: entries}, {engine: unstable}). Refuses a run with too many unstable pages."""
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=max(1, min(jobs, len(ENGINES)))) as pool:
        futures = {e: pool.submit(settle, e, pages, wpt, batch, None if references is None else references.get(e, {}), label) for e in ENGINES}
        results = {e: f.result() for e, f in futures.items()}
    headers = {e: results[e][0] for e in ENGINES}
    answers = {e: results[e][1] for e in ENGINES}
    unstable = {e: results[e][2] for e in ENGINES}
    names = sorted({p for e in ENGINES for p in unstable[e]})
    for e in ENGINES:
        for p in sorted(unstable[e]):
            print('css_page_record: UNSTABLE %s in %s: %d answers (%s)' % (p, e, len(unstable[e][p]), ' | '.join(json.dumps(x, sort_keys=True)[:100] for x in unstable[e][p][:2])), file=sys.stderr)
    if len(names) > UNSTABLE_LIMIT:
        raise SystemExit('css_page_record: %d pages answered differently from one try to the next (more than %d): this host is too loaded to be trusted; run it on a quiet one' % (len(names), UNSTABLE_LIMIT))
    if names:
        print('css_page_record: %d unstable page(s), listed above' % len(names), file=sys.stderr)
    return headers, answers, unstable


def record(pages, out, wpt, batch, against=None, jobs=2):
    import oracle_env
    references = None
    if against:
        _, committed = load(against)
        references = {e: {p: x[e] for p, x in committed.items() if e in x} for e in ENGINES}
    headers, answers, _ = settle_engines(pages, wpt, batch, references, jobs=jobs)
    head = {
        'wpt': pin(),
        'harness': sha256_file(os.path.join(wpt, 'resources', 'testharness.js')),
        'driver': oracle_env.mounted_driver(DRIVER),
        'engines': headers,
    }
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#' + json.dumps(head, sort_keys=True) + '\n')
        for page in sorted(pages):
            entry = {'page': page, 'sha': sha256_file(os.path.join(wpt, page))}
            for engine in ENGINES:
                entry[engine] = answers[engine][page]
            f.write(json.dumps(entry, sort_keys=True, ensure_ascii=True) + '\n')
    print('css_page_record: wrote %s (%d pages)' % (out, len(pages)))


def load(path):
    """(header, {page: entry}) of a record file."""
    header, entries = None, {}
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            if n == 1 and line.startswith('#'):
                header = json.loads(line[1:])
            elif line.strip():
                e = json.loads(line)
                entries[e['page']] = e
    return header, entries


def problems(header, entries, pages, wpt, driver=DRIVER):
    """The refusals for a record: the pin, the harness, the driver, a page with no entry, a page that changed."""
    out = []
    if header is None:
        return ['RECORD: the page record has no header line']
    if header.get('wpt') != pin():
        out.append('RECORD: the record is of the suite at %s, the pin is %s; record again' % (header.get('wpt'), pin()))
    harness = os.path.join(wpt, 'resources', 'testharness.js')
    if not os.path.exists(harness):
        out.append('RECORD: %s is not in the checkout; fetch it (tools/css/fetch.sh)' % harness)
    elif header.get('harness') != sha256_file(harness):
        out.append('RECORD: the record was made under testharness.js %s, the checkout has %s' % (str(header.get('harness'))[:12], sha256_file(harness)[:12]))
    want = {os.path.basename(driver): sha256_file(driver)}
    if header.get('driver') != want:
        out.append('RECORD: the record was made by the page driver %s, the repository has %s; record again' % (json.dumps(header.get('driver')), json.dumps(want)))
    missing = [p for p in pages if p not in entries]
    for p in missing[:20]:
        out.append('RECORD: %s has no entry' % p)
    if len(missing) > 20:
        out.append('RECORD: and %d pages more have no entry' % (len(missing) - 20))
    stale = 0
    for p in pages:
        e = entries.get(p)
        if e is None:
            continue
        if e['sha'] != sha256_file(os.path.join(wpt, p)):
            stale += 1
            if stale <= 20:
                out.append('RECORD: %s changed since it was recorded' % p)
    if stale > 20:
        out.append('RECORD: and %d pages more changed' % (stale - 20))
    for p in pages:
        e = entries.get(p)
        if e is not None:
            for engine in ENGINES:
                if engine not in e:
                    out.append('RECORD: %s has no %s answer' % (p, engine))
    return out


# ---- the replay -------------------------------------------------------------------------------------------------------------------------------

def answer_of(message):
    """The actual value of a failed assertion, as testharness.js writes it (text after the last ' but got '), or None."""
    if message is None:
        return None
    m = re.search(r' but got (.*)$', message, re.S)
    return m.group(1).strip() if m else None


def replay_index(name):
    m = re.match(r'(\d+)\|', name)
    return int(m.group(1)) if m else None


def replay_tests(entry, engine):
    """{test index: (status, message)} of a replay entry, or None when the engine did not finish."""
    a = entry.get(engine, {})
    if 'error' in a or 'tests' not in a:
        return None
    return {replay_index(n): (st, msg) for n, st, msg in a['tests'] if replay_index(n) is not None}


def replay_differences(page, page_entry, replay_entry, modeled):
    """The lines that say where the replay of a page is not the page: `modeled` is the indexes of the tests the evaluator turned into ops."""
    out = []
    for engine in ENGINES:
        mine = replay_tests(replay_entry, engine)
        if mine is None:
            out.append('REPLAY: %s did not finish in %s: %s' % (page, engine, replay_entry.get(engine, {}).get('error', 'no tests')))
            continue
        theirs = page_entry[engine]
        if 'error' in theirs:
            continue
        run = {i: (st, msg) for i, (_n, st, msg) in enumerate(theirs['tests'])}
        for i in sorted(modeled):
            if i not in mine:
                out.append('REPLAY: %s#%d has no result in the replay in %s' % (page, i, engine))
                continue
            if i not in run:
                continue
            (ps, pm), (rs, rm) = run[i], mine[i]
            if ps != rs:
                out.append('REPLAY: %s#%d is status %d in %s and %d in its replay: the program is not the page' % (page, i, ps, engine, rs))
            elif ps == 1 and answer_of(pm) is not None and answer_of(rm) is not None and answer_of(pm) != answer_of(rm):
                out.append('REPLAY: %s#%d answers %s in %s and %s in its replay: the program is not the page' % (page, i, answer_of(pm), engine, answer_of(rm)))
    return out


def record_replay(programs_path, out, batch, plants=None, pages=None, against=None, jobs=2):
    """Replay the programs in the browsers: the overlay of build/css-replay-root is served in place of the checkout."""
    import oracle_env
    import shutil
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'css'))
    import replay_gen as G
    shutil.rmtree(REPLAY_ROOT, ignore_errors=True)
    os.makedirs(os.path.dirname(REPLAY_ROOT), exist_ok=True)
    done = G.generate(programs_path, WPT, REPLAY_ROOT, pages, plants)
    names = sorted(done)
    references = None
    if against:
        _, committed = load(against)
        references = {e: {p: x[e] for p, x in committed.items() if e in x} for e in ENGINES}
    headers, answers, _ = settle_engines(names, REPLAY_ROOT, batch, references, label='replay ', jobs=jobs)
    head = {
        'wpt': pin(),
        'harness': sha256_file(os.path.join(WPT, 'resources', 'testharness.js')),
        'driver': oracle_env.mounted_driver(DRIVER),
        'generator': sha256_file(GENERATOR),
        'engines': headers,
    }
    lines = ['#' + json.dumps(head, sort_keys=True)]
    for page in names:
        entry = {'page': page, 'program': done[page]}
        for engine in ENGINES:
            entry[engine] = answers[engine][page]
        lines.append(json.dumps(entry, sort_keys=True, ensure_ascii=True))
    if out:
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        print('css_page_record: wrote %s (%d pages replayed)' % (out, len(names)))
    return lines


def replay_problems(header, entries, program_shas, wpt, driver=DRIVER, generator=GENERATOR):
    """The refusals for a replay record: another pin, harness, driver or generator, a program with no replay, a program that changed since it was replayed."""
    out = []
    if header is None:
        return ['REPLAY: the replay record has no header line']
    if header.get('wpt') != pin():
        out.append('REPLAY: the replay record is of the suite at %s, the pin is %s; replay again' % (header.get('wpt'), pin()))
    harness = os.path.join(wpt, 'resources', 'testharness.js')
    if os.path.exists(harness) and header.get('harness') != sha256_file(harness):
        out.append('REPLAY: the replay record was made under testharness.js %s, the checkout has %s' % (str(header.get('harness'))[:12], sha256_file(harness)[:12]))
    want = {os.path.basename(driver): sha256_file(driver)}
    if header.get('driver') != want:
        out.append('REPLAY: the replay record was made by the page driver %s, the repository has %s; replay again' % (json.dumps(header.get('driver')), json.dumps(want)))
    if header.get('generator') != sha256_file(generator):
        out.append('REPLAY: the replay record was made by replay_gen.py %s, the repository has %s; replay again' % (str(header.get('generator'))[:12], sha256_file(generator)[:12]))
    missing = [p for p in sorted(program_shas) if p not in entries]
    for p in missing[:20]:
        out.append('REPLAY: %s has a program and no replay; replay again' % p)
    if len(missing) > 20:
        out.append('REPLAY: and %d programs more have no replay' % (len(missing) - 20))
    stale = [p for p in sorted(program_shas) if p in entries and entries[p]['program'] != program_shas[p]]
    for p in stale[:20]:
        out.append('REPLAY: the program of %s changed since it was replayed; replay again' % p)
    if len(stale) > 20:
        out.append('REPLAY: and %d programs more changed' % (len(stale) - 20))
    for p in sorted(set(entries) - set(program_shas))[:20]:
        out.append('REPLAY: %s was replayed and has no program' % p)
    return out


def plant_candidates(progs, record):
    """For each plant of replay_gen.py, two small pages on which the plant touches a test whose verdict it must change: a test that passes in both engines (a failing assertion planted in it),
    or, for drop-assert, a test that fails in both engines on its only assertion (the assertion removed)."""
    import replay_gen as G
    found = {k: [] for k in G.PLANTS}
    taken = set()
    for page in sorted(progs):
        e = record.get(page)
        if e is None or 'error' in e['chromium'] or 'error' in e['firefox']:
            continue
        sha, html, ops = progs[page]
        if len(html) > 8000:
            continue
        test_of, cur = [], None
        for op in ops:
            if op[0] == 'test':
                cur = int(op[1])
            test_of.append(cur)
            if op[0] == 'end':
                cur = None
        for plant in G.PLANTS:
            if len(found[plant]) >= 2 or page in taken:
                continue
            at = G.plant_index(ops, plant)
            if at is None or test_of[at] is None:
                continue
            if plant == 'negate-cmp':
                # a comparison that only guards an assertion (`if (a === b) { assert... }`, the iftrue/iffalse of a script that tests a value before it asserts on it) is skipped
                # when negated and the test still passes: it is not a plant the verdict must show
                reg = ops[at][1]
                if any(o[0] in ('iftrue', 'iffalse') and o[1] == 'r:' + reg for o in ops):
                    continue
            idx = test_of[at]
            ch, ff = e['chromium']['tests'][idx][1], e['firefox']['tests'][idx][1]
            if ch != ff:
                continue
            if plant == 'drop-assert':
                n = sum(1 for o, t in zip(ops, test_of) if t == idx and o[0].startswith('assert_'))
                if ch != 1 or n != 1:
                    continue
            elif ch != 0:
                continue
            found[plant].append((page, idx))
            taken.add(page)
    return found


def policy_controls():
    """The retry policy against a browser that is made to answer as scripted (no browser, no container): a page that never differs is as recorded; one that differs once and
    answers the record's on the retry is unstable and listed; one that differs twice is a change and stays different; from nothing, two answers that agree are the answer and
    answers that never agree twice in a row are an error; and a host with more than UNSTABLE_LIMIT unstable pages is refused. Returns the number of controls that failed."""
    global ask
    import contextlib
    import io
    real = ask
    X0 = {'status': 0, 'tests': [['t', 0, None]]}
    X1 = {'status': 0, 'tests': [['t', 1, 'assert_equals: expected "1" but got "2"']]}

    def row(page, x):
        return {'page': page, 'status': x['status'], 'message': None, 'tests': [{'name': n, 'status': st, 'message': m} for n, st, m in x['tests']]}

    def scripted(table):
        calls = {}

        def fake(engine, pages, wpt):
            out = []
            for p in pages:
                k = (engine, p)
                seq = table[p]
                out.append(row(p, seq[min(calls.get(k, 0), len(seq) - 1)]))
                calls[k] = calls.get(k, 0) + 1
            return {'browser': 'fake', 'image': 'fake'}, out
        return fake
    failed = 0

    def expect(name, ok):
        nonlocal failed
        print('policy control %s: %s' % (name, 'caught' if ok else 'NOT caught'))
        failed += 0 if ok else 1
    quiet = contextlib.redirect_stderr(io.StringIO())   # the unstable pages these scripted answers make are listed on standard error by the code under test
    quiet.__enter__()
    try:
        pages = ['a', 'b', 'c']
        ref = {e: {'a': entry_of(row('a', X0)), 'b': entry_of(row('b', X0)), 'c': entry_of(row('c', X0))} for e in ENGINES}
        ask = scripted({'a': [X0], 'b': [X1, X0], 'c': [X1, X1]})
        _, got, unstable = settle_engines(pages, '.', 10, ref, jobs=1)
        expect('a flaky page is listed and answers as recorded', set(unstable['chromium']) == {'b'} and got['chromium']['b'] == ref['chromium']['b'])
        expect('a changed page stays changed and is not called unstable', got['chromium']['c'] != ref['chromium']['c'] and 'c' not in unstable['chromium'])
        expect('a page that never differs is not listed', 'a' not in unstable['chromium'])
        ask = scripted({'a': [X0, X0], 'b': [X0, X1, X1], 'c': [X0, X1, X0, X1, X0, X1]})
        _, got, unstable = settle_engines(pages, '.', 10, None, jobs=1)
        expect('from nothing, two answers that agree are the answer', got['chromium']['a'] == entry_of(row('a', X0)) and 'a' not in unstable['chromium'])
        expect('from nothing, a page that settles is listed', got['chromium']['b'] == entry_of(row('b', X1)) and 'b' in unstable['chromium'])
        expect('from nothing, a page that never settles is an error', 'error' in got['chromium']['c'])
        many = ['p%d' % i for i in range(UNSTABLE_LIMIT + 1)]
        ask = scripted({p: [X1, X0] for p in many})
        try:
            settle_engines(many, '.', 100, {e: {p: entry_of(row(p, X0)) for p in many} for e in ENGINES}, jobs=1)
            expect('too many unstable pages refuse the run', False)
        except SystemExit as x:
            expect('too many unstable pages refuse the run', 'too loaded' in str(x))
        # declared flaky pages: only the page, the engine and the answers declared are excused
        global FLAKY
        saved = FLAKY
        FLAKY = {('f', 'chromium'): [entry_of(row('f', X1))]}
        try:
            one = ['f']
            fref = {e: {'f': entry_of(row('f', X0))} for e in ENGINES}
            X2 = {'status': 0, 'tests': [['t', 1, 'assert_equals: expected "1" but got "3"']]}

            def run(table, engine='chromium', reference=fref):
                global ask
                ask = scripted(table)
                _, got, unstable = settle_engines(one, '.', 10, reference, jobs=1)
                return got[engine]['f'], unstable[engine]
            got, unst = run({'f': [X1, X0]})
            expect('a declared page that answers an alternate then the record is unstable and as recorded', got == fref['chromium']['f'] and 'f' in unst)
            got, unst = run({'f': [X1, X1, X1, X0] + [X1] * 40})
            expect('a declared page may answer the alternate most of the time', got == fref['chromium']['f'] and 'f' in unst)
            got, unst = run({'f': [X1]})
            expect('a declared page that never answers the record is a change', got != fref['chromium']['f'] and 'f' not in unst)
            got, unst = run({'f': [X1, X0, X2]})
            expect('a declared page that answers something else once is a change', got != fref['chromium']['f'] and 'f' not in unst)
            got, unst = run({'f': [X2, X0]})
            expect('a declared page whose first answer is something else is a change', got != fref['chromium']['f'] and 'f' not in unst)
            fref_alt = {e: {'f': entry_of(row('f', X2))} for e in ENGINES}
            got, unst = run({'f': [X1, X0]}, reference=fref_alt)
            expect('a record changed to another answer is not excused by the declaration', got != fref_alt['chromium']['f'])
            FLAKY = {('f', 'firefox'): [entry_of(row('f', X1))]}
            ask = scripted({'f': [X1, X1, X0, X0]})
            _, got, unst = settle_engines(one, '.', 10, fref, jobs=1)
            expect('a declaration for the other engine excuses nothing', got['chromium']['f'] != fref['chromium']['f'] and 'f' not in unst['chromium'] and 'f' in unst['firefox'])
            FLAKY = {('f', 'chromium'): [entry_of(row('f', X1))]}
            ask = scripted({'g': [X1, X0]})
            _, got, unst = settle_engines(['g'], '.', 10, {e: {'g': entry_of(row('g', X0))} for e in ENGINES}, jobs=1)
            expect('a declaration excuses its own page only (an undeclared page is retried once)', 'g' in unst['chromium'])
            ask = scripted({'g': [X1, X1]})
            _, got, unst = settle_engines(['g'], '.', 10, {e: {'g': entry_of(row('g', X0))} for e in ENGINES}, jobs=1)
            expect('an undeclared page that answers the alternate twice is a change', got['chromium']['g'] != entry_of(row('g', X0)))
            ask = scripted({'f': [X0, X1, X0, X1] + [X0] * 40})
            _, got, unst = settle_engines(one, '.', 10, None, jobs=1)
            expect('from nothing, a declared page answers the one answer that is not an alternate', got['chromium']['f'] == entry_of(row('f', X0)) and 'f' in unst['chromium'])
            ask = scripted({'f': [X0, X2, X0] + [X0] * 40})
            _, got, unst = settle_engines(one, '.', 10, None, jobs=1)
            expect('from nothing, a declared page with two answers that are not alternates is an error', 'error' in got['chromium']['f'])
        finally:
            FLAKY = saved
    finally:
        ask = real
        quiet.__exit__(None, None, None)
    return failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--record', action='store_true')
    ap.add_argument('--update', action='store_true', help='with --record: ask only for the pages the record at --out does not have or has stale, and keep the rest')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--pages')
    ap.add_argument('--out', default=RECORD)
    ap.add_argument('--record-file', default=RECORD)
    ap.add_argument('--batch', type=int, default=150)
    ap.add_argument('--compare', nargs=2, metavar=('COMMITTED', 'NEW'))
    ap.add_argument('--wpt', default=WPT)
    ap.add_argument('--replay', action='store_true', help='record the replay of the programs in the browsers')
    ap.add_argument('--programs', help='script.tsv of the programs to replay')
    ap.add_argument('--replay-compare', nargs=2, metavar=('COMMITTED', 'NEW'))
    ap.add_argument('--replay-controls', action='store_true')
    ap.add_argument('--policy-controls', action='store_true', help='check the retry policy against scripted answers (no browser)')
    ap.add_argument('--against', help='with --record or --replay: ask once and ask again, alone, only the pages that differ from this committed record (the run that checks it); without it every page is asked twice')
    ap.add_argument('--jobs', type=int, default=2, help='1: the engines one after the other; 2 (default): both at the same time, each in its own container')
    a = ap.parse_args()
    if a.policy_controls:
        return 1 if policy_controls() else 0
    if a.replay_compare:
        h1, e1 = load(a.replay_compare[0])
        h2, e2 = load(a.replay_compare[1])
        out = []
        for key in ('wpt', 'harness', 'driver', 'generator', 'engines'):
            if h1.get(key) != h2.get(key):
                out.append('REPLAY: the header differs in %s: %s against %s' % (key, json.dumps(h1.get(key))[:200], json.dumps(h2.get(key))[:200]))
        for pg in sorted(set(e1) | set(e2)):
            if e1.get(pg) != e2.get(pg):
                out.append('REPLAY: %s is not what the browsers answer now' % pg)
        if out:
            print('\n'.join(out[:60]))
            return 1
        print('replay record: %d pages, the browsers answer as recorded' % len(e1))
        return 0
    if a.replay_controls:
        # mistranslate pages on purpose, replay them in the browsers, and require the comparison with the page's record to refuse every one
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'css'))
        import replay_gen as G
        progs = G.split_programs(open(a.programs, 'rb').read())
        header, page_rec = load(a.record_file)
        found = plant_candidates(progs, page_rec)
        plants = {}
        for plant, items in found.items():
            if not items:
                print('replay control %s: no page to plant it on' % plant, file=sys.stderr)
                return 1
            for page, idx in items:
                plants[page] = plant
        lines = record_replay(a.programs, None, a.batch, plants, sorted(plants), jobs=a.jobs)
        replay = {json.loads(l)['page']: json.loads(l) for l in lines[1:]}
        bad = 0
        for plant, items in found.items():
            for page, idx in items:
                diffs = replay_differences(page, page_rec[page], replay[page], {idx})
                if not diffs:
                    print('replay control %s: the mistranslation of %s#%d was not noticed' % (plant, page, idx), file=sys.stderr)
                    bad += 1
                else:
                    print('control replay-%s: caught (%s#%d: %s)' % (plant, page, idx, diffs[0][:100]))
        # and the unplanted translation of the same pages agrees with the page
        clean = record_replay(a.programs, None, a.batch, {}, sorted(plants), jobs=a.jobs)
        replay = {json.loads(l)['page']: json.loads(l) for l in clean[1:]}
        for plant, items in found.items():
            for page, idx in items:
                diffs = replay_differences(page, page_rec[page], replay[page], {idx})
                if diffs:
                    print('replay control %s: the faithful translation of %s#%d differs from the page: %s' % (plant, page, idx, diffs[0]), file=sys.stderr)
                    bad += 1
        return 1 if bad else 0
    if a.compare:
        h1, e1 = load(a.compare[0])
        h2, e2 = load(a.compare[1])
        out = []
        for key in ('wpt', 'harness', 'driver', 'engines'):
            if h1.get(key) != h2.get(key):
                out.append('RECORD: the header differs in %s: %s against %s' % (key, json.dumps(h1.get(key))[:200], json.dumps(h2.get(key))[:200]))
        for p in sorted(set(e1) | set(e2)):
            if e1.get(p) != e2.get(p):
                out.append('RECORD: %s is not what the browsers answer now' % p)
        if out:
            print('\n'.join(out[:60]))
            return 1
        print('page record: %d pages, the browsers answer as recorded' % len(e1))
        return 0
    if a.replay:
        if a.update and os.path.exists(a.out):
            # ask again only for the programs whose replay is not the record's; the rest of the record stays
            sys.path.insert(0, os.path.join(ROOT, 'tools', 'css'))
            import replay_gen as G
            progs = G.split_programs(open(a.programs, 'rb').read())
            header, entries = load(a.out)
            if header.get('generator') != sha256_file(GENERATOR):
                print('css_page_record: the replay generator changed; replay every program')
                record_replay(a.programs, a.out, a.batch, jobs=a.jobs)
                return 0
            want = [pg for pg in sorted(progs) if pg not in entries or entries[pg]['program'] != progs[pg][0]]
            tmp = a.out + '.new'
            if want:
                record_replay(a.programs, tmp, a.batch, None, want, jobs=a.jobs)
                h2, e2 = load(tmp)
                os.unlink(tmp)
                if any(h2.get(k) != header.get(k) for k in ('wpt', 'harness', 'driver', 'generator', 'engines')):
                    print('css_page_record: the new answers were made under another pin, harness, driver or engine than the record; replay every program')
                    return 1
                entries.update(e2)
            keep = [pg for pg in sorted(entries) if pg in progs]
            with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
                f.write('#' + json.dumps(header, sort_keys=True) + '\n')
                for pg in keep:
                    f.write(json.dumps(entries[pg], sort_keys=True, ensure_ascii=True) + '\n')
            print('css_page_record: %d programs replayed again, %d in the record' % (len(want), len(keep)))
            return 0
        record_replay(a.programs, a.out, a.batch, against=a.against, jobs=a.jobs)
        return 0
    pages = read_pages(a.pages)
    if a.record:
        if a.update and os.path.exists(a.out):
            header, entries = load(a.out)
            want = [p for p in pages if p not in entries or entries[p]['sha'] != sha256_file(os.path.join(a.wpt, p))]
            if want:
                tmp = a.out + '.new'
                record(want, tmp, a.wpt, a.batch, jobs=a.jobs)
                h2, e2 = load(tmp)
                os.unlink(tmp)
                if any(h2.get(k) != header.get(k) for k in ('wpt', 'harness', 'driver', 'engines')):
                    print('css_page_record: the new answers were made under another pin, harness, driver or engine than the record; record every page again')
                    return 1
                entries.update(e2)
            keep = [pg for pg in sorted(entries) if pg in set(pages)]
            with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
                f.write('#' + json.dumps(header, sort_keys=True) + '\n')
                for pg in keep:
                    f.write(json.dumps(entries[pg], sort_keys=True, ensure_ascii=True) + '\n')
            print('css_page_record: %d pages recorded again, %d in the record' % (len(want), len(keep)))
            return 0
        record(pages, a.out, a.wpt, a.batch, a.against, a.jobs)
        return 0
    header, entries = load(a.record_file)
    out = problems(header, entries, pages, a.wpt)
    if out:
        print('\n'.join(out))
        return 1
    print('page record: %d pages, as recorded' % len(pages))
    return 0


if __name__ == '__main__':
    sys.exit(main())
