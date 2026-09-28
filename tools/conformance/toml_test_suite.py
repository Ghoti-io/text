"""Score this library's TOML parser against toml-test.

Usage: toml_test_suite.py <suite-dir> <runner-command...>

The runner reads one document on stdin; exit 0 with tagged JSON on stdout
means accepted, non-zero means refused. Which cases are asked comes from the
suite's own `tests/files-toml-1.0.0` manifest rather than from a directory
walk: the two lists differ, and the difference is exactly the ten cases where
1.0.0 and the 1.1.0 draft disagree. Walking the tree would silently score this
1.0.0 parser against 1.1.0 expectations for those, which is the shape of
mistake that reads as a parser defect.

Comparison of a valid case is by value and not by text. The suite tags every
scalar - {"type": "integer", "value": "42"} - and two spellings of one value
are the same value: 1e3 and 1000.0, +0 and 0, 07:32:00.000 and 07:32:00. A
textual comparison would report those as failures and bury the real ones.

Copyright 2026 by Corey Pennycuff
"""
import json
import math
import os
import re
import subprocess
import sys

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


def run(path):
    with open(os.path.join(SUITE, 'tests', path), 'rb') as fh:
        data = fh.read()
    try:
        done = subprocess.run(RUNNER, input=data, capture_output=True,
                              timeout=30)
    except subprocess.TimeoutExpired:
        return None, 'timed out'
    if done.returncode == 0:
        return done.stdout.decode('utf-8', 'replace'), None
    detail = done.stderr.decode('utf-8', 'replace').strip().split('\n')
    return None, (detail[-1] if detail else 'exit %d' % done.returncode)


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
            fa, fb = float(a), float(b)
            if math.isnan(fa) and math.isnan(fb):
                return None
            if fa != fb:
                return '%s: %r, expected %r' % (where, fa, fb)
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


accepted_ok = []
valid_failures = []
for path in valid:
    expectation = os.path.join(SUITE, 'tests', path[:-5] + '.json')
    if not os.path.exists(expectation):
        valid_failures.append((path, 'no .json expectation in the suite'))
        continue
    out, why = run(path)
    if out is None:
        valid_failures.append((path, 'refused: %s' % why))
        continue
    try:
        mine = json.loads(out)
    except ValueError as exc:
        valid_failures.append((path, 'output is not JSON: %s' % exc))
        continue
    with open(expectation) as fh:
        theirs = json.load(fh)
    bad = same(mine, theirs)
    if bad:
        valid_failures.append((path, bad))
    else:
        accepted_ok.append(path)

invalid_failures = []
for path in invalid:
    out, _ = run(path)
    if out is not None:
        invalid_failures.append((path, 'accepted, and should not have been'))

asked = len(valid) + len(invalid)
passed = len(accepted_ok) + (len(invalid) - len(invalid_failures))

print("=== toml-test, TOML %s ===" % VERSION)
print("  the manifest lists %d valid and %d invalid cases"
      % (len(valid), len(invalid)))
print("  asked %d of them, which is all: this parser needs no case skipped"
      % asked)
print("\nvalid    %d of %d" % (len(accepted_ok), len(valid)))
print("invalid  %d of %d refused"
      % (len(invalid) - len(invalid_failures), len(invalid)))
print("total    %d of %d  (%.1f%%)" % (passed, asked, 100.0 * passed / asked))

if valid_failures:
    print("\n%d valid cases wrong:" % len(valid_failures))
    for path, why in valid_failures:
        print("  %-46s %s" % (path[:46], why[:110]))
if invalid_failures:
    print("\n%d invalid cases accepted:" % len(invalid_failures))
    for path, why in invalid_failures:
        print("  %s" % path)

report = os.environ.get('TOML_REPORT')
if report:
    with open(report, 'w') as fh:
        for path, why in valid_failures:
            fh.write("valid   %-50s %s\n" % (path, why))
        for path, why in invalid_failures:
            fh.write("invalid %-50s %s\n" % (path, why))
    print("\nfailures written to %s" % report)

floor = os.environ.get('TOML_MIN')
if floor:
    pct = 100.0 * passed / asked
    if pct + 0.05 < float(floor):
        print("conformance: %.1f%% is below the floor of %s%%" % (pct, floor))
        sys.exit(1)
    print("conformance: %.1f%% meets the floor of %s%%" % (pct, floor))
