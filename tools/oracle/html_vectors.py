#!/usr/bin/env python3
"""Prove the HTML oracle against the standard's own tree-construction vectors.

    html_vectors.py [--propose] [--plant KIND]

The differential in html_diff.py is only as good as the two references it asks, so
this runs first and is its own gate: every tree-construction vector in the pinned
web-platform-tests tree (html/syntax/parsing/resources/*.dat, the format the
html5lib-tests project defined and then moved there) that does not depend on the
scripting flag is put to **both** references, through the same driver and the same
dump format, and each must reproduce the expected tree exactly. The vectors that
are written for one scripting setting (`#script-on`, `#script-off`) are run with
that setting and reported separately; they are scored the same way.

A reference that does not reproduce a vector is a **known deviation**, and the list
is the gate: tools/oracle/html_deviations.txt has one line per (vector, reference)
with a class and a reason, and the run fails on a deviation that is not listed, on a
listed one that now passes (stale), and on one whose answer has changed (the line
locks a digest of the expected and the actual tree, so a different failure of the
same vector cannot hide behind the old entry). The classes:

  reference-old      html5lib 1.1 is from 2020 and predates a later change to the standard
  reference-api      the reference's API cannot be asked the question (a foreign fragment context)
  reference-wrong    the reference disagrees with the standard in a way no change explains
  vector-unreachable the vector cannot be put to a reference as written

There is no class for "the vector is wrong": the vectors are the standard's own
conformance data and are pinned by commit.

**The oracle is then made to fail.** `--plant KIND` corrupts the run on purpose and
the gate must exit 3 with the heading for that kind; `run-html-oracle.sh --controls`
runs every plant:

  expectation  one vector's expected tree is changed (the vector lies)
  answer       one reference answer is changed (the reference lies)
  stale        a listed deviation is made to pass
  signature    a listed deviation's locked digest is changed

Copyright 2026 by Corey Pennycuff
"""

import collections
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import html_ref  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
WPT = os.path.join(ROOT, "third_party", "html", "wpt", "html", "syntax", "parsing", "resources")
LIST = os.path.join(HERE, "html_deviations.txt")
CLASSES = ("reference-old", "reference-api", "reference-wrong", "vector-unreachable")
PLANTS = ("expectation", "answer", "stale", "signature")
# The population must not shrink silently: a parser fault that drops files scores clean
# on fewer vectors. These are floors measured on the pinned tree, not targets.
MIN_VECTORS = 1950
MIN_FILES = 55


class Vector:
    def __init__(self, ident, data, context, scripting, expected):
        self.ident = ident
        self.data = data
        self.context = context
        self.scripting = scripting  # None: independent of it; True or False: written for that setting
        self.expected = expected


def parse_dat(path, name):
    """The vectors of one .dat file, in order.

    Sections start at a line `#name`; every other line belongs to the current section,
    blank lines included, and a test ends where the next `#data` begins. The separator
    blank line before it is the last line of `#document` and is dropped.
    """
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    tests = []
    current = None
    section = None
    for line in lines:
        if line.startswith("#") and line[1:] in ("data", "errors", "new-errors", "document-fragment",
                                                 "script-on", "script-off", "document", "description"):
            heading = line[1:]
            if heading == "data":
                current = {"data": [], "errors": [], "new-errors": [], "document-fragment": [],
                           "script-on": [], "script-off": [], "document": [], "description": [], "flags": set()}
                tests.append(current)
            if heading in ("script-on", "script-off"):
                current["flags"].add(heading)
            section = heading
            continue
        if current is not None and section is not None:
            current[section].append(line)
    out = []
    for n, t in enumerate(tests, 1):
        doc = t["document"]
        while doc and doc[-1] == "":
            doc.pop()
        flags = t["flags"]
        if "script-on" in flags and "script-off" in flags:
            raise SystemExit("%s test %d: both script-on and script-off" % (name, n))
        scripting = True if "script-on" in flags else False if "script-off" in flags else None
        context = t["document-fragment"][0] if t["document-fragment"] else None
        out.append(Vector("%s#%d" % (name, n), "\n".join(t["data"]), context, scripting, "\n".join(doc)))
    return out


# Files whose vectors only mean something when the script in them runs: the harness that
# scores them executes the script and compares the tree afterwards. No reference here
# executes script, so they cannot be put to one as written. They are counted and named.
SCRIPT_EXECUTING = ("scripted_adoption01.dat", "scripted_ark.dat", "scripted_foster01.dat",
                    "scripted_webkit01.dat")


def load_vectors():
    if not os.path.isdir(WPT):
        raise SystemExit("the web-platform-tests tree-construction vectors are not here; run tools/html/fetch.sh")
    vectors = []
    files = 0
    for name in sorted(os.listdir(WPT)):
        if name.endswith(".dat") and name not in SCRIPT_EXECUTING:
            files += 1
            vectors.extend(parse_dat(os.path.join(WPT, name), name))
    return vectors, files


def signature(expected, got):
    return hashlib.sha256((expected + "\x00" + got).encode("utf-8")).hexdigest()[:10]


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
            if len(parts) != 5:
                raise SystemExit("%s:%d: not <id> <reference> <signature> <class> <reason>" % (path, n))
            ident, ref, sig, cls, reason = parts
            if ref not in html_ref.REFERENCES:
                raise SystemExit("%s:%d: %r is not a reference" % (path, n, ref))
            if cls not in CLASSES:
                raise SystemExit("%s:%d: %r is not a class (%s)" % (path, n, cls, ", ".join(CLASSES)))
            if not reason.strip():
                raise SystemExit("%s:%d: %s has no reason" % (path, n, ident))
            if (ident, ref) in table:
                raise SystemExit("%s:%d: %s %s is listed twice" % (path, n, ident, ref))
            table[(ident, ref)] = (sig, cls, reason)
    return table


def classify(vector, ref, got):
    """A guessed class and reason, for --propose; the guess is not the reading."""
    if "error" in got:
        if "fragment context by HTML name only" in got["error"]:
            return ("reference-api", "html5lib's fragment API takes an HTML element name and cannot express a "
                                     "foreign context element")
        return ("reference-wrong", got["error"][:120])
    return ("reference-old" if ref == "html5lib" else "reference-wrong", "UNREAD")


def first_diff(expected, got):
    a, b = expected.split("\n"), got.split("\n")
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else "(end)"
        y = b[i] if i < len(b) else "(end)"
        if x != y:
            return "line %d: expected %r, got %r" % (i + 1, x[:100], y[:100])
    return "identical"


def main(argv):
    propose = "--propose" in argv
    plant = None
    if "--plant" in argv:
        plant = argv[argv.index("--plant") + 1]
        if plant not in PLANTS:
            raise SystemExit("--plant: one of %s" % ", ".join(PLANTS))
    vectors, files = load_vectors()
    if len(vectors) < MIN_VECTORS or files < MIN_FILES:
        sys.stderr.write("\033[0;31m### The vector population has shrunk ###\033[0m\n  %d vectors in %d files "
                         "(floors %d and %d)\n" % (len(vectors), files, MIN_VECTORS, MIN_FILES))
        return 3

    reqs = [html_ref.request(v.data, bool(v.scripting), v.context) for v in vectors]
    answers = html_ref.ask_all(reqs)
    expected = [v.expected for v in vectors]
    if plant == "expectation":
        # Change one expectation: the first vector that has a tree at all.
        k = next(i for i, v in enumerate(vectors) if v.expected and v.ident not in ("",))
        expected[k] = expected[k] + "\n| <planted>"
    if plant == "answer":
        for name in html_ref.REFERENCES:
            k = next(i for i, a in enumerate(answers[name]) if "dump" in a and a["dump"])
            answers[name][k] = dict(answers[name][k], dump=answers[name][k]["dump"] + "\n| <planted>")
            break

    listed = load_list()
    found = {}
    counts = collections.Counter()
    for i, v in enumerate(vectors):
        group = "independent of scripting" if v.scripting is None else "written for scripting %s" % (
            "on" if v.scripting else "off")
        for name in html_ref.REFERENCES:
            got = answers[name][i]
            counts[(group, name, "run")] += 1
            if "dump" in got and got["dump"] == expected[i]:
                counts[(group, name, "ok")] += 1
                continue
            text = got.get("dump", "ERROR " + got.get("error", "?"))
            found[(v.ident, name)] = (signature(expected[i], text), first_diff(expected[i], text), v, got)

    if plant == "stale":
        victim = sorted(k for k in listed if k in found)[0]
        del found[victim]
    if plant == "signature":
        victim = sorted(k for k in listed if k in found)[0]
        listed[victim] = (listed[victim][0] + "x",) + listed[victim][1:]

    unlisted = sorted(k for k in found if k not in listed)
    stale = sorted(k for k in listed if k not in found)
    changed = sorted(k for k in listed if k in found and found[k][0] != listed[k][0])

    print("vectors: %d in %d files (%d independent of scripting, %d written for it)" % (
        len(vectors), files, sum(v.scripting is None for v in vectors), sum(v.scripting is not None for v in vectors)))
    skipped = sum(len(parse_dat(os.path.join(WPT, name), name)) for name in SCRIPT_EXECUTING
                  if os.path.exists(os.path.join(WPT, name)))
    print("  not run: %d vectors in %s, whose trees are what the script in them makes" % (
        skipped, ", ".join(SCRIPT_EXECUTING)))
    for name in html_ref.REFERENCES:
        for group in ("independent of scripting", "written for scripting on", "written for scripting off"):
            run = counts[(group, name, "run")]
            if run:
                print("  %-8s %-26s %d of %d reproduce the expected tree" % (name, group, counts[(group, name, "ok")], run))
    classes = collections.Counter(listed[k][1] for k in found if k in listed)
    print("deviations: %d found, %d listed (%s), %d unlisted, %d stale, %d with a changed signature" % (
        len(found), sum(1 for k in found if k in listed),
        ", ".join("%s %d" % kv for kv in sorted(classes.items())) or "none", len(unlisted), len(stale), len(changed)))

    if propose:
        for k in unlisted:
            sig, detail, v, got = found[k]
            cls, reason = classify(v, k[1], got)
            print("%s\t%s\t%s\t%s\t%s" % (k[0], k[1], sig, cls, reason + " | " + detail.replace("\t", " ")))
        return 0

    failed = False
    if unlisted:
        failed = True
        sys.stderr.write("\033[0;31m\n### Vectors a reference does not reproduce, with no entry in "
                         "tools/oracle/html_deviations.txt ###\033[0m\n")
        for k in unlisted[:40]:
            sig, detail, v, got = found[k]
            sys.stderr.write("  %s  %s\n    %s\n    input %r\n" % (k[0], k[1], detail, v.data[:120]))
        if len(unlisted) > 40:
            sys.stderr.write("  ... and %d more (--propose prints them all)\n" % (len(unlisted) - 40))
    if stale:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed deviations that are gone ###\033[0m\n")
        for k in stale[:40]:
            sys.stderr.write("  %s  %s  %s\n" % (k[0], k[1], listed[k][2][:100]))
    if changed:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed deviations whose signature has changed ###\033[0m\n")
        for k in changed[:40]:
            sys.stderr.write("  %s  %s\n    listed [%s] now [%s]\n" % (k[0], k[1], listed[k][0], found[k][0]))
    if failed:
        return 3
    print("html vectors: every deviation is listed, none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
