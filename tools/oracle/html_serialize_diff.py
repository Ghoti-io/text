#!/usr/bin/env python3
"""The HTML serializer against two HTML serializers.

    html_serialize_diff.py <runner> [--count N] [--seed S] [--plant KIND] [--propose] [--explain N] [--no-corpus]

The serializer (src/html/html_serialize.c) is scored on html5lib's serializer vectors, and its round trip on the
standard's tree-construction vectors, by `make conformance-html-serializer`. This asks the question of a population
nobody wrote: **the same tree put to html5lib 1.1's serializer, parse5 8.0.1's and this library's**, in the pinned image
(containers/html), at the standard's settings, and the three strings compared byte for byte.

*The tree travels as ops, not as markup and not as the tree-construction dump.* A parse of the markup would make the
comparison a comparison of parsers (tools/oracle/html_tree_diff.py is that), and the dump sorts an element's attributes,
whose order is what a serializer writes. So this library parses the input (html_serialize_ours.c, `parse`), the tree it
built is turned into ops, and the three serializers are each given **that tree**: this library builds it again from the ops
(`ser`), the drivers (containers/html/serialize.py, serialize.mjs) build it as html5lib tokens and as a parse5 tree. Whatever
differs is then a difference between serializers.

The population, each tree tagged with where it came from:

  parsed   the documents and fragments of tools/oracle/html_tree_diff.py's population (the generated, mutated, foreign
           content and fragment documents, the in-scope tree-construction vectors, and the hand-chosen cases), parsed by this
           library, with the scripting flag the document was generated for
  built    trees no parser makes, generated here: void elements with children, a script that holds markup or its own end
           tag, a template with contents, elements in other namespaces with prefixes, an attribute of every namespace, a
           value with every character that has an escape, a comment with `-->`, processing instructions, document types
           with identifiers, deep and wide trees

Each tree is written two ways: the **children** of the root (a document, or the fragment), and the root's first child
**itself** (the document element, or a fragment's first node); parse5's `serialize` and `serializeOuter`, html5lib's
tokens for the children or for the node, and this library's CHILDREN and OUTER scopes.

**What fails the run.** A tree on which this library's output differs from a reference's is a divergence, and its *kind* is
the pair (reference, features): the features of the tree that explain it, found by taking them out. A feature is a
property of the tree (a no-break space in a text, a `<` in an attribute value, an attribute in a namespace, a
document type with identifiers, a `noscript`, ...) with a transformation that removes it (replaces the character, drops the
attribute, renames the element to `span`); a divergence is explained by a set of features when the tree with those removed
is written alike by this library and the reference, and the set is minimal. Every kind is a line of
tools/oracle/html_serialize_divergences.txt with a count and a reason (a reference older than the standard, or the standard
decides), or the run fails on

  * a divergence that no set of features explains (`unexplained`), or a kind with no entry,
  * an entry that no longer occurs in the default population (stale) - except those counted `hunt`,
  * an entry whose count has changed (the count is the signature of the population),
  * a reference that could not answer, or a population that has drifted (too few trees).

*The limit of the method*, and what stands in for it: a feature explains every divergence in a tree that has it, so a defect
of this library in the same construct could hide behind it. Every reference deviation therefore has a WITNESS: a small tree
and the output worked out by hand from the standard's text, which this library gives and the reference does not, checked on
every run.

`--plant KIND` corrupts the run on purpose and `run-html-serialize-oracle.sh --controls` requires it to fail for each; this
library's serializer is also compiled with one rule undone (the controls of that script).

Copyright 2026 by Corey Pennycuff
"""

import collections
import copy
import itertools
import json
import os
import random
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.setrecursionlimit(6000)  # a tree 250 deep is copied and written by recursive library code
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "conformance"))

import html_ref  # noqa: E402
import html_tree_diff as treediff  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
LIST = os.path.join(HERE, "html_serialize_divergences.txt")
DRIVERS = {
    "html5lib": ["/opt/html-oracle/venv/bin/python3", os.path.join(HERE, "containers", "html", "serialize.py")],
    "parse5": ["node", os.path.join(HERE, "containers", "html", "serialize.mjs")],
}
DEFAULT_COUNT, DEFAULT_SEED = 3000, 1
MIN_TREES = 13000
PLANTS = {
    "ours-extra-char": "one of our outputs gets a character it should not have",
    "ours-lost-char": "one of our outputs loses a character",
    "stale-entry": "the list gets an entry for a divergence that does not occur",
    "change-count": "the count of one listed entry is changed",
    "no-list": "the list is empty",
    "witness": "a witness of a reference deviation does not hold",
}

NS_HTML = "http://www.w3.org/1999/xhtml"
NS_SVG = "http://www.w3.org/2000/svg"
NS_MATH = "http://www.w3.org/1998/Math/MathML"
NS_XLINK = "http://www.w3.org/1999/xlink"
NS_XML = "http://www.w3.org/XML/1998/namespace"
NS_XMLNS = "http://www.w3.org/2000/xmlns/"
NS_OTHER = "urn:example:other"


def hx(s):
    return "x" + s.encode("utf-8", "surrogatepass").hex()


def unhex(w):
    return None if w == "-" else bytes.fromhex(w[1:]).decode("utf-8", "surrogatepass")


def nul(s):
    return "-" if s is None else hx(s)


# ---- trees as JSON and as ops ------------------------------------------------------------------------------------
# {"k": "doc"|"frag", "c": [...]}; {"k": "e", "ns", "p", "n", "a": [{"ns","p","n","v"}], "c": [...], "t": [...]|None};
# {"k": "t"|"k"|"c", "d"}; {"k": "p", "t", "d"}; {"k": "d", "n", "pub", "sys"}

def ops_of(tree):
    out = ["R,doc" if tree["k"] == "doc" else "R,frag"]
    # iterative: a deep tree must not need the Python stack
    work = [("n", n) for n in reversed(tree["c"])]
    while work:
        what, n = work.pop()
        if what == "w":
            out.append(n)
            continue
        k = n["k"]
        if k == "e":
            fields = ["e", nul(n["ns"]), nul(n["p"]), hx(n["n"]), str(len(n["a"]))]
            for a in n["a"]:
                fields += [nul(a["ns"]), nul(a["p"]), hx(a["n"]), hx(a["v"])]
            out.append(",".join(fields))
            work.append(("w", "x"))
            for c in reversed(n["c"]):
                work.append(("n", c))
            if n["t"] is not None:
                work.append(("w", "]"))
                for c in reversed(n["t"]):
                    work.append(("n", c))
                work.append(("w", "["))
        elif k in ("t", "k", "c"):
            out.append("%s,%s" % (k, hx(n["d"])))
        elif k == "p":
            out.append("p,%s,%s" % (hx(n["t"]), hx(n["d"])))
        elif k == "d":
            out.append("d,%s,%s,%s" % (hx(n["n"]), nul(n["pub"]), nul(n["sys"])))
    return out


def tree_of_ops(ops):
    """Like tree_of, with the template contents made when `[` opens them."""
    root = {"k": "doc" if ops[0] == "R,doc" else "frag", "c": []}
    containers = [root["c"]]
    elements = []
    for w in ops[1:]:
        if w == "x":
            containers.pop()
            elements.pop()
        elif w == "[":
            el = elements[-1]
            el["t"] = []
            containers.append(el["t"])
        elif w == "]":
            containers.pop()
            # back to the element's own children
        else:
            f = w.split(",")
            if f[0] == "e":
                n = int(f[4])
                attrs = [{"ns": unhex(f[5 + 4 * i]), "p": unhex(f[6 + 4 * i]), "n": unhex(f[7 + 4 * i]),
                          "v": unhex(f[8 + 4 * i])} for i in range(n)]
                el = {"k": "e", "ns": unhex(f[1]), "p": unhex(f[2]), "n": unhex(f[3]), "a": attrs, "c": [], "t": None}
                containers[-1].append(el)
                elements.append(el)
                containers.append(el["c"])
            elif f[0] in ("t", "k", "c"):
                containers[-1].append({"k": f[0], "d": unhex(f[1])})
            elif f[0] == "p":
                containers[-1].append({"k": "p", "t": unhex(f[1]), "d": unhex(f[2])})
            elif f[0] == "d":
                containers[-1].append({"k": "d", "n": unhex(f[1]), "pub": unhex(f[2]), "sys": unhex(f[3])})
    return root


def outer_tree(tree):
    """The tree whose children are the root's first child alone (what OUTER writes), or None if there is none."""
    if not tree["c"]:
        return None
    first = tree["c"][0]
    # a document type is a child of a document only
    return {"k": "doc" if first["k"] == "d" else "frag", "c": [first]}


# ---- this library -------------------------------------------------------------------------------------------------

def run_batch(runner, requests):
    data = ("\n".join(requests) + "\n").encode("ascii")
    done = subprocess.run([runner], input=data, capture_output=True, timeout=1800)
    if done.returncode != 0:
        return None, done.stderr.decode("utf-8", "replace")[-1500:]
    answers, cur = [], []
    for line in done.stdout.decode("utf-8", "replace").split("\n"):
        if not line:
            continue
        cur.append(line)
        if line.startswith("END "):
            answers.append(cur)
            cur = []
    if len(answers) != len(requests):
        return None, "got %d answers for %d requests" % (len(answers), len(requests))
    return answers, ""


def ours_run(runner, requests, step=300):
    """[answer lines] for each request; a request that kills the runner is a failure of the run."""
    answers = []
    for lo in range(0, len(requests), step):
        part = requests[lo:lo + step]
        got, why = run_batch(runner, part)
        if got is None:
            for r in part:
                one, why1 = run_batch(runner, [r])
                if one is None:
                    raise SystemExit("this library's runner failed on %r:\n%s" % (r[:200], why1))
                answers.append(one[0])
        else:
            answers.extend(got)
    return answers


def ours_parse(runner, items):
    """Fill item["tree"] for each item {html, scripting, context}; returns the items whose parse answered."""
    reqs = []
    for it in items:
        reqs.append("parse %d %s%s" % (1 if it["scripting"] else 0, hx(it["html"]), (" ctx:" + hx(it["context"])) if it.get("context") else ""))
    kept, stopped = [], collections.Counter()
    for it, lines in zip(items, ours_run(runner, reqs)):
        tree_line = next((l for l in lines if l.startswith("TREE ")), None)
        status = next((l.split(" ")[1] for l in lines if l.startswith("STATUS ")), None)
        if tree_line is None:
            stopped[status] += 1
            if status not in ("E_LIMIT_DEPTH", "E_LIMIT_INPUT") and not (status or "").startswith("E_LIMIT"):
                raise SystemExit("this library's parser failed on a document (%s): %r" % (status, it["html"][:200]))
            continue
        it["tree"] = tree_of_ops(tree_line[5:].split(" "))
        direct = next((l for l in lines if l.startswith("DIRECT ")), None)
        it["direct"] = unhex(direct.split(" ")[2]) if direct else None
        kept.append(it)
    return kept, stopped


def ours_ser(runner, queries):
    """[(status, output)] for each (tree, mode, scripting)."""
    reqs = []
    for tree, mode, scripting in queries:
        reqs.append("ser %s %d %s" % (mode, 1 if scripting else 0, " ".join(ops_of(tree))))
    out = []
    for lines in ours_run(runner, reqs, step=200):
        o = next((l for l in lines if l.startswith("OUT ")), None)
        st = next((l.split(" ")[1] for l in lines if l.startswith("STATUS ")), "NONE")
        out.append((st, unhex(o[4:]) if o else None))
    return out


# ---- the references -------------------------------------------------------------------------------------------------

def _drive(which, payload, out):
    cmd = oracle_env.command("html", DRIVERS[which])
    done = subprocess.run(cmd, input=payload, capture_output=True)
    if done.returncode != 0:
        out[which] = ("fail", "the %s driver exited %d:\n%s" % (
            which, done.returncode, oracle_env.reference_stderr(done.stderr.decode("utf-8", "replace"))[-2000:]))
        return
    out[which] = ("ok", done.stdout.decode("utf-8").split("\n"))


def ask_refs(queries, chunk=1500):
    """{reference: [answer]} for each (tree, mode, scripting)."""
    html_ref.check_image()
    answers = {name: [] for name in DRIVERS}
    # resolved on one thread: oracle_env fills its tables lazily
    oracle_env.command("html", DRIVERS["html5lib"])
    for lo in range(0, len(queries), chunk):
        part = queries[lo:lo + chunk]
        payload = ("\n".join(json.dumps({"tree": t, "mode": m, "scripting": bool(s)}, ensure_ascii=True)
                             for t, m, s in part) + "\n").encode("ascii")
        out = {}
        threads = [threading.Thread(target=_drive, args=(w, payload, out)) for w in DRIVERS]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for w in DRIVERS:
            kind, value = out[w]
            if kind == "fail":
                raise SystemExit(value)
            lines = [l for l in value if l]
            if len(lines) != len(part):
                raise SystemExit("%s: sent %d requests, got %d answers" % (w, len(part), len(lines)))
            answers[w].extend(json.loads(l) for l in lines)
    return answers


# ---- the features -----------------------------------------------------------------------------------------------------

RCDATA_BY_NAME = {"style", "script", "xmp", "iframe", "noembed", "noframes", "noscript"}
OURS_VOID = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr", "img", "input", "keygen", "link",
             "meta", "param", "source", "track", "wbr"}
H5L_VOID = {"base", "command", "event-source", "link", "meta", "hr", "br", "img", "embed", "param", "area", "col", "input",
            "source", "track"}


def nodes_of(tree):
    """Every node under the root, with its parent element (or None), depth first, without recursion."""
    work = [(c, None, False) for c in reversed(tree["c"])]
    while work:
        n, parent, _ = work.pop()
        yield n, parent
        if n["k"] == "e":
            for c in reversed(n["c"]):
                work.append((c, n, False))
            if n["t"] is not None:
                for c in reversed(n["t"]):
                    work.append((c, n, True))


def is_html(n):
    return n["k"] == "e" and n["ns"] == NS_HTML


def rename_span(n):
    n["n"] = "span"
    n["p"] = None


def tr_nbsp_text(tree):
    for n, _ in nodes_of(tree):
        if n["k"] in ("t", "k") and "\u00a0" in n["d"]:
            n["d"] = n["d"].replace("\u00a0", " ")


def tr_nbsp_attr(tree):
    for n, _ in nodes_of(tree):
        if n["k"] == "e":
            for a in n["a"]:
                a["v"] = a["v"].replace("\u00a0", " ")


def tr_angle_attr(tree):
    for n, _ in nodes_of(tree):
        if n["k"] == "e":
            for a in n["a"]:
                a["v"] = a["v"].replace("<", "x").replace(">", "x")


def tr_rename(pred):
    def f(tree):
        for n, _ in nodes_of(tree):
            if n["k"] == "e" and pred(n):
                rename_span(n)
    return f


def tr_doctype(tree):
    for n, _ in nodes_of(tree):
        if n["k"] == "d":
            n["pub"] = None
            n["sys"] = None


def tr_attr_ns(tree):
    for n, _ in nodes_of(tree):
        if n["k"] == "e":
            n["a"] = [a for a in n["a"] if a["ns"] is None]


def tr_attr_ns_noprefix(tree):
    for n, _ in nodes_of(tree):
        if n["k"] == "e":
            n["a"] = [a for a in n["a"] if not (a["ns"] is not None and a["ns"] not in (NS_XML, NS_XMLNS, NS_XLINK) and not a["p"])]


def tr_pi(tree):
    def strip(nodes):
        return [n for n in nodes if n["k"] != "p"]
    tree["c"] = strip(tree["c"])
    for n, _ in nodes_of(tree):
        if n["k"] == "e":
            n["c"] = strip(n["c"])
            if n["t"] is not None:
                n["t"] = strip(n["t"])


def has(tree, pred):
    return any(pred(n) for n, _ in nodes_of(tree))


def elements_with_children(n):
    return n["k"] == "e" and any(c["k"] == "e" for c in n["c"])


# feature name -> (applies(tree, scripting), transform). Per reference below.
def f_nbsp_text(tree):
    return has(tree, lambda n: n["k"] in ("t", "k") and "\u00a0" in n["d"])


def f_nbsp_attr(tree):
    return has(tree, lambda n: n["k"] == "e" and any("\u00a0" in a["v"] for a in n["a"]))


def f_angle_attr(tree):
    return has(tree, lambda n: n["k"] == "e" and any("<" in a["v"] or ">" in a["v"] for a in n["a"]))


def f_noscript(tree):
    return has(tree, lambda n: is_html(n) and n["n"] == "noscript")


def f_raw_nested(tree):
    return has(tree, lambda n: is_html(n) and n["n"] in RCDATA_BY_NAME and n["n"] != "noscript" and elements_with_children(n))


def f_raw_foreign(tree):
    return has(tree, lambda n: n["k"] == "e" and n["ns"] != NS_HTML and n["n"] in RCDATA_BY_NAME)


def f_plaintext(tree):
    return has(tree, lambda n: is_html(n) and n["n"] == "plaintext")


def f_doctype(tree):
    return has(tree, lambda n: n["k"] == "d" and (n["pub"] or n["sys"]))


def f_attr_ns(tree):
    return has(tree, lambda n: n["k"] == "e" and any(a["ns"] is not None for a in n["a"]))


def f_attr_ns_noprefix(tree):
    return has(tree, lambda n: n["k"] == "e" and any(
        a["ns"] is not None and a["ns"] not in (NS_XML, NS_XMLNS, NS_XLINK) and not a["p"] for a in n["a"]))


def f_void_set(tree):
    return has(tree, lambda n: is_html(n) and ((n["n"] in OURS_VOID) != (n["n"] in H5L_VOID)))


def f_pi(tree):
    return has(tree, lambda n: n["k"] == "p")


FEATURES = {
    "html5lib": [
        ("nbsp-in-text", f_nbsp_text, tr_nbsp_text),
        ("nbsp-in-attribute", f_nbsp_attr, tr_nbsp_attr),
        ("angle-in-attribute", f_angle_attr, tr_angle_attr),
        ("noscript", f_noscript, tr_rename(lambda n: n["ns"] == NS_HTML and n["n"] == "noscript")),
        ("raw-text-with-elements", f_raw_nested,
         tr_rename(lambda n: n["ns"] == NS_HTML and n["n"] in RCDATA_BY_NAME and elements_with_children(n))),
        ("raw-name-in-foreign-namespace", f_raw_foreign,
         tr_rename(lambda n: n["ns"] != NS_HTML and n["n"] in RCDATA_BY_NAME)),
        ("plaintext", f_plaintext, tr_rename(lambda n: n["ns"] == NS_HTML and n["n"] == "plaintext")),
        ("doctype-identifiers", f_doctype, tr_doctype),
        ("attribute-namespace", f_attr_ns, tr_attr_ns),
        ("void-set", f_void_set, tr_rename(lambda n: n["ns"] == NS_HTML and ((n["n"] in OURS_VOID) != (n["n"] in H5L_VOID)))),
        ("processing-instruction", f_pi, tr_pi),
    ],
    "parse5": [
        ("angle-in-attribute", f_angle_attr, tr_angle_attr),
        ("attribute-namespace-without-prefix", f_attr_ns_noprefix, tr_attr_ns_noprefix),
        ("processing-instruction", f_pi, tr_pi),
    ],
}


def transformed(tree, which, names):
    t = copy.deepcopy(tree)
    table = {n: tr for n, _, tr in FEATURES[which]}
    for name in names:
        table[name](t)
    return t


# ---- a generator of trees no parser makes ---------------------------------------------------------------------------------

HTML_NAMES = ["div", "p", "span", "b", "a", "ul", "li", "table", "tr", "td", "pre", "textarea", "listing", "title", "template",
              "script", "style", "xmp", "iframe", "noembed", "noframes", "noscript", "plaintext", "select", "option", "form",
              "button", "br", "hr", "img", "input", "wbr", "meta", "link", "col", "area", "base", "keygen", "param", "source",
              "track", "embed", "basefont", "bgsound", "frame", "html", "head", "body", "DIV", "Br", "my-element", "x-y"]
ATTR_NAMES = ["class", "id", "title", "href", "disabled", "hidden", "data-x", "style", "lang", "a", "b", "xmlns", "viewBox"]
SPECIAL_TEXT = ["", "x", "a b", "<", ">", "&", "&amp;", "\"", "'", "\u00a0", "a\u00a0b", "\n", "\r", "\r\n", "\t", "x\ny", "</script>",
                "<!--", "-->", "&lt;", "caf\u00e9", "\u20ac", "\U0001f600", "<b>x</b>", "\u2028", "  ", "`", "="]
ATTR_VALUES = ["", "x", "a b", "a\"b", "a'b", "a\"b'c", "<", ">", "a<b>c", "&", "&amp;", "&quot;", "\u00a0", "a\u00a0b", "\n", "\t",
               "`", "=", "caf\u00e9", "\U0001f600", "x/y", "http://x/y?a=1&b=2", "javascript:a<b"]
COMMENT_TEXT = ["", "c", " c ", "a-->b", "<!--", "--", "-", ">", "\u00a0", "x\ny"]
FOREIGN_NS = [NS_SVG, NS_MATH, NS_OTHER, None]
PREFIXES = [None, None, "p", "svg", "x"]


class Built:
    def __init__(self, rng):
        self.r = rng

    def pick(self, seq):
        return seq[self.r.randrange(len(seq))]

    def text(self):
        t = self.pick(SPECIAL_TEXT)
        if self.r.random() < 0.3:
            t += self.pick(SPECIAL_TEXT)
        return {"k": "t", "d": t}

    def attrs(self):
        out, seen = [], set()
        for _ in range(self.r.choice([0, 0, 1, 1, 2, 3, 5])):
            r = self.r.random()
            if r < 0.7:
                ns, p, n = None, None, self.pick(ATTR_NAMES)
            elif r < 0.8:
                ns, p, n = NS_XLINK, "xlink", self.pick(["href", "title", "role"])
            elif r < 0.9:
                ns, p, n = NS_XML, "xml", self.pick(["lang", "space"])
            elif r < 0.95:
                ns, p, n = NS_XMLNS, self.pick([None, "xmlns"]), self.pick(["xmlns", "xlink"])
                if p is None:
                    n = "xmlns"
            else:
                ns, p, n = NS_OTHER, self.pick([None, "o"]), self.pick(["a", "b"])
            if (ns, n) in seen:
                continue
            seen.add((ns, n))
            out.append({"ns": ns, "p": p, "n": n, "v": self.pick(ATTR_VALUES)})
        return out

    def element(self, depth):
        r = self.r.random()
        if r < 0.75:
            ns, p, n = NS_HTML, None, self.pick(HTML_NAMES)
        elif r < 0.85:
            ns, p, n = NS_SVG, None, self.pick(["svg", "a", "style", "script", "foreignObject", "path", "br", "title"])
        elif r < 0.92:
            ns, p, n = NS_MATH, None, self.pick(["math", "mi", "annotation-xml", "style"])
        else:
            ns = self.pick(FOREIGN_NS[2:])
            p = self.pick(PREFIXES)
            if ns is None:
                p = None
            n = self.pick(["x", "y", "style", "p", "Mixed"])
        if ns in (NS_HTML, NS_SVG, NS_MATH) and self.r.random() < 0.08:
            p = self.pick(["h", "s", "m"])  # a prefix the tree keeps and the standard does not write in these three namespaces
        el = {"k": "e", "ns": ns, "p": p, "n": n, "a": self.attrs(), "c": [], "t": None}
        if depth < 4:
            for _ in range(self.r.choice([0, 0, 1, 1, 2, 3])):
                el["c"].append(self.node(depth + 1))
            if ns == NS_HTML and n == "template" and self.r.random() < 0.8:
                el["t"] = [self.node(depth + 1) for _ in range(self.r.choice([0, 1, 2]))]
        else:
            el["c"].append(self.text())
        return el

    def node(self, depth):
        r = self.r.random()
        if r < 0.40:
            return self.text()
        if r < 0.50:
            return {"k": "c", "d": self.pick(COMMENT_TEXT)}
        if r < 0.53:
            return {"k": "p", "t": self.pick(["t", "php", "xml-stylesheet"]), "d": self.pick(["", "d", "a b", "x>y", "?"])}
        if r < 0.55:
            return {"k": "k", "d": self.pick(SPECIAL_TEXT)}
        return self.element(depth)

    def tree(self):
        if self.r.random() < 0.25:
            doc = {"k": "doc", "c": []}
            if self.r.random() < 0.7:
                doc["c"].append({"k": "d", "n": self.pick(["html", "HTML", "", "x"]), "pub": self.pick([None, None, "", "-//W3C//DTD HTML 4.01//EN"]),
                                 "sys": self.pick([None, None, "", "http://x/y.dtd", "a\"b"])})
            for _ in range(self.r.choice([0, 1, 2])):
                doc["c"].append({"k": "c", "d": self.pick(COMMENT_TEXT)})
            if self.r.random() < 0.8:
                doc["c"].append(self.element(0))
            if self.r.random() < 0.3:
                doc["c"].append({"k": "c", "d": self.pick(COMMENT_TEXT)})
            return doc
        return {"k": "frag", "c": [self.node(0) for _ in range(self.r.choice([1, 1, 2, 3, 4]))]}

    def deep(self):
        depth = self.r.choice([20, 100, 250])
        root = {"k": "frag", "c": []}
        cur = root["c"]
        for _ in range(depth):
            el = {"k": "e", "ns": NS_HTML, "p": None, "n": self.pick(["div", "span", "b"]), "a": [], "c": [], "t": None}
            cur.append(el)
            cur.append({"k": "t", "d": "x"})
            cur = el["c"]
        cur.append({"k": "t", "d": "<>&\u00a0"})
        return root


# ---- the witnesses -------------------------------------------------------------------------------------------------------

def el(n, ns=NS_HTML, a=None, c=None, t=None, p=None):
    return {"k": "e", "ns": ns, "p": p, "n": n, "a": a or [], "c": c or [], "t": t}


def txt(d):
    return {"k": "t", "d": d}


def attr(n, v, ns=None, p=None):
    return {"ns": ns, "p": p, "n": n, "v": v}


# (name, reference, tree, mode, scripting, what the standard's text gives). Worked by hand from the algorithm as served on
# 2026-10-12: "Replace any occurrences of the U+00A0 NO-BREAK SPACE character by "&nbsp;"", "... by "&lt;" ...", "&gt;"
# in text and in attribute mode, `"` in attribute mode; the raw text parents; "an attribute's serialized name"; "Append
# "<!DOCTYPE ", followed by the value of current node's name and U+003E (>)".
WITNESSES = [
    ("html5lib does not escape a no-break space in text", "html5lib",
     {"k": "frag", "c": [el("p", c=[txt("a\u00a0b")])]}, "children", False, "<p>a&nbsp;b</p>"),
    ("html5lib does not escape a no-break space in an attribute value", "html5lib",
     {"k": "frag", "c": [el("p", a=[attr("title", "a\u00a0b")])]}, "children", False, '<p title="a&nbsp;b"></p>'),
    ("neither reference escapes < and > in an attribute value", "html5lib",
     {"k": "frag", "c": [el("p", a=[attr("title", "a<b>c")])]}, "children", False, '<p title="a&lt;b&gt;c"></p>'),
    ("neither reference escapes < and > in an attribute value (parse5)", "parse5",
     {"k": "frag", "c": [el("p", a=[attr("title", "a<b>c")])]}, "children", False, '<p title="a&lt;b&gt;c"></p>'),
    ("html5lib escapes the text of a plaintext", "html5lib",
     {"k": "frag", "c": [el("plaintext", c=[txt("a<b")])]}, "children", False, "<plaintext>a<b</plaintext>"),
    ("html5lib writes the identifiers of a document type", "html5lib",
     {"k": "doc", "c": [{"k": "d", "n": "html", "pub": "-//W3C//DTD HTML 4.01//EN", "sys": "http://x/y.dtd"}]}, "children", False,
     "<!DOCTYPE html>"),
    ("html5lib drops the namespace prefix of an attribute", "html5lib",
     {"k": "frag", "c": [el("a", ns=NS_SVG, a=[attr("href", "u", NS_XLINK, "xlink")])]}, "children", False,
     '<a xlink:href="u"></a>'),
    ("parse5 writes 'undefined' for the missing prefix of an attribute in another namespace", "parse5",
     {"k": "frag", "c": [el("p", a=[attr("a", "1", NS_OTHER, None)])]}, "children", False, '<p a="1"></p>'),
    ("html5lib's raw text flag outlasts a noscript's child elements", "html5lib",
     {"k": "frag", "c": [el("noscript", c=[el("p", c=[txt("a<b")])])]}, "children", False, "<noscript><p>a&lt;b</p></noscript>"),
    ("html5lib has no wbr among its void elements", "html5lib",
     {"k": "frag", "c": [el("wbr")]}, "children", False, "<wbr>"),
    ("html5lib reads a raw text element by its name alone", "html5lib",
     {"k": "frag", "c": [el("style", ns=NS_SVG, c=[txt("a<b")])]}, "children", False, "<style>a&lt;b</style>"),
    ("neither reference has a processing instruction to write", "html5lib",
     {"k": "frag", "c": [{"k": "p", "t": "t", "d": "d"}]}, "children", False, "<?t d?>"),
    ("neither reference has a processing instruction to write (parse5)", "parse5",
     {"k": "frag", "c": [{"k": "p", "t": "t", "d": "d"}]}, "children", False, "<?t d?>"),
    ("html5lib's raw text flag is by name, not by parent", "html5lib",
     {"k": "frag", "c": [el("script", c=[el("b", c=[txt("a<b")])])]}, "children", False, "<script><b>a&lt;b</b></script>"),
]


def check_witnesses(runner):
    queries = [(w[2], w[3], w[4]) for w in WITNESSES]
    mine = ours_ser(runner, queries)
    refs = ask_refs(queries)
    bad, held = [], 0
    for w, (st, ours), a5, p5 in zip(WITNESSES, mine, refs["html5lib"], refs["parse5"]):
        ref = {"html5lib": a5, "parse5": p5}[w[1]]
        if st != "OK" or ours != w[5]:
            bad.append("%s: this library gives %r, the standard's text gives %r" % (w[0], ours, w[5]))
        elif ref.get("out") == w[5]:
            bad.append("%s: %s gives the standard's output, so it is no deviation" % (w[0], w[1]))
        elif "out" not in ref:
            bad.append("%s: %s could not answer: %s" % (w[0], w[1], ref.get("error")))
        else:
            held += 1
    return held, bad


# ---- the population -------------------------------------------------------------------------------------------------------

def population(runner, count, seed, corpus):
    rng = random.Random(seed ^ 0x5eed)
    items = []
    base = treediff.population(runner, count, seed, corpus)
    for it in base:
        items.append({"id": it["id"], "html": it["html"], "scripting": it["scripting"], "context": it.get("context")})
    kept, stopped = ours_parse(runner, items)
    for it in kept:
        it["source"] = "parsed"
    g = Built(rng)
    built = []
    for i in range(count):
        built.append({"id": "built:%d" % i, "tree": g.tree(), "scripting": bool(rng.getrandbits(1)), "source": "built"})
    for i in range(max(3, count // 100)):
        built.append({"id": "deep:%d" % i, "tree": g.deep(), "scripting": False, "source": "built"})
    return kept + built, stopped


def queries_of(items):
    """[(item, mode, tree)] with the queries to ask."""
    out = []
    for it in items:
        out.append((it, "children", it["tree"]))
        o = outer_tree(it["tree"])
        if o is not None:
            out.append((it, "outer", o))
    return out


def load_list(path):
    table = {}
    if not os.path.exists(path):
        return table
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#") or raw.startswith("@"):
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


def explain(runner, divergent):
    """For each (query, ref) in divergent, the name of the feature set that explains it, found by taking features out."""
    # signature -> representative; stage 1: all applicable features out
    results = [None] * len(divergent)
    present = []
    for q, ref, theirs in divergent:
        tree, mode, scripting = q["tree"], q["mode"], q["scripting"]
        names = [n for n, applies, _ in FEATURES[ref] if applies(tree)]
        present.append(names)

    def agree(group):
        """For (index, names) pairs: True where ours and the reference agree on the tree with those features out."""
        if not group:
            return []
        trees = [transformed(divergent[i][0]["tree"], divergent[i][1], names) for i, names in group]
        # the node OUTER writes is the first child of the root: with none left there is nothing to write, for anyone
        asked = [k for k, (t, (i, _)) in enumerate(zip(trees, group)) if not (divergent[i][0]["mode"] == "outer" and not t["c"])]
        qs = [(trees[k], divergent[group[k][0]][0]["mode"], divergent[group[k][0]][0]["scripting"]) for k in asked]
        mine = ours_ser(runner, qs)
        refs = ask_refs(qs)
        verdict = [True] * len(group)
        for j, k in enumerate(asked):
            i = group[k][0]
            ref = refs[divergent[i][1]][j]
            verdict[k] = mine[j][0] == "OK" and "out" in ref and mine[j][1] == ref["out"]
        return verdict

    todo = [(i, names) for i, names in enumerate(present) if names]
    ok_all = agree(todo)
    explained = {}
    signature = {}
    for (i, names), ok in zip(todo, ok_all):
        if ok:
            explained[i] = tuple(names)
            signature.setdefault((divergent[i][1], tuple(names)), []).append(i)
    # stage 2: the smallest subset that explains, for one representative of each signature; all the signatures' subsets
    # of one size are asked together
    chosen = {}
    pending = dict(signature)
    for size in range(1, max([len(sig[1]) for sig in signature] + [1])):
        group, meta = [], []
        for sig, members in pending.items():
            if size < len(sig[1]):
                for subset in itertools.combinations(sig[1], size):
                    group.append((members[0], list(subset)))
                    meta.append((sig, subset))
        for (sig, subset), ok in zip(meta, agree(group)):
            if ok and sig not in chosen:
                chosen[sig] = subset
        pending = {sig: m for sig, m in pending.items() if sig not in chosen}
    for sig in signature:
        chosen.setdefault(sig, sig[1])
    # stage 3: every member of a signature must be explained by the representative's subset
    check = []
    for sig, members in signature.items():
        if chosen[sig] != sig[1]:
            for i in members:
                check.append((i, list(chosen[sig])))
    verdict = agree(check)
    failed = {i for (i, _), ok in zip(check, verdict) if not ok}
    for sig, members in signature.items():
        for i in members:
            names = chosen[sig] if i not in failed else sig[1]
            results[i] = "+".join(names)
    for i in range(len(divergent)):
        if results[i] is None:
            results[i] = "unexplained"
    return results


def main(argv):
    runner = None
    count, seed = DEFAULT_COUNT, DEFAULT_SEED
    plant = None
    propose = False
    corpus = True
    explain_n = 0
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
            explain_n = int(args.pop(0))
        elif runner is None:
            runner = a
        else:
            raise SystemExit("unexpected argument %r" % a)
    if runner is None:
        raise SystemExit(__doc__)
    count = int(os.environ.get("HTML_SERIALIZE_ORACLE_COUNT", count))
    seed = int(os.environ.get("HTML_SERIALIZE_ORACLE_SEED", seed))
    hunting = (count, seed) != (DEFAULT_COUNT, DEFAULT_SEED) or not corpus

    items, stopped = population(runner, count, seed, corpus)
    queries = []
    for it, mode, tree in queries_of(items):
        queries.append({"id": it["id"], "source": it["source"], "mode": mode, "tree": tree, "scripting": it["scripting"],
                        "html": it.get("html"), "direct": it.get("direct")})
    mine = ours_ser(runner, [(q["tree"], q["mode"], q["scripting"]) for q in queries])
    for q, (st, out) in zip(queries, mine):
        if q["mode"] == "children" and q.get("direct") is not None and st == "OK" and out != q["direct"]:
            sys.stderr.write("\033[0;31m\n### The tree rebuilt from its ops is written differently from the tree it was ###\033[0m\n  %s\n    direct %r\n    rebuilt %r\n" % (
                q["id"], q["direct"][:200], out[:200]))
            return 3
        if st != "OK":
            sys.stderr.write("\033[0;31m\n### The serializer failed on a tree (%s) ###\033[0m\n  %s %s\n" % (st, q["id"], q["mode"]))
            return 3
        q["ours"] = out
    if plant == "ours-extra-char" and queries:
        queries[0]["ours"] += "!"
    if plant == "ours-lost-char":
        for q in queries:
            if q["ours"]:
                q["ours"] = q["ours"][:-1]
                break
    answers = ask_refs([(q["tree"], q["mode"], q["scripting"]) for q in queries])

    refs_failed = []
    divergent = []  # (query, ref, theirs)
    equal = {"html5lib": 0, "parse5": 0}
    agree_refs = 0
    for q, a5, p5 in zip(queries, answers["html5lib"], answers["parse5"]):
        if "out" not in a5 or "out" not in p5:
            refs_failed.append((q, a5.get("error"), p5.get("error")))
            continue
        agree_refs += a5["out"] == p5["out"]
        for name, a in (("html5lib", a5), ("parse5", p5)):
            if q["ours"] == a["out"]:
                equal[name] += 1
            else:
                divergent.append((q, name, a["out"]))

    by_source = collections.Counter(it["source"] for it in items)
    by_origin = collections.Counter(it["id"].split(":")[0] for it in items)
    print("trees: " + ", ".join("%s %d" % (s, by_source[s]) for s in sorted(by_source)) + " (" +
          ", ".join("%s %d" % (s, by_origin[s]) for s in sorted(by_origin)) + ")")
    print("stopped before a comparison (a limit of the parser): " + ("; ".join("%d x %s" % (n, s) for s, n in sorted(stopped.items())) or "none"))
    print("questions asked (a tree is written two ways): %d; ours equals html5lib on %d, parse5 on %d; the references equal each other on %d" % (
        len(queries), equal["html5lib"], equal["parse5"], agree_refs))

    causes = explain(runner, divergent)
    # A divergence counts once for each feature that was needed to explain it (a tree with a no-break space and a prefixed
    # attribute is in both), so the kinds are the pairs (reference, feature) and the list stays as short as the features.
    kinds = collections.defaultdict(list)
    for (q, ref, theirs), cause in zip(divergent, causes):
        for feature in cause.split("+"):
            kinds["%s:%s" % (ref, feature)].append((q, theirs))
    print("divergence kinds: %d, divergences: %d" % (len(kinds), len(divergent)))

    listed = {} if plant == "no-list" else load_list(LIST)
    if plant == "stale-entry":
        listed["html5lib:planted-gone"] = ("1", "reference-old", "planted")
    if plant == "change-count":
        for k in sorted(listed):
            if listed[k][0] != "hunt":
                listed[k] = (str(int(listed[k][0]) + 1),) + listed[k][1:]
                break
    if propose:
        for k in sorted(kinds):
            print("%s\t%d\treference-old\tUNREAD" % (k, len(kinds[k])))
        return 0
    if explain_n:
        for k in sorted(kinds):
            for q, theirs in kinds[k][:explain_n]:
                print("== %s %s %s\n   html: %r\n   ours:   %r\n   theirs: %r" % (k, q["id"], q["mode"], (q.get("html") or "")[:300], q["ours"][:300], theirs[:300]))
        return 0

    failed = False
    held, witness_bad = check_witnesses(runner)
    print("witnesses: %d of %d reference deviations hold (this library gives the output worked out by hand from the standard's text, the reference does not)" % (held, len(WITNESSES)))
    if plant == "witness":
        witness_bad = ["planted: a witness that does not hold"]
    if witness_bad:
        failed = True
        sys.stderr.write("\033[0;31m\n### A witness of a reference deviation does not hold ###\033[0m\n")
        for line in witness_bad:
            sys.stderr.write("  %s\n" % line)
    if refs_failed:
        failed = True
        sys.stderr.write("\033[0;31m\n### A reference could not answer ###\033[0m\n")
        for q, e1, e2 in refs_failed[:10]:
            sys.stderr.write("  %s: html5lib %s, parse5 %s\n" % (q["id"], e1, e2))
    unexplained = [k for k in kinds if k.endswith(":unexplained")]
    if unexplained:
        failed = True
        sys.stderr.write("\033[0;31m\n### Divergences that no feature of the tree explains ###\033[0m\n")
        for k in unexplained:
            for q, theirs in kinds[k][:5]:
                sys.stderr.write("  %s %s %s\n    html %r\n    ours   %r\n    theirs %r\n" % (k, q["id"], q["mode"], (q.get("html") or "")[:200], q["ours"][:200], theirs[:200]))
    unlisted = sorted(k for k in kinds if k not in listed and not k.endswith(":unexplained"))
    if unlisted:
        failed = True
        sys.stderr.write("\033[0;31m\n### Divergences with no entry (tools/oracle/html_serialize_divergences.txt) ###\033[0m\n")
        for k in unlisted[:30]:
            q, theirs = kinds[k][0]
            sys.stderr.write("  %s  x%d\n    e.g. %s %s\n      ours   %r\n      theirs %r\n" % (k, len(kinds[k]), q["id"], q["mode"], q["ours"][:160], theirs[:160]))
    gone = [] if hunting else sorted(k for k in listed if k not in kinds and listed[k][0] != "hunt")
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
        if len(items) < MIN_TREES:
            failed = True
            sys.stderr.write("\033[0;31m\n### The population has shrunk: %d trees, floor %d ###\033[0m\n" % (len(items), MIN_TREES))
    if failed:
        return 3
    print("html serializer differential: every divergence is explained and listed, none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
