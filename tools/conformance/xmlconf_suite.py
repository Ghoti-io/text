"""Score an XML parser against the W3C XML Conformance Test Suite.

Usage: xmlconf_suite.py <xmlconf-dir> <runner> [exceptions-file]

The runner reads a document on stdin and prints one line: `OK`, or
`FAIL <status> <offset> <line> <column> <message>` (xmlconf_suite.c). It is run
three ways on every case: `dom` (gtext_xml_parse()), `whole` (the event parser,
fed the document in one call) and `bytes` (the event parser, one byte per call).
The first is scored as described below. The other two are not scored on their
own: they must give **exactly the DOM parse's line** - the same status, offset,
line, column and message for a refusal, and for an acceptance the same tree
(the runner replays the events and compares every name, value and location). Any
case on which they differ is a failure of the gate, with no exceptions list: the
event parsers claim the DOM parser's verdicts, and a case where they do not is a
defect in one of them.

There is one difference, and it is by construction: gtext_xml_parse() converts
the whole of a UTF-16, UTF-32, ISO-8859-1 or US-ASCII document before it reads
any of it, so it reports a byte that is not valid in the encoding
(GTEXT_XML_E_INVALID_SEQUENCE) even if the document is not well-formed earlier;
the event parser reports whichever comes first. Where the DOM parse says
INVALID_SEQUENCE and the event parser instead refuses the document for a
well-formedness reason at or before that offset, both have refused it and the
case is counted apart ("conversion error ordering"), not failed.

A fourth way, `roundtrip`, is run on every case the parser accepted: the document
is written in UTF-8, UTF-16 (both byte orders), ISO-8859-1 and US-ASCII, and each
output must parse to a tree equal to the original (the XML declaration included),
and writing that tree again must give the same bytes. ISO-8859-1 and US-ASCII
may refuse a document with characters they lack in a name, comment, instruction or
CDATA section (GTEXT_XML_E_UNREPRESENTABLE, counted); every other refusal or
difference fails the gate.

What is scored, by the case's TYPE in the catalogue:

  not-wf    must be REFUSED.
  valid /   are well-formed documents, so must be ACCEPTED - except one the
  invalid   parser refuses for naming an entity (GTEXT_XML_E_ENTITY), which
            needs the DTD that declares it processed and is counted apart.
            Whether they are *valid* is not asked until DTD processing exists;
            the two classes are reported separately and their validity is
            never scored.
  error     the specification leaves it to the processor: reported, unscored.

Every case is assigned to XML 1.0 or XML 1.1 from catalogue metadata and, when
metadata is absent, its XML declaration (an absent declaration means 1.0).
Namespaced cases also identify the applicable Namespaces Recommendation.
Cases intentionally excluded from scoring are listed by ID and reason in the
exclusions manifest; unlisted and stale exclusions fail the gate.

A scored case that fails is a failure of the gate unless it is listed in the
exceptions file as `<id><TAB><profile><TAB><reason>`. An exception that no longer fails is
also a failure: a list that cannot shrink stops meaning anything. The exit
status is the verdict; it is non-zero if any scored case fails unexplained, if
any exception is stale, if the runner crashed on any case, or if fewer than
XMLCONF_MIN_SCORED cases were scored.
"""
import collections
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

from xml_profile import profile_for

SUITE = sys.argv[1]
RUNNER = sys.argv[2]
EXCEPTIONS = sys.argv[3] if len(sys.argv) > 3 else None

XML_BASE = '{http://www.w3.org/XML/1998/namespace}base'


def read_text(path):
    """A catalogue file as text, whatever it declares."""
    with open(path, 'rb') as fh:
        data = fh.read()
    m = re.match(rb'<\?xml[^>]*encoding=["\']([A-Za-z0-9._-]+)["\']', data)
    return data.decode(m.group(1).decode() if m else 'utf-8')


def strip_declaration(text):
    return re.sub(r'^\s*<\?xml[^>]*\?>', '', text)


def load_catalogue():
    """xmlconf.xml with its external entities (one per sub-catalogue) inlined.

    The top-level catalogue is a DOCTYPE with SYSTEM entities and a body that
    references them; no XML library here will read them, and one that did would
    be doing exactly what the parser under test refuses to.
    """
    top = read_text(os.path.join(SUITE, 'xmlconf.xml'))
    entities = dict(re.findall(r'<!ENTITY\s+(\S+)\s+SYSTEM\s+"([^"]+)"', top))
    if not entities:
        sys.exit("xmlconf.xml names no sub-catalogues; is this the suite?")
    body = re.sub(r'<!DOCTYPE.*?\n\]>', '', top, flags=re.S)
    body = re.sub(r'<\?xml-stylesheet[^>]*\?>', '', strip_declaration(body))
    for name, rel in entities.items():
        sub = strip_declaration(read_text(os.path.join(SUITE, rel)))
        sub = re.sub(r'<!DOCTYPE[^>]*>', '', sub)
        # The sub-catalogue's own relative paths are relative to its folder.
        folder = os.path.dirname(rel)
        sub = re.sub(r'(<TESTCASES\b)', r'\1 SUBDIR="%s"' % folder, sub, count=1)
        body = body.replace('&%s;' % name, sub)
    return ET.fromstring(body)


def walk(node, base, subdir, out):
    if node.tag == 'TESTCASES':
        base = os.path.join(base, node.get(XML_BASE, '')) if node.get(XML_BASE) else base
        sub = node.get('SUBDIR')
        if sub:
            subdir = sub
    for child in node:
        if child.tag == 'TESTCASES':
            walk(child, base, subdir, out)
        elif child.tag == 'TEST':
            own = child.get(XML_BASE)
            path = os.path.join(base, own) if own else base
            where = os.path.normpath(os.path.join(SUITE, path, child.get('URI')))
            # The 2013 catalogue gives eduni/misc's cases the base
            # eduni/namespaces/misc/, where there are no files; they are in the
            # sub-catalogue's own folder. Looked for there second, and counted.
            beside = os.path.normpath(os.path.join(SUITE, subdir, child.get('URI')))
            out.append((child, where, beside))


def classify_scope(t):
    """Stable reason a catalogue case is outside this gate's contract."""
    recommendation = t.get('RECOMMENDATION')
    if recommendation and recommendation not in ('XML1.0', 'NS1.0', 'XML1.1', 'NS1.1'):
        return 'unsupported recommendation %s' % recommendation
    if t.get('NAMESPACE') == 'no':
        return 'catalogue requires a non-namespace-aware processor'
    edition = t.get('EDITION')
    if edition is not None and '5' not in edition.split():
        return 'catalogue targets an XML edition before Fifth Edition'
    return None


def run_line(path, mode):
    """The runner's one line for a file in a mode, or (None, why) if it died."""
    with open(path, 'rb') as fh:
        data = fh.read()
    try:
        done = subprocess.run([RUNNER, mode], input=data, capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        return None, ('TIMEOUT', '')
    line = done.stdout.decode('utf-8', 'replace').strip()
    if done.returncode != 0 or not line:
        return None, ('CRASH', 'exit %s %s' % (done.returncode, done.stderr.decode('utf-8', 'replace')[:80]))
    return line, None


def run(path):
    line, died = run_line(path, 'dom')
    if died:
        return died + (None,)
    if line == 'OK':
        return ('OK', '', line)
    parts = line.split(' ', 5)
    if parts[0] != 'FAIL' or len(parts) < 6:
        return ('CRASH', 'unreadable runner output: %r' % line[:80], line)
    return (parts[1], '%s:%s %s' % (parts[3], parts[4], parts[5]), line)


def load_manifest(path, label, fields):
    table = {}
    if path and os.path.exists(path):
        with open(path, encoding='utf-8') as fh:
            for raw in fh:
                raw = raw.rstrip('\n')
                if not raw.strip() or raw.startswith('#'):
                    continue
                parts = raw.split('\t')
                if len(parts) != fields or not all(p.strip() for p in parts):
                    sys.exit("%s entry must have %d nonempty tab-separated fields: %r" % (label, fields, raw))
                ident = parts[0].strip()
                if ident in table:
                    sys.exit("duplicate %s entry %r" % (label, ident))
                table[ident] = tuple(p.strip() for p in parts[1:])
    return table


# A not-well-formed case is refused only if the parser says so for a
# well-formedness reason. A limit, an out-of-memory, a bad argument or a failed
# invariant is a refusal for the wrong reason and must not score as a pass.
WELL_FORMEDNESS = tuple('GTEXT_XML_E_' + n for n in (
    'SYNTAX', 'UNEXPECTED_EOF', 'UTF8', 'CHARACTER', 'MISMATCHED_TAG',
    'DUPLICATE_ATTRIBUTE', 'ENTITY', 'NAMESPACE', 'DOCTYPE',
    'INVALID_SEQUENCE', 'ENCODING_MISMATCH'))

UNSUPPORTED = ('GTEXT_XML_E_UNSUPPORTED_ENCODING', 'GTEXT_XML_E_UNSUPPORTED_VERSION')

# The cases allowed to differ by the conversion-ordering rule, by id. Any other
# case that needs the rule is a failure: the list is the whole exemption.
REORDERED_IDS = {'hst-lhs-009'}


def reordered(dom_line, stream_line):
    """Both refused it; the DOM parse reached the encoding error first."""
    d = dom_line.split(' ', 5)
    s = stream_line.split(' ', 5)
    return (d[0] == 'FAIL' and s[0] == 'FAIL'
            and d[1] == 'GTEXT_XML_E_INVALID_SEQUENCE'
            and s[1] in WELL_FORMEDNESS and s[1] != d[1]
            and int(s[2]) <= int(d[2]))


cases = []
walk(load_catalogue(), '', '', cases)
if not cases:
    sys.exit("the catalogue yields no cases")
exceptions = load_manifest(EXCEPTIONS, 'exception', 3)
EXCLUSIONS = os.path.join(os.path.dirname(EXCEPTIONS), 'XMLCONF_EXCLUSIONS') if EXCEPTIONS else None
exclusions = load_manifest(EXCLUSIONS, 'exclusion', 2)

out_of_scope = collections.Counter()
profile_scored = collections.Counter()
unsupported = collections.Counter()
scored = collections.Counter()           # (type, outcome) over scored cases
unscored = collections.Counter()         # (type, outcome) over reported-only cases
failures = []                            # unexplained scored failures
explained = []                           # (id, profile, reason) exceptions seen to fail
crashes = []
disagreements = []                       # (id, mode, dom line, stream line)
round_trip_failures = []                 # (id, runner line)
round_trip_checked = 0
round_trip_checked_by_version = collections.Counter()
round_trip_refusals = 0                  # documents some one-byte encoding refused
ordering = collections.Counter()         # mode -> cases refused first for another reason
stream_checked = 0
seen_ids = set()
missing = []

rebased = 0
seen_exclusions = set()
for t, path, beside in cases:
    ident = t.get('ID')
    seen_ids.add(ident)
    if not os.path.exists(path) and os.path.exists(beside):
        path = beside
        rebased += 1
    if not os.path.exists(path):
        missing.append((ident, path))
        continue
    profile = profile_for(t, path)
    why = classify_scope(t)
    if why:
        entry = exclusions.get(ident)
        if entry != (why,):
            failures.append((ident, 'excluded', 'exclusion manifest says %r; current reason is %r' % (entry, why), path))
        else:
            seen_exclusions.add(ident)
        out_of_scope[(profile, why)] += 1
        continue
    kind = t.get('TYPE')
    outcome, detail, dom_line = run(path)
    if outcome in ('CRASH', 'TIMEOUT'):
        crashes.append((ident, outcome, detail))
        continue
    # The event parser, whole and a byte at a time, must say what the DOM parse
    # said. This is every case that is in scope - the unsupported ones included,
    # since an unsupported encoding is refused the same way by both.
    for mode in ('whole', 'bytes'):
        line, died = run_line(path, mode)
        if died:
            crashes.append((ident + ' [' + mode + ']', died[0], died[1]))
        elif line != dom_line:
            if reordered(dom_line, line) and ident in REORDERED_IDS:
                ordering[mode] += 1
            else:
                disagreements.append((ident, mode, dom_line, line))
        stream_checked += 1
    if outcome in UNSUPPORTED:
        reason = 'parser does not support %s for this case' % outcome.replace('GTEXT_XML_E_', '')
        entry = exclusions.get(ident)
        if entry != (reason,):
            failures.append((ident, 'excluded', 'exclusion manifest says %r; current reason is %r' % (entry, reason), path))
        else:
            seen_exclusions.add(ident)
        unsupported[(profile, kind, outcome)] += 1
        continue
    accepted = outcome == 'OK'
    if accepted:
        line, died = run_line(path, 'roundtrip')
        if died:
            crashes.append((ident + ' [roundtrip]', died[0], died[1]))
        elif line.split(' ')[0] == 'OK':
            round_trip_checked += 1
            round_trip_checked_by_version[profile.split('/', 1)[0]] += 1
            round_trip_refusals += 1 if line.split(' ')[1:] not in ([], ['0']) else 0
        else:
            round_trip_failures.append((ident, line))
    if kind == 'not-wf':
        passed = outcome in WELL_FORMEDNESS
    elif kind in ('valid', 'invalid'):
        if outcome == 'GTEXT_XML_E_ENTITY':
            # It refers to an entity its DTD declares, and reading the document
            # needs that declaration processed: Phase 4. (The catalogue's own
            # ENTITIES attribute does not say this: "none" means no *external*
            # entities, and plenty of "none" documents use an internal one.)
            # Reported, not scored, and counted so the number is visible.
            unscored[(kind + ' (names an entity: needs DTD processing)', 'refused')] += 1
            continue
        passed = accepted
    else:
        unscored[(kind, 'accepted' if accepted else 'refused')] += 1
        continue
    scored[(profile, kind, 'pass' if passed else 'fail')] += 1
    profile_scored[(profile, kind, 'pass' if passed else 'fail')] += 1
    if not passed:
        expected = 'refused' if kind == 'not-wf' else 'accepted'
        entry = (ident, profile, '%s: %s expected; got %s %s' % (kind, expected, outcome, detail), path)
        if ident in exceptions:
            if exceptions[ident][0] != profile:
                failures.append((ident, profile, 'exception profile mismatch (%s vs %s): %s' % (
                    exceptions[ident][0], profile, kind), path))
            else:
                explained.append((ident, profile, exceptions[ident][1]))
        else:
            failures.append(entry)

total_scored = sum(scored.values())
total_pass = sum(v for (_profile, _kind, outcome), v in scored.items() if outcome == 'pass')

for ident, entry in exclusions.items():
    if ident not in seen_exclusions:
        failures.append((ident, 'excluded', 'stale exclusion: %s' % entry[0], ''))
print("W3C XML Conformance Test Suite: %d cases in the catalogue" % len(cases))
if rebased:
    print("(%d found in their sub-catalogue's folder: the catalogue's xml:base for them names a folder with no files)" % rebased)
print()
print("scored by profile and expected class:")
for profile in ('XML1.0/NS1.0', 'XML1.0/NS1.1', 'XML1.1/NS1.0',
                'XML1.1/NS1.1', 'XML1.0/NSnone', 'XML1.1/NSnone'):
    print("  %s" % profile)
    for kind in ('not-wf', 'valid', 'invalid'):
        p = scored[(profile, kind, 'pass')]
        f = scored[(profile, kind, 'fail')]
        what = 'refused' if kind == 'not-wf' else 'accepted (well-formed)'
        print("    %-8s %4d of %4d %s" % (kind, p, p + f, what))
print("  total    %4d of %4d" % (total_pass, total_scored))
print()
print("reported, not scored (the valid and invalid classes are never scored for validity here):")
for (kind, outcome), v in sorted(unscored.items()):
    print("  %-34s %-9s %4d" % (kind, outcome, v))
for (profile, kind, outcome), v in sorted(unsupported.items()):
    print("  %-14s %-10s %-34s %-9s %4d" % (profile, kind, outcome[len('GTEXT_XML_E_'):].lower(), 'declined', v))
print()
print("out of scope:")
for (profile, why), v in sorted(out_of_scope.items()):
    print("  %-14s %-54s %4d" % (profile, why, v))
print()
print("event parser, whole and by bytes, against the DOM parse: %d runs, %d disagree" % (
    stream_checked, len(disagreements)))
print("  (conversion error ordering, both refused: %d whole, %d by bytes)" % (
    ordering['whole'], ordering['bytes']))
print()
print("round trip (written in UTF-8, UTF-16 LE and BE, ISO-8859-1, US-ASCII; read back equal; written again identically):")
print("  %d accepted documents, %d differ; %d refused by a one-byte encoding for a character it lacks" % (
    round_trip_checked + len(round_trip_failures), len(round_trip_failures), round_trip_refusals))
print("  by XML version: XML1.0 %d successful round trips; XML1.1 %d" % (
    round_trip_checked_by_version['XML1.0'], round_trip_checked_by_version['XML1.1']))
print()
print("exceptions (scored cases that fail, each with its reason): %d" % len(explained))
reasons = collections.Counter(r for _, _, r in explained)
for r, v in reasons.most_common():
    print("  %4d  %s" % (v, r))

status = 0
if failures:
    status = 1
    print()
    print("UNEXPLAINED FAILURES: %d" % len(failures))
    for ident, kind, detail, path in failures[:200]:
        print("  %-40s %-14s %s" % (ident, kind, detail))
        print("  %-40s         %s" % ('', os.path.relpath(path, SUITE)))
if disagreements:
    status = 1
    print()
    print("EVENT PARSER DISAGREES WITH THE DOM PARSE: %d" % len(disagreements))
    for ident, mode, dom_line, stream_line in disagreements[:60]:
        print("  %-40s %s" % (ident, mode))
        print("      dom:    %s" % dom_line)
        print("      stream: %s" % stream_line)
if round_trip_failures:
    status = 1
    print()
    print("ROUND TRIP FAILURES: %d" % len(round_trip_failures))
    for ident, line in round_trip_failures[:60]:
        print("  %-40s %s" % (ident, line))
if crashes:
    status = 1
    print()
    print("RUNNER CRASHES / TIMEOUTS: %d" % len(crashes))
    for ident, outcome, detail in crashes[:50]:
        print("  %-40s %s %s" % (ident, outcome, detail))
if missing:
    status = 1
    print()
    print("CATALOGUED FILES THAT ARE NOT THERE: %d" % len(missing))
    for ident, path in missing[:20]:
        print("  %-40s %s" % (ident, path))
stale = [i for i in exceptions if i not in {e[0] for e in explained}]
if stale:
    status = 1
    print()
    print("STALE EXCEPTIONS (listed, but the case no longer fails, or is not in the suite): %d" % len(stale))
    for ident in stale[:50]:
        print("  %s: %s" % (ident, exceptions[ident]))

# Cases that leave the scored pool are counted, and a ceiling stops a
# regression from hiding in them.
for name, have, env in (
        ('names an entity', sum(v for (k, o), v in unscored.items() if 'names an entity' in k), 'XMLCONF_MAX_ENTITY'),
        ('declined (unsupported encoding or version)', sum(unsupported.values()), 'XMLCONF_MAX_DECLINED')):
    ceiling = os.environ.get(env)
    if ceiling and have > int(ceiling):
        status = 1
        print()
        print("conformance: %d cases %s, above the ceiling of %s" % (have, name, ceiling))

rt_ceiling = os.environ.get('XMLCONF_MAX_ROUNDTRIP_REFUSED')
if rt_ceiling and round_trip_refusals > int(rt_ceiling):
    status = 1
    print()
    print("conformance: %d documents refused by a one-byte encoding, above the ceiling of %s" % (
        round_trip_refusals, rt_ceiling))

for xml_version, env in (('XML1.0', 'XMLCONF_MIN_ROUNDTRIP_10'),
                         ('XML1.1', 'XMLCONF_MIN_ROUNDTRIP_11')):
    rt_floor = os.environ.get(env)
    have = round_trip_checked_by_version[xml_version]
    if rt_floor and have < int(rt_floor):
        status = 1
        print()
        print("conformance: %s has %d successful round trips, below the floor of %s" % (
            xml_version, have, rt_floor))

for xml_version, env in (('XML1.0', 'XMLCONF_MIN_SCORED_10'),
                         ('XML1.1', 'XMLCONF_MIN_SCORED_11')):
    floor = os.environ.get(env)
    have = sum(v for (p, _k, _o), v in scored.items() if p.startswith(xml_version + '/'))
    if floor and have < int(floor):
        status = 1
        print("conformance: %s scored %d cases, below the floor of %s" % (xml_version, have, floor))
floor = os.environ.get('XMLCONF_MIN_SCORED')
if floor and total_scored < int(floor):
    status = 1
    print("conformance: only %d cases were scored, below the floor of %s" % (total_scored, floor))
if status == 0:
    print()
    print("conformance: every scored case passes or is an explained exception")
sys.exit(status)
