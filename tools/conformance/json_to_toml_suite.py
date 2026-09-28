"""Score gtext_json_to_toml() over JSONTestSuite's documents.

Usage: json_to_toml_suite.py <suite-dir> <runner>

**Why a corpus at all.** The `via json` mode of toml_test_suite.py sends TOML
out to JSON and back, so every document it converts came from TOML: there is no
`null` in it, no integer wider than `int64_t`, and no root that is not a table.
Those are exactly the three places TOML is narrower than JSON, and they were
reached by unit tests and by nothing with a population behind it.
JSONTestSuite's `test_parsing` files are that population, and they are already
cloned and pinned by run-json.sh.

The oracle is Python. For each document it walks the values in document order
and names the first obstacle the conversion should meet; the runner says which
one it met. Predicting the *first* one rather than the set is the point - the
conversion walks in document order too, so a document with a `null` before a
too-large integer has one right answer and not two.

Four modes, each over every case it applies to:

  outcome        the documented refusal, or none, for every document
  round trip     JSON -> TOML -> text -> TOML -> JSON, equal by value
  null policy    NULL_SKIP drops a member and still refuses inside an array
  table root     a root that is not a table is refused, and one that is is not

There is no floor to set. The oracle names one right answer per case, so a
single disagreement fails the run - a percentage would be a way of agreeing to
some of them. JTT_REPORT names a file to write any to.

Copyright 2026 by Corey Pennycuff
"""
import glob
import json
import os
import subprocess
import sys

if len(sys.argv) < 3:
    sys.exit(__doc__.strip().splitlines()[2])
SUITE, RUNNER = sys.argv[1], sys.argv[2]

FILES = sorted(glob.glob(os.path.join(SUITE, 'test_parsing', 'y_*.json'))
               + glob.glob(os.path.join(SUITE, 'test_parsing', 'i_*.json')))
if not FILES:
    sys.exit("no test_parsing files under %s" % SUITE)

INT64_MIN, INT64_MAX = -(2 ** 63), 2 ** 63 - 1


def pairs(items):
    """An object, last occurrence winning, the way --dupkeys=last asks for."""
    out = {}
    for key, value in items:
        out[key] = value
    return out


# "Python would not read it", which is not None: `null` is a document, and
# JSONTestSuite has it as y_structure_lonely_null.json. Reading one as the other
# dropped exactly that file from the population - and it is the corpus's only
# `null` that the wrapping turns into an object *member* rather than an array
# element, so the half of the null-policy mode that drops one had nothing in it.
OPAQUE = object()


def load(path):
    """The document as Python values, or OPAQUE if Python will not read it."""
    with open(path, 'rb') as handle:
        raw = handle.read()
    try:
        return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs), raw
    except (UnicodeDecodeError, ValueError):
        return OPAQUE, raw


# --- the oracle ------------------------------------------------------------
#
# What gtext_json_to_toml() should do with this document, as the first obstacle
# in document order. `None` means it converts.
#
# The rules are the ones toml_json.h states, and they are short enough to state
# here rather than to infer: a `null` is refused unless the policy drops it and
# it is a member rather than an element; an integer outside int64_t is E_RANGE;
# a root that is not a table is E_UNREPRESENTABLE. A number's type is its
# spelling, which is why the walk needs the text and not only the value - and
# json.loads throws the spelling away, so integers are recognised by Python's
# own int/float split, which agrees with it: json.loads gives an int exactly
# when the lexeme has no `.`, `e` or `E`.


def obstacle(value, null_skip, in_array=False):
    if value is None:
        if null_skip and not in_array:
            return None
        return 'E_UNREPRESENTABLE'
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        if value < INT64_MIN or value > INT64_MAX:
            return 'E_RANGE'
        return None
    if isinstance(value, list):
        for item in value:
            found = obstacle(item, null_skip, True)
            if found:
                return found
        return None
    if isinstance(value, dict):
        for item in value.values():
            found = obstacle(item, null_skip, False)
            if found:
                return found
        return None
    return None


def has_null(value):
    """Is there a `null` anywhere in this document?"""
    if value is None:
        return True
    if isinstance(value, list):
        return any(has_null(item) for item in value)
    if isinstance(value, dict):
        return any(has_null(item) for item in value.values())
    return False


def nonfinite(value):
    """Does the document hold a float no JSON writer will spell?

    An infinity is a perfectly good TOML float and gtext_toml_to_json() refuses
    it by default, so the round trip stops at the last step rather than the
    first. That is the documented `nonfinite` policy and not a failure of the
    conversion, so it is predicted rather than counted against it.
    """
    if isinstance(value, float):
        return value != value or value in (float('inf'), float('-inf'))
    if isinstance(value, list):
        return any(nonfinite(item) for item in value)
    if isinstance(value, dict):
        return any(nonfinite(item) for item in value.values())
    return False


def same(a, b):
    """Equal as documents, with the two narrowings this path states.

    A number's TOML type is its JSON spelling, and JSON has one number type, so
    `20e1` goes out as a float and comes back as `200`: compared as numbers
    rather than as an int and a float. Everything else is compared exactly.
    """
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return float(a) == float(b)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if type(a) is not type(b):
        return False
    return a == b


# --- running the runner ----------------------------------------------------

def unescape(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == '\\' and i + 1 < len(text):
            out.append({'t': '\t', 'n': '\n', 'r': '\r',
                        '\\': '\\'}.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def run(flags):
    """path -> (stage, code, payload)."""
    out = subprocess.run([RUNNER] + flags + FILES, capture_output=True,
                         timeout=600)
    if out.returncode != 0:
        sys.exit("runner failed: %s" % out.stderr.decode('utf-8', 'replace'))
    records = {}
    # split('\n') rather than splitlines(): Python breaks lines at eleven more
    # characters than a newline - U+2028 and U+0085 among them - and
    # JSONTestSuite has strings holding several of those. The runner escapes the
    # newline it uses as a terminator, so splitting on that one is exact.
    for line in out.stdout.decode('utf-8', 'replace').split('\n'):
        if not line:
            continue
        parts = line.split('\t')
        if len(parts) < 4:
            sys.exit("runner wrote a record with %d fields: %r"
                     % (len(parts), line))
        records[parts[0]] = (parts[1], parts[2], unescape(parts[3]))
    missing = [p for p in FILES if p not in records]
    if missing:
        sys.exit("the runner said nothing about %d files, e.g. %s"
                 % (len(missing), missing[0]))
    return records


DOCS = {}  # path -> the document as Python values, or OPAQUE
for path in FILES:
    value, _raw = load(path)
    DOCS[path] = value

failures = []


def score(name, blurb, rows):
    """rows: iterable of (path, ok, why). Returns (asked, passed)."""
    asked = passed = 0
    for path, ok, why in rows:
        asked += 1
        if ok:
            passed += 1
        else:
            failures.append((name, os.path.basename(path), why))
    rate = 100.0 * passed / asked if asked else 0.0
    print("  %-14s %4d / %-4d %5.1f%%  %s"
          % (name, passed, asked, rate, blurb))
    return asked, passed


print("=== gtext_json_to_toml() over JSONTestSuite ===")
print("  %d documents: every y_ and i_ file of test_parsing" % len(FILES))

# Mode 0: which documents this library's JSON parser reads at all. Not a score -
# a document it refuses is not an input to the conversion - but it is printed and
# checked against the reason, so that a document silently leaving the population
# is visible.
wrapped = run(['--wrap', '--roundtrip', '--dupkeys=last'])
unread = [p for p in FILES if wrapped[p][0] == 'json']
print("  %d of them this library's JSON parser refuses, so they are not inputs "
      "to a conversion at all; run-json.sh is where that is scored"
      % len(unread))
reached = [p for p in FILES if p not in unread]
# Python has to be able to read what the oracle is asked about. It is looser
# than this library in places - a lone surrogate, for one - so the two sets are
# not the same, and a document only one of them reads is named rather than
# quietly dropped.
opaque = [p for p in reached if DOCS[p] is OPAQUE]
if opaque:
    print("  %d this library reads and Python will not, so they have no oracle: "
          "%s" % (len(opaque),
                  ', '.join(sorted(os.path.basename(p) for p in opaque))))
    reached = [p for p in reached if p not in opaque]

# Mode: outcome.
rows = []
for path in reached:
    want = obstacle({'wrapped': DOCS[path]}, False)
    stage, code, _payload = wrapped[path]
    if want is None:
        ok = stage in ('ok', 'to-json')
        why = 'expected the conversion to succeed, got %s %s' % (stage, code)
    else:
        ok = stage == 'to-toml' and code == want
        why = 'expected to-toml %s, got %s %s' % (want, stage, code)
    rows.append((path, ok, why))
score('outcome', 'the documented refusal, or none', rows)

# Mode: round trip.
rows = []
for path in reached:
    want = obstacle({'wrapped': DOCS[path]}, False)
    if want is not None:
        continue
    doc = {'wrapped': DOCS[path]}
    stage, code, payload = wrapped[path]
    if nonfinite(doc):
        ok = stage == 'to-json' and code == 'E_UNREPRESENTABLE'
        why = ('a non-finite float: expected to-json E_UNREPRESENTABLE, '
               'got %s %s' % (stage, code))
    elif stage != 'ok':
        ok, why = False, 'stopped at %s %s' % (stage, code)
    else:
        try:
            back = json.loads(payload, object_pairs_hook=pairs)
        except ValueError as exc:
            ok, why = False, 'what came back is not JSON: %s' % exc
        else:
            ok = same(doc, back)
            why = 'came back different: %r' % (payload[:160],)
    rows.append((path, ok, why))
score('round trip', 'out to TOML text and back, equal by value', rows)

# Mode: the null policy.
skipped = run(['--wrap', '--dupkeys=last', '--null-skip'])
rows = []
dropped = refused = 0
for path in reached:
    # Only the documents that hold one. A mode whose denominator is the whole
    # corpus would be 100% on the strength of the cases it does not concern.
    if not has_null(DOCS[path]):
        continue
    want = obstacle({'wrapped': DOCS[path]}, True)
    stage, code, _payload = skipped[path]
    if want is None:
        dropped += 1
        ok = stage == 'ok'
        why = 'NULL_SKIP should have dropped it, got %s %s' % (stage, code)
    else:
        refused += 1
        ok = stage == 'to-toml' and code == want
        why = 'expected to-toml %s under NULL_SKIP, got %s %s' % (
            want, stage, code)
    rows.append((path, ok, why))
score('null policy', 'NULL_SKIP drops a member, never an element', rows)
# Both halves, in the denominator rather than beside it. The option has two
# behaviours and a run that saw only one of them has measured half a policy -
# and would say 100% for it.
if dropped == 0 or refused == 0:
    sys.exit("FAIL: the null-policy mode saw %d member `null`s and %d inside an "
             "array; it needs at least one of each to have measured the policy "
             "rather than one side of it" % (dropped, refused))

# Mode: the table root.
unwrapped = run(['--roundtrip', '--dupkeys=last'])
rows = []
for path in reached:
    doc = DOCS[path]
    if isinstance(doc, dict):
        want = obstacle(doc, False)
        stage, code, _payload = unwrapped[path]
        ok = (stage in ('ok', 'to-json') if want is None
              else (stage == 'to-toml' and code == want))
        why = 'a table root should convert, got %s %s' % (stage, code)
    else:
        stage, code, _payload = unwrapped[path]
        ok = stage == 'to-toml' and code == 'E_UNREPRESENTABLE'
        why = ('a root that is not a table must be E_UNREPRESENTABLE, got %s %s'
               % (stage, code))
    rows.append((path, ok, why))
score('table root', 'v1.0.0: a TOML document is a table', rows)

if failures:
    print("\n  disagreements:")
    for mode, name, why in failures[:40]:
        print("    %-12s %-44s %s" % (mode, name, why))
    if len(failures) > 40:
        print("    ... and %d more" % (len(failures) - 40))
if os.environ.get('JTT_REPORT'):
    with open(os.environ['JTT_REPORT'], 'w') as fh:
        for mode, name, why in failures:
            fh.write('%s\t%s\t%s\n' % (mode, name, why))
    print("  disagreements written to %s" % os.environ['JTT_REPORT'])

print("\nSuite commit: %s" % os.environ.get('JTS_COMMIT', '(unpinned)'))
if failures:
    sys.exit("FAIL: %d disagreements" % len(failures))
print("\033[0;32mEvery mode agreed with the oracle on every case.\033[0m")
