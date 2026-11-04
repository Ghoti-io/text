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

Pages are a file of paths, one a line (build/.../script-pages.txt, written by the gate); the browsers see them at /<path> of a loopback server over the checkout.
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


def record(pages, out, wpt, batch):
    import oracle_env
    answers = {e: {} for e in ENGINES}
    headers = {}
    for engine in ENGINES:
        for i in range(0, len(pages), batch):
            part = pages[i:i + batch]
            header, rows = ask(engine, part, wpt)
            headers[engine] = {'browser': header['browser'], 'image': header['image']}
            for r in rows:
                if r['page'] not in part:
                    raise SystemExit('css_page_record: %s answered a page that was not asked: %s' % (engine, r['page']))
                answers[engine][r['page']] = r
            print('css_page_record: %s %d of %d pages' % (engine, min(i + batch, len(pages)), len(pages)), file=sys.stderr)
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
                a = answers[engine][page]
                if 'error' in a:
                    entry[engine] = {'error': a['error']}
                else:
                    entry[engine] = {'status': a['status'], 'tests': [[normalize(t['name']), t['status'], normalize(t['message'])] for t in a['tests']]}
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--record', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--pages')
    ap.add_argument('--out', default=RECORD)
    ap.add_argument('--record-file', default=RECORD)
    ap.add_argument('--batch', type=int, default=150)
    ap.add_argument('--compare', nargs=2, metavar=('COMMITTED', 'NEW'))
    ap.add_argument('--wpt', default=WPT)
    a = ap.parse_args()
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
    pages = read_pages(a.pages)
    if a.record:
        record(pages, a.out, a.wpt, a.batch)
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
