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
"""The HTML tokenizer against html5lib's and parse5's tokenizers.

    html_tokenizer_diff.py <tokenizer-runner> [--count N] [--seed S] [--plant KIND]
                           [--show N] [--no-corpus] [--propose]

The tokenizer (src/html/html_tokenizer.c) was written from the standard's text and is
scored on the html5lib-tests vectors; this is the second, independent check, over
inputs nobody wrote a vector for. The same input goes to the tokenizer (through the
runner of tools/conformance, which also cuts it a byte at a time and in random
pieces), to html5lib's tokenizer and to parse5's, all in the shape of the vectors'
output (containers/html/driver.py describes the request), and the three are compared.

**The population** (every input is run in the data state and in a few of the others,
with the last start tag the state needs):

  vector  the input of every html5lib-tests tokenizer vector, in its initial states;
  wpt     the input of every tree-construction vector of web-platform-tests (real
          documents, hand-made to be odd), in the data state and in RCDATA, RAWTEXT,
          script data and PLAINTEXT with the tag name that state is for;
  gen     generated: a sequence of pieces drawn from a vocabulary of the tokenizer's
          own edges (every delimiter, the names of the character references, escapes
          of script data, quotes, controls, noncharacters, astral characters), and
          tags, comments, DOCTYPEs and references built from them;
  mut     the vectors' and the documents' inputs mutated (a span deleted, duplicated,
          swapped; a piece inserted; the input cut off at a random point, which is
          every "end of file in ..." state).

**What is compared.** The tokens: kind, every field, attributes in source order, with
adjacent characters coalesced and the end-of-file token and an end tag's attributes
and flag left out (the vectors' shape has neither). And the errors: ours against
parse5's, as (code, line, column) in order. html5lib's errors are its own codes
without positions (and fewer kinds of them than the standard names), so they are not compared
with ours at all: html5lib is a reference for tokens only.

**Inputs that contain `<?` are left out** (but in PLAINTEXT, which never leaves its state;
in the others the state returns to data at an end tag or `]]>`). The standard has had
processing instruction states since 2025; neither reference has them, so every such
input would differ and say nothing. They are counted, and the vectors of
html5lib-tests (which do have them) are what score them.

**What fails the run.**

  * Our tokenizer giving a different answer when the input is cut a byte at a time or in
    random pieces: never listed.
  * A divergence from a reference whose cause is not one of the MAPPERS, PREDICATES and ANSWER_CAUSES below: each cause is a
    behaviour of one reference that the standard does not have (or the other way), found
    by reading the standard and the reference, and it is a line of
    tools/oracle/html_tokenizer_divergences.txt with the count of inputs showing it in the
    default population, a class and a reason. An entry that does not occur, or occurs a
    different number of times, fails (the count is the signature).
  * A reference that could not answer, or a population that has shrunk.

`--plant KIND` corrupts the tokenizer's answer on purpose and run-html-oracle.sh
--controls requires the run to fail for each; a differential that cannot be seen to fail
prints the same clean line as one that passes.

Copyright 2026 by Corey Pennycuff
"""

import collections
import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "conformance"))

import html_ref  # noqa: E402
import html_tokenizer_lib as lib  # noqa: E402
import html_vectors  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
LIST = os.path.join(HERE, "html_tokenizer_divergences.txt")
VECTORS = os.path.join(ROOT, "third_party", "html", "html5lib-tests", "tokenizer")
ENTITIES = os.path.join(ROOT, "third_party", "html", "entities.json")
DEFAULT_COUNT, DEFAULT_SEED = 6000, 1

CLASSES = ("reference-old", "reference-wrong", "standard-newer", "reference-api")

PLANTS = {
    "drop-token": "a token is dropped from our answer",
    "change-attribute": "an attribute value is changed",
    "swap-error-code": "an error code is changed",
    "move-error": "an error's column is moved",
    "extra-error": "an error is added",
    "stale-entry": "a listed divergence is made to go away",
    "change-count": "a listed divergence's count is changed",
    "no-list": "the list is read as empty",
}
OUTPUT_PLANTS = ("drop-token", "change-attribute", "swap-error-code", "move-error", "extra-error")

# The default population must stay a population.
MIN_INPUTS = 13000
MIN_REFERENCE_AGREEMENT = 0.90

LAST = {"rcdata": ["title", "textarea"], "rawtext": ["style", "xmp", "iframe", "noembed", "noframes", "noscript"],
        "script": ["script"], "plaintext": [None], "cdata": [None]}


def hunting_run(count, seed):
    return (count, seed) != (DEFAULT_COUNT, DEFAULT_SEED)


# ---- the population ------------------------------------------------------------------

def vector_inputs():
    out = []
    for name in sorted(os.listdir(VECTORS)):
        if not name.endswith(".test"):
            continue
        with open(os.path.join(VECTORS, name), encoding="utf-8") as fh:
            data = json.load(fh)
        for index, t in enumerate(data.get("tests", data.get("xmlViolationTests", []))):
            text = t["input"]
            if t.get("doubleEscaped"):
                text = lib.unescape(text)
            for st in t.get("initialStates", ["Data state"]):
                out.append(("vector:%s#%d" % (name, index), text, lib.STATES[st], t.get("lastStartTag")))
    return out


def wpt_inputs(rng):
    vectors, _files = html_vectors.load_vectors()
    out = []
    for v in vectors:
        if v.context is not None:
            continue
        out.append(("wpt:" + v.ident, v.data, "data", None))
        # A fifth of them in another state too.
        if rng.random() < 0.2:
            state = rng.choice(["rcdata", "rawtext", "script", "plaintext"])
            out.append(("wpt:%s@%s" % (v.ident, state), v.data, state, rng.choice(LAST[state])))
    return out


def entity_names():
    with open(ENTITIES, encoding="utf-8") as fh:
        names = [k[1:] for k in json.load(fh)]
    legacy = [n for n in names if not n.endswith(";") and n + ";" in names]
    return names, legacy


class Generator:
    TAGS = ["a", "b", "p", "div", "span", "script", "style", "title", "textarea", "xmp", "iframe",
            "noembed", "noframes", "noscript", "plaintext", "br", "svg", "math", "table", "td", "select",
            "option", "A", "SCRIPT", "Style", "x-y", "a-b.c", "h1", "image", "button"]
    ATTRS = ["id", "class", "href", "src", "a", "B", "data-x", "xlink:href", "xmlns", "=", "\"", "'", "<", "`",
             "a=b", "ONCLICK", "é", " ", "x\x00y", "a/b", "a b"]
    VALUES = ["", "x", "1", "a b", "&amp;", "&", "&lt", "&notit;", "&copy", "&copy=", "'", "\"", "`", "<>", "=",
              "x\x00y", "\r\n", "&#x41;", "&#0;", "&#128512;", "é", "\U0001f600"]
    TEXTS = ["foo", " ", "\n", "\r\n", "\r", "\t", "\f", "\x00", "\x0b", "\x01", "\x7f", "\x85", "\x9f", "é",
             "€", "\U0001f600", "﷐", "￾", "￿", "\U0010ffff", "﻿", " ", "�"]
    PIECES = ["<", ">", "/", "!", "-", "--", "---", "-->", "--!>", "--!", "<!--", "<!-", "<!", "</", "<?", "?>",
              "?", "]]>", "]]", "]", "[", "<![CDATA[", "[CDATA[", "<!DOCTYPE", "<!doctype html", "<!DOCTYPE html>",
              " PUBLIC ", " SYSTEM ", "public", "system", "'", "\"", "=", "&", ";", "&#", "&#x", "&#X", "#", "x",
              "<script>", "</script>", "<script", "</script", "<!--<script>", "</scriPt>", "<style>", "</style>",
              "<title>", "</title>", "<textarea>", "</textarea>", "<plaintext>", "</", " ", "\t", "\n", "a", "Z",
              "0", "9", "_", ":", "`", "<<", ">>", "&&", "--", "<!-- ", " -->", "<br/>", "/>", "<a/", "<a ",
              "<a b", "<a b=", "<a b='", "<a b=\"", "&amp", "&amp;", "&AMP", "&Amp;"]

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.names, self.legacy = entity_names()

    def pick(self, seq):
        return seq[self.rng.randrange(len(seq))]

    def reference(self):
        r = self.rng.random()
        if r < 0.35:
            n = self.pick(self.names)
        elif r < 0.55:
            n = self.pick(self.legacy)
        elif r < 0.65:
            n = self.pick(self.legacy) + self.pick("abcxyz0=;-_")
        elif r < 0.75:
            # A real name cut short or extended.
            base = self.pick(self.names)
            n = base[:self.rng.randrange(1, len(base) + 1)]
        elif r < 0.9:
            kind = self.rng.choice(["#x%X", "#X%x", "#%d", "#x%x", "#%08d"])
            code = self.pick([0, 9, 10, 13, 0x7f, 0x80, 0x81, 0x8d, 0x95, 0x9f, 0xa0, 0xd7ff, 0xd800, 0xdbff, 0xdc00,
                              0xdfff, 0xe000, 0xfdd0, 0xfdef, 0xfffe, 0xffff, 0x10000, 0x10ffff, 0x110000,
                              0x7fffffff, 0xffffffff, 0x100000000, self.rng.randrange(0, 0x110000),
                              self.rng.randrange(0, 0x200), 65, 0x41])
            n = kind % code
            if self.rng.random() < 0.7:
                n += ";"
        else:
            n = self.pick(["#", "#x", "#X", "#;", "#x;", "x", "", "9", "a1;"])
        return "&" + n + self.pick([";", "", "", " ", "x", "=", "<", "&", "1"] if not n.endswith(";") else [""])

    def attribute(self):
        name = self.pick(self.ATTRS)
        r = self.rng.random()
        if r < 0.15:
            return name
        v = self.pick(self.VALUES)
        if self.rng.random() < 0.3:
            v += self.reference()
        quote = self.pick(['"', "'", "", '"', "'"])
        return name + self.pick(["=", " = ", "=", "=\t"]) + quote + v + (quote if self.rng.random() < 0.93 else "")

    def tag(self):
        name = self.pick(self.TAGS)
        end = self.rng.random() < 0.3
        parts = ["</" if end else "<", name]
        for _ in range(self.rng.choice([0, 0, 1, 1, 2, 3, 5])):
            parts.append(self.pick([" ", " ", "  ", "\n", "/", "\t", "", "\f"]))
            parts.append(self.attribute())
        if self.rng.random() < 0.1:
            parts.append("/")
        if self.rng.random() < 0.94:
            parts.append(">")
        return "".join(parts)

    def comment(self):
        body = "".join(self.pick(self.PIECES + self.TEXTS) for _ in range(self.rng.choice([0, 1, 1, 2, 4])))
        opener = self.pick(["<!--", "<!--", "<!--", "<!-", "<!", "</", "<?"])
        closer = self.pick(["-->", "-->", "--!>", "->", ">", "--", "", "-- >", "--->"])
        return opener + body + closer

    def doctype(self):
        parts = [self.pick(["<!DOCTYPE", "<!doctype", "<!DocType", "<!DOCTYP", "<!DOCTYPEhtml"])]
        for _ in range(self.rng.choice([0, 1, 2, 3, 4])):
            parts.append(self.pick([" ", "  ", "\t", "", "\n"]))
            parts.append(self.pick(["html", "HTML", "x\x00y", "PUBLIC", "public", "SYSTEM", "system", "\"a\"", "'b'",
                                    "\"", "'", "\"a'", ">", "x", "-//W3C//DTD//EN", "\"-//W3C//DTD HTML 4.01//EN\"",
                                    "\"http://www.w3.org/TR/html4/strict.dtd\"", "PUBLIC\"a\"", "SYSTEM'b'"]))
        if self.rng.random() < 0.9:
            parts.append(">")
        return "".join(parts)

    def fragment(self):
        r = self.rng.random()
        if r < 0.22:
            return self.tag()
        if r < 0.32:
            return self.comment()
        if r < 0.38:
            return self.doctype()
        if r < 0.55:
            return self.reference()
        if r < 0.70:
            return self.pick(self.PIECES)
        if r < 0.82:
            return self.pick(self.TEXTS)
        if r < 0.9:
            return "<![CDATA[" + self.pick(self.TEXTS + ["]", "]]", "x"]) + self.pick(["]]>", "]]", ""])
        return self.pick(["<script>", "</script>", "<!--", "-->", "<!--<script>", "</script>", "<scRipt "]) + self.pick(self.TEXTS)

    def sequence(self):
        return "".join(self.fragment() for _ in range(self.rng.choice([1, 1, 2, 3, 4, 6, 10, 20])))

    def mutate(self, text):
        rng = self.rng
        for _ in range(rng.choice([1, 1, 2, 3])):
            if not text:
                text = self.fragment()
                continue
            kind = rng.choice(["delete", "duplicate", "swap", "insert", "truncate", "replace"])
            a = rng.randrange(len(text))
            b = min(len(text), a + rng.choice([1, 1, 2, 3, 5, 12, 40]))
            if kind == "delete":
                text = text[:a] + text[b:]
            elif kind == "duplicate":
                text = text[:b] + text[a:b] + text[b:]
            elif kind == "swap":
                c = rng.randrange(len(text))
                d = min(len(text), c + (b - a))
                text = text[:a] + text[c:d] + text[b:c] + text[a:b] + text[d:] if a < c else text
            elif kind == "insert":
                text = text[:a] + self.fragment() + text[a:]
            elif kind == "truncate":
                text = text[:a]
            else:
                text = text[:a] + self.fragment() + text[b:]
        return text


def population(count, seed, corpus):
    rng = random.Random(seed)
    gen = Generator(seed)
    items = []
    if corpus:
        items.extend(vector_inputs())
        items.extend(wpt_inputs(rng))
    base = [i for i in items if i[2] == "data" and len(i[1]) < 600]
    for n in range(count):
        r = rng.random()
        if r < 0.45 or not base:
            text = gen.sequence()
            origin = "gen"
        else:
            text = gen.mutate(rng.choice(base)[1])
            origin = "mut"
        state = rng.choice(["data"] * 5 + ["rcdata", "rawtext", "script", "plaintext", "cdata"])
        last = rng.choice(LAST[state]) if state in LAST else None
        items.append(("%s:%d" % (origin, n), text, state, last))
    # Unique requests only; the first id stands for the rest.
    seen = {}
    out = []
    for ident, text, state, last in items:
        key = (text, state, last)
        if key in seen:
            continue
        seen[key] = True
        out.append({"id": ident, "text": text, "state": state, "last": last})
    return out


# ---- reading the answers ---------------------------------------------------------------

def reference_tokens(tokens):
    out = []
    for t in tokens:
        kind = t[0]
        if kind == "DOCTYPE":
            name, public, system, correct = t[1:5]
            out.append(("D", name is not None, lib.enc(name or ""), public is not None, lib.enc(public or ""),
                        system is not None, lib.enc(system or ""), not correct))
        elif kind == "StartTag":
            pairs = tuple((lib.enc(k), lib.enc(v)) for k, v in t[2])
            out.append(("S", lib.enc(t[1]), pairs, len(pairs), bool(t[3])))
        elif kind == "EndTag":
            out.append(("E", lib.enc(t[1])))
        elif kind == "Comment":
            out.append(("M", lib.enc(t[1])))
        elif kind == "Character":
            out.append(("C", lib.enc(t[1])))
        else:
            raise SystemExit("unknown reference token %r" % (t,))
    return lib.coalesce(out)


def run_ours(runner, items):
    requests = []
    for n, it in enumerate(items):
        last = "-" if it["last"] is None else lib.hexs(lib.enc(it["last"]))
        base = "%s %s " % (it["state"], last) + lib.hexs(lib.enc(it["text"]))
        requests.append("whole " + base)
        requests.append("bytes " + base)
        requests.append("chunks:%d " % (n + 1) + base)
    answers, crashed = lib.run_all(runner, requests)
    if crashed:
        raise SystemExit("the tokenizer's runner died on %d requests, the first %s" % (len(crashed), requests[crashed[0]][:200]))
    ours = []
    cut_differs = []
    for n, it in enumerate(items):
        whole, by_bytes, chunks = answers[3 * n], answers[3 * n + 1], answers[3 * n + 2]
        if whole != by_bytes or whole != chunks:
            cut_differs.append(it["id"])
        tokens, errors, dropped, status = lib.parse_answer(whole, ordered=True)
        ours.append({"tokens": [t for t in lib.coalesce(tokens) if t[0] != "F"],
                     "errors": [(e[0], e[1], e[2]) for e in errors], "status": status})
    return ours, cut_differs


def apply_plant(ours, plant):
    """Corrupt our answers on purpose; returns how many it changed."""
    fired = 0
    if plant not in OUTPUT_PLANTS:
        return 0
    for n, a in enumerate(ours):
        if n % 7 != 3:
            continue
        if plant == "drop-token" and a["tokens"]:
            del a["tokens"][-1]
            fired += 1
        elif plant == "change-attribute":
            for i, t in enumerate(a["tokens"]):
                if t[0] == "S" and t[2]:
                    k, v = t[2][0]
                    a["tokens"][i] = (t[0], t[1], ((k, v + b"!"),) + t[2][1:], t[3], t[4])
                    fired += 1
                    break
        elif plant == "swap-error-code" and a["errors"]:
            code, line, col = a["errors"][0]
            a["errors"][0] = ("eof-in-tag" if code != "eof-in-tag" else "eof-in-comment", line, col)
            fired += 1
        elif plant == "move-error" and a["errors"]:
            code, line, col = a["errors"][-1]
            a["errors"][-1] = (code, line, col + 1)
            fired += 1
        elif plant == "extra-error":
            a["errors"].append(("eof-in-tag", 1, 1))
            fired += 1
    return fired


# ---- comparing ----------------------------------------------------------------------------

def first_difference(a, b):
    """(index, ours, theirs) of the first token that differs, or None."""
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        if x != y:
            return i, x, y
    return None


def kind_of(t):
    return "-" if t is None else t[0]


# The causes. A divergence is explained when some set of the causes below accounts for the
# whole of it. A MAPPER moves our answer toward the reference's by exactly what the cause says
# (a missing DOCTYPE name becomes an empty one; the unknown-named-character-reference errors
# in attribute values are dropped; ...) and may be combined with the others, because an input
# can show two causes; a PREDICATE is a cause too irregular to map (the reference tokenizes
# differently from that point on) and is tried last, on our answer as the mappers left it. The
# explanation is the smallest set of mappers (then one predicate) after which the two answers
# are equal, every one of which changed the answer, so a cause that does nothing is not
# credited with the divergence. Each cause is a behaviour found by reading the standard and the
# reference's source and probing it, and it is a line of
# tools/oracle/html_tokenizer_divergences.txt with the count of inputs showing it.
MAPPERS = collections.defaultdict(list)
PREDICATES = collections.defaultdict(list)


def mapper(reference, kind, name, klass, reason):
    def register(fn):
        MAPPERS[(reference, kind)].append((name, klass, reason, fn))
        return fn
    return register


def predicate(reference, kind, name, klass, reason):
    def register(fn):
        PREDICATES[(reference, kind)].append((name, klass, reason, fn))
        return fn
    return register


def explain(reference, kind, item, mine, theirs, ours, ans):
    """The names of the causes that account for mine != theirs, or None."""
    import itertools
    mappers = MAPPERS[(reference, kind)]
    for size in range(len(mappers) + 1):
        for subset in itertools.combinations(range(len(mappers)), size):
            current = mine
            used = []
            for k in subset:
                name, _klass, _reason, fn = mappers[k]
                changed = fn(item, current, ours, ans)
                if changed != current:
                    used.append(name)
                    current = changed
            if len(used) != size:
                continue
            if current == theirs and used:
                return used
            for name, _klass, _reason, fn in PREDICATES[(reference, kind)]:
                if fn(item, current, theirs, ours, ans):
                    return used + [name]
    return None


ANSWER_CAUSES = []


def answer_cause(reference, name, klass, reason):
    def register(fn):
        ANSWER_CAUSES.append((reference, name, klass, reason, fn))
        return fn
    return register


@answer_cause("parse5", "throws-on-two-low-surrogates", "reference-wrong",
              "parse5 8.0.1 throws `RangeError: Invalid code point` on two lone low surrogates in a row (U+DC00 to U+DFFF, "
              "twice), as if they were a pair. Lone surrogates cannot occur in UTF-8 and the vectors have them only as JSON "
              "escapes; the inputs here are the vectors' and their mutations. The tokenizer reads each as a surrogate "
              "(surrogate-in-input-stream) in WTF-8 mode.")
def _two_low_surrogates(item, ans):
    return ans.get("error", "").startswith("RangeError: Invalid code point") and \
        re.search("[\udc00-\udfff][\udc00-\udfff]", item["text"]) is not None


def their_errors(ans):
    return [(e["code"], e["line"], e["col"]) for e in ans["errors"]]


STREAM_ERRORS = ("surrogate-in-input-stream", "noncharacter-in-input-stream", "control-character-in-input-stream")


@mapper("html5lib", "tokens", "doctype-name-empty-not-missing", "reference-old",
        "html5lib's tokenizer gives a DOCTYPE with no name the name \"\" and not None; the standard (and the vectors) "
        "have a distinct missing name, which parse5 and this tokenizer keep. Every other field agrees.")
def _doctype_name(item, tokens, ours, ans):
    return [("D", True) + t[2:] if t[0] == "D" and not t[1] else t for t in tokens]


@mapper("html5lib", "tokens", "nul-in-cdata-section", "standard-newer",
        "html5lib replaces a U+0000 in a CDATA section with U+FFFD in the tokenizer; the standard emits it as it is "
        "(\"U+0000 NULL characters are handled in the tree construction stage, as part of the rules for parsing tokens in "
        "foreign content\"), so a tree builder can tell it from a replacement character. Only the NULs before the "
        "first `]]>` are in the CDATA section; after it the state is data, and html5lib keeps them there too.")
def _nul_cdata(item, tokens, ours, ans):
    if item["state"] != "cdata":
        return tokens
    norm = item["text"].replace("\r\n", "\n").replace("\r", "\n")
    end = norm.find("]]>")
    remaining = (norm if end < 0 else norm[:end]).count("\x00")
    mapped = []
    for t in tokens:
        if t[0] == "C" and remaining:
            data = t[1]
            out = b""
            for k in range(len(data)):
                if data[k:k + 1] == b"\x00" and remaining:
                    out += b"\xef\xbf\xbd"
                    remaining -= 1
                else:
                    out += data[k:k + 1]
            t = ("C", out)
        mapped.append(t)
    return lib.coalesce(mapped)


@predicate("html5lib", "tokens", "nul-after-comment-open-keeps-the-start-state", "reference-wrong",
           "html5lib 1.1 does not leave the comment start state (or the comment start dash state) when it reads a "
           "U+0000: `<!--\\x00>` ends the comment at the `>` (the standard goes to the comment state, where `>` is data) "
           "and `<!---\\x00!` puts a second hyphen before the `!`. The standard's states are 13.2.5.43 and 13.2.5.44. "
           "The input has a NUL directly after `<!--` or `<!---`, and the first token that differs is a comment.")
def _nul_comment_start(item, tokens, theirs, ours, ans):
    if "<!--\x00" not in item["text"] and "<!---\x00" not in item["text"]:
        return False
    d = first_difference(tokens, theirs)
    return d is not None and (kind_of(d[1]) == "M" or kind_of(d[2]) == "M")


def _offsets(text):
    """{(line, col): index into text} for every character of the normalized text."""
    norm = text.replace("\r\n", "\n").replace("\r", "\n")
    line, col = 1, 1
    out = {}
    for i, ch in enumerate(norm):
        out[(line, col)] = i
        if ch == "\n":
            line, col = line + 1, 1
        else:
            col += 2 if ord(ch) > 0xFFFF else 1
    out[(line, col)] = len(norm)
    return norm, out


def _in_attribute_value(norm, index, state="data", last=None):
    """Is norm[index] inside the value of an attribute of a tag? A scan that follows the
    tokenizer through a tag's insides only (the states from tag name to after the quoted
    value), enough to say where an `&name;` is in the inputs this generates. It does not
    know comments, DOCTYPEs or the text states, so an input where it guesses wrong shows as
    a divergence with no entry."""
    n = len(norm)
    ws = " \t\n\f"
    i = 0
    # The state the tokenizer started in holds until its end: the appropriate end tag, or `]]>`.
    if state == "plaintext":
        return False
    if state == "cdata":
        end = norm.find("]]>")
        i = n if end < 0 else end + 3
    elif state in ("rcdata", "rawtext", "script"):
        m = re.search(r"</%s[ \t\n\f/>]" % re.escape(last or ""), norm, re.IGNORECASE)
        i = n if m is None else m.start()
    if index < i:
        return False
    while i < n:
        if norm[i] == "<" and i + 1 < n and norm[i + 1] in "!?/" and not (
                norm[i + 1] == "/" and i + 2 < n and norm[i + 2].isalpha()):
            # A comment, a bogus comment or a DOCTYPE: nothing in it is a reference.
            if norm.startswith("<!--", i):
                ends = [k for k in (norm.find("-->", i + 4), norm.find("--!>", i + 4)) if k >= 0]
                if norm.startswith("<!-->", i):
                    i += 5
                elif norm.startswith("<!--->", i):
                    i += 6
                else:
                    i = (min(ends) + 3) if ends else n
            else:
                k = norm.find(">", i)
                i = n if k < 0 else k + 1
            continue
        opens = norm[i] == "<" and i + 1 < n and (norm[i + 1].isalpha() or (
            norm[i + 1] == "/" and i + 2 < n and norm[i + 2].isalpha()))
        if not opens:
            i += 1
            continue
        state = "tagname"
        quote = ""
        j = i + (2 if norm[i + 1] == "/" else 1)
        while j < n:
            ch = norm[j]
            if j == index:
                return state in ("quoted", "unquoted")
            if state == "tagname":
                if ch in ws or ch == "/":
                    state = "before_name"
                elif ch == ">":
                    break
            elif state == "before_name":
                if ch == ">":
                    break
                if ch not in ws and ch != "/":
                    state = "name"
            elif state == "name":
                if ch in ws:
                    state = "after_name"
                elif ch == "/":
                    state = "before_name"
                elif ch == ">":
                    break
                elif ch == "=":
                    state = "before_value"
            elif state == "after_name":
                if ch == "/":
                    state = "before_name"
                elif ch == "=":
                    state = "before_value"
                elif ch == ">":
                    break
                elif ch not in ws:
                    state = "name"
            elif state == "before_value":
                if ch in "\"'":
                    state, quote = "quoted", ch
                elif ch == ">":
                    break
                elif ch not in ws:
                    state = "unquoted"
            elif state == "quoted":
                if ch == quote:
                    state = "after_quoted"
            elif state == "after_quoted":
                if ch in ws or ch == "/":
                    state = "before_name"
                elif ch == ">":
                    break
                else:
                    state = "name"
            elif state == "unquoted":
                if ch in ws:
                    state = "before_name"
                elif ch == ">":
                    break
            j += 1
        i = j + 1
    return False


@mapper("parse5", "errors", "unknown-named-reference-in-attribute", "reference-wrong",
        "parse5 does not report unknown-named-character-reference for a reference in an attribute value (`<a b='&foo;'>`); "
        "the ambiguous ampersand state raises it on the semicolon wherever the reference is, and the vectors only cover "
        "it in text and RCDATA. What is missing from parse5's list is that error alone, each at a semicolon inside "
        "the value of a tag's attribute.")
def _unknown_in_attribute(item, errors, ours, ans):
    theirs = their_errors(ans)
    norm, where = _offsets(item["text"])
    out = []
    for e in errors:
        if (e[0] == "unknown-named-character-reference" and e not in theirs and e[1:] in where
                and _in_attribute_value(norm, where[e[1:]], item["state"], item["last"])):
            continue
        out.append(e)
    return out


REFERENCE_ERRORS = ("missing-semicolon-after-character-reference", "absence-of-digits-in-numeric-character-reference",
                    "null-character-reference", "character-reference-outside-unicode-range",
                    "surrogate-character-reference", "noncharacter-character-reference",
                    "control-character-reference", "unknown-named-character-reference")


def _ordered(errors):
    """At one position, the errors of a character reference before the input stream's error for the character
    that is there (where that is all the position holds)."""
    out = []
    i = 0
    while i < len(errors):
        j = i
        while j < len(errors) and errors[j][1:] == errors[i][1:]:
            j += 1
        run = errors[i:j]
        stream = [e for e in run if e[0] in STREAM_ERRORS]
        rest = [e for e in run if e[0] not in STREAM_ERRORS]
        out.extend(rest + stream if stream and rest and all(e[0] in REFERENCE_ERRORS for e in rest) else run)
        i = j
    return out


@mapper("parse5", "errors", "stream-error-after-reference-errors", "reference-wrong",
        "parse5 reports the input stream's error for the character that ends a character reference (`&#xD` then U+FFFF) "
        "after the reference's own errors, because its reference decoder reads ahead of the preprocessor; the "
        "standard's input stream errors belong to the character, which the state machine consumes first, and this "
        "tokenizer raises it then. The same errors, at the same positions, in another order.")
def _stream_order(item, errors, ours, ans):
    return _ordered(errors) if "&" in item["text"] else errors


def _amp_lf_shifts(text):
    """(line, col) of every line feed that directly follows an `&`, in the normalized text."""
    norm = text.replace("\r\n", "\n").replace("\r", "\n")
    line, col, prev = 1, 1, ""
    out = []
    for ch in norm:
        if ch == "\n":
            if prev == "&":
                out.append((line, col))
            line, col = line + 1, 1
        else:
            col += 2 if ord(ch) > 0xFFFF else 1
        prev = ch
    return out


@mapper("parse5", "errors", "line-feed-after-ampersand-counted-twice", "reference-wrong",
        "parse5 8.0.1 counts a line feed that directly follows an `&` as two lines (`&\\n` then any error reports line 3), "
        "because it reads the character to see whether a reference follows, and reads it again. Lines after such a line "
        "feed are one too many for each; columns are right. Ours are the lines of the normalized input.")
def _amp_lf(item, errors, ours, ans):
    shifts = _amp_lf_shifts(item["text"])
    if not shifts:
        return errors
    return [(c, l + sum(1 for s in shifts if s < (l, k)), k) for c, l, k in errors]


def compare(items, ours, answers):
    """{(reference, kind, cause): [ids]} for each divergence, the unexplained ones, and the agreement counts."""
    found = collections.defaultdict(list)
    unexplained = []
    agree = collections.Counter()
    for n, it in enumerate(items):
        o = ours[n]
        for ref in ("parse5", "html5lib"):
            ans = answers[ref][n]
            if "tokens" not in ans:
                names = [c[1] for c in ANSWER_CAUSES if c[0] == ref and c[4](it, ans)]
                if names:
                    found[(ref, "answer", names[0])].append(it["id"])
                else:
                    unexplained.append((it, ref, "answer", o, ans))
                continue
            theirs_tokens = reference_tokens(ans["tokens"])
            checks = [("tokens", o["tokens"], theirs_tokens)]
            if ref == "parse5":
                # html5lib's errors are its own codes without positions: nothing to compare.
                checks.append(("errors", o["errors"], their_errors(ans)))
            for kind, mine, theirs in checks:
                if mine == theirs:
                    agree[(ref, kind)] += 1
                    continue
                names = explain(ref, kind, it, mine, theirs, o, ans)
                if names is None:
                    unexplained.append((it, ref, kind, o, ans))
                else:
                    for name in names:
                        found[(ref, kind, name)].append(it["id"])
    return found, unexplained, agree


# ---- the list ------------------------------------------------------------------------------

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
                raise SystemExit("%s:%d: not <key> <count> <class> <reason>" % (path, n))
            key, count, klass, reason = parts
            if klass not in CLASSES:
                raise SystemExit("%s:%d: %r is not a class (%s)" % (path, n, klass, ", ".join(CLASSES)))
            if not reason.strip():
                raise SystemExit("%s:%d: %s has no reason" % (path, n, key))
            if key in table:
                raise SystemExit("%s:%d: %s is listed twice" % (path, n, key))
            table[key] = (count, klass, reason)
    return table


def main(argv):
    runner = argv[1]
    count, seed = DEFAULT_COUNT, DEFAULT_SEED
    plant = None
    show = 12
    corpus = True
    propose = False
    args = argv[2:]
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--plant":
            plant = args.pop(0)
            if plant not in PLANTS:
                raise SystemExit("unknown plant %r (%s)" % (plant, ", ".join(PLANTS)))
        elif a == "--show":
            show = int(args.pop(0))
        elif a == "--no-corpus":
            corpus = False
        elif a == "--propose":
            propose = True
        else:
            raise SystemExit("unknown argument %r" % a)
    hunting = hunting_run(count, seed) or not corpus
    html_ref.check_image()
    items = population(count, seed, corpus)
    excluded = [it for it in items if it["state"] != "plaintext" and "<?" in it["text"]]
    items = [it for it in items if not (it["state"] != "plaintext" and "<?" in it["text"])]
    print("html tokenizer differential: %d inputs (%d with <? left out), seed %d, count %d"
          % (len(items), len(excluded), seed, count), flush=True)

    ours, cut_differs = run_ours(runner, items)
    fired = apply_plant(ours, plant)
    if plant in OUTPUT_PLANTS and not fired:
        raise SystemExit("the plant %s never changed an answer, so a pass proves nothing" % plant)
    requests = [{"tokenize": it["text"], "state": it["state"], "last": it["last"]} for it in items]
    answers = html_ref.ask_all(requests)
    status = 0
    found, unexplained, agree = compare(items, ours, answers)

    total = len(items)
    print("  tokens agree with parse5 on %d of %d, with html5lib on %d; errors agree with parse5 on %d"
          % (agree[("parse5", "tokens")], total, agree[("html5lib", "tokens")], agree[("parse5", "errors")],
             ))
    print("  our tokenizer cut a byte at a time and in pieces against whole: %d inputs, %d differ" % (total, len(cut_differs)))

    if cut_differs:
        status = 3
        sys.stderr.write("\033[0;31m\n### Our answer depends on how the input was cut ###\033[0m\n  %s\n" % ", ".join(cut_differs[:10]))
    if unexplained:
        status = 3
        sys.stderr.write("\033[0;31m\n### Divergences with no entry ###\033[0m\n")
        shapes = collections.Counter()
        for it, ref, kind, o, ans in unexplained:
            shapes[(ref, kind)] += 1
        sys.stderr.write("  %s\n" % ", ".join("%s %s: %d" % (r, k, v) for (r, k), v in sorted(shapes.items())))
        for it, ref, kind, o, ans in unexplained[:show]:
            sys.stderr.write("  %s [%s] last=%s %s\n    input %r\n" % (it["id"], it["state"], it["last"], "%s %s" % (ref, kind), it["text"][:200]))
            if kind == "answer":
                sys.stderr.write("    the reference could not answer: %s\n" % ans.get("error", "?"))
            elif kind == "tokens":
                theirs = reference_tokens(ans["tokens"])
                d = first_difference(o["tokens"], theirs)
                lo = max(0, d[0] - 1)
                sys.stderr.write("    tokens differ at %d:\n    ours:   %r\n    theirs: %r\n" % (d[0], o["tokens"][lo:lo + 3], theirs[lo:lo + 3]))
            else:
                theirs = [(e["code"], e["line"], e["col"]) for e in ans["errors"]]
                d = first_difference(o["errors"], theirs)
                lo = max(0, d[0] - 1)
                sys.stderr.write("    errors differ at %d: ours %r theirs %r\n    ours:   %r\n    theirs: %r\n"
                                 % (d[0], d[1], d[2], o["errors"][lo:lo + 4], theirs[lo:lo + 4]))
    listed = {} if plant == "no-list" else load_list()
    seen = {}
    for (ref, kind, name), ids in sorted(found.items()):
        seen["%s:%s:%s" % (ref, kind, name)] = len(ids)
    if plant == "stale-entry":
        listed = dict(listed)
        listed["parse5:tokens:planted-gone"] = ("1", "reference-old", "a planted entry that never occurs")
    if plant == "change-count":
        listed = dict(listed)
        for k in sorted(listed):
            if listed[k][0].startswith("n="):
                listed[k] = ("n=%d" % (int(listed[k][0][2:]) + 1),) + listed[k][1:]
                break
    if propose:
        info = {}
        for (ref, kind), group in list(MAPPERS.items()) + list(PREDICATES.items()):
            for name, klass, reason, _fn in group:
                info["%s:%s:%s" % (ref, kind, name)] = (klass, reason)
        for ref, name, klass, reason, _fn in ANSWER_CAUSES:
            info["%s:answer:%s" % (ref, name)] = (klass, reason)
        for key, n in sorted(seen.items()):
            print("%s\tn=%d\t%s\t%s" % (key, n, info[key][0], info[key][1]))
    if not hunting:
        unlisted = [k for k in seen if k not in listed]
        gone = [k for k in listed if k not in seen and listed[k][0] != "hunt"]
        changed = [(k, listed[k][0], "n=%d" % seen[k]) for k in seen
                   if k in listed and listed[k][0] not in ("hunt", "n=%d" % seen[k])]
        if unlisted:
            status = 3
            sys.stderr.write("\033[0;31m\n### Divergences with no entry ###\033[0m\n")
            for k in unlisted:
                sys.stderr.write("  %s: %d (e.g. %s)\n" % (k, seen[k], ", ".join(found[tuple(k.split(":", 2))][:3])))
        if gone:
            status = 3
            sys.stderr.write("\033[0;31m\n### Listed divergences that are gone ###\033[0m\n")
            for k in gone:
                sys.stderr.write("  %s: %s\n" % (k, listed[k][0]))
        if changed:
            status = 3
            sys.stderr.write("\033[0;31m\n### Listed divergences whose count has changed ###\033[0m\n")
            for k, was, now in changed:
                sys.stderr.write("  %s: listed %s, now %s\n" % (k, was, now))
        if total < MIN_INPUTS:
            status = 3
            sys.stderr.write("\033[0;31m\n### The population has shrunk: %d inputs, floor %d ###\033[0m\n" % (total, MIN_INPUTS))
        if agree[("parse5", "tokens")] < MIN_REFERENCE_AGREEMENT * total:
            status = 3
            sys.stderr.write("\033[0;31m\n### parse5 agrees on too few inputs: %d of %d ###\033[0m\n" % (agree[("parse5", "tokens")], total))
    else:
        unlisted = [k for k in seen if k not in listed]
        if unlisted:
            print("  (a hunt: %d kinds of divergence are not in the list: %s)" % (len(unlisted), ", ".join(unlisted)))
    print("  divergences explained: %d inputs in %d kinds" % (sum(seen.values()), len(seen)))
    if status == 0:
        print("html tokenizer differential: clean")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
