"""Score this library's TOML reader and writer against toml-test.

Usage: toml_test_suite.py <suite-dir> <runner-command...>

Several scores, all from the one corpus:

  decode           TOML in, tagged JSON out, compared to the case's .json.
  roundtrip        TOML in, written back out, read again, and the *second*
                   read compared to the same .json - once per table style, so
                   the style option is measured rather than asserted.
  encode           the case's .json in, TOML out, and that TOML read by
                   **tomllib** rather than by this library. This is the only
                   score with a second implementation in it: everything else
                   here proves the two halves agree with each other.
  events           the document rebuilt from gtext_toml_read_events() alone,
                   compared to the same .json. The rebuild is in the runner and
                   not in the library, so this is a second consumer of the
                   format rather than the parser agreeing with itself, and it
                   is the only measurement of whether the event stream carries
                   everything the tree does.
  comments         every case read with comments retained, written, and read
                   again, requiring the same comments both times. Asked only of
                   the cases that have a comment the tree keeps, and the header
                   prints how many comments the corpus holds against how many
                   of those a tree can hold - the gap is the ones only the
                   event stream reports.
  via json         every valid case out through gtext_toml_to_json() and back
                   through gtext_json_to_toml(), compared to the same .json
                   transformed by the two losses a JSON round trip makes - a
                   date-time becomes a string, and a float JSON writes without a
                   point comes back an integer. A case holding a non-finite
                   float must be *refused*, since JSON cannot write one.
  crossed          each version arm run over the cases the *other* manifest
                   decides, and required to get them wrong. See below.

Which cases are asked comes from the suite's own `tests/files-toml-1.0.0`
manifest rather than from a directory walk: the two lists differ, and the
difference is exactly the cases where 1.0.0 and the 1.1.0 draft disagree.
Walking the tree would silently score this 1.0.0 parser against 1.1.0
expectations for those, which is the shape of mistake that reads as a parser
defect. TOML_SUITE_VERSION picks the manifest and is passed to the runner as
--version=, so the arm and the expectations always come from the same place.

The crossed mode exists because running a gate once per option setting is not a
measurement of the option: each arm is only ever asked the cases its own
manifest decides, and 33 of this mode's 66 runs are cases no manifest asks
anybody. Its worth was measured, not argued - of eight defects planted in the
version option, the two manifests' ordinary rows caught five and this mode
caught six, and one of its six was seen by nothing else here: a lone carriage
return admitted at 1.0.0, whose only two cases sit in the 1.1.0 invalid list
and in neither 1.0.0 list. (Two further defects were invisible to the corpus
altogether and live in tests/test-toml-version.cpp.)

The four populations are computed from the two manifests rather than listed by
hand: a hand-copied copy of the subject's own list is two spellings of one set,
and the copy is the one that goes stale.

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

# The tagged-JSON and native-value comparisons, shared with the tomllib
# differential rather than written twice. See tools/conformance/toml_native.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toml_native import (actual_native, expected_native,  # noqa: E402
                         normalise_datetime, same_float, same_native)

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

ARM = ['--version=%s' % VERSION]


def read_manifest(version):
    path = os.path.join(SUITE, 'tests', 'files-toml-%s' % version)
    if not os.path.exists(path):
        sys.exit("no manifest %s; the crossed mode needs both" % path)
    with open(path) as fh:
        return set(line.strip() for line in fh if line.strip())


LIST_1_0 = read_manifest('1.0.0')
LIST_1_1 = read_manifest('1.1.0')


def cross_sets():
    """The four populations the version switch actually separates.

    Derived from the two manifests, and each one returned with the expectation
    it carries rather than as a bare list, so a case cannot be in a population
    without something being asserted about it under both arms.

    `spec-1.0.0/` and `spec-1.1.0/` are the one place a difference between the
    lists is not a difference between the versions: they are extracts of two
    prose documents, and the second renumbered every heading, so all sixteen
    files appear in exactly one list while being about rules neither version
    touched. They are therefore not excluded but given their own population,
    with the expectation that both arms refuse all sixteen - which is a claim
    that fails loudly if a later revision does move one of those rules.
    """
    def only_in(source, other, prefix):
        return sorted(p for p in source
                      if p.endswith('.toml') and p.startswith(prefix)
                      and p not in other
                      and not p.startswith('invalid/spec-')
                      and not p.startswith('valid/spec-'))

    # In 1.0.0's invalid list and in neither 1.1.0 list: 1.1.0 made these legal
    # and the suite dropped them rather than writing an expectation, so the
    # assertion is acceptance and not a value.
    relaxed = only_in(LIST_1_0, LIST_1_1, 'invalid/')
    # In 1.1.0's valid list and in neither 1.0.0 list: syntax 1.0.0 refuses.
    added = only_in(LIST_1_1, LIST_1_0, 'valid/')
    # In 1.1.0's invalid list and in neither 1.0.0 list: 1.1.0 settled a
    # question 1.0.0 left contradictory (a lone CR inside a multi-line string,
    # which 1.0.0's prose permits and its ABNF forbids). This module takes the
    # ABNF's reading under both arms, so both must refuse.
    tightened = only_in(LIST_1_1, LIST_1_0, 'invalid/')
    restated = sorted(p for p in (LIST_1_0 | LIST_1_1)
                      if p.endswith('.toml') and p.startswith('invalid/spec-'))
    return relaxed, added, tightened, restated


def score_cross():
    """Run each arm over what the other manifest decides."""
    relaxed, added, tightened, restated = cross_sets()
    if not relaxed or not added or not tightened or not restated:
        sys.exit("the crossed mode found %d relaxed, %d added, %d tightened "
                 "and %d restated cases; an empty population agrees with "
                 "everything" % (len(relaxed), len(added), len(tightened),
                                 len(restated)))
    # (population, at 1.0.0, at 1.1.0) where True means "must be accepted".
    plan = [('relaxed', relaxed, False, True),
            ('added', added, False, True),
            ('tightened', tightened, False, False),
            ('restated', restated, False, False)]
    failures = []
    passed = 0
    asked = 0
    for label, paths, want_1_0, want_1_1 in plan:
        for path in paths:
            data = read_case(path)
            for version, want in (('1.0.0', want_1_0), ('1.1.0', want_1_1)):
                asked += 1
                out, why, code = run(data, ['--version=%s' % version])
                got = out is not None
                if code == 3:
                    failures.append((path, '%s at %s: exit 3, the runner '
                                           'failing rather than a verdict: %s'
                                     % (label, version, why)))
                elif got != want:
                    failures.append(
                        (path, '%s at %s: %s, expected it to be %s'
                         % (label, version, 'accepted' if got else 'refused',
                            'accepted' if want else 'refused')))
                else:
                    passed += 1
    return asked, passed, failures


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


def same(mine, theirs, where='$'):
    """Compare two decoded tagged-JSON trees, returning None or a complaint."""
    if isinstance(theirs, dict) and 'type' in theirs and 'value' in theirs:
        if not (isinstance(mine, dict) and 'type' in mine):
            return '%s: expected a %s, got %r' % (where, theirs['type'], mine)
        if theirs['type'] == 'string-datetime':
            pass
        elif mine['type'] != theirs['type']:
            return '%s: type %s, expected %s' % (where, mine['type'],
                                                 theirs['type'])
        kind = theirs['type']
        a, b = mine['value'], theirs['value']
        if kind == 'string-datetime':
            # A date-time that a conversion turned into a string. The claim is
            # that the *instant* survived, so the comparison is the date-time
            # one: the string is whichever spelling chron writes, which is not
            # always the spelling the case was written in.
            if mine['type'] != 'string':
                return '%s: %s, expected a string' % (where, mine['type'])
            if normalise_datetime(a) != normalise_datetime(b):
                return '%s: %s, expected the instant %s' % (where, a, b)
            return None
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
# The modes
#
# The encoder direction's reader is `tomllib`, and the conversions it needs -
# tagged JSON to native values, and one native tree against another - are in
# toml_native.py, imported above and shared with the tomllib differential.
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


DATETIME_TAGS = ('datetime', 'datetime-local', 'date-local', 'time-local')


def via_json_expect(node):
    """The suite's expectation as a round trip through JSON returns it.

    Two documented losses, and both are computed from the expectation rather
    than from anything this library printed:

      - a date-time becomes a string, so the tag becomes `string-datetime` and
        the comparison asks whether the instant survived;
      - a float whose shortest lossless spelling carries no point and no
        exponent - which is what JSON writes, since JSON has one number type -
        comes back as an integer.

    A non-finite float means the whole case must be *refused*, because the
    default nonfinite policy refuses and JSON has no spelling for one. That is
    returned as the first element rather than skipping the case: a conversion
    that quietly emitted `null` there would otherwise score as a pass.

    Encoding the two losses here is deliberate, and it is not the same mistake
    as copying the subject's own list: a scorer that *skipped* these cases would
    say nothing about them, where this says exactly what each must become, so a
    change in either rule moves the score.
    """
    if isinstance(node, dict) and 'type' in node and 'value' in node:
        kind, text = node['type'], node['value']
        if kind in DATETIME_TAGS:
            return False, {'type': 'string-datetime', 'value': text}
        if kind == 'float':
            value = float(text)
            if math.isnan(value) or math.isinf(value):
                return True, node
            spelled = '%.17g' % value
            if '.' not in spelled and 'e' not in spelled and 'E' not in spelled:
                return False, {'type': 'integer', 'value': spelled}
            return False, node
        return False, node
    if isinstance(node, dict):
        refuse = False
        out = {}
        for key, value in node.items():
            bad, out[key] = via_json_expect(value)
            refuse = refuse or bad
        return refuse, out
    if isinstance(node, list):
        refuse = False
        out = []
        for value in node:
            bad, converted = via_json_expect(value)
            out.append(converted)
            refuse = refuse or bad
        return refuse, out
    return False, node


def score_via_json(extra):
    """Every valid case out to a JSON tree and back, scored against the
    expectation the two documented losses transform it into.

    Every case is asked. The alternative - excluding the ones a round trip
    cannot return - would leave the two losses unmeasured, which is the half of
    this claim most likely to be wrong.
    """
    failures = []
    passed = 0
    asked = 0
    refusals = 0
    for path in valid:
        expectation = expectation_of(path)
        if not os.path.exists(expectation):
            continue
        asked += 1
        with open(expectation) as fh:
            theirs = json.load(fh)
        refuse, expected = via_json_expect(theirs)
        out, why, code = run(read_case(path), extra)
        if refuse:
            refusals += 1
            if out is not None:
                failures.append((path, 'converted a non-finite float, which '
                                       'JSON cannot hold'))
            elif code != 1:
                failures.append((path, 'exit %d, expected the refusal: %s'
                                 % (code, why)))
            else:
                passed += 1
            continue
        if out is None:
            failures.append((path, 'refused (exit %d): %s' % (code, why)))
            continue
        try:
            mine = json.loads(out.decode('utf-8'))
        except ValueError as exc:
            failures.append((path, 'output is not JSON: %s' % exc))
            continue
        bad = same(mine, expected)
        if bad:
            failures.append((path, bad))
        else:
            passed += 1
    VIA_JSON_COUNTS['refusals'] = refusals
    return asked, passed, failures


VIA_JSON_COUNTS = {}


def score_comments(extra):
    """Read with comments retained, write, read again, compare.

    A round trip, and blind in the one way a round trip is: a comment dropped on
    the way in and a comment dropped on the way out agree with each other.
    tests/test-toml-comments.cpp is where each half is looked at separately;
    this is what says the pair holds over 208 real files.

    Two things are asked of each case, and the first is the one that makes the
    rule a measurement: the number of comment lines the tree kept must be
    exactly the number the event stream reported outside any container. The
    second is the round trip.

    Only cases that have a comment at all are asked. Counting the rest
    as passes would make this mode's score a measurement of how many TOML files
    have no comments in them - and by the same argument the number of comments
    the corpus contains is printed rather than left implicit, because the
    difference between that and the number the trees held is the population this
    mode structurally cannot say anything about.
    """
    failures = []
    passed = 0
    asked = 0
    in_events = 0
    in_tree = 0
    for path in valid:
        out, why, code = run(read_case(path), extra)
        if out is None:
            failures.append((path, 'refused (exit %d): %s' % (code, why)))
            continue
        lines = out.decode('utf-8', 'replace').split('\n')
        if not lines or not lines[0].startswith('events '):
            failures.append((path, 'no comment listing: %r' % lines[:1]))
            continue
        words = lines[0].split()
        in_events += int(words[1])
        inside = int(words[2])
        try:
            cut = lines.index('first')
            mid = lines.index('second')
        except ValueError:
            failures.append((path, 'the listing has no two halves'))
            continue
        first = sorted(x for x in lines[cut + 1:mid] if x)
        second = sorted(x for x in lines[mid + 1:] if x)
        in_tree += len(first)
        # The rule the tree follows, asked of every case that has a comment at
        # all: it keeps the ones attached to statements, which is the ones the
        # stream reported at depth zero. A case failing this is either a comment
        # lost where one could have been kept, or one kept that the writer will
        # not be able to place.
        if first or inside:
            asked += 1
            if len(first) != int(words[1]) - inside:
                failures.append((path, '%d comment lines kept, %d arrived '
                                       'outside a value'
                                 % (len(first), int(words[1]) - inside)))
                continue
            passed += 1
        if not first:
            # Nothing kept: nothing for the round trip below to say.
            continue
        asked += 1
        if first != second:
            lost = [x for x in first if x not in second]
            gained = [x for x in second if x not in first]
            failures.append((path, 'lost %r gained %r' % (lost[:3], gained[:3])))
        else:
            passed += 1
    return asked, passed, failures, in_events, in_tree


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


COMMENT_COUNTS = {}


def score_comments_mode():
    asked, passed, failures, in_events, in_tree = score_comments(
        ARM + ['--comments', '--style=as-read'])
    COMMENT_COUNTS['events'] = in_events
    COMMENT_COUNTS['tree'] = in_tree
    return asked, passed, failures


SPELL_COUNTS = {}


def score_spell_reach():
    """Whether GTEXT_TOML_Write_Options::spellings reaches a point of use.

    Each valid case is written twice, with the option and without, and the two
    answers have to be opposites: bytes that differ hold a v1.1.0 spelling and
    so must be refused by the 1.0.0 arm, and bytes that do not differ must be
    accepted by it. A bit that never reached its decision makes them agree on
    every case carrying the construct it was meant to change.

    The count of cases that *do* differ is printed rather than required,
    because it is a property of the corpus and not of the library - but a run
    where nothing differed would be a run that measured nothing, so zero is a
    failure of the mode as a whole.
    """
    failures = []
    passed = 0
    asked = 0
    differing = 0
    for path in valid:
        asked += 1
        out, why, code = run(read_case(path),
                             ARM + ['--spell-check', '--spellings=1.1.0'])
        if out is None:
            failures.append((path, 'refused (exit %d): %s' % (code, why)))
            continue
        lines = out.decode('utf-8').split()
        if len(lines) != 4 or lines[0] != 'differs' \
                or lines[2] != 'reads-at-1.0.0':
            failures.append((path, 'unreadable verdict %r' % out[:60]))
            continue
        differs = lines[1] == 'yes'
        reads = lines[3] == 'yes'
        if differs:
            differing += 1
        if differs == reads:
            failures.append((path, 'the spellings option %s the bytes and the '
                                   '1.0.0 arm %s them: those cannot both be '
                                   'true'
                             % ('changed' if differs else 'left',
                                'refused' if not reads else 'accepted')))
        else:
            passed += 1
    SPELL_COUNTS['differing'] = differing
    # In the denominator, not beside it. The first draft of this mode appended
    # a complaint and returned 218 of 218 anyway, so the gate announced that it
    # had measured nothing and still met the floor - which is the whole shape of
    # a sweep that cannot see returning clean.
    asked += 1
    if differing:
        passed += 1
    else:
        failures.append(('(the corpus)', 'not one case was written differently, '
                                         'so this mode asked nothing'))
    return asked, passed, failures


MODES = [
    ('decode', 'the reader, scored against the suite expectations',
     lambda: score_decode(ARM, True)),
    ('events', 'the document rebuilt from the event stream alone',
     lambda: score_decode(ARM + ['--events'], True)),
    ('comments', 'comments kept through a write and a second read',
     score_comments_mode),
    ('via json', 'out to a JSON tree and back, against the same expectations',
     lambda: score_via_json(ARM + ['--via-json'])),
    ('roundtrip as-read', 'parse, write, parse again; tables as they were read',
     lambda: score_decode(ARM + ['--roundtrip', '--style=as-read'], True)),
    ('roundtrip headers', 'the same, with every table forced to a [header]',
     lambda: score_decode(ARM + ['--roundtrip', '--style=headers'], False)),
    ('roundtrip inline', 'the same, with every table forced inline',
     lambda: score_decode(ARM + ['--roundtrip', '--style=inline'], False)),
    ('encode as-read', 'the writer, with tomllib reading what it wrote',
     lambda: score_encode(ARM + ['--encode', '--style=as-read'])),
    ('encode headers', 'the same, with every table forced to a [header]',
     lambda: score_encode(ARM + ['--encode', '--style=headers'])),
    ('crossed', 'each arm over the cases the other manifest decides',
     score_cross),
]

# Two modes for the write-side spelling option, and only on the 1.1.0 arm:
# output carrying a v1.1.0 spelling is by construction not readable by the
# 1.0.0 one, so read-write-read at 1.0.0 would be scoring the option's whole
# purpose as a failure.
if VERSION == '1.1.0':
    MODES.append(
        ('roundtrip 1.1.0 spellings',
         'parse, write with every 1.1.0 spelling, parse again',
         lambda: score_decode(
             ARM + ['--roundtrip', '--style=as-read', '--spellings=1.1.0'],
             False)))
    MODES.append(
        ('spellings reach',
         'the option changed the bytes exactly where 1.0.0 then refuses them',
         score_spell_reach))

print("=== toml-test, TOML %s ===" % VERSION)
print("  the manifest lists %d valid and %d invalid cases"
      % (len(valid), len(invalid)))
print("  nothing is skipped: every mode below is asked every case it applies "
      "to")
_relaxed, _added, _tightened, _restated = cross_sets()
print("  the two manifests differ over %d cases 1.1.0 relaxed, %d it added, "
      "%d it tightened," % (len(_relaxed), len(_added), len(_tightened)))
print("  and %d that differ only because the spec extracts were renumbered"
      % len(_restated))

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

print("\nvia json: %d of the %d valid cases hold a non-finite float, where the "
      "expectation is a refusal" % (VIA_JSON_COUNTS.get('refusals', 0),
                                    len(valid)))
if VERSION == '1.1.0':
    print("\nspellings: %d of the %d valid cases are written differently when "
          "every 1.1.0 spelling is" % (SPELL_COUNTS.get('differing', 0),
                                       len(valid)))
    print("  allowed; each of those is then refused by the 1.0.0 arm, and each "
          "of the rest is not.")
    print("  That count is the mode's denominator in disguise: a zero would "
          "mean the corpus holds none")
    print("  of the constructs the option changes, and it is scored as a "
          "failure rather than reported.")

print("\ncomments: the %d valid cases contain %d comment lines, of which a tree "
      "keeps %d." % (len(valid), COMMENT_COUNTS.get('events', 0),
                     COMMENT_COUNTS.get('tree', 0)))
print("  The rest are inside a value, where there is no statement to attach "
      "them to and no line in")
print("  the document the writer produces to put them on; "
      "gtext_toml_read_events() reports those.")

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
