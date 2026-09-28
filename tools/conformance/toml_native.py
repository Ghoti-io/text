"""The tagged-JSON and native-value comparisons two TOML gates both need.

`toml_test_suite.py` scores this library against toml-test; `toml_diff.py`
compares it against a pinned `tomllib` over generated documents. Both have to
turn a TOML reading into comparable Python values and compare two of them, and
both have to know that TOML tells an integer from a float, that `Z` and `+00:00`
are one offset while `-00:00` is not, and that NaN equals NaN while -0.0 does
not equal 0.0.

That is one set of rules, so it lives in one file. It was written for the first
gate and copied nowhere: the second gate is the reason it moved here rather than
being written again, because a second copy of a comparison drifts and the drift
shows up as a disagreement about the thing being measured.

Copyright 2026 by Corey Pennycuff
"""

import datetime  # noqa: F401 - actual_native() reads its types
import math
import re

def normalise_datetime(text):
    """A canonical form for comparing two spellings of one date-time.

    Case of the T and Z separators is not significant, a fraction of zeroes is
    not a different time from no fraction, and `Z` and `+00:00` are the same
    offset. `-00:00` is deliberately NOT folded into them: TOML gives it the
    meaning "offset unknown", so folding it would make this comparison unable
    to see a parser that lost the distinction.
    """
    text = text.strip().upper().replace(' ', 'T')
    if text.endswith('Z'):
        text = text[:-1] + '+00:00'
    frac = re.search(r'\.(\d+)', text)
    if frac:
        trimmed = frac.group(1).rstrip('0')
        text = text[:frac.start()] + (('.' + trimmed) if trimmed else '') \
            + text[frac.end():]
    return text


def same_float(a, b):
    """Two doubles, with NaN equal to NaN and -0.0 unequal to 0.0."""
    if math.isnan(a) and math.isnan(b):
        return True
    if a != b:
        return False
    if a == 0.0:
        return math.copysign(1.0, a) == math.copysign(1.0, b)
    return True


def expected_native(tagged, where='$'):
    """The suite's tagged JSON as the Python values tomllib would produce."""
    if isinstance(tagged, dict) and 'type' in tagged and 'value' in tagged:
        kind, text = tagged['type'], tagged['value']
        if kind == 'string':
            return text
        if kind == 'integer':
            return int(text)
        if kind == 'float':
            return float(text)
        if kind == 'bool':
            return text == 'true'
        if kind in ('datetime', 'datetime-local', 'date-local', 'time-local'):
            return ('date-time', normalise_datetime(text))
        raise ValueError('%s: unknown tag %r' % (where, kind))
    if isinstance(tagged, dict):
        return dict((k, expected_native(v, '%s.%s' % (where, k)))
                    for k, v in tagged.items())
    if isinstance(tagged, list):
        return [expected_native(v, '%s[%d]' % (where, i))
                for i, v in enumerate(tagged)]
    raise ValueError('%s: the suite encodes no bare %s' % (where,
                                                           type(tagged)))


def actual_native(value):
    """What tomllib returned, in the same shape as expected_native()."""
    if isinstance(value, dict):
        return dict((k, actual_native(v)) for k, v in value.items())
    if isinstance(value, list):
        return [actual_native(v) for v in value]
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time)):
        return ('date-time', normalise_datetime(value.isoformat()))
    return value


def same_native(mine, theirs, where='$'):
    """Compare two native trees. Types are checked, not just values: TOML
    tells an integer from a float, so a writer that spelled 1.0 as `1` has
    changed the document even though Python would call the two equal."""
    if isinstance(theirs, tuple):
        if not isinstance(mine, tuple):
            return '%s: expected a date-time, got %r' % (where, mine)
        if mine[1] != theirs[1]:
            return '%s: %s, expected %s' % (where, mine[1], theirs[1])
        return None
    if isinstance(theirs, bool) or isinstance(mine, bool):
        if type(mine) is not type(theirs) or mine != theirs:
            return '%s: %r, expected %r' % (where, mine, theirs)
        return None
    if isinstance(theirs, float):
        if not isinstance(mine, float):
            return '%s: %r is not a float' % (where, mine)
        if not same_float(mine, theirs):
            return '%s: %r, expected %r' % (where, mine, theirs)
        return None
    if isinstance(theirs, int):
        if not isinstance(mine, int):
            return '%s: %r is not an integer' % (where, mine)
        if mine != theirs:
            return '%s: %r, expected %r' % (where, mine, theirs)
        return None
    if isinstance(theirs, str):
        if not isinstance(mine, str) or mine != theirs:
            return '%s: %r, expected %r' % (where, mine, theirs)
        return None
    if isinstance(theirs, dict):
        if not isinstance(mine, dict):
            return '%s: expected a table, got %r' % (where, mine)
        for key in theirs:
            if key not in mine:
                return '%s: missing key %r' % (where, key)
        for key in mine:
            if key not in theirs:
                return '%s: unexpected key %r' % (where, key)
        for key in theirs:
            bad = same_native(mine[key], theirs[key],
                              '%s.%s' % (where, key))
            if bad:
                return bad
        return None
    if isinstance(theirs, list):
        if not isinstance(mine, list):
            return '%s: expected an array, got %r' % (where, mine)
        if len(mine) != len(theirs):
            return '%s: %d items, expected %d' % (where, len(mine),
                                                  len(theirs))
        for i, (m, t) in enumerate(zip(mine, theirs)):
            bad = same_native(m, t, '%s[%d]' % (where, i))
            if bad:
                return bad
        return None
    return '%s: cannot compare %r' % (where, theirs)
