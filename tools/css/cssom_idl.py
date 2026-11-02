#!/usr/bin/env python3
"""A small reader of the WebIDL of the CSSOM family (C5e), enough to list the members of the interfaces the ledger covers.

Not a WebIDL parser: it strips comments, finds the top-level definitions (``interface``, ``partial interface``, ``interface mixin``, ``namespace``, ``dictionary``,
``callback interface``), splits each body into members at the semicolons that are outside brackets and parentheses, and names each member by its kind and identifier.
``includes`` statements are returned too, since a mixin's members belong to the interfaces that include it.

Copyright 2026 by Corey Pennycuff
"""
import re

DEF = re.compile(r'\b(partial\s+)?(callback\s+)?(interface\s+mixin|interface|namespace|dictionary)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([A-Za-z_][A-Za-z0-9_]*))?\s*\{')
INCLUDES = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\s+includes\s+([A-Za-z_][A-Za-z0-9_]*)\s*;')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def match_brace(text, open_at):
    depth = 0
    for i in range(open_at, len(text)):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i
    raise ValueError('unbalanced braces')


def split_members(body):
    out = []
    depth = 0
    cur = []
    for c in body:
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ';' and depth == 0:
            s = ' '.join(''.join(cur).split())
            if s:
                out.append(s)
            cur = []
        else:
            cur.append(c)
    s = ' '.join(''.join(cur).split())
    if s:
        out.append(s)
    return out


def strip_ext_attrs(s):
    # leading [ExtendedAttributes]
    while s.startswith('['):
        depth = 0
        for i, c in enumerate(s):
            if c == '[':
                depth += 1
            elif c == ']':
                depth -= 1
                if depth == 0:
                    s = s[i + 1:].strip()
                    break
    return s


def member(s, dictionary=False):
    """(kind, name, text) of one member declaration."""
    s = strip_ext_attrs(s)
    if dictionary:
        s = re.sub(r'^required\s+', '', s)
        m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*.*)?$', s)
        return 'key', m.group(1), s
    if s.startswith('const '):
        m = re.match(r'const\s+.*?\s+([A-Za-z_][A-Za-z0-9_]*)\s*=', s)
        return 'const', m.group(1), s
    if re.search(r'\battribute\b', s):
        m = re.search(r'attribute\s+.*?([A-Za-z_][A-Za-z0-9_]*)$', s)
        return 'attribute', m.group(1), s
    if s.startswith('constructor'):
        return 'constructor', 'constructor', s
    if s.startswith('stringifier'):
        return 'stringifier', 'stringifier', s
    for word in ('iterable', 'maplike', 'setlike', 'async iterable'):
        if s.startswith(word):
            return word.replace(' ', '-'), word.replace(' ', '-'), s
    if '(' in s:
        head = s.split('(')[0].split()
        special = None
        while head and head[0] in ('getter', 'setter', 'deleter', 'static', 'stringifier', 'legacycaller', 'inherit', 'readonly'):
            if head[0] in ('getter', 'setter', 'deleter'):
                special = head[0]
            head = head[1:]
        unnamed = len(head) == 1 or (len(head) == 2 and head[0] == 'unsigned')
        if special and unnamed:
            return special, special, s
        return 'operation', head[-1], s
    # dictionary member
    m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*.*)?$', s)
    return 'key', m.group(1), s


def read(text):
    """({(partial, kind, name): [(memberkind, membername, text)...]}, [(a, b)...] includes, {name: parent})"""
    text = strip_comments(text)
    defs = []
    includes = [(a, b) for a, b in INCLUDES.findall(text)]
    inherit = {}
    pos = 0
    while True:
        m = DEF.search(text, pos)
        if not m:
            break
        close = match_brace(text, m.end() - 1)
        body = text[m.end():close]
        kind = m.group(3).replace('interface mixin', 'mixin')
        name = m.group(4)
        if m.group(5):
            inherit[name] = m.group(5)
        members = [member(x, kind == 'dictionary') for x in split_members(body)]
        defs.append((bool(m.group(1)), kind, name, members))
        pos = close + 1
    return defs, includes, inherit
