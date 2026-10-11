#!/usr/bin/env python3
"""Compare this library's XML parser against libxml2 and expat.

    xml_diff.py <ours-runner> [--count N] [--seed S] [--propose]
                              [--plant KIND]

Two references, run in one pinned container (containers/xml): **libxml2** with
recovery off, and **expat** with and without namespace processing. They share no
code. Each input is put to all three programs, and three things are compared:

  verdict   accept or refuse, ours against each reference, with the references
            run namespace-aware (libxml2: well-formed *and* namespace-well-formed;
            expat: its namespace mode), because this parser is. The references'
            non-namespace verdicts are not scored; they are recorded so that a
            refusal for a namespace reason is told from one for a syntax reason.
  dump      for a document everyone accepts, the event list: element and
            attribute names with namespace and prefix, namespace declarations,
            the character data, comments, instructions, the document type's
            names, and the declaration's version and standalone flag.
  agreement the two references against each other, counted, so that "ours agrees
            with libxml2" is read against whether libxml2 and expat agree.

**Every input is scored, the refused ones included.** The denominator is the
whole population. `valid` and `invalid` suite cases are well-formed documents and
are asked of the references as such; whether they are valid is never scored.

**What is normalized, and what is not.** Adjacent character data is one run, and a
CDATA section is character data: the information set does not tell them apart and
expat has no node for either. Attributes and namespace declarations are sorted:
their order is not information, and expat does not promise one. A declaration
with version 1.0 and no standalone is the same as none, because libxml2 reports
1.0 for a document without one. Nothing else is masked. In particular **white
space is not**: a whitespace-only text node is an event, and so is a difference
in how one is spelled.

**A disagreement is classified, and the list is the gate.** Every disagreement is
a line of `tools/oracle/xml_disagreements.txt`: the input, its signature (what
each program said), a class and a reason. The run fails on a disagreement that is
not listed, on a listed one that no longer disagrees, and on a listed one whose
signature has changed. What an entry locks is the verdict of each program and, for an
event-list difference, a digest (`#xxxxxxxx`) of the event lists themselves, so a
different difference for the same input is a changed signature and cannot hide behind
the old entry; for a verdict disagreement the verdicts are the whole difference. An entry
cannot outlive its reason either. The classes are:

  reference-wrong   a reference accepts or refuses against the specification
  version-specific  the pinned reference's behaviour, not the standard's
  spec-ambiguity    the specification can be read either way
  representation    equal information, reported differently
  reference-scope   a reference does not read this encoding or how it is presented
  scope-dtd         needs DTD processing, which this parser does not do
  scope-xml11       XML 1.1, which this parser refuses on purpose
  scope-unsupported an encoding or version this parser declines to read
  scope-limit       a limit this parser enforces and the references do not

There is no class for "ours is wrong". That one is fixed, with a regression test.

`--propose` prints the unlisted disagreements as candidate lines with a guessed
class and nothing else; it is for a human to read, and the guesses are not the
reading. `--plant KIND` corrupts our answers on purpose (see PLANTS) and the
gate must then fail with exit status 3 and the heading, and the kind of difference, for that kind;
`run-xml-oracle.sh --controls` runs every plant and fails if any of them passes
or fails for another reason, and check-xml-oracle runs it after the gate itself.

Copyright 2026 by Corey Pennycuff
"""

import collections
import hashlib
import re
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oracle_env  # noqa: E402
import xml_classify  # noqa: E402
import xml_gen  # noqa: E402

LIST = os.path.join(HERE, "xml_disagreements.txt")
DEFAULT_COUNT, DEFAULT_SEED = 4000, 1

CLASSES = ("reference-wrong", "version-specific", "spec-ambiguity", "representation",
           "reference-scope", "scope-dtd", "scope-xml11", "scope-unsupported", "scope-limit")

PLANTS = {
    "flip-verdict": "our verdict on every 37th input is the opposite one",
    "drop-attribute": "an attribute is dropped from every 53rd accepted document",
    "alter-text": "one character of character data is changed in every 41st accepted document",
    "lose-whitespace": "whitespace-only text is dropped from every accepted document that has some",
    "stale-entry": "a listed disagreement is made to agree (the list then has a stale entry)",
    "change-signature": "a listed disagreement's signature is changed (it still disagrees, differently)",
    "no-list": "the list is read as empty",
}


# ---- reading the two streams -----------------------------------------------------

def unhex(token):
    if token == "-":
        return None
    if token == ".":
        return b""
    return bytes.fromhex(token)


def read_blocks(text):
    """The protocol's documents, in order: a list of line lists, BEGIN..END."""
    docs = []
    current = None
    for line in text.split("\n"):
        if line == "BEGIN":
            current = []
        elif line == "END":
            docs.append(current)
            current = None
        elif current is not None and line:
            current.append(line)
    if current is not None:
        raise SystemExit("a protocol stream ended inside a document")
    return docs


def take_dump(lines, i):
    """Lines of a DUMP..ENDDUMP block starting at lines[i] == 'DUMP'."""
    j = i + 1
    out = []
    while lines[j] != "ENDDUMP":
        out.append(lines[j])
        j += 1
    return out, j + 1


def parse_ours(block):
    head = block[0].split()
    assert head[0] == "OURS", block[0]
    out = {"verdict": head[1], "status": head[2] if len(head) > 2 else None,
           "where": " ".join(head[3:]), "dump": None}
    if head[1] == "ACCEPT":
        out["dump"], _ = take_dump(block, 1)
    return out


def parse_ref(block):
    out = {}
    i = 0
    while i < len(block):
        head = block[i].split(" ")
        if head[0] == "XML2":
            out["xml2"] = {"wf": head[1] == "1", "ns": head[2] == "1", "uri": head[3] == "1", "dump": None,
                           "error": unhex(head[4]).decode("utf-8", "replace") if len(head) > 4 and head[4] != "-" else ""}
            i += 1
            if block[i] == "DUMP":
                out["xml2"]["dump"], i = take_dump(block, i)
        elif head[0] in ("EXPAT-NS", "EXPAT-PLAIN"):
            key = "expat" if head[0] == "EXPAT-NS" else "plain"
            ok = head[1] == "ok"
            out[key] = {"ok": ok, "dump": None,
                        "error": None if ok else "%s %s" % (head[2], unhex(head[3]).decode("utf-8", "replace")),
                        "at": None if ok else int(head[4])}
            i += 1
            if ok and key == "expat" and i < len(block) and block[i] == "DUMP":
                out[key]["dump"], i = take_dump(block, i)
        else:
            raise SystemExit("unreadable reference line %r" % block[i])
    return out


# ---- normalization -----------------------------------------------------------------

def normalize(lines):
    """A dump as a list of tuples, normalized as the module header says."""
    events = []
    text = b""
    pending = None  # an open start tag: [S fields, [N...], [A...]]

    def flush_text():
        nonlocal text
        if text:
            events.append(("T", text))
        text = b""

    def flush_tag():
        nonlocal pending
        if pending is not None:
            head, ns, attrs = pending
            events.append(("S",) + head)
            for n in sorted(ns, key=lambda n: (n[0] or b"", n[1] or b"")):
                events.append(("N",) + n)
            for a in sorted(attrs, key=lambda a: tuple(x or b"" for x in a)):
                events.append(("A",) + a)
            pending = None

    for line in lines:
        tag, *raw = line.split(" ")
        if tag == "X":
            # version (hex or -) and the standalone flag as a plain integer.
            flush_tag()
            flush_text()
            version, standalone = unhex(raw[0]), int(raw[1])
            if not ((version is None or version == b"1.0") and standalone == -1):
                events.append(("X", version, standalone))
            continue
        fields = [unhex(f) for f in raw]
        if tag == "N":
            pending[1].append(tuple(fields))
            continue
        if tag == "A":
            pending[2].append(tuple(fields))
            continue
        flush_tag()
        if tag in ("T", "C"):
            text += fields[0]
            continue
        flush_text()
        if tag == "S":
            pending = [tuple(fields), [], []]
        else:
            events.append((tag,) + tuple(fields))
    flush_tag()
    flush_text()
    return events


def first_difference(a, b):
    """(index, ours, theirs) of the first event that differs, or None."""
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i, a[i], b[i]
    if len(a) != len(b):
        n = min(len(a), len(b))
        return n, (a[n] if len(a) > n else None), (b[n] if len(b) > n else None)
    return None


def show(event):
    if event is None:
        return "(end)"
    out = [event[0]]
    for f in event[1:]:
        if isinstance(f, bytes):
            out.append(repr(f.decode("utf-8", "replace")))
        else:
            out.append(repr(f))
    return " ".join(out)


# ---- scoring ------------------------------------------------------------------------

def ref_verdicts(ref):
    """What each reference said, namespace-aware: 'A' or 'R'."""
    xml2 = "A" if ref["xml2"]["wf"] and ref["xml2"]["ns"] else "R"
    expat = "A" if ref["expat"]["ok"] else "R"
    plain = "A" if ref["plain"]["ok"] else "R"
    return xml2, expat, plain


def dump_only_xml2(sig):
    """Whether the signature is a difference of libxml2's event list alone."""
    return re.search(r" dump=xml2(#|$)", sig) is not None


def score(ident, ours, ref):
    """(signature, detail) for a disagreement, or (None, None) for agreement."""
    xml2, expat, plain = ref_verdicts(ref)
    if ours["verdict"] == "ACCEPT":
        v = "A"
    elif ours["verdict"] == "REFUSE":
        v = "R"
    else:
        v = "D:" + ours["status"].replace("GTEXT_XML_E_", "")
    sig = "ours=%s xml2=%s expat=%s plain=%s" % (v, xml2, expat, plain)
    if v == "A":
        wrong = [n for n, x in (("xml2", xml2), ("expat", expat)) if x == "R"]
        if wrong:
            return sig, "we accept, %s refuse%s" % (" and ".join(wrong), "s" if len(wrong) == 1 else "")
        ours_events = normalize(ours["dump"])
        diffs = []
        notes = []
        lock = hashlib.sha256(repr(ours_events).encode())
        for name, key in (("xml2", "xml2"), ("expat", "expat")):
            theirs = normalize(ref[key]["dump"])
            d = first_difference(ours_events, theirs)
            if d:
                diffs.append(name)
                lock.update(repr(theirs).encode())
                notes.append("%s: event %d ours %s, theirs %s" % (name, d[0], show(d[1]), show(d[2])))
        if diffs:
            # What the entry locks is the difference itself, not only who differed: the digest is
            # over both event lists, so a different difference with the same signature is a
            # changed signature and fails the run.
            return sig + " dump=" + "+".join(diffs) + "#" + lock.hexdigest()[:8], "; ".join(notes)
        return None, None
    if v == "R":
        wrong = [n for n, x in (("xml2", xml2), ("expat", expat)) if x == "A"]
        if wrong:
            return sig, "we refuse (%s), %s accept%s" % (ours["status"].replace("GTEXT_XML_E_", ""),
                                                        " and ".join(wrong), "s" if len(wrong) == 1 else "")
        return None, None
    # Declined: not a verdict. A disagreement only if a reference reads the document.
    if xml2 == "R" and expat == "R":
        return None, None
    return sig, "we decline (%s), a reference reads it" % ours["status"].replace("GTEXT_XML_E_", "")


# ---- the list -------------------------------------------------------------------------

def load_list(path=LIST):
    table = {}
    if not os.path.exists(path):
        return table
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#"):
                continue
            parts = raw.split("\t")
            if len(parts) != 4:
                raise SystemExit("%s:%d: not <id> <signature> <class> <reason>" % (path, n))
            ident, sig, cls, reason = parts
            if cls not in CLASSES:
                raise SystemExit("%s:%d: %r is not a class (%s)" % (path, n, cls, ", ".join(CLASSES)))
            if not reason.strip():
                raise SystemExit("%s:%d: %s has no reason" % (path, n, ident))
            if ident in table:
                raise SystemExit("%s:%d: %s is listed twice" % (path, n, ident))
            table[ident] = (sig, cls, reason)
    return table


# ---- running ----------------------------------------------------------------------------

def frame(items):
    out = []
    for _ident, data, _tags in items:
        out.append(b"%d\n" % len(data))
        out.append(data)
    out.append(b"-1\n")
    return b"".join(out)


def run_ours(runner, batch):
    done = subprocess.run([runner], input=batch, capture_output=True)
    if done.returncode != 0:
        raise SystemExit("our runner exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-2000:]))
    return done.stdout.decode("ascii")


def run_reference(batch):
    cmd = oracle_env.command("xml", ["xml-driver"])
    done = subprocess.run(cmd, input=batch, capture_output=True)
    if done.returncode != 0:
        raise SystemExit("the reference driver exited %d:\n%s" % (
            done.returncode, oracle_env.reference_stderr(done.stderr.decode("utf-8", "replace"))[-2000:]))
    return done.stdout.decode("ascii")


def evaluate(runner, items):
    """[(ours, ref)] for each input, parsed."""
    batch = frame(items)
    ours_blocks = read_blocks(run_ours(runner, batch))
    ref_blocks = read_blocks(run_reference(batch))
    if not (len(ours_blocks) == len(ref_blocks) == len(items)):
        raise SystemExit("sent %d documents, got %d from ours and %d from the references"
                         % (len(items), len(ours_blocks), len(ref_blocks)))
    return [(parse_ours(o), parse_ref(r)) for o, r in zip(ours_blocks, ref_blocks)]


def controls_for(runner, unlisted, found):
    """{ident: {control name: (agrees, ours, ref)}} for the disagreements."""
    todo = []
    for k in unlisted:
        for name, data in xml_classify.make_controls(found[k][3]).items():
            todo.append((k, name, data))
    answers = evaluate(runner, [(k + "/" + n, d, []) for k, n, d in todo]) if todo else []
    out = {}
    for (k, name, _d), (ours, ref) in zip(todo, answers):
        sig, _detail = score(k, ours, ref)
        # A control agrees if nothing differs, or if the only difference is one of
        # libxml2's known ways of reporting the same information (see
        # xml_classify.dump_difference): a control must not fail to separate the
        # cause it isolates because of a second, unrelated and already-understood one.
        agrees = sig is None or (dump_only_xml2(sig)
                                 and xml_classify.dump_difference(ours, ref, normalize) is not None)
        out.setdefault(k, {})[name] = (agrees, ours, ref, _d)

    # A document can have two causes, a DTD that this parser does not process and a
    # reference's leniency elsewhere. When removing the DTD leaves a disagreement, the
    # document without it is classified in its own right (with its own controls, minus
    # the DTD one), and what that finds is recorded as "nodtd-residue".
    second = []
    for k, controls in out.items():
        c = controls.get("nodtd")
        if c and not c[0]:
            second.append((k, c))
    if second:
        todo2 = []
        for k, c in second:
            for name, data in xml_classify.make_controls(c[3]).items():
                if name != "nodtd":
                    todo2.append((k, name, data))
        answers2 = evaluate(runner, [(k + "/" + n, d, []) for k, n, d in todo2]) if todo2 else []
        inner = {}
        for (k, name, _d), (ours, ref) in zip(todo2, answers2):
            sig, _detail = score(k, ours, ref)
            agrees = sig is None or (dump_only_xml2(sig)
                                     and xml_classify.dump_difference(ours, ref, normalize) is not None)
            inner.setdefault(k, {})[name] = (agrees, ours, ref, _d)
        for k, c in second:
            sig_c, detail_c = score(k, c[1], c[2])
            if sig_c is None:
                continue
            cls, reason = xml_classify.classify(c[3], c[1], c[2], sig_c, detail_c, inner.get(k, {}), normalize)
            if cls != "unclassified":
                out[k]["nodtd-residue"] = (cls, reason)
    return out


def plant(kind, i, ours):
    """Corrupt one of our answers on purpose. Returns (answer, whether it was changed)."""
    if kind == "flip-verdict" and i % 37 == 0:
        if ours["verdict"] == "ACCEPT":
            return {"verdict": "REFUSE", "status": "GTEXT_XML_E_SYNTAX", "where": "", "dump": None}, True
        if ours["verdict"] == "REFUSE":
            return {"verdict": "ACCEPT", "status": None, "where": "", "dump": ["S - 61 -", "E"]}, True
    if ours["verdict"] != "ACCEPT":
        return ours, False
    dump = list(ours["dump"])
    changed = False
    if kind == "drop-attribute" and i % 53 == 0:
        for k, line in enumerate(dump):
            if line.startswith("A "):
                del dump[k]
                changed = True
                break
    elif kind == "alter-text" and i % 41 == 0:
        for k, line in enumerate(dump):
            if line.startswith("T ") and len(line) > 2 and line[2:] not in ("-", "."):
                head = line[2:4]
                dump[k] = "T " + ("%02x" % (int(head, 16) ^ 1)) + line[4:]
                changed = True
                break
    elif kind == "lose-whitespace":
        kept = [l for l in dump if not (l.startswith("T ") and set(bytes.fromhex(l[2:]).decode("latin-1")) <= set(" \t\r\n"))]
        changed = len(kept) != len(dump)
        dump = kept
    return (dict(ours, dump=dump) if changed else ours), changed


# The generated population is where the axes are varied, so it is where drift hides: a
# generator edit that quietly stops producing UTF-16 or accepted documents leaves every
# score green on a smaller question. These are the axes it must produce, and the share of
# it this parser accepts must stay between the bounds (measured 6%; it is mostly refused
# because it is built from grammar with defects mixed in at a rate that favours refusals).
REQUIRED_AXES = ("gen:structured", "gen:mutated", "gen:mutated-xmlconf", "enc:utf8-bom", "enc:invalid-utf8",
                 "enc:ascii-declared", "enc:latin1-declared", "enc:utf16le-bom", "enc:utf16be-bom",
                 "enc:utf16le-nobom", "enc:utf16be-nobom", "enc:utf32le-bom", "enc:utf32be-bom",
                 "enc:utf32be-nobom", "enc:utf16-labeled-utf8", "enc:utf8-labeled-utf16")
MIN_PER_AXIS = 10
ACCEPTED_SHARE = (0.03, 0.30)


def check_population(items, answers):
    """Problems with the generated population, as strings."""
    problems = []
    counts = collections.Counter()
    accepted = total = 0
    for (_i, _d, tags), (ours, _ref) in zip(items, answers):
        if "gen:structured" in tags or "gen:mutated" in tags or "gen:mutated-xmlconf" in tags:
            total += 1
            accepted += ours["verdict"] == "ACCEPT"
            for t in tags:
                counts[t] += 1
    for axis in REQUIRED_AXES:
        if counts[axis] < MIN_PER_AXIS:
            problems.append("the generated population has %d documents on the axis %s (at least %d)"
                            % (counts[axis], axis, MIN_PER_AXIS))
    if total and not (ACCEPTED_SHARE[0] <= accepted / total <= ACCEPTED_SHARE[1]):
        problems.append("this parser accepts %.1f%% of the generated population (bounds %d%%-%d%%)"
                        % (100.0 * accepted / total, 100 * ACCEPTED_SHARE[0], 100 * ACCEPTED_SHARE[1]))
    return problems


def main(argv):
    runner = None
    count, seed = DEFAULT_COUNT, DEFAULT_SEED
    propose = False
    planted = None
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--propose":
            propose = True
        elif a == "--plant":
            planted = args.pop(0)
            if planted not in PLANTS:
                raise SystemExit("--plant: one of %s" % ", ".join(PLANTS))
        elif runner is None:
            runner = a
        else:
            raise SystemExit("unexpected argument %r" % a)
    if runner is None:
        raise SystemExit(__doc__)
    count = int(os.environ.get("XML_ORACLE_COUNT", count))
    seed = int(os.environ.get("XML_ORACLE_SEED", seed))

    items = xml_gen.corpus(count, seed)
    answers = evaluate(runner, items)

    listed = {} if planted == "no-list" else load_list()
    found = {}
    pop = collections.Counter()
    verdicts = collections.Counter()
    refref = collections.Counter()
    by_tag = collections.defaultdict(collections.Counter)
    keep = {}
    uri_checks = 0
    nsaxis = collections.Counter()
    fired = 0
    if (count, seed) == (DEFAULT_COUNT, DEFAULT_SEED):
        problems = check_population(items, answers)
        if problems:
            sys.stderr.write("\033[0;31m\n### The generated population has drifted ###\033[0m\n  "
                             + "\n  ".join(problems) + "\n")
            return 3
    for i, ((ident, data, tags), (ours, ref)) in enumerate(zip(items, answers)):
        if planted:
            ours, did = plant(planted, i, ours)
            fired += did
        xml2, expat, plain = ref_verdicts(ref)
        pop["inputs"] += 1
        verdicts["ours " + ours["verdict"]] += 1
        verdicts["libxml2 " + xml2] += 1
        verdicts["expat " + expat] += 1
        refref["libxml2 and expat agree" if xml2 == expat else "libxml2 and expat differ"] += 1
        if ref["xml2"]["uri"]:
            uri_checks += 1
        # The namespace axis: a document that is XML but not XML with namespaces.
        if ref["plain"]["ok"] and not ref["expat"]["ok"]:
            nsaxis["expat refuses only for namespaces"] += 1
            nsaxis["  of which we refuse"] += ours["verdict"] == "REFUSE"
        if ref["xml2"]["wf"] and not ref["xml2"]["ns"]:
            nsaxis["libxml2 refuses only for namespaces"] += 1
            nsaxis["  of which we refuse "] += ours["verdict"] == "REFUSE"
        sig, detail = score(ident, ours, ref)
        for t in tags:
            by_tag[t]["inputs"] += 1
            if sig:
                by_tag[t]["disagree"] += 1
        if sig:
            found[ident] = (sig, detail, tags, data)
            keep[ident] = (ours, ref)
    if planted in ("flip-verdict", "drop-attribute", "alter-text", "lose-whitespace") and not fired:
        raise SystemExit("the plant %s never changed an answer, so a pass proves nothing" % planted)
    if planted == "stale-entry" and listed:
        # Make one listed disagreement agree: drop it from what was found.
        victim = sorted(k for k in listed if k in found)[0]
        del found[victim]
    if planted == "change-signature" and listed:
        # Keep it disagreeing, differently: the entry's locked signature no longer matches.
        victim = sorted(k for k in listed if k in found)[0]
        listed[victim] = (listed[victim][0] + " (changed)", listed[victim][1], listed[victim][2])

    unlisted = sorted(k for k in found if k not in listed)
    stale = sorted(k for k in listed if k not in found)
    changed = sorted(k for k in listed if k in found and found[k][0] != listed[k][0])
    # The list describes the default population (4000 generated documents, seed 1). A
    # run that sizes or seeds it otherwise is a hunt: the generated disagreements it
    # finds cannot be in a list made for another population, so they are classified by
    # the same controls and the run fails only on one nothing explains.
    hunting = (count, seed) != (DEFAULT_COUNT, DEFAULT_SEED)
    hunted = []
    if hunting:
        hunted = [k for k in unlisted if k.startswith("gen:")]
        unlisted = [k for k in unlisted if not k.startswith("gen:")]
        stale = [k for k in stale if not k.startswith("gen:")]
        changed = [k for k in changed if not k.startswith("gen:")]

    print("inputs %d: %s" % (pop["inputs"], ", ".join(
        "%s %d" % (k, verdicts[k]) for k in sorted(verdicts))))
    print("references against each other: %s" % ", ".join(
        "%s %d" % (k, v) for k, v in sorted(refref.items())))
    print("namespace axis: " + ", ".join("%s %d" % (k.strip() if k.startswith(" ") else k, nsaxis[k])
                                       for k in ("expat refuses only for namespaces", "  of which we refuse",
                                                 "libxml2 refuses only for namespaces", "  of which we refuse ")))
    print("libxml2 also objects to %d accepted documents for a namespace name that is not a URI "
          "(a quality check, not a constraint of Namespaces in XML: counted, not scored)" % uri_checks)
    print("  (axes)  " + "  ".join(
        "%s %d/%d" % (t, c["disagree"], c["inputs"]) for t, c in sorted(by_tag.items())
        if t.startswith(("enc:", "not-namespace", "type:", "gen:", "fuzz-seed"))))
    classes = collections.Counter(listed[k][1] for k in found if k in listed)
    print("disagreements: %d found, %d listed (%s), %d unlisted, %d stale, %d with a changed signature"
          % (len(found), sum(1 for k in found if k in listed),
             ", ".join("%s %d" % (c, n) for c, n in sorted(classes.items())) or "none",
             len(unlisted) + len(hunted), len(stale), len(changed)))

    if hunting and not propose:
        ctl = controls_for(runner, hunted, found)
        classes_seen = collections.Counter()
        unexplained = []
        for k in hunted:
            sig, detail, tags, data = found[k]
            ours, ref = keep[k]
            cls, _reason = xml_classify.classify(data, ours, ref, sig, detail, ctl.get(k, {}), normalize)
            classes_seen[cls] += 1
            if cls == "unclassified":
                unexplained.append(k)
        print("hunting (population %d, seed %d is not the list's): %d generated disagreements, %s"
              % (count, seed, len(hunted), ", ".join("%s %d" % kv for kv in sorted(classes_seen.items())) or "none"))
        if unexplained:
            sys.stderr.write("\033[0;31m\n### Generated disagreements no class explains ###\033[0m\n")
            for k in unexplained[:40]:
                sig, detail, tags, data = found[k]
                sys.stderr.write("  %s  [%s]\n    %s\n    %r\n" % (k, sig, detail[:300], data[:160]))
            return 3

    if propose:
        ctl = controls_for(runner, unlisted + (hunted if hunting else []), found)
        for k in unlisted + (hunted if hunting else []):
            sig, detail, tags, data = found[k]
            ours, ref = keep[k]
            cls, reason = xml_classify.classify(data, ours, ref, sig, detail, ctl.get(k, {}), normalize)
            print("%s\t%s\t%s\t%s" % (k, sig, cls, reason.replace("\t", " ")))
        return 0

    failed = False
    # Two headings, because they are two mechanisms: a disagreement about whether the
    # document is XML, and an event list that differs for a document everyone accepts.
    for heading, group in (("Verdict disagreements with no entry in tools/oracle/xml_disagreements.txt",
                            [k for k in unlisted if " dump=" not in found[k][0]]),
                           ("Event-list differences with no entry in tools/oracle/xml_disagreements.txt",
                            [k for k in unlisted if " dump=" in found[k][0]])):
        if not group:
            continue
        failed = True
        sys.stderr.write("\033[0;31m\n### %s ###\033[0m\n" % heading)
        for k in group[:40]:
            sig, detail, tags, data = found[k]
            sys.stderr.write("  %s  [%s]\n    %s\n    %r\n" % (k, sig, detail[:300], data[:160]))
        if len(group) > 40:
            sys.stderr.write("  ... and %d more (--propose prints them all)\n" % (len(group) - 40))
    if stale:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed disagreements that are gone ###\033[0m\n")
        for k in stale[:40]:
            sys.stderr.write("  %s  [%s] %s\n" % (k, listed[k][0], listed[k][2][:100]))
    if changed:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed disagreements whose signature has changed ###\033[0m\n")
        for k in changed[:40]:
            sys.stderr.write("  %s\n    listed [%s]\n    now    [%s]\n" % (k, listed[k][0], found[k][0]))
    if failed:
        return 3
    print("xml oracle: every disagreement is listed, none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
