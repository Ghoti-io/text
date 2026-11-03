"""The recorded answers of both browsers for the vectors of tentative pages that are not declarations (selectors, values with no property of their own, `@supports selector()`):
tools/conformance/css_probe_answers.jsonl, written by tools/oracle/css_probe_record.py. A `wpt-tentative` exception of the selector, values and conditions gates names its vector in
`[record: <vector id>]`, and the gate looks the case up and refuses an exception whose case is absent or lacks the answer of an engine.

Copyright 2026 by Corey Pennycuff
"""
import json
import os
import re

RECORD = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'css_probe_answers.jsonl')


def load():
    """id -> {'chromium': answer, 'firefox': answer}."""
    have = {}
    if not os.path.exists(RECORD):
        return have
    for line in open(RECORD, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        r = json.loads(line)
        have[r['id']] = {'chromium': r.get('chromium'), 'firefox': r.get('firefox')}
    return have


def problems(ident, reason, record):
    """The faults of one tentative exception: no `[record: id]`, a record id that is not this vector's, or a case without both engines' answers."""
    m = re.search(r'\[record: ([^\]]+)\]', reason)
    if not m:
        return ['%s: a wpt-tentative exception names the recorded answers of both browsers as [record: <vector id>]' % ident]
    if m.group(1).strip() != ident:
        return ['%s: the record named is %r, which is not this vector' % (ident, m.group(1).strip())]
    case = record.get(ident)
    if case is None:
        return ['%s: the record (tools/conformance/css_probe_answers.jsonl) has no case of this vector' % ident]
    return ['%s: the record case lacks an answer of %s' % (ident, e) for e in ('chromium', 'firefox') if case.get(e) is None]
