#!/usr/bin/env python3
"""The HTML tree builder against two HTML parsers.

    html_tree_diff.py <tree-runner> [--count N] [--seed S] [--plant KIND] [--propose]
                                    [--no-corpus] [--explain N]

The tree builder (src/html/html_tree*.c, stage 3c) is scored on the standard's own vectors by
`make conformance-html-tree`. This asks the same question of a population nobody wrote: documents
generated from the vocabulary of the constructs the stage implements, mutated documents, and the
inputs of the vectors, each put to **html5lib 1.1 and parse5 8.0.1** (the pinned image,
containers/html) and to this library, with the scripting flag off (and on where the document holds
a `noscript`). The three answers are trees in the tree-construction dump format; ours comes from
the runner of tools/conformance/html_tree_suite.c, theirs from the drivers, and nothing is
converted.

The population, each document tagged with where it came from:

  wpt    the input of every in-scope initial-document vector that the tree builder parses (one
         that stops at a construct of a later stage is not asked: it is counted)
  extra  the hand-chosen cases of tools/conformance/html_tree_extra.dat
  gen    generated documents: well-formed-ish nesting of blocks, lists, headings, formatting
         elements (with attributes, for the Noah's Ark clause), void elements, raw text
         elements, head elements, framesets, forms, buttons, markers, comments, text with
         references and white space, end tags of anything in any place
  mut    those, with tokens deleted, duplicated, swapped, inserted, or the document cut

**The generator avoids the constructs of later stages** (tables, template, svg, math) and the
ones the references parse by the older rules (select; processing instructions, which neither
reference has): a document that reaches one is counted as "stopped", and the run fails if that is
more than a small share of the generated ones, so that the generator cannot drift out of the
implemented constructs unnoticed.

**What fails the run.** A document on which this library's tree differs from a reference's is a
divergence, and its *kind* is the pair (reference, signature), the signature being why: the
elements the reference reads by older rules (verified: removing them makes the two agree), or, for
html5lib alone with parse5 agreeing with this library, its older adoption agency rules. Every kind is a line of tools/oracle/html_tree_divergences.txt with a count and
a reason (a reference older than the standard, or the standard decides), or the run fails on

  * a divergence of a kind with no entry,
  * an entry that no longer occurs in the default population (stale) - except the ones counted
    `hunt`, which only larger populations reach,
  * an entry whose count has changed (the count is the signature of the population),
  * a reference that could not answer, or a population that has drifted (too few documents).

A divergence that BOTH references share and this library does not is listed under `both`: those
are the cases where the standard has moved past both references, and each has a reason.

`--plant KIND` corrupts the run on purpose and `run-html-oracle.sh --controls` requires it to
fail for each; a differential that cannot be seen to fail reports the same clean line as one
that passes. The tree builder itself is planted by compiling it with one rule undone (the
controls of run-html-oracle.sh).

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "conformance"))

import html_ref  # noqa: E402
import html_tree_suite as suite  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
LIST = os.path.join(HERE, "html_tree_divergences.txt")
EXTRA = os.path.join(ROOT, "tools", "conformance", "html_tree_extra.dat")
WPT = os.path.join(ROOT, "third_party", "html", "wpt", "html", "syntax", "parsing", "resources")
EXCEPTIONS = os.path.join(ROOT, "tools", "conformance", "HTML_TREE_EXCEPTIONS")
DEFAULT_COUNT, DEFAULT_SEED = 6000, 1
MIN_DOCUMENTS = 8000
PLANTS = {
    "ours-extra-line": "one of our trees gets a line it should not have",
    "ours-lost-line": "one of our trees loses a line",
    "stale-entry": "the list gets an entry for a divergence that does not occur",
    "change-count": "the count of one listed entry is changed",
    "no-list": "the list is empty",
}

# ---- the generator ----------------------------------------------------------------------------

BLOCKS = ["div", "p", "section", "article", "aside", "blockquote", "center", "details", "dialog", "dir",
          "fieldset", "figure", "figcaption", "footer", "header", "hgroup", "main", "menu", "nav", "search",
          "summary", "address"]
HEADINGS = ["h1", "h2", "h3", "h4", "h5", "h6"]
LISTS = ["ul", "ol", "dl", "li", "dd", "dt"]
FORMATTING = ["a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"]
INLINE = ["span", "sub", "sup", "label", "abbr", "cite", "q", "mark", "time", "var", "kbd", "samp", "bdi", "bdo"]
VOID = ["br", "img", "input", "area", "embed", "wbr", "param", "source", "track", "keygen", "hr"]
MARKERS = ["marquee", "object", "applet"]
RAW = ["textarea", "xmp", "iframe", "noembed", "pre", "listing"]
PLAIN = ["plaintext"]
HEADISH = ["title", "meta", "link", "base", "script", "style", "noscript", "noframes", "head", "body", "html",
           "basefont", "bgsound"]
RUBY = ["ruby", "rb", "rt", "rp", "rtc"]
FRAMES = ["frameset", "frame"]
FORMS = ["form", "button", "legend", "optgroup", "option", "caption", "col", "colgroup", "td", "tr", "th",
         "tbody", "thead", "tfoot"]  # the table ones are ignored in the body; none enters a table mode
UNKNOWN = ["foo", "bar", "x-y", "a$b"]
ATTRS = ["", "", "", " class=a", " class=b", " id=x", " a", " a b", " b a", " href=u", " type=hidden",
         " data-x=\"1 2\"", " class=a id=x", " id=x class=a", " style='x:y'"]
TEXTS = ["x", "y z", " ", "\n", "a&amp;b", "&lt;", "&#65;", "t\tu", "  ", "\n\n", "&nbsp;", "&copy", "q"]
COMMENTS = ["<!-- c -->", "<!---->", "<!--a-->"]
DOCTYPES = ["<!DOCTYPE html>", "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\">",
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\" \"http://x\">", "<!doctype HTML>",
            "<!DOCTYPE>", "<!DOCTYPE foo>"]
ALL_NAMES = BLOCKS + HEADINGS + LISTS + FORMATTING + INLINE + MARKERS + RAW + HEADISH + RUBY + FORMS + UNKNOWN
# Inputs the references cannot judge: select (they parse it by the rules the standard has replaced),
# and processing instructions (neither has the states). The vectors that hold them are the
# conformance gate's.
UNASKABLE_RE = re.compile(r"<\s*/?\s*select\b|<\?", re.I)


class Gen:
    def __init__(self, rng):
        self.r = rng

    def pick(self, seq):
        return seq[self.r.randrange(len(seq))]

    def start(self, name):
        return "<%s%s>" % (name, self.pick(ATTRS) if name not in ("input",) else self.pick(ATTRS + [" type=hidden"]))

    def end(self, name):
        return "</%s>" % name

    def token(self):
        r = self.r.random()
        if r < 0.20:
            return self.pick(TEXTS)
        if r < 0.40:
            return self.start(self.pick(FORMATTING))
        if r < 0.50:
            return self.end(self.pick(FORMATTING))
        if r < 0.62:
            return self.start(self.pick(BLOCKS + HEADINGS))
        if r < 0.68:
            return self.end(self.pick(BLOCKS + HEADINGS))
        if r < 0.76:
            return self.start(self.pick(LISTS))
        if r < 0.80:
            return self.end(self.pick(LISTS))
        if r < 0.84:
            return self.start(self.pick(VOID + ["image"]))
        if r < 0.87:
            return self.start(self.pick(INLINE + UNKNOWN))
        if r < 0.89:
            return self.end(self.pick(INLINE + UNKNOWN))
        if r < 0.91:
            return self.start(self.pick(MARKERS))
        if r < 0.92:
            return self.end(self.pick(MARKERS))
        if r < 0.93:
            return self.pick(COMMENTS)
        if r < 0.94:
            return self.start(self.pick(FORMS))
        if r < 0.95:
            return self.end(self.pick(FORMS))
        if r < 0.96:
            return self.start(self.pick(RAW)) + self.pick(TEXTS) + self.end(self.pick(RAW))
        if r < 0.97:
            return self.start(self.pick(HEADISH))
        if r < 0.98:
            return self.end(self.pick(HEADISH))
        if r < 0.99:
            return self.start(self.pick(RUBY + FRAMES))
        return self.end(self.pick(RUBY + FRAMES))

    def nested(self, depth):
        """Mostly well-formed nesting with some end tags missing or in the wrong order."""
        out = []
        for _ in range(self.r.randint(1, 4)):
            r = self.r.random()
            if r < 0.3 or depth > 4:
                out.append(self.pick(TEXTS) if self.r.random() < 0.7 else self.token())
            else:
                name = self.pick(FORMATTING * 3 + BLOCKS + HEADINGS + LISTS + INLINE + MARKERS)
                inner = self.nested(depth + 1)
                close = self.r.random()
                if close < 0.6:
                    out.append(self.start(name) + inner + self.end(name))
                elif close < 0.8:
                    out.append(self.start(name) + inner)
                else:
                    other = self.pick(FORMATTING + BLOCKS)
                    out.append(self.start(name) + inner + self.end(other))
        return "".join(out)

    def document(self):
        r = self.r.random()
        head = ""
        if self.r.random() < 0.4:
            head = self.pick(DOCTYPES)
        if r < 0.55:
            body = "".join(self.token() for _ in range(self.r.randint(1, 24)))
        elif r < 0.90:
            body = self.nested(0)
            if self.r.random() < 0.5:
                body += "".join(self.token() for _ in range(self.r.randint(1, 6)))
        else:
            pre = "".join(self.pick(["<html%s>" % self.pick(ATTRS), "<head>", "</head>", "<body%s>" % self.pick(ATTRS),
                                      self.pick(COMMENTS), " ", "<title>t</title>", "<frameset>", "<frame>",
                                      "<noscript>", "</noscript>", "<style>x</style>", "<script>y</script>"])
                          for _ in range(self.r.randint(1, 6)))
            body = pre + "".join(self.token() for _ in range(self.r.randint(0, 10)))
        return head + body


TOKEN_RE = re.compile(r"<[^>]*>|[^<]+|<")


def mutate(rng, text, gen):
    toks = TOKEN_RE.findall(text) or [text]
    for _ in range(rng.randint(1, 3)):
        k = rng.randrange(6)
        i = rng.randrange(len(toks))
        if k == 0 and len(toks) > 1:
            del toks[i]
        elif k == 1:
            toks.insert(i, toks[i])
        elif k == 2 and len(toks) > 1:
            j = rng.randrange(len(toks))
            toks[i], toks[j] = toks[j], toks[i]
        elif k == 3:
            toks.insert(i, gen.token())
        elif k == 4:
            toks = toks[:i + 1]
        else:
            toks[i] = gen.token()
    return "".join(toks)


# ---- asking -------------------------------------------------------------------------------------

def hexs(s):
    return "x" + s.encode("utf-8", "surrogatepass").hex()


def ours_batch(runner, items):
    """[(status, notyet, dump)] for each (html, scripting)."""
    answers = []
    step = 400
    for lo in range(0, len(items), step):
        part = items[lo:lo + step]
        reqs = ["whole %d %s" % (1 if sc else 0, hexs(h)) for h, sc in part]
        got, _ = suite.run_batch(runner, reqs)
        if got is None:
            # isolate a crash
            got = []
            for r in reqs:
                one, _ = suite.run_batch(runner, [r])
                got.append(one[0] if one else ["STATUS CRASH", "END CRASH"])
        for lines in got:
            a = suite.parse_answer(lines)
            answers.append((a["status"], a["notyet"], a["dump"], a["inv"]))
    return answers


def first_diff(a, b):
    la, lb = a.split("\n"), b.split("\n")
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else ""
        y = lb[i] if i < len(lb) else ""
        if x != y:
            return i, la, lb
    return None, la, lb


LINE_NAME = re.compile(r'^\|( *)(.*)$')


def line_info(lines, i):
    """(kind, names on the path from the root to the line, the line's own name)."""
    if i >= len(lines):
        return "end", [], ""
    m = LINE_NAME.match(lines[i])
    indent = len(m.group(1)) if m else 0
    body = m.group(2) if m else lines[i]

    def kind_name(b):
        if b.startswith("<!--"):
            return "comment", ""
        if b.startswith("<!DOCTYPE"):
            return "doctype", ""
        if b.startswith("<?"):
            return "pi", ""
        if b.startswith("<"):
            return "element", b[1:-1].split(" ")[-1] if b.endswith(">") else b
        if b.startswith('"'):
            return "text", ""
        return "attribute", b.split("=")[0]

    kind, own = kind_name(body)
    path = []
    want = indent
    for j in range(i - 1, -1, -1):
        mj = LINE_NAME.match(lines[j])
        if not mj:
            continue
        ind = len(mj.group(1))
        if ind < want:
            k, n = kind_name(mj.group(2))
            if k == "element":
                path.append(n)
            want = ind
            if ind == 0:
                break
    return kind, path, own


def coarse(ours, theirs):
    """The kinds of the two first-differing lines: used only when no element name explains a divergence."""
    i, la, lb = first_diff(ours, theirs)
    ka, _, _ = line_info(la, i)
    kb, _, _ = line_info(lb, i)
    return "%s/%s" % (ka, kb)


NAME_RE = re.compile(r"<([A-Za-z][A-Za-z0-9$-]*)")

# Elements a reference reads by rules older than the standard's (or does not know). A divergence
# is explained by them only if removing every one of them from the document makes the two agree;
# the list is the classification, not a licence: a divergence in a document without one, or one
# that survives their removal, is unexplained and fails the run. html5lib 1.1 is from 2020: it
# has no `search`, treats `summary`, `main`, `hgroup`, `figcaption` and `dialog` as ordinary
# elements in places the standard makes special or block, keeps `isindex`, `command` and
# `menuitem`, and has the older ruby and frameset rules. parse5 8.0.1 has everything but `search`.
KNOWN_OLD = {
    "html5lib": ["search", "summary", "dialog", "isindex", "ruby", "rb", "rt", "rp", "rtc", "main", "hgroup",
                 "figcaption", "command", "menuitem", "menu", "frameset"],
    "parse5": ["search"],
}
KNOWN_OLD["both"] = KNOWN_OLD["parse5"]
KNOWN_OLD["three-way"] = KNOWN_OLD["parse5"]


def without(html, names):
    pattern = "|".join(re.escape(n) for n in sorted(names))
    return re.sub(r"</?(?:%s)(?=[\s/>])[^>]*>?" % pattern, "", html, flags=re.I)


def explain_all(runner, divergent):
    """Key each divergence (item, who, theirs-dump, parse5-dump), and verify the explanation by asking again.

    The explanation is a set of elements the reference reads by older rules (KNOWN_OLD): the ones the
    document holds are removed, and the divergence is explained only if this builder and that reference then
    agree. `who` is html5lib (alone: parse5 agrees with this builder), parse5, both (both differ from this
    builder and agree with each other) or three-way (both differ from it and from each other; judged
    against parse5). An explained divergence is keyed by the first such element the document holds; one
    the removal does not explain is `unverified` (html5lib alone: its older adoption agency and Noah's Ark
    rules, which no element name stands for) or `unexplained` and fails the run."""
    keys = [""] * len(divergent)
    queries = []
    for idx, (it, who, theirs, p5) in enumerate(divergent):
        present = set(n.lower() for n in NAME_RE.findall(it["html"]))
        cand = [n for n in KNOWN_OLD[who] if n in present]
        if cand:
            queries.append((idx, cand, without(it["html"], cand), it["scripting"]))
    if queries:
        ours = ours_batch(runner_global, [(q[2], q[3]) for q in queries])
        answers = html_ref.ask_all([html_ref.request(q[2], q[3]) for q in queries])
        for (idx, cand, v, sc), (status, notyet, dump, inv), a5, p5 in zip(
                queries, ours, answers["html5lib"], answers["parse5"]):
            who = divergent[idx][1]
            ref = a5 if who == "html5lib" else p5
            mine = dump[:-1] if dump.endswith("\n") else dump
            if status == "OK" and "dump" in ref and ref["dump"] == mine:
                keys[idx] = cand[0]
    for idx, (it, who, theirs, p5) in enumerate(divergent):
        if not keys[idx]:
            keys[idx] = "unverified" if who == "html5lib" and p5 == it["ours"] else \
                "unexplained/" + coarse(it["ours"], theirs)
    return keys


def load_list(path):
    table = {}
    if not os.path.exists(path):
        return table
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#"):
                continue
            parts = raw.split("\t")
            if len(parts) != 4 or not parts[3].strip():
                raise SystemExit("%s:%d: not <key> <count> <class> <reason>" % (path, n))
            key, count, cls, reason = parts
            if cls not in ("reference-old", "reference-wrong", "standard-newer"):
                raise SystemExit("%s:%d: %r is not a class" % (path, n, cls))
            if count != "hunt" and not count.isdigit():
                raise SystemExit("%s:%d: count must be a number or `hunt`" % (path, n))
            if key in table:
                raise SystemExit("%s:%d: %s is listed twice" % (path, n, key))
            table[key] = (count, cls, reason)
    return table


def population(runner, count, seed, corpus):
    rng = random.Random(seed)
    gen = Gen(rng)
    items = []  # dicts: id, html, scripting
    if corpus:
        vectors, _, _ = suite.load(WPT)
        listed = suite.load_exceptions(EXCEPTIONS)
        for v in vectors:
            if v["fragment"] or v["id"] in listed or UNASKABLE_RE.search(v["data"]):
                continue
            settings = [v["scripting"]] if v["scripting"] is not None else [False]
            if v["scripting"] is None and "noscript" in v["data"]:
                settings = [False, True]
            for sc in settings:
                items.append({"id": "wpt:%s" % v["id"], "html": v["data"], "scripting": bool(sc)})
        for v in suite.parse_dat(EXTRA, "extra"):
            items.append({"id": "extra:%s" % v["id"], "html": v["data"], "scripting": v["scripting"] is True})
    base = len(items)
    for i in range(count):
        h = gen.document()
        sc = bool(rng.getrandbits(1)) if "noscript" in h else False
        items.append({"id": "gen:%d" % i, "html": h, "scripting": sc})
    for i in range(count // 2):
        src = items[rng.randrange(len(items))]["html"] if corpus and rng.random() < 0.5 else items[base + rng.randrange(count)]["html"]
        h = mutate(rng, src, gen)
        sc = bool(rng.getrandbits(1)) if "noscript" in h else False
        items.append({"id": "mut:%d" % i, "html": h, "scripting": sc})
    return items


runner_global = None


def main(argv):
    runner = None
    count, seed = DEFAULT_COUNT, DEFAULT_SEED
    plant = None
    propose = False
    corpus = True
    explain = 0
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
        elif a == "--no-corpus":
            corpus = False
        elif a == "--explain":
            explain = int(args.pop(0))
        elif runner is None:
            runner = a
        else:
            raise SystemExit("unexpected argument %r" % a)
    if runner is None:
        raise SystemExit(__doc__)
    count = int(os.environ.get("HTML_TREE_ORACLE_COUNT", count))
    seed = int(os.environ.get("HTML_TREE_ORACLE_SEED", seed))
    hunting = (count, seed) != (DEFAULT_COUNT, DEFAULT_SEED) or not corpus

    items = population(runner, count, seed, corpus)
    ours = ours_batch(runner, [(it["html"], it["scripting"]) for it in items])
    stopped = collections.Counter()
    asked = []
    for it, (status, notyet, dump, inv) in zip(items, ours):
        src = it["id"].split(":")[0]
        if status == "E_NOT_YET" or status.startswith("E_LIMIT"):
            stopped[(src, status if status != "E_NOT_YET" else "not yet: %s" % notyet[0])] += 1
            continue
        if status != "OK":
            # A crash, an out-of-memory, an invalid-argument status: HTML parsing never fails for content.
            sys.stderr.write("\033[0;31m\n### The tree builder failed on a document (%s) ###\033[0m\n  %r\n" % (
                status, it["html"][:200]))
            return 3
        if inv != "OK":
            sys.stderr.write("\033[0;31m\n### The invariant checker failed ###\033[0m\n  %r: %s\n" % (it["html"][:120], inv))
            return 3
        it["ours"] = dump[:-1] if dump.endswith("\n") else dump
        asked.append(it)
    answers = html_ref.ask_all([html_ref.request(it["html"], it["scripting"]) for it in asked])

    if plant == "ours-extra-line" and asked:
        asked[0]["ours"] += "\n| <planted>"
    if plant == "ours-lost-line":
        for it in asked:
            if it["ours"].count("\n") > 3:
                it["ours"] = it["ours"].rsplit("\n", 1)[0]
                break

    by_source = collections.Counter(it["id"].split(":")[0] for it in items)
    asked_by_source = collections.Counter(it["id"].split(":")[0] for it in asked)
    print("documents: " + ", ".join("%s %d (compared %d)" % (s, by_source[s], asked_by_source[s]) for s in sorted(by_source)))
    print("stopped before a comparison (a construct of a later stage, or a limit): " +
          ("; ".join("%d x %s %s" % (n, s, w) for (s, w), n in sorted(stopped.items())) or "none"))

    kinds = collections.defaultdict(list)
    agree_refs = 0
    equal_html5lib = equal_parse5 = equal_both = 0
    refs_failed = []
    divergent = []  # (item, who, theirs, parse5's)
    for it, a5, p5 in zip(asked, answers["html5lib"], answers["parse5"]):
        if "dump" not in a5 or "dump" not in p5:
            refs_failed.append((it, a5.get("error"), p5.get("error")))
            continue
        agree_refs += a5["dump"] == p5["dump"]
        d_a = it["ours"] != a5["dump"]
        d_p = it["ours"] != p5["dump"]
        equal_html5lib += not d_a
        equal_parse5 += not d_p
        equal_both += (not d_a) and (not d_p)
        if d_a and d_p and a5["dump"] == p5["dump"]:
            divergent.append((it, "both", a5["dump"], p5["dump"]))
        else:
            if d_a and not d_p:
                divergent.append((it, "html5lib", a5["dump"], p5["dump"]))
            if d_p:
                divergent.append((it, "three-way" if d_a else "parse5", p5["dump"], p5["dump"]))
    global runner_global
    runner_global = runner
    causes = explain_all(runner, divergent)
    for (it, who, theirs, _), cause in zip(divergent, causes):
        kinds["%s:%s" % (who if who != "html5lib" else "html5lib:alone", cause)].append((it, theirs))
    n = len(asked)
    print("compared %d documents: ours equals html5lib on %d, parse5 on %d, both on %d; the references equal each other on %d" % (
        n, equal_html5lib, equal_parse5, equal_both, agree_refs))
    print("divergence kinds: %d, documents in them: %d" % (len(kinds), sum(len(v) for v in kinds.values())))

    listed = {} if plant == "no-list" else load_list(LIST)
    if plant == "stale-entry":
        listed["html5lib:element/element:planted-gone"] = ("1", "reference-old", "planted")
    if plant == "change-count":
        for k in sorted(listed):
            if listed[k][0] != "hunt":
                listed[k] = (str(int(listed[k][0]) + 1),) + listed[k][1:]
                break

    if propose:
        for k in sorted(kinds):
            print("%s\t%d\treference-old\tUNREAD" % (k, len(kinds[k])))
        return 0
    if explain:
        for k in sorted(kinds):
            for it, theirs in kinds[k][:explain]:
                print("== %s %s %r" % (k, it["id"], it["html"][:300]))
                print("ours:\n" + it["ours"] + "\ntheirs:\n" + theirs)
        return 0

    failed = False
    if refs_failed:
        failed = True
        sys.stderr.write("\033[0;31m\n### A reference could not answer ###\033[0m\n")
        for it, e1, e2 in refs_failed[:10]:
            sys.stderr.write("  %r: html5lib %s, parse5 %s\n" % (it["html"][:80], e1, e2))
    unlisted = sorted(k for k in kinds if k not in listed)
    if unlisted:
        failed = True
        sys.stderr.write("\033[0;31m\n### Divergences with no entry (tools/oracle/html_tree_divergences.txt) ###\033[0m\n")
        for k in unlisted[:30]:
            it, theirs = kinds[k][0]
            sys.stderr.write("  %s  x%d\n    e.g. %s %r\n" % (k, len(kinds[k]), it["id"], it["html"][:160]))
    gone = sorted(k for k in listed if k not in kinds and listed[k][0] != "hunt")
    if gone:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed divergences that are gone ###\033[0m\n")
        for k in gone:
            sys.stderr.write("  %s  %s\n" % (k, listed[k][2][:100]))
    if not hunting:
        changed = sorted(k for k in listed if k in kinds and listed[k][0] != "hunt" and int(listed[k][0]) != len(kinds[k]))
        if changed:
            failed = True
            sys.stderr.write("\033[0;31m\n### Listed divergences whose count has changed ###\033[0m\n")
            for k in changed:
                sys.stderr.write("  %s  listed %s, now %d\n" % (k, listed[k][0], len(kinds[k])))
    if not hunting:
        if len(items) < MIN_DOCUMENTS:
            failed = True
            sys.stderr.write("\033[0;31m\n### The population has shrunk: %d documents, floor %d ###\033[0m\n" % (len(items), MIN_DOCUMENTS))
        gen_stopped = sum(v for (s, w), v in stopped.items() if s in ("gen", "mut"))
        gen_total = by_source["gen"] + by_source["mut"]
        if gen_stopped * 50 > gen_total:
            failed = True
            sys.stderr.write("\033[0;31m\n### The generator left the implemented constructs: %d of %d generated documents stopped ###\033[0m\n" % (
                gen_stopped, gen_total))
    if failed:
        return 3
    print("html tree differential: every divergence is listed, none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
