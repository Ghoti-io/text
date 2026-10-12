#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Score the HTML serializer against html5lib-tests' serializer vectors, and round-trip the tree vectors.

    html_serializer_suite.py <html5lib-tests/serializer dir> <runner> <exceptions file> [<tree vectors dir> [<extra .dat>]] [--propose] [--verbose]

**The serializer vectors** (`core`, `injectmeta`, `optionaltags`, `options`, `whitespace`; 230 vectors, 18
with an `xhtml` variant that is scored as a second vector) are token streams with html5lib's serializer options. The
runner (html_serializer_suite.c) feeds each stream to the token-stream serializer under the options
below and the output must be one of the vector's `expected` strings.

*The options.* A vector with no `options` is html5lib's defaults, which are not the standard's: attribute values are
quoted only when the syntax needs it (`quote_attr_values=when_needed`), the quote is chosen to need no escape,
`omit_optional_tags` and `minimize_boolean_attributes` are on, attributes come out sorted, the identifiers of a
document type are written, and `noscript` is raw text. The `xhtml` variant of a vector is the same stream written
as XHTML needs it: every attribute quoted, no minimization, no omitted tags, `<br />`, and the text of `script` and
the like escaped. `escape_lt_in_attrs` is not mapped: the standard escapes `<` and `>` in an attribute value
always, so the option has nothing to switch (the vector that sets it passes without it); `resolve_entities` is not
mapped: no vector has an entity token and the tree has none.

*Exceptions* (HTML_SERIALIZER_EXCEPTIONS) are one line per vector that does not pass, `<file>#<n>[:xhtml] TAB
<kind> TAB <reason> TAB <digest>`, where the kind is `standard` (the vector's expected string is not what the
standard's algorithm gives, and the reason quotes the sentence of the standard served on the date in
documentation/formats/html.md) or `vocabulary` (the vector depends on a table of html5lib's that the standard does not
have), and the digest locks the first difference, so that a different failure of the same vector cannot hide behind
the entry. The gate fails on

  UNEXPLAINED FAILURES  a vector that does not pass and is not listed
  STALE EXCEPTIONS      a listed vector that now passes, fails differently, or does not exist
  below the floor       fewer vectors loaded or passing than HTML_SERIALIZER_MIN_VECTORS / _MIN_PASS

**The round trip** (given the tree-construction vectors' directory, and the hand-chosen extra file the tree gate also
scores): every in-scope vector's input is parsed (as a
document, or as a fragment in its context), serialized with the standard's defaults, parsed again and serialized
again. The properties asserted, per vector and scripting setting:

  * the first serialization succeeds (HTML serialization does not fail on content);
  * the second serialization is the first one (idempotence), **or** the vector's tree has a cause the standard
    names for a serialization that does not read back (below);
  * the second tree is the first, or the tree has such a cause.

A tree whose reparse differs is *classified*: each cause is a predicate on the first tree (a `pre` that starts with a
line feed, an `a` inside an `a`, ...), and a tree that matches none is UNEXPLAINED and fails the gate. The counts are printed.

Copyright 2026 by Corey Pennycuff
"""
import collections
import glob
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SERIALIZER_FILES = ("core", "injectmeta", "optionaltags", "options", "whitespace")

TIMEOUT = int(os.environ.get("HTML_SERIALIZER_RUNNER_TIMEOUT", "900"))


def hx(s):
    return "x" + s.encode("utf-8", "surrogatepass").hex()


def nul(s):
    return "-" if s is None else hx(s)


# ---- the serializer vectors -------------------------------------------------------------------------

HTML_DEFAULTS = {
    "quote_attr_values": "when_needed", "use_best_quote_char": "1", "omit_optional_tags": "1",
    "minimize_boolean_attributes": "1", "alphabetical_attributes": "1", "doctype_identifiers": "1",
    "scripting": "1",
}
XHTML_DEFAULTS = {
    "quote_attr_values": "always", "use_best_quote_char": "1", "omit_optional_tags": "0",
    "minimize_boolean_attributes": "0", "use_trailing_solidus": "1", "space_before_trailing_solidus": "1",
    "escape_rcdata": "1", "alphabetical_attributes": "1", "doctype_identifiers": "1", "scripting": "1",
}


def option_words(vector_options, xhtml):
    """Our options for a vector's html5lib options."""
    words = dict(XHTML_DEFAULTS if xhtml else HTML_DEFAULTS)
    for key, value in vector_options.items():
        if key == "quote_char":
            words["quote_char"] = {"'": "apos", '"': "quote"}.get(value, "other")
            words["use_best_quote_char"] = "0"
        elif key == "quote_attr_values":
            words["quote_attr_values"] = "always" if value else "when_needed"
        elif key == "encoding":
            words["encoding"] = hx(value)
        elif key in ("minimize_boolean_attributes", "use_trailing_solidus", "escape_rcdata", "strip_whitespace",
                     "inject_meta_charset"):
            words[key] = "1" if value else "0"
            if key == "use_trailing_solidus" and value:
                words["space_before_trailing_solidus"] = "1"
        elif key == "escape_lt_in_attrs":
            pass  # the standard escapes < and > in attribute values always
        else:
            raise SystemExit("serializer vector option %r has no mapping" % key)
    return words


def token_words(tokens):
    out = []
    for tok in tokens:
        kind = tok[0]
        if kind in ("StartTag", "EmptyTag"):
            if kind == "StartTag":
                ns, name, attrs = tok[1], tok[2], tok[3]
            else:
                ns, name, attrs = None, tok[1], tok[2]
            attrs = attrs if isinstance(attrs, list) else []
            fields = ["S" if kind == "StartTag" else "M", nul(ns), hx(name), str(len(attrs))]
            for a in attrs:
                fields += [nul(a["namespace"]), "-", hx(a["name"]), hx(a["value"])]
            out.append(",".join(fields))
        elif kind == "EndTag":
            out.append(",".join(["E", nul(tok[1]), hx(tok[2])]))
        elif kind == "Characters":
            out.append("C," + hx(tok[1]))
        elif kind == "Comment":
            out.append("K," + hx(tok[1]))
        elif kind == "Doctype":
            name = tok[1]
            pub = tok[2] if len(tok) > 2 else None
            sys_ = tok[3] if len(tok) > 3 else None
            out.append(",".join(["D", hx(name), nul(pub), nul(sys_)]))
        else:
            raise SystemExit("unknown serializer token %r" % kind)
    return out


def load_serializer_vectors(dirpath):
    vectors = []
    for name in SERIALIZER_FILES:
        path = os.path.join(dirpath, name + ".test")
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        for n, t in enumerate(data["tests"], 1):
            for xhtml in (False, True):
                if xhtml and "xhtml" not in t:
                    continue
                vectors.append({
                    "id": "%s.test#%d%s" % (name, n, ":xhtml" if xhtml else ""),
                    "description": t["description"],
                    "expected": t["xhtml"] if xhtml else t["expected"],
                    "request": "tok " + " ".join("%s=%s" % kv for kv in sorted(option_words(t.get("options", {}), xhtml).items()))
                               + " -- " + " ".join(token_words(t["input"])),
                })
    return vectors


# ---- the runner ---------------------------------------------------------------------------------------

LAST_STDERR = ""


def run_batch(runner, requests):
    global LAST_STDERR
    data = ("\n".join(requests) + "\n").encode("ascii")
    try:
        done = subprocess.run([runner], input=data, capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        LAST_STDERR = "the runner did not answer %d requests within %d seconds" % (len(requests), TIMEOUT)
        return None
    if done.returncode != 0:
        LAST_STDERR = done.stderr.decode("utf-8", "replace")[-1500:]
        return None
    answers, cur = [], []
    for line in done.stdout.decode("utf-8", "replace").split("\n"):
        if not line:
            continue
        cur.append(line)
        if line.startswith("END "):
            answers.append(cur)
            cur = []
    if len(answers) != len(requests):
        return None
    return answers


def run_all(runner, requests, step=200):
    """Answers for every request; a request that kills the runner is isolated and reported."""
    answers = [None] * len(requests)
    crashed = {}
    for lo in range(0, len(requests), step):
        part = requests[lo:lo + step]
        got = run_batch(runner, part)
        if got is not None:
            answers[lo:lo + len(part)] = got
            continue
        for i, r in enumerate(part):
            one = run_batch(runner, [r])
            if one is None:
                crashed[lo + i] = LAST_STDERR
            else:
                answers[lo + i] = one[0]
    return answers, crashed


def unhex(word):
    if not word.startswith("x"):
        raise ValueError("a hex field must start with x: %r" % word[:20])
    return bytes.fromhex(word[1:]).decode("utf-8", "surrogatepass")


def parse_tok_answer(lines):
    a = {"out": None, "status": None, "end": None, "counts": None}
    for line in lines:
        f = line.split(" ")
        if f[0] == "OUT":
            a["out"] = unhex(f[1])
        elif f[0] == "STATUS":
            a["status"] = f[1]
        elif f[0] == "COUNTS":
            a["counts"] = tuple(int(x) for x in f[1:])
        elif f[0] == "END":
            a["end"] = f[1]
    return a


def digest(text):
    return hashlib.sha256(text.encode("utf-8", "surrogatepass")).hexdigest()[:10]


def load_exceptions(path):
    table = {}
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#"):
                continue
            parts = raw.split("\t")
            if len(parts) != 4 or parts[1] not in ("standard", "vocabulary") or not parts[2].strip() or not parts[3].strip():
                raise SystemExit("%s:%d: not <id> <standard|vocabulary> <reason> <digest>" % (path, n))
            if parts[0] in table:
                raise SystemExit("%s:%d: %s is listed twice" % (path, n, parts[0]))
            table[parts[0]] = (parts[1], parts[2], parts[3])
    return table


def score_vectors(dirpath, runner, listed, propose, verbose):
    vectors = load_serializer_vectors(dirpath)
    answers, crashed = run_all(runner, [v["request"] for v in vectors])
    passes = 0
    unexplained, stale = [], []
    kinds = collections.Counter()
    for i, v in enumerate(vectors):
        if i in crashed or answers[i] is None:
            detail = "the runner crashed or timed out: %s" % crashed.get(i, "")[-300:].replace("\n", " | ")
            ok, got = False, None
        else:
            a = parse_tok_answer(answers[i])
            got = a["out"]
            if a["end"] != "DONE" or a["status"] != "OK":
                detail = "runner answered %s / %s" % (a["end"], a["status"])
                ok = False
            elif got in v["expected"]:
                ok, detail = True, ""
            else:
                ok = False
                detail = "got %r, expected %r" % (got, v["expected"])
        tag = listed.get(v["id"])
        if ok:
            passes += 1
            if tag:
                stale.append("%s  now passes (listed %s)" % (v["id"], tag[0]))
        else:
            if propose:
                print("%s\tstandard\tUNREAD: %s\t%s" % (v["id"], v["description"], digest(detail)))
                continue
            if tag and tag[2] == digest(detail):
                kinds[tag[0]] += 1
                continue
            if tag:
                stale.append("%s  fails differently now: %s [%s]" % (v["id"], detail[:140], digest(detail)))
            else:
                unexplained.append("%s (%s)  %s  [%s]" % (v["id"], v["description"], detail[:200], digest(detail)))
    known = {v["id"] for v in vectors}
    for vid in listed:
        if vid not in known:
            stale.append("%s  is not a vector" % vid)
    print("serializer vectors: %d (%d with an xhtml variant counted as their own); passing: %d; listed exceptions: %d (%s)" % (
        len(vectors), sum(1 for v in vectors if v["id"].endswith(":xhtml")), passes, len(listed),
        ", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "none"))
    return len(vectors), passes, unexplained, stale


# ---- the round trip -----------------------------------------------------------------------------------

def load_tree_module():
    spec = importlib.util.spec_from_file_location("html_tree_suite", os.path.join(HERE, "html_tree_suite.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def dump_lines(dump):
    return dump.rstrip("\n").split("\n")


def parse_rt_answer(lines):
    a = {"r1": None, "r2": None, "dump1": None, "dump2": None, "ser1": None, "ser2": None, "equal": None, "end": None}
    for line in lines:
        f = line.split(" ")
        k = f[0]
        if k == "R1":
            a["r1"] = f[1:]
        elif k == "R2":
            a["r2"] = f[1:]
        elif k in ("DUMP1", "DUMP2", "SER1", "SER2"):
            a[k.lower()] = unhex(f[1])
        elif k == "EQUAL":
            a["equal"] = f[1] == "1"
        elif k == "END":
            a["end"] = f[1]
    return a


# The first tree's features that make a serialize-reparse not give the tree back. Each is a predicate on the dump of the
# first tree, and each is a case the standard names in 13.3 ("It is possible that the output of this algorithm, if parsed
# with an HTML parser, will not return the original tree structure": a p that contains a ul, a nested form, a nested a
# after foster parenting, the line feed that pre, textarea and listing lose, and a script that holds its own end tag), or
# follows from the algorithm in the same way and is named here: a plaintext element (the algorithm writes an end tag
# that the parser reads as text), a script whose text opens the double escaped state (its end tag is swallowed), a
# carriage return in a tree (written as it is, read as a line feed: the preprocessing of the input stream) and a document
# type with identifiers (the algorithm writes its name only). The first that holds is the cause; all are counted.
P_CLOSERS = {"address", "article", "aside", "blockquote", "details", "dialog", "div", "dl", "fieldset", "figcaption",
             "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "main", "menu",
             "nav", "ol", "p", "pre", "search", "section", "table", "ul"}


def dump_nodes(dump):
    """[[depth, text, first line]] of a tree in the vectors' dump format; a text with line feeds in it is one node."""
    nodes = []
    if dump.endswith("\n"):
        dump = dump[:-1]
    for lineno, line in enumerate(dump.split("\n")):
        if line.startswith("| "):
            body = line[2:]
            depth = (len(body) - len(body.lstrip(" "))) // 2
            nodes.append([depth, body.strip(" ") if not body.lstrip(" ").startswith('"') else body.lstrip(" "), lineno])
        elif nodes:
            nodes[-1][1] += "\n" + line
    return nodes


def causes_of(dump1):
    """[(cause, line)]: each cause the standard names that the tree carries, with the line of its node."""
    found = []
    stack = []  # (depth, name)
    nodes = dump_nodes(dump1)
    for idx, (depth, text, line) in enumerate(nodes):
        while stack and stack[-1][0] >= depth:
            stack.pop()
        names = [n for _, n in stack]
        if text.startswith("<") and not text.startswith("<!") and not text.startswith("<?") and text.endswith(">"):
            name = text[1:-1]
            if name == "a" and "a" in names:
                found.append(("nested-a", line))
            if name == "form" and "form" in names:
                found.append(("nested-form", line))
            if name == "nobr" and "nobr" in names:
                found.append(("nested-nobr", line))
            if name in P_CLOSERS and "p" in names:
                found.append(("p-contains-block", line))
            if name == "plaintext":
                found.append(("plaintext", line))
            if name == "script":
                # the text below it
                body = []
                for d2, t2, _ in nodes[idx + 1:]:
                    if d2 <= depth:
                        break
                    body.append(t2)
                joined = " ".join(body).lower()
                i = joined.find("<!--")
                if i >= 0 and "<script" in joined[i:]:
                    found.append(("script-double-escape", line))
            stack.append((depth, name))
        elif text.startswith('"') and stack and stack[-1][1] in ("pre", "textarea", "listing"):
            if depth == stack[-1][0] + 1 and text == '"' + text[1:] and text[1:2] == "\n":
                # the first child text of the element starts with a line feed
                prev = nodes[idx - 1]
                if prev[0] == stack[-1][0] and prev[1] == "<%s>" % stack[-1][1]:
                    found.append(("leading-newline", line))
        if text.startswith('"') or text.startswith("<!--") or "=" in text:
            if "\r" in text:
                found.append(("carriage-return", line))
        if text.startswith("<!DOCTYPE") and '"' in text:
            found.append(("doctype-identifiers", line))
    return found


def first_difference(dump1, dump2):
    """The first line (0-based) at which two dumps differ, or None."""
    l1, l2 = dump_lines(dump1 or ""), dump_lines(dump2 or "")
    for k in range(max(len(l1), len(l2))):
        if (l1[k] if k < len(l1) else None) != (l2[k] if k < len(l2) else None):
            return k
    return None


# A cause explains the difference it can make and no other: what the standard names changes the tree from the node
# that carries it onward (or, for a carriage return, a text that carries it, and a document type with identifiers,
# that line), so the first line at which the two trees differ must be that node's line or a later one. A defect that
# changes something before the cause is not explained by it, however many causes the tree carries.
def explained_causes(dump1, dump2, idempotent):
    found = causes_of(dump1)
    blank = lambda d: "\n".join("| <!DOCTYPE>" if l.startswith("| <!DOCTYPE") else l for l in dump_lines(d or ""))
    doctype_differs = [l for l in dump_lines(dump1 or "") if l.startswith("| <!DOCTYPE")] != \
        [l for l in dump_lines(dump2 or "") if l.startswith("| <!DOCTYPE")]
    first = first_difference(blank(dump1), blank(dump2))
    if first is None and idempotent and not doctype_differs:
        return []
    if first is None:
        first = 0 # the bytes differ and the trees do not (apart from the document type): no line to tie to
    out = []
    if doctype_differs:
        # A document type with identifiers is written as its name, which changes that line and nothing else.
        if not any(c == "doctype-identifiers" for c, _ in found):
            return []
        out.append("doctype-identifiers")
        if idempotent and first is not None and blank(dump1) == blank(dump2):
            return out
    for cause, line in found:
        if cause != "doctype-identifiers" and first >= line:
            out.append(cause)
    if not out or out == ["doctype-identifiers"] and not (idempotent and blank(dump1) == blank(dump2)):
        return []
    return out


def selftest_causes():
    """The tie to the first differing line, shown to refuse what it must (run on every invocation of the gate)."""
    d1 = '| <html>\n|   <head>\n|   <body>\n|     "x"\n|     <p>\n|       <div>\n'
    ok = '| <html>\n|   <head>\n|   <body>\n|     "x"\n|     <p>\n|     <div>\n'
    early = '| <html>\n|   <head>\n|   <body>\n|     "y"\n|     <p>\n|     <div>\n'
    problems = []
    if explained_causes(d1, ok, True) != ["p-contains-block"]:
        problems.append("a p holding a block was not what explains the block moving out of it")
    if explained_causes(d1, early, True) != []:
        problems.append("a difference before the p was explained by the p")
    return problems


def round_trip(tree, resources, extra, runner, scripting_floor, verbose):
    vectors, scripted, files = tree.load(resources)
    if extra:
        vectors.extend(tree.parse_dat(extra, os.path.basename(extra)))
    requests, owners = [], []
    for v in vectors:
        settings = [v["scripting"]] if v["scripting"] is not None else [False, True]
        for sc in settings:
            req = "rt %d %s" % (1 if sc else 0, hx(v["data"]))
            if v["fragment"]:
                req += " ctx:" + hx(v["context"])
            requests.append(req)
            owners.append((v, sc))
    answers, crashed = run_all(runner, requests)
    stats = collections.Counter()
    unexplained = []
    cause_counts = collections.Counter()
    for i, (v, sc) in enumerate(owners):
        if i in crashed or answers[i] is None:
            unexplained.append("%s scripting %s: the runner crashed: %s" % (v["id"], sc, crashed.get(i, "")[-300:]))
            continue
        a = parse_rt_answer(answers[i])
        stats["runs"] += 1
        stats["vector:" + v["id"]] = 1
        if a["end"] != "DONE" or a["r1"] is None or a["r1"][0] != "OK":
            unexplained.append("%s scripting %s: the first parse answered %s" % (v["id"], sc, a["r1"]))
            continue
        if a["r1"][1] != "OK":
            unexplained.append("%s scripting %s: serialization failed: %s" % (v["id"], sc, a["r1"]))
            continue
        stable_tree = a["dump1"] == a["dump2"] and a["r1"][2] == a["r2"][2] and a["equal"]
        idempotent = a["ser1"] == a["ser2"]
        if stable_tree and not idempotent:
            unexplained.append("%s scripting %s: the tree came back the same and the bytes did not: %r vs %r" % (
                v["id"], sc, (a["ser1"] or "")[:80], (a["ser2"] or "")[:80]))
            continue
        if stable_tree and idempotent:
            stats["stable"] += 1
            continue
        causes = explained_causes(a["dump1"], a["dump2"], idempotent)
        if not stable_tree:
            stats["tree-differs"] += 1
        if not idempotent:
            stats["not-idempotent"] += 1
        stats["unstable:" + v["id"]] = 1
        if causes:
            for c in set(causes):
                cause_counts[c] += 1
            stats["explained"] += 1
        else:
            first = ""
            if a["dump1"] != a["dump2"]:
                l1, l2 = dump_lines(a["dump1"]), dump_lines(a["dump2"] or "")
                for k in range(max(len(l1), len(l2))):
                    x = l1[k] if k < len(l1) else "(end)"
                    y = l2[k] if k < len(l2) else "(end)"
                    if x != y:
                        first = "line %d: %r vs %r" % (k + 1, x[:60], y[:60])
                        break
            unexplained.append("%s scripting %s: %s%s ser1=%r ser2=%r %s" % (
                v["id"], sc, "tree differs; " if not stable_tree else "", "not idempotent;" if not idempotent else "",
                (a["ser1"] or "")[:80], (a["ser2"] or "")[:80], first))
    return stats, cause_counts, unexplained


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    propose = "--propose" in argv
    verbose = "--verbose" in argv
    if len(args) not in (3, 4, 5):
        raise SystemExit(__doc__)
    dirpath, runner, exceptions_path = args[:3]
    min_vectors = int(os.environ.get("HTML_SERIALIZER_MIN_VECTORS", "0"))
    min_pass = int(os.environ.get("HTML_SERIALIZER_MIN_PASS", "0"))
    min_stable = int(os.environ.get("HTML_SERIALIZER_MIN_STABLE", "0"))
    for problem in selftest_causes():
        sys.stderr.write("\033[0;31m\n### UNEXPLAINED ROUND TRIPS: the cause rule is wrong: %s ###\033[0m\n" % problem)
        return 1
    listed = load_exceptions(exceptions_path)
    total, passes, unexplained, stale = score_vectors(dirpath, runner, listed, propose, verbose)
    if propose:
        return 0
    rt_unexplained = []
    rt_stats = collections.Counter()
    if len(args) >= 4:
        tree = load_tree_module()
        rt_stats, causes, rt_unexplained = round_trip(tree, args[3], args[4] if len(args) == 5 else None, runner, 0, verbose)
        print("round trip over the tree vectors: %d inputs, %d runs (each under the scripting settings it names, or both); stable (same tree, same bytes): %d; tree differs: %d; not idempotent: %d; "
              "explained by a cause the standard names: %d; unexplained: %d; inputs with an unstable run: %d" % (
                  sum(1 for k in rt_stats if k.startswith("vector:")), rt_stats["runs"], rt_stats["stable"], rt_stats["tree-differs"], rt_stats["not-idempotent"],
                  rt_stats["explained"], len(rt_unexplained), sum(1 for k in rt_stats if k.startswith("unstable:"))))
        print("  causes (a run can have several): " + (", ".join("%s %d" % kv for kv in sorted(causes.items())) or "none"))
    failed = False
    for title, items in (("UNEXPLAINED FAILURES", unexplained), ("STALE EXCEPTIONS", stale),
                         ("UNEXPLAINED ROUND TRIPS", rt_unexplained)):
        if items:
            failed = True
            sys.stderr.write("\033[0;31m\n### %s (%d) ###\033[0m\n" % (title, len(items)))
            for line in items[:60 if not verbose else 100000]:
                sys.stderr.write("  %s\n" % line)
    if total < min_vectors:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d vectors loaded, floor %d ###\033[0m\n" % (total, min_vectors))
    if passes < min_pass:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d passing, floor %d ###\033[0m\n" % (passes, min_pass))
    if rt_stats and rt_stats["stable"] < min_stable:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d stable round trips, floor %d ###\033[0m\n" % (rt_stats["stable"], min_stable))
    if failed:
        return 3
    print("html serializer: every vector passes or is a listed exception; none is stale%s." % (
        "; every round trip is stable or has a cause the standard names" if rt_stats else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
