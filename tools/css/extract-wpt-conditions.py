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


def media_blocks(css):
    """(prelude, body, offset) of each top-level @media rule of a style sheet's text."""
    out = []
    i = 0
    while True:
        mm = re.compile(r'@media', re.I).search(css, i)
        if not mm:
            return out
        j = mm.start()
        k = css.find('{', j)
        if k < 0:
            return out
        depth = 1
        m = k + 1
        while m < len(css) and depth:
            if css[m] == '{':
                depth += 1
            elif css[m] == '}':
                depth -= 1
            m += 1
        out.append((css[j + 6:k].strip(), css[k + 1:m - 1], j))
        i = m


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
                css = sm.group(1)
                blocks = []
                rest = css
                for prelude, body, off in media_blocks(css):
                    colors = re.findall(r'background(?:-color)?\s*:\s*(green|red|lime)\b', body)
                    colors = ['green' if c in ('green', 'lime') else c for c in colors]
                    if colors and prelude:
                        blocks.append([prelude, colors[-1], line_of(text, sm.start(1) + off)])
                    pass
                if '@custom-media' in css:
                    unextracted.append((rel, 1, 'the page defines @custom-media rules in its style sheet (scored with the sheet: the definitions are collected from rules, C4c)'))
                    break
                if blocks:
                    basec = re.findall(r'background(?:-color)?\s*:\s*(green|red|lime)\b', re.sub(r'@media[^{]*\{.*?\}\s*\}', '', css, flags=re.S))
                    vectors.append({'file': rel, 'line': blocks[0][2], 'kind': 'reftest', 'query': '(reftest)', 'blocks': blocks,
                                    'base': ('green' if basec[-1] in ('green', 'lime') else 'red') if basec else None, 'expected': 'green'})
        n_js = len(JSCALL.findall(text))
        if n_js:
            unextracted.append((rel, n_js, 'the calls are inside loops and template strings of a script (needs a JavaScript engine; scored by tools/conformance/run-css-conditions-js.sh)'))
        elif len(vectors) == n0:
            unextracted.append((rel, 1, 'no statically readable assertion (a script that computes its queries, a stylesheet or a visual state of the page)'))
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'media.json'), 'w', encoding='utf-8') as f:
        json.dump(vectors, f)
    with open(os.path.join(outdir, 'UNEXTRACTED_CONDITIONS'), 'w', encoding='utf-8') as f:
        for rel, n, why in unextracted:
            f.write('%s\t%d\t%s\n' % (rel, n, why))
    print('%d vectors, %d pages not read' % (len(vectors), len(unextracted)))


if __name__ == '__main__':
    main()
