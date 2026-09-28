"""Score this library's TOML reader and writer against toml-test.

Usage: toml_test_suite.py <suite-dir> <runner-command...>

Four scores, all from the one corpus:

  decode           TOML in, tagged JSON out, compared to the case's .json.
  roundtrip        TOML in, written back out, read again, and the *second*
                   read compared to the same .json - once per table style, so
                   the style option is measured rather than asserted.
  encode           the case's .json in, TOML out, and that TOML read by
                   **tomllib** rather than by this library. This is the only
                   score with a second implementation in it: everything else
                   here proves the two halves agree with each other.

Which cases are asked comes from the suite's own `tests/files-toml-1.0.0`
manifest rather than from a directory walk: the two lists differ, and the
difference is exactly the cases where 1.0.0 and the 1.1.0 draft disagree.
Walking the tree would silently score this 1.0.0 parser against 1.1.0
expectations for those, which is the shape of mistake that reads as a parser
defect.

Comparison of a valid case is by value and not by text. The suite tags every
scalar - {"type": "integer", "value": "42"} - and two spellings of one value
are the same value: 1e3 and 1000.0, +0 and 0, 07:32:00.000 and 07:32:00. A
textual comparison would report those as failures and bury the real ones. The
one thing that looks like a spelling and is not is the sign of a zero, which
the corpus distinguishes ("-0" against "0" in valid/float/zero.json), so the
float comparison checks it.

Exit status 3 from the runner is never a verdict about a case: it means the
runner could not write, or could not read back, its own output. An invalid case
producing it is a failure, not the refusal it was hoping for.

Copyright 2026 by Corey Pennycuff
"""
import datetime
import json
import math
import os
import re
import subprocess
import sys

try:
    import tomllib
except ImportError:  # pragma: no cover - the floor below depends on it
    sys.exit("no tomllib; the encoder direction has no second decoder without "
             "it, and scoring the writer only against this library's own "
             "reader is the measurement this mode exists to avoid. "
             "Python 3.11 or newer is required.")

SUITE = sys.argv[1]
RUNNER = sys.argv[2:]
if not RUNNER:
    sys.exit("usage: toml_test_suite.py <suite-dir> <runner-command...>")

VERSION = os.environ.get('TOML_SUITE_VERSION', '1.0.0')
manifest = os.path.join(SUITE, 'tests', 'files-toml-%s' % VERSION)
if not os.path.exists(manifest):
    sys.exit("no manifest %s; is the suite checked out at the pinned commit?"
             % manifest)

with open(manifest) as fh:
    listed = [line.strip() for line in fh if line.strip()]

valid = sorted(p for p in listed
               if p.startswith('valid/') and p.endswith('.toml'))
invalid = sorted(p for p in listed
                 if p.startswith('invalid/') and p.endswith('.toml'))
if not valid or not invalid:
    sys.exit("manifest %s lists %d valid and %d invalid cases; that is not a "
             "corpus" % (manifest, len(valid), len(invalid)))


def run(data, extra=()):
    """Feed `data` to the runner, returning (stdout, complaint, returncode)."""
    try:
        done = subprocess.run(list(RUNNER) + list(extra), input=data,
                              capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None, 'timed out', -1
    detail = done.stderr.decode('utf-8', 'replace').strip().split('\n')
    why = detail[0] if detail and detail[0] else 'exit %d' % done.returncode
    if done.returncode == 0:
        return done.stdout, None, 0
    return None, why, done.returncode


def read_case(path):
    with open(os.path.join(SUITE, 'tests', path), 'rb') as fh:
        return fh.read()


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


def same(mine, theirs, where='$'):
    """Compare two decoded tagged-JSON trees, returning None or a complaint."""
    if isinstance(theirs, dict) and 'type' in theirs and 'value' in theirs:
        if not (isinstance(mine, dict) and 'type' in mine):
            return '%s: expected a %s, got %r' % (where, theirs['type'], mine)
        if mine['type'] != theirs['type']:
            return '%s: type %s, expected %s' % (where, mine['type'],
                                                 theirs['type'])
        kind = theirs['type']
        a, b = mine['value'], theirs['value']
        if kind == 'integer':
            if int(a) != int(b):
                return '%s: %s, expected %s' % (where, a, b)
            return None
        if kind == 'float':
            if not same_float(float(a), float(b)):
                return '%s: %s, expected %s' % (where, a, b)
            return None
        if kind in ('datetime', 'datetime-local', 'date-local', 'time-local'):
            if normalise_datetime(a) != normalise_datetime(b):
                return '%s: %s, expected %s' % (where, a, b)
            return None
        if a != b:
            return '%s: %r, expected %r' % (where, a, b)
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
            bad = same(mine[key], theirs[key], '%s.%s' % (where, key))
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
            bad = same(m, t, '%s[%d]' % (where, i))
            if bad:
                return bad
        return None
    if mine != theirs:
        return '%s: %r, expected %r' % (where, mine, theirs)
    return None


# --------------------------------------------------------------------------
# The encoder direction, where tomllib is the reader
# --------------------------------------------------------------------------

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


# --------------------------------------------------------------------------
# The modes
# --------------------------------------------------------------------------

def expectation_of(path):
    return os.path.join(SUITE, 'tests', path[:-5] + '.json')


def score_decode(extra, want_invalid):
    """TOML in, tagged JSON out. Optionally also require every invalid case to
    be refused - with exit 1 and not exit 3, which would be this runner's own
    output failing rather than the case being refused."""
    failures = []
    passed = 0
    asked = 0
    for path in valid:
        asked += 1
        expectation = expectation_of(path)
        if not os.path.exists(expectation):
            failures.append((path, 'no .json expectation in the suite'))
            continue
        out, why, code = run(read_case(path), extra)
        if out is None:
            failures.append((path, 'refused (exit %d): %s' % (code, why)))
            continue
        try:
            mine = json.loads(out.decode('utf-8'))
        except ValueError as exc:
            failures.append((path, 'output is not JSON: %s' % exc))
            continue
        with open(expectation) as fh:
            theirs = json.load(fh)
        bad = same(mine, theirs)
        if bad:
            failures.append((path, bad))
        else:
            passed += 1
    if want_invalid:
        for path in invalid:
            asked += 1
            out, why, code = run(read_case(path), extra)
            if out is not None:
                failures.append((path, 'accepted, and should not have been'))
            elif code == 3:
                failures.append((path, 'exit 3, which is the runner failing '
                                       'rather than a refusal: %s' % why))
            else:
                passed += 1
    return asked, passed, failures


def score_encode(extra):
    """The case's .json in, TOML out, read back by tomllib."""
    failures = []
    passed = 0
    asked = 0
    for path in valid:
        expectation = expectation_of(path)
        if not os.path.exists(expectation):
            continue
        asked += 1
        with open(expectation, 'rb') as fh:
            tagged_bytes = fh.read()
        out, why, code = run(tagged_bytes, extra)
        if out is None:
            failures.append((path, 'the writer refused (exit %d): %s'
                             % (code, why)))
            continue
        try:
            theirs = expected_native(json.loads(tagged_bytes.decode('utf-8')))
        except ValueError as exc:
            failures.append((path, 'the suite expectation did not convert: %s'
                             % exc))
            continue
        try:
            mine = actual_native(tomllib.loads(out.decode('utf-8')))
        except UnicodeDecodeError as exc:
            failures.append((path, 'the TOML written is not UTF-8: %s' % exc))
            continue
        except tomllib.TOMLDecodeError as exc:
            failures.append((path, 'tomllib refused what was written: %s'
                             % exc))
            continue
        bad = same_native(mine, theirs)
        if bad:
            failures.append((path, bad))
        else:
            passed += 1
    return asked, passed, failures


MODES = [
    ('decode', 'the reader, scored against the suite expectations',
     lambda: score_decode([], True)),
    ('roundtrip as-read', 'parse, write, parse again; tables as they were read',
     lambda: score_decode(['--roundtrip', '--style=as-read'], True)),
    ('roundtrip headers', 'the same, with every table forced to a [header]',
     lambda: score_decode(['--roundtrip', '--style=headers'], False)),
    ('roundtrip inline', 'the same, with every table forced inline',
     lambda: score_decode(['--roundtrip', '--style=inline'], False)),
    ('encode as-read', 'the writer, with tomllib reading what it wrote',
     lambda: score_encode(['--encode', '--style=as-read'])),
    ('encode headers', 'the same, with every table forced to a [header]',
     lambda: score_encode(['--encode', '--style=headers'])),
]

print("=== toml-test, TOML %s ===" % VERSION)
print("  the manifest lists %d valid and %d invalid cases"
      % (len(valid), len(invalid)))
print("  nothing is skipped: every mode below is asked every case it applies "
      "to")

total_asked = 0
total_passed = 0
all_failures = []
for name, blurb, scorer in MODES:
    asked, passed, failures = scorer()
    total_asked += asked
    total_passed += passed
    all_failures.extend((name, path, why) for path, why in failures)
    print("\n%-20s %4d of %4d   %s" % (name, passed, asked, blurb))
    for path, why in failures[:12]:
        print("    %-44s %s" % (path[:44], str(why)[:100]))
    if len(failures) > 12:
        print("    ... and %d more" % (len(failures) - 12))

print("\ntotal    %d of %d  (%.1f%%)"
      % (total_passed, total_asked, 100.0 * total_passed / total_asked))

report = os.environ.get('TOML_REPORT')
if report:
    with open(report, 'w') as fh:
        for name, path, why in all_failures:
            fh.write("%-20s %-50s %s\n" % (name, path, why))
    print("failures written to %s" % report)

floor = os.environ.get('TOML_MIN')
if floor:
    pct = 100.0 * total_passed / total_asked
    if pct + 0.05 < float(floor):
        print("conformance: %.1f%% is below the floor of %s%%" % (pct, floor))
        sys.exit(1)
    print("conformance: %.1f%% meets the floor of %s%%" % (pct, floor))
