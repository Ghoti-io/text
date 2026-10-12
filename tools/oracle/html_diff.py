#!/usr/bin/env python3
"""The XHTML write mode against two HTML parsers.

    html_diff.py <ours-runner> [--count N] [--seed S] [--plant KIND] [--propose]
                               [--explain N] [--no-corpus]

The claim under test is the one the writer's documentation makes: a tree with no
hazards (`xhtml_hazards == 0`), written with `xhtml`, is read by an HTML parser
as the same tree an XML parser reads. Nothing had checked it. This writes trees
with the mode, puts the bytes to **html5lib and parse5** (the pinned image,
containers/html), and holds what each returns to the tree that was written, under
`html_equivalent` (html_equivalent.h: the one implementation, which the unit tests
also use).

The population, each tree tagged with where it came from:

  gen     generated trees (html_ours.c): half *tame*, built to keep clear of the
          hazards the writer lists so that "no hazards" is tested on a large number
          of non-trivial trees; half *wild*, any element of any vocabulary in any
          place, so that a hazard the list does not know can be found. HTML, SVG and
          MathML elements, namespace switches, integration points, attributes with
          and without namespaces, text with specials, comments, doctypes, whitespace.
          Each is read with the scripting flag off and on.
  xmlconf every well-formed document of the W3C XML suite, parsed and re-written.
  wpt     the expected tree of every document-mode tree-construction vector (the
          standard's own HTML trees: odd, real, and foreign content included).

Every tree is written once; one the mode refuses is counted by its reason and not
compared. The rest are compared. For each one, four verdicts: the source against
what the XML parser reads from the bytes, against html5lib, against parse5, and
html5lib against parse5.

**What fails the run.**

  * A tree with no hazards whose HTML tree differs from the source in *both* references:
    the writer or the hazard list is wrong, and is fixed, never listed.
  * The XML parser reading something other than what was written.
  * A tree with no hazards on which only one reference differs, unless its cause is one of
    KNOWN_CAUSES below (each established by the vectors and by reading) and the count of
    trees of that cause is the one tools/oracle/html_divergences.txt records.
  * A divergence on a tree *with* hazards whose kind of divergence (the first hazard, who
    differed, how) is not in that file, a listed one that no longer occurs in the default
    population, or one whose count has changed. This is what xml_disagreements.txt is for the
    XML references, and it gates the same way.
  * A reference that could not answer, or a drifted population: too few trees accepted, too
    few without hazards.

**Disagreement between the references is recorded, not hidden**: it is part of every
entry's key (who differed), and the count of trees on which html5lib and parse5 read alike
is printed beside the verdicts.

`--plant KIND` corrupts the writer's *output* on purpose (html_ours.c apply_plant)
and `run-html-oracle.sh --controls` requires the run to fail for each; a differential
that cannot be seen to fail reports the same clean line as one that passes.

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import html_ref  # noqa: E402
import html_vectors  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
LIST = os.path.join(HERE, "html_divergences.txt")
XMLCONF = os.path.join(ROOT, "third_party", "xml", "xmlconf")
DEFAULT_COUNT, DEFAULT_SEED = 4000, 1

PLANTS = {
    "selfclose-empty": "an empty element is written <x/> (HTML reads an open tag)",
    "cdata-script": "script text is wrapped in a CDATA section",
    "void-end-tag": "a void element is given an end tag",
    "drop-doctype": "the document type is dropped",
    "wrong-escape": "an ampersand escape is changed to a different character",
    "stale-entry": "a listed divergence is made to go away",
    "change-signature": "a listed divergence's count is changed",
    "no-list": "the list is read as empty",
}
OUTPUT_PLANTS = ("selfclose-empty", "cdata-script", "void-end-tag", "drop-doctype", "wrong-escape")

CLASSES = ("hazard-expected", "html5lib-wrong", "parse5-wrong", "standard-unclear", "reference-old")

# The default population must stay a population: a generator edit that quietly stops
# producing accepted or hazard-free trees leaves every score green on a smaller question.
MIN_ACCEPTED_SHARE = 0.20
MIN_HAZARD_FREE_SHARE = 0.10
MIN_CORPUS_ACCEPTED = 200
MIN_LATIN_HIGH = 15      # measured 29 in the default population
MIN_FIXED_FREE = 150
# What the corpora leave uncompared in the default run: the XML suite's documents that are not
# well formed, the vectors' trees this library's markup cannot hold (a processing instruction, a name XML
# does not allow), and documents whose dump is ambiguous. A different number is a different corpus.
CORPUS_PINS = {("xmlconf", "NOTPARSED"): 1100, ("wpt", "UNBUILDABLE"): 6}


def hunting_run(count, seed):
    return (count, seed) != (DEFAULT_COUNT, DEFAULT_SEED)


# ---- reading our side ------------------------------------------------------------

def unhex(token):
    return b"" if token == "." else bytes.fromhex(token)


def run_ours(runner, args, stdin=b""):
    done = subprocess.run([runner] + args, input=stdin, capture_output=True)
    if done.returncode != 0:
        raise SystemExit("%s %s exited %d:\n%s" % (runner, " ".join(args), done.returncode,
                                                   done.stderr.decode("utf-8", "replace")[-2000:]))
    return done.stdout


def parse_items(blob):
    """The ITEM records of one run of ours, in order, and the PLANT count if there is one."""
    items = []
    fired = None
    pos = 0
    while pos < len(blob):
        eol = blob.index(b"\n", pos)
        line = blob[pos:eol].decode("ascii")
        pos = eol + 1
        if line.startswith("PLANT "):
            fired = int(line.split()[1])
            continue
        if not line.startswith("ITEM "):
            raise SystemExit("unreadable record %r" % line[:80])
        f = line.split(" ")
        ident, tags, status, hazards, kind, path, msg, length = f[1], f[2], f[3], int(f[4]), f[5], f[6], f[7], int(f[8])
        data = blob[pos:pos + length]
        pos += length + 1
        items.append({"id": ident, "tags": tags, "status": status, "hazards": hazards, "kind": kind,
                      "path": unhex(path).decode("utf-8", "replace"), "message": unhex(msg).decode("utf-8", "replace"),
                      "bytes": data, "source": None})
    return items, fired


# ---- the population ---------------------------------------------------------------

def frames(entries):
    out = []
    for ident, data in entries:
        if " " in ident or "\n" in ident:
            raise SystemExit("an id with white space in it cannot be framed: %r" % ident)
        out.append(b"%s %d\n" % (ident.encode("utf-8"), len(data)))
        out.append(data)
        out.append(b"\n")
    return b"".join(out)


def corpus_xmlconf():
    entries = []
    for dirpath, _dirs, files in sorted(os.walk(XMLCONF)):
        for name in sorted(files):
            if name.endswith(".xml"):
                path = os.path.join(dirpath, name)
                with open(path, "rb") as fh:
                    entries.append(("xmlconf:" + os.path.relpath(path, XMLCONF), fh.read()))
    return entries


def corpus_wpt():
    vectors, _files = html_vectors.load_vectors()
    entries = []
    for v in vectors:
        if v.context is None and v.expected:
            entries.append(("wpt:" + v.ident, v.expected.encode("utf-8")))
    return entries


def population(runner, count, seed, plant, corpus):
    gen_items, fired = parse_items(run_ours(runner, ["gen", str(count), str(seed)] + ([plant] if plant in OUTPUT_PLANTS else [])))
    for it in gen_items:
        # A pretty-printed record has white space the tree it was written from does not: it is
        # held to what the XML parser reads from the bytes, which is the question for it.
        if "/pretty" in it["tags"]:
            it["source"] = b"xml:" + it["bytes"]
        else:
            it["source"] = ("gen:%s" % it["id"][4:]).encode("ascii")
        it["variants"] = (False, True)
    items = list(gen_items)
    # The fixed case: every entry of every vocabulary table of the library, one tree each, so that a
    # transcription error in a table reaches both references. Held to what the XML parser reads.
    fixed, _ = parse_items(run_ours(runner, ["fixed"]))
    for it in fixed:
        it["source"] = b"xml:" + it["bytes"]
        it["variants"] = (False, True)
    items.extend(fixed)
    if plant in OUTPUT_PLANTS and not fired:
        raise SystemExit("the plant %s never changed an output, so a pass proves nothing" % plant)
    if corpus:
        for label, entries, mode, prefix in (("xmlconf", corpus_xmlconf(), "fromxml", b"xml:"),
                                             ("wpt", corpus_wpt(), "fromdump", b"dump:")):
            found, _ = parse_items(run_ours(runner, [mode], frames(entries)))
            by_id = dict(entries)
            for it in found:
                it["source"] = prefix + by_id[it["id"]]
                it["variants"] = (False,)
            items.extend(found)
    return items


# ---- comparing ---------------------------------------------------------------------

def parse_cmp(blob):
    out = {}
    for line in blob.decode("ascii").split("\n"):
        if not line:
            continue
        f = line.split(" ")
        if f[0] != "CMP":
            raise SystemExit("unreadable comparison %r" % line[:80])
        if f[2] == "SOURCEFAIL":
            out[f[1]] = ("SOURCEFAIL", unhex(f[3]).decode("utf-8", "replace"))
            continue
        parts = []
        for i in range(4):
            rc, path, why, ctx = int(f[2 + 4 * i]), f[3 + 4 * i], f[4 + 4 * i], f[5 + 4 * i]
            parts.append((rc, unhex(path).decode("utf-8", "replace"), unhex(why).decode("utf-8", "replace"),
                          unhex(ctx).decode("utf-8", "replace").split()))
        out[f[1]] = ("OK", parts)
    return out


def category(why):
    return why.split(":", 1)[0] if why else "?"


def classify(result):
    """(outcome, category, detail) of one comparison; outcome 'same' when both parsers agree with the source."""
    kind, parts = result
    if kind == "SOURCEFAIL":
        return "sourcefail", "source", parts
    x, h, p, hp = parts
    for who, part in (("xml", x), ("html5lib", h), ("parse5", p)):
        if part[0] not in (0, 1):
            return "unanswered", who, part[2]
    if x[0] != 1:
        return "xml-defect", category(x[2]), "%s %s" % (x[1], x[2])
    if h[0] == 1 and p[0] == 1:
        return "same", "", ""
    who = "both" if h[0] != 1 and p[0] != 1 else "html5lib" if h[0] != 1 else "parse5"
    first = p if p[0] != 1 else h
    return who, category(first[2]), "%s %s" % (first[1], first[2])


def shape_of(cat, detail):
    """A grouping key for a difference: how it differs and at which element, indexes removed."""
    import re
    path, _, rest = detail.partition(" ")
    leaf = re.sub(r"\[\d+\]", "", path).rsplit("/", 1)[-1]
    rest = re.sub(r"\d+", "N", rest)
    return "%s@%s %s" % (cat, leaf, rest[:70])


# A line of a tree dump that begins `| ` starts an entry, and a text or an attribute value with a line
# feed in it is continued on the lines after it, so a value holding "\n| " cannot be told from the next
# entry. Such a document is counted and left out of the comparison, never guessed at.
AMBIGUOUS = re.compile(rb"(?:\n|&#10;|&#[xX]0*[aA];)\| ")


def reference_text(it):
    """The characters an HTML parser's decoder would give the references for this record's bytes."""
    if "/latin1" in it["tags"]:
        # Labelled ISO-8859-1 and read as windows-1252, which agrees with it above 0x9F (the writer
        # refuses 0x80-0x9F). The XML declaration it carries is a comment to an HTML parser; it is
        # taken off here as the part of the document that is not the tree.
        text = it["bytes"].decode("latin-1")
        return re.sub(r"\A<\?xml[^>]*\?>\s*", "", text)
    return it["bytes"].decode("utf-8-sig")


def evaluate(runner, items):
    """[(item, variant, outcome, category, detail, refs)] for each accepted tree and scripting setting."""
    todo = []
    for it in items:
        if it["status"] != "OK":
            continue
        it["ambiguous"] = bool(AMBIGUOUS.search(it["bytes"]))
        if it["ambiguous"]:
            continue
        for scripting in it["variants"]:
            todo.append((it, scripting))
    # An HTML parser's decoder strips a leading byte order mark; the references are given
    # characters, so it is stripped here (utf-8-sig) and not left as text for them to keep.
    reqs = [html_ref.request(reference_text(it), scripting, None) for it, scripting in todo]
    answers = html_ref.ask_all(reqs) if reqs else {name: [] for name in html_ref.REFERENCES}
    entries = []
    refs_failed = []
    for k, (it, scripting) in enumerate(todo):
        ident = it["id"] + ("@script" if scripting else "")
        dumps = []
        for name in html_ref.REFERENCES:
            a = answers[name][k]
            if "dump" not in a:
                refs_failed.append((ident, name, a.get("error", "?")))
                dumps.append("| <reference-error>")
            else:
                dumps.append(a["dump"])
        entries.append((ident, [it["source"], it["bytes"], dumps[0].encode("utf-8"), dumps[1].encode("utf-8")]))
    blob = frames([(ident + "#%d" % n, data) for ident, group in entries for n, data in enumerate(group)])
    # The frame ids for the four parts of one record are made distinct above; ours reports the id of the second.
    raw = run_ours(runner, ["compare"], blob)
    results = {}
    for line in raw.decode("ascii").split("\n"):
        if line:
            ident = line.split(" ")[1].rsplit("#", 1)[0]
            results[ident] = parse_cmp(line.encode("ascii") + b"\n")[line.split(" ")[1]]
    out = []
    missing = [ident for _it, ident in ((t[0], e[0]) for t, e in zip(todo, entries)) if ident not in results]
    if missing:
        sys.stderr.write("\033[0;31m\n### The harness gave no comparison for %d records ###\033[0m\n  %s\n"
                         % (len(missing), ", ".join(missing[:10])))
        raise SystemExit(3)
    for (it, scripting), (ident, _group) in zip(todo, entries):
        outcome, cat, detail = classify(results[ident])
        out.append((it, scripting, ident, outcome, cat, detail, results[ident]))
    return out, refs_failed


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
                raise SystemExit("%s:%d: not <key> <signature> <class> <reason>" % (path, n))
            key, sig, cls, reason = parts
            if cls not in CLASSES:
                raise SystemExit("%s:%d: %r is not a class (%s)" % (path, n, cls, ", ".join(CLASSES)))
            if not reason.strip():
                raise SystemExit("%s:%d: %s has no reason" % (path, n, key))
            if key in table:
                raise SystemExit("%s:%d: %s is listed twice" % (path, n, key))
            table[key] = (sig, cls, reason)
    return table


# Why one reference alone differs from the source on a tree with no hazards, when that is
# something already read and understood. Each rule is a cause established by the pinned
# vectors (html_deviations.txt) or by reading the trees, never "this reference is odd":
#
#   template   html5lib 1.1 has no template insertion mode (it moves a template out of the head
#              and into tables, and keeps its contents as children): every template.dat vector
#              it fails, and it is the only reference that does
#   select-hr  html5lib 1.1 drops an hr inside a select, which the standard has allowed since
#              2020; parse5 reads it as the standard does
#   ruby       html5lib 1.1 implements the ruby rules from before their 2018 revision
#   command    html5lib 1.1 still treats command as a void element; the standard dropped it
#   summary    html5lib 1.1 does not count summary among the special elements, so a dt, dd or li
#              inside a summary is still found through it and ended; the standard stops at it
#
# A rule decides only for the reference it names, and only when the other reference read the
# tree as written. A cause not here is an unclassified disagreement and fails the run.
def _has(*names):
    return lambda ctx: any(n in ctx for n in names)


KNOWN_CAUSES = (
    ("html5lib", "template", _has("template")),
    ("html5lib", "select-hr", lambda ctx: "select" in ctx and "hr" in ctx),
    ("html5lib", "ruby", _has("ruby", "rb", "rt", "rtc", "rp")),
    ("html5lib", "command", _has("command")),
    ("html5lib", "summary", lambda ctx: "summary" in ctx and any(n in ctx for n in ("dt", "dd", "li"))),
)


def known_both_cause(cat, detail):
    """The cause of a difference that BOTH references share on a tree with no hazards, when it is one understood
    and the standard decides: feDropShadow is in the standard's table of SVG element names whose case an HTML
    parser restores, and neither reference's copy of the table has it, so both read `feDropShadow` as
    `fedropshadow`. (The writer's vocabulary, markup_html.c, took the entry from the standard in stage 3e; it had
    been copied from parse5, which lacks it.)"""
    if cat != "element name":
        return None
    # `path what: ours A, theirs B` is what html_equivalent.h writes: the path of the element where the trees
    # first differ, the kind of difference (the category, checked above) and the two names.
    m = re.match(r"(\S+) element name: ours (\S+), theirs (\S+)$", detail)
    if not m or re.sub(r"\[\d+\]", "", m.group(1)).rsplit("/", 1)[-1] != "svg":
        return None
    return "feDropShadow" if (m.group(2), m.group(3)) == ("feDropShadow", "fedropshadow") else None


def known_cause(outcome, ctx):
    """The cause of a one-reference difference, decided from the names of the elements in the subtrees
    where the two trees first differ (not from the whole document, where an unrelated element could
    stand in for the real cause)."""
    for who, name, test in KNOWN_CAUSES:
        if outcome == who and test(ctx):
            return name
    return None


def show(text, limit=400):
    return text if len(text) <= limit else text[:limit] + "..."


def main(argv):
    runner = None
    count, seed = DEFAULT_COUNT, DEFAULT_SEED
    plant = None
    propose = False
    explain = 0
    corpus = True
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--plant":
            plant = args.pop(0)
            if plant not in PLANTS:
                raise SystemExit("--plant: one of %s" % ", ".join(PLANTS))
        elif a == "--propose":
            propose = True
        elif a == "--explain":
            explain = int(args.pop(0))
        elif a == "--no-corpus":
            corpus = False
        elif runner is None:
            runner = a
        else:
            raise SystemExit("unexpected argument %r" % a)
    if runner is None:
        raise SystemExit(__doc__)
    count = int(os.environ.get("HTML_ORACLE_COUNT", count))
    seed = int(os.environ.get("HTML_ORACLE_SEED", seed))
    hunting = (count, seed) != (DEFAULT_COUNT, DEFAULT_SEED) or not corpus

    items = population(runner, count, seed, plant, corpus)
    results, refs_failed = evaluate(runner, items)

    # ---- the population ----
    refusals = collections.Counter()
    by_source = collections.Counter()
    accepted = collections.Counter()
    hazard_free = collections.Counter()
    for it in items:
        src = it["id"].split(":", 1)[0]
        by_source[src] += 1
        if it["status"] == "OK":
            accepted[src] += 1
            hazard_free[src] += it["hazards"] == 0
        else:
            refusals[(src, it["status"], it["message"] or it["path"])] += 1
    print("trees: " + ", ".join("%s %d (accepted %d, no hazards %d)" % (s, by_source[s], accepted[s], hazard_free[s])
                                for s in sorted(by_source)))
    refused_total = collections.Counter()
    for (src, status, why), n in refusals.items():
        refused_total[(status, why)] += n
    print("refused, by reason: " + "; ".join("%d x %s: %s" % (n, s, w[:70]) for (s, w), n in
                                              sorted(refused_total.items(), key=lambda kv: -kv[1])[:12]))
    shapes = collections.defaultdict(collections.Counter)
    for it in items:
        if it["id"].startswith("gen:"):
            base = it["tags"].split("/")[0]
            shapes[base]["trees"] += 1
            shapes[base]["accepted"] += it["status"] == "OK"
            shapes[base]["no hazards"] += it["status"] == "OK" and it["hazards"] == 0
    if shapes:
        print("generated, by shape: " + "; ".join(
            "%s %d (accepted %d, no hazards %d)" % (k, v["trees"], v["accepted"], v["no hazards"])
            for k, v in sorted(shapes.items())))
    kinds = collections.Counter(it["kind"] for it in items if it["status"] == "OK" and it["hazards"])
    print("first hazard of the accepted trees that have one: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items())))

    problems = []
    gen_total = by_source["gen"]
    if (count, seed) == (DEFAULT_COUNT, DEFAULT_SEED) and not plant:
        if accepted["gen"] < MIN_ACCEPTED_SHARE * gen_total:
            problems.append("the writer accepts %d of %d generated trees (floor %d%%)" % (
                accepted["gen"], gen_total, 100 * MIN_ACCEPTED_SHARE))
        if hazard_free["gen"] < MIN_HAZARD_FREE_SHARE * gen_total:
            problems.append("only %d of %d generated trees are accepted without hazards (floor %d%%)" % (
                hazard_free["gen"], gen_total, 100 * MIN_HAZARD_FREE_SHARE))
        if corpus and (accepted["xmlconf"] < MIN_CORPUS_ACCEPTED or accepted["wpt"] < MIN_CORPUS_ACCEPTED):
            problems.append("the corpora yield %d (xmlconf) and %d (wpt) accepted trees (floor %d each)" % (
                accepted["xmlconf"], accepted["wpt"], MIN_CORPUS_ACCEPTED))
    # Records that were never compared: said out loud, and pinned for the corpora (a corpus that
    # silently stopped being built would score clean on less).
    unbuilt = collections.Counter()
    for it in items:
        src = it["id"].split(":", 1)[0]
        if it["status"] in ("NOTPARSED", "UNBUILDABLE"):
            unbuilt[(src, it["status"])] += 1
        if it.get("ambiguous"):
            unbuilt[(src, "AMBIGUOUS-DUMP")] += 1
    print("not compared: " + (", ".join("%s %s %d" % (a, b, n) for (a, b), n in sorted(unbuilt.items())) or "none"))
    for (src, status), n in unbuilt.items():
        if src in ("gen", "fixed"):
            problems.append("%d %s records of the %s population were not compared" % (n, status, src))
    if corpus and not hunting_run(count, seed):
        if dict(unbuilt) != {k: v for k, v in CORPUS_PINS.items()}:
            problems.append("the corpora's uncompared records are %r, pinned %r" % (dict(unbuilt), CORPUS_PINS))
    latin_high = sum(1 for it in items if "/latin1" in it["tags"] and it["status"] == "OK"
                     and re.search(rb"[\x80-\xff]", it["bytes"]))
    fixed_free = sum(1 for it in items if it["id"].startswith("fixed:") and "-folded:" not in it["id"]
                     and it["status"] == "OK" and it["hazards"] == 0)
    print("ISO-8859-1 records holding bytes above 0x7F: %d; table entries written without hazards: %d"
          % (latin_high, fixed_free))
    if (count, seed) == (DEFAULT_COUNT, DEFAULT_SEED) and not plant:
        if latin_high < MIN_LATIN_HIGH:
            problems.append("only %d ISO-8859-1 records hold a byte above 0x7F (floor %d)" % (latin_high, MIN_LATIN_HIGH))
    folded_ok = [it["id"] for it in items if "-folded:" in it["id"] and it["status"] == "OK" and it["hazards"] == 0]
    if folded_ok:
        sys.stderr.write("\033[0;31m\n### The writer calls the folded form of a table name safe (%d) ###\033[0m\n  %s\n"
                         "  (HTML restores the entry, which is another name)\n" % (len(folded_ok), ", ".join(folded_ok[:6])))
        return 3
    if fixed_free < MIN_FIXED_FREE:
        problems.append("only %d table entries were written without hazards (floor %d)" % (fixed_free, MIN_FIXED_FREE))
    if problems:
        sys.stderr.write("\033[0;31m\n### The population has drifted ###\033[0m\n  " + "\n  ".join(problems) + "\n")
        return 3

    # ---- the verdicts ----
    outcomes = collections.Counter()
    xml_defects = []
    free_defects = []
    free_disagreements = []
    hazardous = collections.defaultdict(list)
    sourcefail = []
    unanswered = []
    for it, scripting, ident, outcome, cat, detail, raw in results:
        free = it["hazards"] == 0
        outcomes[("no hazards" if free else "hazards", outcome)] += 1
        if outcome == "same":
            continue
        if outcome == "unanswered":
            unanswered.append((ident, cat, detail))
        elif outcome == "sourcefail":
            sourcefail.append((ident, detail))
        elif outcome == "xml-defect":
            xml_defects.append((ident, cat, detail, it))
        elif free and outcome == "both" and known_both_cause(cat, detail):
            free_disagreements.append((ident, outcome, cat, detail, it, raw))
        elif free and outcome == "both":
            free_defects.append((ident, cat, detail, it, raw))
        elif free:
            free_disagreements.append((ident, outcome, cat, detail, it, raw))
        else:
            hazardous[(it["kind"], outcome, cat)].append((ident, detail, it))
    total = collections.Counter()
    for (group, outcome), n in outcomes.items():
        total[group] += n
    for group in ("no hazards", "hazards"):
        print("%s: %d compared, %d read as written by both parsers; differ: %s" % (
            group, total[group], outcomes[(group, "same")],
            ", ".join("%s %d" % (o, n) for (g, o), n in sorted(outcomes.items()) if g == group and o != "same") or "none"))
    both_agree = sum(1 for _, _, _, _, _, _, raw in results if raw[0] == "OK" and raw[1][3][0] == 1)
    print("html5lib and parse5 read %d of %d trees alike (html_equivalent)" % (both_agree, len(results)))
    if free_disagreements:
        kinds = collections.Counter()
        first = {}
        for ident, outcome, cat, detail, it, raw in free_disagreements:
            k = "%s: %s" % (outcome, shape_of(cat, detail))
            kinds[k] += 1
            first.setdefault(k, (ident, detail))
        print("trees with no hazards on which one reference differs from the source, by kind:")
        for k, n in kinds.most_common(12):
            print("  %4d  %s   e.g. %s" % (n, k, first[k][0]))
    if refs_failed:
        print("a reference gave no answer for %d inputs, first: %r" % (len(refs_failed), refs_failed[0]))

    # ---- the list ----
    listed = {} if plant == "no-list" else load_list()
    found = {}
    for ident, outcome, cat, detail, it, raw in free_disagreements:
        cause = known_both_cause(cat, detail) if outcome == "both" else known_cause(
            outcome, raw[1][1 if outcome == "html5lib" else 2][3])
        key = "ref:%s:%s" % (outcome, cause) if cause else "ref:%s:unexplained:%s" % (outcome, ident)
        found.setdefault(key, (outcome, [], detail))[1].append(ident)
    for (kind, outcome, cat), rows in hazardous.items():
        # The kind of difference (`cat`) is in the detail the report prints, not in the key: the
        # same first hazard shows up as a different first difference from tree to tree.
        entry = found.setdefault("hazard:%s:%s" % (kind, outcome), (outcome, [], rows[0][1]))
        entry[1].extend(r[0] for r in rows)
    # The signature of a count: n, and for a cause shared by both references also which trees (their ids are the
    # fixed table entries, so a change of the population or of the cause shows).
    sigs = {k: ("n=%d" % len(v[1])) if not k.startswith("ref:both:") else
            "n=%d;%s" % (len(v[1]), ",".join(sorted(set(v[1])))) for k, v in found.items()}
    if plant == "stale-entry" and listed:
        victim = sorted(k for k in listed if k in found)[0]
        del found[victim]
        del sigs[victim]
    if plant == "change-signature" and listed:
        victim = sorted(k for k in listed if k in found)[0]
        listed[victim] = (listed[victim][0] + "0",) + listed[victim][1:]

    unlisted = sorted(k for k in found if k not in listed)
    # An entry whose count is `hunt` was seen only on larger populations (a hunt, with another
    # seed): it need not occur in the default one, and if it does its count must be written.
    stale = sorted(k for k in listed if k not in found and listed[k][0] != "hunt")
    changed = sorted(k for k in listed if k in found and sigs[k] != listed[k][0])
    if hunting:
        # The list describes the default population; a hunt finds classes and counts of its own.
        # What it must not find is a class nothing explains.
        stale, changed = [], []
    classes = collections.Counter(listed[k][1] for k in found if k in listed)
    print("divergence classes: %d found, %d listed (%s), %d unlisted, %d stale, %d with a changed count" % (
        len(found), sum(1 for k in found if k in listed),
        ", ".join("%s %d" % kv for kv in sorted(classes.items())) or "none", len(unlisted), len(stale), len(changed)))

    if propose:
        for k in sorted(found):
            outcome, ids, detail = found[k]
            print("%s\tn=%d\t%s\t%s" % (k, len(ids), "CLASS", "READ ME | e.g. %s: %s" % (ids[0], show(detail, 160))))
        return 0

    failed = False
    if unanswered:
        failed = True
        sys.stderr.write("\033[0;31m\n### Comparisons that could not be made (%d) ###\033[0m\n" % len(unanswered))
        for ident, who, why in unanswered[:10]:
            sys.stderr.write("  %s (%s): %s\n" % (ident, who, why[:200]))
    if refs_failed:
        failed = True
        sys.stderr.write("\033[0;31m\n### A reference could not answer (%d) ###\033[0m\n" % len(refs_failed))
        for ident, name, why in refs_failed[:10]:
            sys.stderr.write("  %s %s: %s\n" % (ident, name, why[:200]))
    if free_defects:
        failed = True
        sys.stderr.write("\033[0;31m\n### Trees with no hazards that an HTML parser reads differently (%d) ###\033[0m\n" % len(free_defects))
        # Grouped by how they differ, so that one defect with many instances reads as one.
        groups = collections.OrderedDict()
        for row in free_defects:
            ident, cat, detail, it, raw = row
            groups.setdefault(shape_of(cat, detail), []).append(row)
        sys.stderr.write("  %d kinds of difference: %s\n" % (len(groups), "; ".join(
            "%s x%d" % (k, len(v)) for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:12])))
        for key, rows in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:(explain or 8)]:
            ident, cat, detail, it, raw = rows[0]
            sys.stderr.write("  %s x%d  e.g. %s\n    %s\n    wrote %r\n" % (
                key, len(rows), ident, show(detail, 300), it["bytes"][:400]))
    if xml_defects:
        failed = True
        sys.stderr.write("\033[0;31m\n### The XML parser does not read back what was written (%d) ###\033[0m\n" % len(xml_defects))
        for ident, cat, detail, it in xml_defects[:15]:
            sys.stderr.write("  %s  [%s] %s\n    wrote %r\n" % (ident, cat, show(detail, 300), it["bytes"][:300]))
    if sourcefail:
        failed = True
        sys.stderr.write("\033[0;31m\n### Sources that could not be rebuilt (%d) ###\033[0m\n" % len(sourcefail))
        for ident, detail in sourcefail[:10]:
            sys.stderr.write("  %s %s\n" % (ident, detail))
    if unlisted:
        failed = True
        sys.stderr.write("\033[0;31m\n### Divergences with no entry in tools/oracle/html_divergences.txt (%d) ###\033[0m\n" % len(unlisted))
        for k in unlisted[:40]:
            outcome, ids, detail = found[k]
            sys.stderr.write("  %s  n=%d\n    e.g. %s: %s\n" % (k, len(ids), ids[0], show(detail, 300)))
    if stale:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed divergences that are gone ###\033[0m\n")
        for k in stale[:40]:
            sys.stderr.write("  %s  [%s] %s\n" % (k, listed[k][0], listed[k][2][:100]))
    if changed:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed divergences whose count has changed ###\033[0m\n")
        for k in changed[:40]:
            sys.stderr.write("  %s\n    listed [%s] now [%s]\n" % (k, listed[k][0], sigs[k]))
    if failed:
        return 3
    print("html oracle: no tree without hazards is read differently, and every divergence is listed, none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
