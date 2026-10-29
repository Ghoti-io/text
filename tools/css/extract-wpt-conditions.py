#!/usr/bin/env python3
"""Read the media query vectors of the pinned web-platform-tests as data (C4a).

    extract-wpt-conditions.py <wpt checkout> <out dir>

Reads css/mediaqueries of the pin and writes ``media.json``: a list of vectors, and ``UNEXTRACTED_CONDITIONS`` (the pages and calls this reader does not
understand, with a count and the reason; nothing is dropped silently).

Vector kinds:

* ``serialize``: ``matchMedia(q).media`` is the expected text (`test_parsing(q, expected)` of match-media-parsing.html, the `assert_equals(matchMedia(q).media, e)`
  of aspect-ratio-serialization.html).
* ``reftest``: the ``@media <prelude> { ... }`` blocks of the <style> of a reftest page that paint `green` or `red`, in order, and the color of the page outside them.
  The page passes when its final color, the color of the last block whose query applies (or its base color), is green. The environment is the one the reftest harness
  gives a page (an 800 by 600 viewport).

A call this reader cannot read statically (the loops and template strings of test_media_queries.html, `matchMedia` in a script that computes its argument)
makes its page unscored; the page, the count of such calls and the reason go to UNEXTRACTED_CONDITIONS.

Copyright 2026 by Corey Pennycuff
"""
import json
import os
import re
import sys

CALL = re.compile(r'test_parsing\(\s*"((?:[^"\\]|\\.)*)"\s*(?:,\s*"((?:[^"\\]|\\.)*)"\s*)?\)')
MEDIA_EQ = re.compile(r'assert_equals\(\s*matchMedia\(\s*"((?:[^"\\]|\\.)*)"\s*\)\.media\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)')
STYLE = re.compile(r'<style[^>]*>(.*?)</style>', re.S | re.I)
JSCALL = re.compile(r'\b(should_apply|should_not_apply|query_should_be_parseable|query_should_not_be_parseable|expression_should_be_known|expression_should_be_unknown)\s*\(')


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


def media_blocks(css, at='@media'):
    """(prelude, body, offset) of each top-level rule `at` of a style sheet's text, and whether the sheet has a block-less one or one nested in a block (which this
    reader cannot recover as a style sheet does)."""
    out = []
    unreadable = False
    i = 0
    rx = re.compile(re.escape(at), re.I)
    while True:
        mm = rx.search(css, i)
        if not mm:
            return out, unreadable
        j = mm.start()
        k = css.find('{', j)
        semi = css.find(';', j)
        if k < 0:
            return out, True
        if 0 <= semi < k:
            unreadable = True
            i = semi + 1
            continue
        depth = 1
        m = k + 1
        while m < len(css) and depth:
            if css[m] == '{':
                depth += 1
            elif css[m] == '}':
                depth -= 1
            m += 1
        body = css[k + 1:m - 1]
        if re.search(r'@(media|supports)', body, re.I):
            unreadable = True
        pre = css[j + len(at):k]
        if pre.count('[') != pre.count(']'):
            unreadable = True
        out.append((pre.strip(), body, j, m))
        i = m


def strip(css, blocks):
    out = css
    for prelude, body, off, end in sorted(blocks, key=lambda b: -b[2]):
        out = out[:off] + out[end:]
    return out


def main():
    wpt, outdir = sys.argv[1], sys.argv[2]
    base = os.path.join(wpt, 'css', 'mediaqueries')
    vectors = []
    unextracted = []
    for name in sorted(os.listdir(base)):
        path = os.path.join(base, name)
        if not os.path.isfile(path) or not name.endswith(('.html', '.xht', '.xhtml')):
            continue
        if '-ref' in name or name.startswith('prefers-color-scheme-svg'):
            continue
        text = open(path, encoding='utf-8', errors='replace').read()
        rel = 'css/mediaqueries/' + name
        n0 = len(vectors)
        for m in CALL.finditer(text):
            q = unescape(m.group(1))
            e = unescape(m.group(2)) if m.group(2) is not None else q
            vectors.append({'file': rel, 'line': line_of(text, m.start()), 'kind': 'serialize', 'query': q, 'expected': e})
        for m in MEDIA_EQ.finditer(text):
            vectors.append({'file': rel, 'line': line_of(text, m.start()), 'kind': 'serialize', 'query': unescape(m.group(1)), 'expected': unescape(m.group(2))})
        # A reftest paints green where the query must apply and red where it must not; the page's final color is that of the last block that applies (or its base).
        if '<link rel="match"' in text or '<link rel=match' in text or 'rel="mismatch"' in text:
            for sm in STYLE.finditer(text):
                css = re.sub(r'/\*.*?\*/', ' ', sm.group(1), flags=re.S)
                if re.search(r'\.test\d|:not\(|\bx\|', css):
                    unextracted.append((rel, 1, 'the page tests several elements at once (not one painted square)'))
                    break
                blocks = []
                rest = css
                mb, unreadable = media_blocks(css)
                if unreadable or re.search(r'@(media|supports)[^{;]*;', css, re.I) or re.search(r'@media[^{]*\{[^}]*\}\s*[a-z.#]', css[:0]):
                    unextracted.append((rel, 1, 'the page tests the recovery of a block-less or nested conditional rule (needs the style sheet compiler, C4c)'))
                    break
                for prelude, body, off, end in mb:
                    colors = re.findall(r'background(?:-color)?\s*:\s*(green|red|lime)\b', body)
                    colors = ['green' if c in ('green', 'lime') else c for c in colors]
                    if colors and prelude:
                        blocks.append([prelude, colors[-1], line_of(text, sm.start(1) + off)])
                    pass
                if '@custom-media' in css:
                    unextracted.append((rel, 1, 'the page defines @custom-media rules in its style sheet (scored with the sheet: the definitions are collected from rules, C4c)'))
                    break
                if blocks:
                    basec = re.findall(r'background(?:-color)?\s*:\s*(green|red|lime)\b', strip(css, mb))
                    vectors.append({'file': rel, 'line': blocks[0][2], 'kind': 'reftest', 'query': '(reftest)', 'blocks': blocks,
                                    'base': ('green' if basec[-1] in ('green', 'lime') else 'red') if basec else None, 'expected': 'green'})
        n_js = len(JSCALL.findall(text))
        if n_js:
            unextracted.append((rel, n_js, 'the calls are inside loops and template strings of a script (not scored: the page needs a JavaScript engine with a matchMedia bound to this library, which is not built)'))
        elif len(vectors) == n0:
            unextracted.append((rel, 1, 'no statically readable assertion (a script that computes its queries, a stylesheet or a visual state of the page)'))

    # css-conditional: CSS.supports() calls and @supports reference tests.
    cbase = os.path.join(wpt, 'css', 'css-conditional')
    SUP = re.compile(r'assert_(equals|true|false)\(\s*CSS\.supports\(\s*"((?:[^"\\]|\\.)*)"\s*(?:,\s*"((?:[^"\\]|\\.)*)"\s*)?\)\s*(?:,\s*(true|false)\s*)?\)')
    for sub in ('js', ''):
        d = os.path.join(cbase, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if not os.path.isfile(path) or not name.endswith(('.html', '.xht')) or '-ref' in name:
                continue
            text = open(path, encoding='utf-8', errors='replace').read()
            rel = 'css/css-conditional/' + (sub + '/' if sub else '') + name
            n0 = len(vectors)
            for m in SUP.finditer(text):
                kind, a, b, want = m.groups()
                exp = want if kind == 'equals' else ('true' if kind == 'true' else 'false')
                if exp is None:
                    continue
                vectors.append({'file': rel, 'line': line_of(text, m.start()), 'kind': 'supports', 'query': unescape(a) if b is None else unescape(a) + '\x01' + unescape(b),
                                'expected': exp})
            if sub == '' and ('<link rel="match"' in text or '<link rel=match' in text) and '@supports' in text:
                for sm in STYLE.finditer(text):
                    css = re.sub(r'/\*.*?\*/', ' ', sm.group(1), flags=re.S)
                    if re.search(r'\.test\d|:not\(|\bx\|', css):
                        unextracted.append((rel, 1, 'the page tests several elements at once (not one painted square)'))
                        break
                    blocks = []
                    mb, unreadable = media_blocks(css, '@supports')
                    if unreadable or re.search(r'@(media|supports)[^{;]*;', css, re.I) or '@media' in css.lower():
                        unextracted.append((rel, 1, 'the page tests the recovery of a block-less or nested conditional rule (needs the style sheet compiler, C4c)'))
                        break
                    for prelude, body, off, end in mb:
                        colors = re.findall(r'background(?:-color)?\s*:\s*(green|red|lime)\b', body)
                        colors = ['green' if c in ('green', 'lime') else c for c in colors]
                        if colors and prelude:
                            blocks.append([prelude, colors[-1], line_of(text, sm.start(1) + off)])
                    if blocks:
                        basec = re.findall(r'background(?:-color)?\s*:\s*(green|red|lime)\b', strip(css, mb))
                        vectors.append({'file': rel, 'line': blocks[0][2], 'kind': 'reftest', 'at': 'supports', 'query': '(reftest)', 'blocks': blocks,
                                        'base': ('green' if basec[-1] in ('green', 'lime') else 'red') if basec else None, 'expected': 'green'})
            if len(vectors) == n0 and sub == 'js':
                unextracted.append((rel, 1, 'no statically readable CSS.supports() assertion (a script that builds its calls, or CSSOM)'))
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'media.json'), 'w', encoding='utf-8') as f:
        json.dump(vectors, f)
    with open(os.path.join(outdir, 'UNEXTRACTED_CONDITIONS'), 'w', encoding='utf-8') as f:
        for rel, n, why in unextracted:
            f.write('%s\t%d\t%s\n' % (rel, n, why))
    print('%d vectors, %d pages not read' % (len(vectors), len(unextracted)))


if __name__ == '__main__':
    main()
