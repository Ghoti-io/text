"""The recorded answers of the two pinned browsers (tools/conformance/css_browser_answers.jsonl, made by tools/oracle/css_browser_record.py) as the conformance
gates of the computed and the property layer read them: a `served-text` or `browsers-differ` exception names the recorded case of both engines, and the gate looks it up.
"""

import json
import os
import re

BROWSER_RECORD = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'css_browser_answers.jsonl')

# the specified values the engines wrote for a case's declarations: id -> engine -> tuple of the specified texts (the specified-value gate's browsers-differ is about these: `specified=True`)
SPECIFIED = {}


def load_record():
    """The ids of the recorded browser answers (css_browser_answers.jsonl) that have an answer of both engines: id -> set of engines."""
    have = {}
    if not os.path.exists(BROWSER_RECORD):
        return have
    for line in open(BROWSER_RECORD, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        r = json.loads(line)
        have[r['id']] = {e: a.get('computed', {}) for e, a in r['engines'].items() if a.get('computed') or a.get('declarations')}
        have['style:' + r['style']] = have[r['id']]
        SPECIFIED[r['id']] = {e: tuple(x.get('specified', '') for _, x in sorted(a.get('declarations', {}).items())) for e, a in r['engines'].items()}
        SPECIFIED['style:' + r['style']] = SPECIFIED[r['id']]
    return have


def record_problems(ident, reason, record, prop=None, differ=False, value=None, specified=False):
    """A `served-text` or `wpt-tentative` exception quotes the sentence (checked as for the other classes) and names the recorded answers of both browsers: `[record: *]` is this vector's
    own case in the record (CSS_COMPUTED_DEFECTS:<vector>, CSS_PROPERTY_DEFECTS:<vector> or, once the line is an exception, CSS_COMPUTED_EXCEPTIONS:<vector> or CSS_PROPERTY_EXCEPTIONS:<vector>), or ids of hand cases are listed after `record:` separated by semicolons."""
    m = re.search(r'\[record: ([^\]]+)\]', reason)
    if not m:
        return ['%s: a served-text or wpt-tentative exception names the recorded answers of both browsers as [record: *] or [record: <case id>; ...]' % ident]
    ids = [x.strip() for x in m.group(1).split(';')]
    out = []
    for i in ids:
        if i == '*':
            cands = ['CSS_COMPUTED_DEFECTS:' + ident, 'CSS_PROPERTY_DEFECTS:' + ident, 'CSS_COMPUTED_EXCEPTIONS:' + ident, 'CSS_PROPERTY_EXCEPTIONS:' + ident, 'style:%s: %s' % (prop, value)]
            ok = [c for c in cands if c in record]
            if not ok:
                out.append('%s: the record has no case of this vector' % ident)
                continue
            i = ok[0]
        if i not in record:
            out.append('%s: the record has no case %r' % (ident, i))
        elif set(record[i]) != {'chromium', 'firefox'}:
            out.append('%s: the record case %r lacks an answer of %s' % (ident, i, ', '.join(sorted({'chromium', 'firefox'} - set(record[i])))))
        elif differ and prop is not None and record[i]['chromium'].get(prop) == record[i]['firefox'].get(prop) and not (specified and SPECIFIED.get(i, {}).get('chromium') != SPECIFIED.get(i, {}).get('firefox')):
            out.append('%s: the class is browsers-differ and the record case %r has the same answer in both browsers for %s' % (ident, i, prop))
    return out
