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
"""Score custom functions against the css-mixins pages of web-platform-tests that are made of templates.

    css_mixins_suite.py <vectors dir> <runner> <exceptions> <defects> [--unextracted <listed> <fresh>] [--drafts DIR]

The vectors are made by tools/css/extract-wpt-mixins.py from the pinned checkout (`templates.json`): each is the whole document a `<template data-name>` test builds (the
page's own markup and style sheets, the template's content in `<div id=main>`). The runner is tools/conformance/css_mixins_suite.c, which builds that document as a consumer of
the library builds one (the Markup tree, the Markup binding, the CSSOM document, the computed style) and answers with two computed values; this program compares them. What a
test says: the page's script asks `getComputedStyle()` of an element for two properties and requires them equal (`--actual` and `--expected` of `#target`, or `list-style-image`
and `width` of `#actual` and `#expected`). Stages: `value` (the two are not equal), `run` (the runner could not build or read the document).

Every failure has to be listed, one line per case, in one of two files. CSS_MIXINS_EXCEPTIONS holds the cases where the page's expectation does not bind: the vector, a summary,
the stage, the answer the library gives, a class (draft-newer, wpt-tentative, needs-environment, wpt-wrong) and the sentence of the served draft that says so, quoted as
`css-mixins-1: "the sentence"`. CSS_MIXINS_DEFECTS holds the cases where the page is right and the library is not: the same columns with an owner and what the library lacks; the
number of defects has a ceiling that only goes down (CSS_MIXINS_MAX_DEFECTS). In both lists the library must give exactly the answer listed. The gate fails on a failure that is in
neither list ("UNEXPLAINED FAILURES"), on a listed case that passes, does not exist or has another stage or answer ("STALE EXCEPTIONS"), on a reason that quotes no sentence, one the
draft does not say (when the draft is given with --drafts: "QUOTE") or a class that is not one of the four ("SUITE PROBLEMS"), on fewer scored cases than the floor
(CSS_MIXINS_MIN_SCORED), and on a page the extractor does not read in full that tools/css/UNEXTRACTED_MIXINS does not list, or lists with another count ("UNEXTRACTED PAGES").
"""

import json
import os
import re
import subprocess
import sys

CLASSES = ('draft-newer', 'wpt-tentative', 'needs-environment', 'wpt-wrong')
STAGES = ('value', 'run')


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


class Runner:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.buf = b''

    def line(self):
        while b'\n' not in self.buf:
            chunk = os.read(self.p.stdout.fileno(), 1 << 16)
            if not chunk:
                print('SUITE PROBLEMS: the runner stopped answering (exit %s)' % self.p.poll())
                sys.exit(1)
            self.buf += chunk
        raw, self.buf = self.buf.split(b'\n', 1)
        return raw.decode('utf-8', 'surrogateescape')

    def ask(self, kind, props, html):
        """('R', first, second) or ('E', what)."""
        self.p.stdin.write(('V\t%s\t%s\t%s\n' % (escape(kind), escape(','.join(props)), escape(html))).encode('utf-8', 'surrogateescape'))
        self.p.stdin.flush()
        f = self.line().split('\t')
        if f[0] == 'R' and len(f) == 3:
            return ('R', unescape(f[1]).strip(), unescape(f[2]).strip())
        return ('E', unescape(f[1]) if len(f) > 1 else 'the runner answered nothing')

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def quotes(reason):
    """The quotations of a reason: [(draft, sentence)] for `css-mixins-1: "sentence"` and ('', sentence) for a bare one."""
    found = []
    for m in re.finditer(r'(?:([a-z][a-z0-9-]*-\d+):\s*)?"([^"]*)"', reason):
        found.append((m.group(1) or '', m.group(2)))
    return found


def quotes_a_sentence(reason):
    return any(len(q) >= 20 and len(q.split()) >= 4 for _, q in quotes(reason))


def draft_text(path):
    """The draft as a page shows it, as far as a quotation needs: markup read, white space collapsed, typographic quotes and dashes made plain."""
    text = open(path, encoding='utf-8').read()
    text = re.sub(r"\[=([^\]=]*?)(?:\|[^\]=]*)?=\]", r'\1', text)
    text = re.sub(r"\[\[!?([A-Za-z0-9-]+)\]\]", r'', text)
    text = re.sub(r"<<([^>]*)>>", r'<\1>', text)
    text = re.sub(r"''([^']*)''", r'\1', text)
    text = re.sub(r"</?[a-zA-Z][^>]*>", ' ', text)
    text = re.sub(r"\{\{([^}]*)\}\}", r'\1', text)
    text = re.sub(r"\|([A-Za-z][^|\n]*?)\|", r'\1', text)   # a variable of an algorithm: |arg value|
    text = text.replace('\u2018', "'").replace('\u2019', "'").replace('\u201c', '"').replace('\u201d', '"').replace('\u2013', '-').replace('\u2014', '-')
    text = re.sub(r"&[a-z]+;", ' ', text)
    return re.sub(r'\s+', ' ', text)


def norm(text):
    text = text.replace('\u2018', "'").replace('\u2019', "'").replace('\u201c', '"').replace('\u201d', '"')
    return re.sub(r'\s+', ' ', text).strip()


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


def score(runner, v):
    """None for a pass, else (stage, answer)."""
    res = runner.ask(v['kind'], v['props'], v['html'])
    if res[0] == 'E':
        return ('run', res[1])
    if res[1] == res[2]:
        return None
    return ('value', 'actual=%s | expected=%s' % (res[1], res[2]))


def main():
    argv = sys.argv[1:]
    unext, drafts = None, None
    pos = []
    i = 0
    while i < len(argv):
        if argv[i] == '--unextracted':
            unext = (argv[i + 1], argv[i + 2])
            i += 3
        elif argv[i] == '--drafts':
            drafts = argv[i + 1]
            i += 2
        else:
            pos.append(argv[i])
            i += 1
    vec_dir, runner_path, exceptions_path, defects_path = pos[:4]
    vectors = json.load(open(os.path.join(vec_dir, 'templates.json'), encoding='utf-8'))

    problems = []
    exceptions, p = load_table(exceptions_path, 6, 'not <id> TAB <summary> TAB <stage> TAB <answer> TAB <class> TAB <reason>')
    problems += p
    texts = {}

    def draft(name):
        if name not in texts:
            path = os.path.join(drafts, name + '.bs') if drafts else None
            texts[name] = norm(draft_text(path)) if path and os.path.exists(path) else None
        return texts[name]

    quote_bad = []
    for ident, f in exceptions.items():
        if f[2] not in STAGES:
            problems.append('%s: stage %r is not one of %s' % (ident, f[2], ', '.join(STAGES)))
        if f[4] not in CLASSES:
            problems.append('%s: class %r is not one of %s' % (ident, f[4], ', '.join(CLASSES)))
        if f[4] == 'wpt-tentative':
            if 'tentative' not in ident:
                problems.append('%s: the class is wpt-tentative and the page is not named tentative' % ident)
        elif not quotes_a_sentence(f[5]):
            problems.append('%s: the reason quotes no sentence of the draft (a quotation of four words and twenty characters at least)' % ident)
        elif drafts:
            for name, sentence in quotes(f[5]):
                if len(sentence) < 20 or len(sentence.split()) < 4:
                    continue
                if not name:
                    problems.append('%s: a quotation names no draft (write css-mixins-1: "the sentence")' % ident)
                    continue
                text = draft(name)
                if text is None:
                    problems.append('%s: the draft %s is not in %s' % (ident, name, drafts))
                elif norm(sentence) not in text:
                    quote_bad.append('%s: %s does not say "%s"' % (ident, name, sentence[:100]))
    defects, p = load_table(defects_path, 6, 'not <id> TAB <summary> TAB <stage> TAB <answer> TAB <owner> TAB <what the library lacks>')
    problems += p
    for ident, f in defects.items():
        if f[2] not in STAGES:
            problems.append('%s: stage %r is not one of %s' % (ident, f[2], ', '.join(STAGES)))
        if ident in exceptions:
            problems.append('%s: both an exception and a defect' % ident)
        if not f[4].strip() or len(f[5].strip()) < 20:
            problems.append('%s: a defect names its owner and says what the library lacks (twenty characters at least)' % ident)

    runner = Runner(runner_path)
    scored, failures, seen = [], {}, set()
    for v in vectors:
        ident = '%s#%d' % (v['file'], v['n'])
        if ident in seen:
            problems.append('%s: two vectors share the identity' % ident)
        seen.add(ident)
        scored.append(ident)
        res = score(runner, v)
        if res:
            failures[ident] = (res[0], res[1], v)
    runner.close()

    unexplained = {i: f for i, f in failures.items() if i not in exceptions and i not in defects}
    stale = []
    for what, table in (('exception', exceptions), ('defect', defects)):
        for ident, f in table.items():
            if ident not in failures:
                stale.append('%s: the case passes or does not exist (%s)' % (ident, what))
                continue
            stage, answer = failures[ident][0], failures[ident][1]
            if f[2] != stage:
                stale.append('%s: the case fails at %s, the %s says %s' % (ident, stage, what, f[2]))
            elif unescape(f[3]) != answer:
                stale.append('%s: the library answers %r, the %s says %r' % (ident, answer, what, unescape(f[3])))

    unext_bad = []
    if unext:
        def read_unext(path):
            out = {}
            for line in open(path, encoding='utf-8'):
                f = line.rstrip('\n').split('\t')
                if line.startswith('#') or len(f) < 2:
                    continue
                out[f[0]] = int(f[1])
            return out
        want, got = read_unext(unext[0]), read_unext(unext[1])
        for f in sorted(set(got) - set(want)):
            unext_bad.append('%s: not read (%d) and the list has no line for it' % (f, got[f]))
        for f in sorted(set(want) - set(got)):
            unext_bad.append('%s: listed (%d) and the extractor reads it in full now (stale)' % (f, want[f]))
        for f in sorted(set(want) & set(got)):
            if want[f] != got[f]:
                unext_bad.append('%s: the extractor leaves %d unread and the list says %d' % (f, got[f], want[f]))

    bad = False
    if problems:
        print('SUITE PROBLEMS:')
        for q in problems[:40]:
            print('  ' + q)
        bad = True
    if quote_bad:
        print('QUOTE: %d sentences the draft does not say' % len(quote_bad))
        for q in quote_bad[:40]:
            print('  ' + q)
        bad = True
    if unexplained:
        print('UNEXPLAINED FAILURES: %d' % len(unexplained))
        for ident, (stage, answer, v) in list(unexplained.items())[:(10**9 if os.environ.get('CSS_MIXINS_LIST_ALL') else 60)]:
            print('  %s  %s: %s' % (ident, stage, answer[:300]))
        bad = True
    if unext_bad:
        print('UNEXTRACTED PAGES: %d differ from tools/css/UNEXTRACTED_MIXINS' % len(unext_bad))
        for q in unext_bad[:40]:
            print('  ' + q)
        bad = True
    if stale:
        print('STALE EXCEPTIONS: %d' % len(stale))
        for s in stale[:40]:
            print('  ' + s)
        bad = True
    floor = int(os.environ.get('CSS_MIXINS_MIN_SCORED', '0'))
    if len(scored) < floor:
        print('scored %d cases, below the floor of %d' % (len(scored), floor))
        bad = True
    ceiling = int(os.environ.get('CSS_MIXINS_MAX_DEFECTS', '1000000'))
    if len(defects) > ceiling:
        print('%d defects listed, above the ceiling of %d (the ceiling only goes down)' % (len(defects), ceiling))
        bad = True
    if os.environ.get('CSS_MIXINS_DUMP'):
        json.dump({i: [f[0], f[1]] for i, f in failures.items()}, open(os.environ['CSS_MIXINS_DUMP'], 'w'))
    pages = len({v['file'] for v in vectors})
    print('css mixins: %d scored (%d pages), %d passed, %d listed exceptions, %d listed DEFECTS of the library' % (
        len(scored), pages, len(scored) - len(failures), sum(1 for i in failures if i in exceptions), sum(1 for i in failures if i in defects)))
    sys.exit(1 if bad else 0)


main()
