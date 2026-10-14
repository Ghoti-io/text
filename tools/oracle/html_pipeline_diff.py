#!/usr/bin/env python3
"""The whole HTML pipeline against html5lib and parse5.

    html_pipeline_diff.py <ours-runner> [--count N] [--seed S] [--chunk N] [--plant NAME] [--write-list] [--show N] [--no-corpus]

Every stage of Phase 3 has its own differential (tokenizer, tree builder, sniffing, serializer). This one asks the public API as one
thing: **bytes** go in (documents generated across the constructs of the tree differential, the mutations of them, the inputs of the
tree-construction vectors, fragments in random contexts), in the encodings of the Encoding Standard with and without a byte order mark,
a `<meta>`, a transport label (certain or tentative) or a caller default, and what comes out is held to the references:

* **The encoding decision** (the label the parser ended with, a restart included) against html5lib's `documentEncoding` (its input stream
  with `useChardet` off). A disagreement is explained by the smallest set of html5lib's departures (html_sniff_model.QUIRKS, the same
  fifteen html_sniff_diff.py names, each with a line in html_sniff_divergences.txt) that makes the second reading of the standard answer as
  html5lib did, or it fails the run.
* **The reading of the bytes**: what the Encoding module decodes them to against what html5lib's codec for the same encoding makes of them.
  A difference is a class of the key (encoding, this library's code point, html5lib's), listed with the sentence of the Encoding Standard.
* **The tree**: this library's tree for the bytes against html5lib's for the same bytes and against html5lib's and parse5's for the text
  this library decoded them to (parse5 reads strings only), in the tree-construction dump format. Fragments (document, `td`, `svg path`...
  random contexts) against parse5, and against html5lib where it can take the context. A divergence is **reduced** to a minimal
  reproducer (delta debugging over the tokens of the document, each step asked of this library and of the reference again) and classified
  by what the reproducer holds; a divergence whose reproducer holds nothing the list knows fails the run, and prints the reproducer.
  Nothing is explained by deleting a construct from the document and seeing the divergence go.
* **This library against itself**: the tree of the bytes equals the tree of their decoded text parsed as UTF-8 (the integration is the same
  parser fed through a different route).
* **The serializer on what the parser made**: the tree's serialization against each reference's serializer on its own tree, where the
  trees agree. A difference is a class of the key (reference, what this library wrote -> what it wrote), listed with the standard's
  sentence (the serializer differential's classes, html_serialize_divergences.txt, found again here).
* **Parse-serialize-parse**: this library's serialization read again by this library and by both references (the trees must agree, or the
  divergence is classified as above on the serialization as the document), and the references' own serializations read again by
  themselves: the verdict "the second tree equals the first" must be the same for this library and for each reference, wherever the
  first trees and the serializations agree.

The list, html_pipeline_divergences.txt, has a line per class with its count for the default population (a count of `hunt` is a class only
larger populations reach), its class and the reason. An unlisted class, a stale one and a changed count fail the run; so does a reference
that cannot answer (except as listed: html5lib cannot take a foreign fragment context, and stops on an assertion in its template
handling), and a population that has drifted. `--plant NAME` corrupts the run to show each of those fails (run-html-pipeline-oracle.sh
--controls), and the same script compiles this library with a rule undone in each layer and requires the run to fail.

Copyright 2026 by Corey Pennycuff
"""

import collections
import json
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "conformance"))

import encoding_model  # noqa: E402
import html_ref  # noqa: E402
import html_sniff_diff as sniff_diff  # noqa: E402
import html_sniff_model  # noqa: E402
import html_tree_diff as tree_diff  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
LIST = os.path.join(HERE, "html_pipeline_divergences.txt")
TREE_LIST = os.path.join(HERE, "html_tree_divergences.txt")
SER_LIST = os.path.join(HERE, "html_serialize_divergences.txt")
VENV_PYTHON = "/opt/html-oracle/venv/bin/python3"
PIPELINE_PY = os.path.join(HERE, "containers", "html", "pipeline.py")
PIPELINE_MJS = os.path.join(HERE, "containers", "html", "pipeline.mjs")
DEFAULT_COUNT, DEFAULT_SEED = 1500, 1
MIN_DOCUMENTS = 5500
EXIT_FAIL = 3
RUNNER_SECONDS = 600

PLANTS = {
    "ours-extra-line": "one of this library's trees gets a line it should not have",
    "ours-wrong-encoding": "one decision of this library is changed to an encoding html5lib did not pick",
    "ours-bytes-differ": "the tree of one document's bytes differs from the tree of its text",
    "ours-unstable": "one serialization of this library is changed, so that its second tree is different",
    "stale-entry": "the list gets an entry that does not occur",
    "change-count": "the count of one listed entry is changed",
    "no-list": "the list is empty",
    "theirs-wrong": "one tree of a reference is changed",
    "driver-wrong": "the gate's drivers are found to dump a tree differently from the pinned ones",
    "witness": "a witness of a class no longer shows it",
}


# ---- asking this library ---------------------------------------------------------------------------

def hexs(b):
    return "-" if b is None else ("=" if b == b"" else b.hex())


def ours_line(mode, doc, transport=None, certain=True, default=None, ignore=False):
    return "%s %s %s %s %d x%s" % (mode, hexs(transport), "c" if certain else "t", hexs(default), 1 if ignore else 0, doc.hex())


def run_ours(runner, requests):
    payload = ("\n".join(requests) + "\n").encode("ascii")
    try:
        done = subprocess.run([runner], input=payload, capture_output=True, timeout=RUNNER_SECONDS)
    except subprocess.TimeoutExpired:
        print("FAIL: This library's runner did not answer in %d seconds (a loop that does not end)" % RUNNER_SECONDS)
        sys.exit(EXIT_FAIL)
    if done.returncode != 0:
        print("FAIL: This library's runner exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-2000:]))
        sys.exit(EXIT_FAIL)
    lines = done.stdout.decode("ascii", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if len(lines) != len(requests):
        raise SystemExit("sent %d requests to this library's runner, got %d answers" % (len(requests), len(lines)))
    return lines


def unx(h):
    return bytes.fromhex(h[1:]).decode("utf-8", "surrogatepass")


def parse_answer(line):
    f = line.split(" ")
    if f[0] != "OK":
        return {"error": line}
    dump, text, ser = unx(f[7]), unx(f[8]), unx(f[9])
    return {"enc": f[1].lower(), "source": int(f[2]), "conf": int(f[3]), "bom": int(f[4]), "reparsed": f[5] == "1", "flags": f[6],
            "dump": dump[:-1] if dump.endswith("\n") else dump, "text": text, "text_hex": f[8][1:], "ser": ser, "ser_hex": f[9][1:],
            "dump2": None if f[10] == "-" else (lambda d: d[:-1] if d.endswith("\n") else d)(unx(f[10])),
            "ser2": None if f[11] == "-" else unx(f[11])}


def ours_docs(runner, items, step=2000):
    """[answer] for each item (dict with kind, bytes, scripting, context, transport, certain, default, ignore)."""
    reqs = []
    for it in items:
        mode = ("doc:%d" % (1 if it["scripting"] else 0)) if it["kind"] == "doc" else \
            "frag:%d:x%s" % (1 if it["scripting"] else 0, it["context"].encode().hex())
        reqs.append(ours_line(mode, it["bytes"], it.get("transport"), it.get("certain", True), it.get("default"), it.get("ignore", False)))
    out = []
    for lo in range(0, len(reqs), step):
        out.extend(parse_answer(l) for l in run_ours(runner, reqs[lo:lo + step]))
    return out


def ours_texts(runner, texts):
    """[(status, dump)] of each (text, scripting, context) parsed as UTF-8 under a certain label."""
    reqs = []
    for text, scripting, context in texts:
        mode = ("doc:%d" % (1 if scripting else 0)) if context is None else "frag:%d:x%s" % (1 if scripting else 0, context.encode().hex())
        reqs.append(ours_line(mode, text.encode("utf-8", "surrogatepass"), b"utf-8", True, None, False))
    out = []
    for lo in range(0, len(reqs), 2000):
        for l in run_ours(runner, reqs[lo:lo + 2000]):
            a = parse_answer(l)
            out.append(("OK", a["dump"], a) if "error" not in a else (a["error"], "", a))
    return out


# ---- asking the references -------------------------------------------------------------------------

def _pipe(cmd, requests, chunk=3000):
    out = []
    for lo in range(0, len(requests), chunk):
        part = requests[lo:lo + chunk]
        payload = ("\n".join(json.dumps(r) for r in part) + "\n").encode("ascii")
        done = subprocess.run(cmd, input=payload, capture_output=True)
        if done.returncode != 0:
            raise SystemExit("a reference exited %d:\n%s" % (
                done.returncode, oracle_env.reference_stderr(done.stderr.decode("utf-8", "replace"))[-2000:]))
        lines = [l for l in done.stdout.decode("ascii").split("\n") if l]
        if len(lines) != len(part):
            raise SystemExit("sent %d requests to a reference, got %d answers" % (len(part), len(lines)))
        out.extend(json.loads(l) for l in lines)
    return out


def fix(a):
    if "dump" in a:
        a["dump"] = html_ref.normalize_dump(a["dump"])
    return a


def ask_html5lib(requests):
    html_ref.check_image()
    return [fix(a) for a in _pipe(oracle_env.command("html", [VENV_PYTHON, PIPELINE_PY]), requests)]


def ask_parse5(requests):
    html_ref.check_image()
    return [fix(a) for a in _pipe(oracle_env.command("html", ["node", PIPELINE_MJS]), requests)]


def ask_both(requests):
    """(html5lib answers, parse5 answers) for string requests, the two run side by side."""
    import threading
    res = {}
    # resolve the commands on this thread (oracle_env fills tables lazily)
    h_cmd = oracle_env.command("html", [VENV_PYTHON, PIPELINE_PY])
    p_cmd = oracle_env.command("html", ["node", PIPELINE_MJS])
    html_ref.check_image()

    def go(name, cmd):
        try:
            res[name] = [fix(a) for a in _pipe(cmd, requests)]
        except SystemExit as e:
            res[name] = e

    ts = [threading.Thread(target=go, args=(n, c)) for n, c in (("h", h_cmd), ("p", p_cmd))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    for n in ("h", "p"):
        if isinstance(res[n], SystemExit):
            raise res[n]
    return res["h"], res["p"]


def string_request(text, scripting, context):
    ctx = context
    return {"op": "string", "hex": text.encode("utf-8", "surrogatepass").hex(), "context": ctx, "scripting": bool(scripting)}


def foreign_context(c):
    return bool(c) and (c.startswith("svg ") or c.startswith("math "))


# ---- the population --------------------------------------------------------------------------------

# label (as the meta writes it), python codec, a snippet of the language that the codec encodes the same way the standard's encoder does
LANGS = [
    ("windows-1252", "cp1252", "caf\u00e9 \u00fcber \u00f1 \u20ac"),
    ("latin1", "cp1252", "na\u00efve \u00e0 \u00e8"),
    ("iso-8859-1", "cp1252", "\u00e9\u00e8\u00ea"),
    ("windows-1251", "cp1251", "\u041f\u0440\u0438\u0432\u0435\u0442"),
    ("koi8-r", "koi8-r", "\u041f\u0440\u0438\u0432\u0435\u0442"),
    ("iso-8859-2", "iso8859-2", "\u0105\u0119\u0142"),
    ("windows-1250", "cp1250", "\u0105\u0119\u0142"),
    ("iso-8859-7", "iso8859-7", "\u03b1\u03b2\u03b3"),
    ("windows-1253", "cp1253", "\u03b1\u03b2\u03b3"),
    ("iso-8859-15", "iso8859-15", "\u20ac\u0153"),
    ("shift_jis", "shift_jis", "\u65e5\u672c\u8a9e\u3042\u3044\u3046\u30c6\u30ad\u30b9\u30c8"),
    ("sjis", "shift_jis", "\u65e5\u672c\u8a9e"),
    ("euc-jp", "euc_jp", "\u65e5\u672c\u8a9e\u3042\u3044\u3046"),
    ("iso-2022-jp", "iso2022_jp", "\u65e5\u672c\u8a9e\u3042\u3044\u3046"),
    ("gbk", "gbk", "\u6c49\u5b57\u6587\u672c"),
    ("gb2312", "gbk", "\u6c49\u5b57"),
    ("big5", "big5", "\u6f22\u5b57\u6587\u672c"),
    ("euc-kr", "euc_kr", "\ud55c\uad6d\uc5b4"),
    ("utf-8", "utf-8", "\u65e5\u672c\u8a9e \u00e9 \U0001f600"),
]
UTF8_SNIPPETS = ["\u00e9\u00e8", "\u65e5\u672c\u8a9e", "\U0001f600", "a\u00a0b", "\u0301", "\ufffd", "\u202e", "\ufdd0", "\u0080\u009f"]
META_FORMS = [
    "<meta charset=%s>", "<meta charset=\"%s\">", "<meta charset='%s'>", "<META CHARSET=%s>", "<meta  charset = %s >",
    "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=%s\">", "<meta http-equiv=content-type content=text/html;charset=%s>",
    "<meta content=\"text/html; charset=%s\" http-equiv=\"content-type\">", "<meta/charset=%s>",
    # a repeated attribute: the first is the one that counts (html5lib's last, one of its departures)
    "<meta charset=%s charset=utf-8>", "<meta charset=nonsense charset=%s>", "<meta http-equiv=content-type content=\"text/html; charset=%s\" content=\"text/html; charset=utf-8\">",
]


def splice(rng, text, snippet, k):
    """Put the snippet after `k` of the `>` of the text (so, in text), or at the end if there are none."""
    parts = text.split(">")
    if len(parts) < 2:
        return text + snippet
    for _ in range(k):
        i = rng.randrange(len(parts) - 1)
        parts[i] = parts[i] + snippet
    return ">".join(parts)


def put_meta(rng, text, meta):
    if text.lstrip().lower().startswith("<!doctype") and rng.random() < 0.8:
        i = text.find(">")
        if i >= 0:
            return text[:i + 1] + meta + text[i + 1:]
    if text.lstrip().lower().startswith("<html") and rng.random() < 0.6:
        i = text.find(">")
        if i >= 0:
            return text[:i + 1] + "<head>" + meta + text[i + 1:]
    return meta + text


def encode(text, codec):
    return text.encode(codec, "replace")


def make_bytes(rng, it):
    """Fill in bytes and the encoding options of an item (a document), and name the scenario."""
    text = it["html"]
    if "\n" in text and rng.random() < 0.08:
        text = text.replace("\n", rng.choice(["\r\n", "\r"]))
    r = rng.random()
    it["certain"], it["transport"], it["default"], it["ignore"] = True, None, None, False
    if r < 0.30:
        it["scenario"] = "utf-8"
        if rng.random() < 0.5:
            text = splice(rng, text, rng.choice(UTF8_SNIPPETS), rng.randint(1, 3))
        it["bytes"] = text.encode("utf-8", "surrogatepass")
    elif r < 0.36:
        it["scenario"] = "utf-8 bom"
        it["bytes"] = b"\xef\xbb\xbf" + splice(rng, text, rng.choice(UTF8_SNIPPETS), 1).encode("utf-8")
    elif r < 0.40:
        it["scenario"] = "utf-8 transport"
        it["transport"] = rng.choice([b"utf-8", b"UTF-8", b" utf8 "])
        it["bytes"] = splice(rng, text, rng.choice(UTF8_SNIPPETS), 1).encode("utf-8")
    elif r < 0.47:
        it["scenario"] = "utf-16"
        le = rng.random() < 0.5
        body = splice(rng, text, "\u65e5\u672c\U0001f600", 1)
        if rng.random() < 0.85:
            it["bytes"] = (b"\xff\xfe" if le else b"\xfe\xff") + body.encode("utf-16-le" if le else "utf-16-be", "surrogatepass")
        else:
            it["scenario"] = "utf-16 transport"
            it["transport"] = b"utf-16le" if le else b"utf-16be"
            it["bytes"] = body.encode("utf-16-le" if le else "utf-16-be", "surrogatepass")
    elif r < 0.85:
        label, codec, snippet = rng.choice(LANGS)
        it["scenario"] = "meta " + label
        meta = rng.choice(META_FORMS) % (label if rng.random() < 0.8 else label.upper())
        t = splice(rng, text, snippet, rng.randint(1, 3))
        if rng.random() < 0.12:
            # an XML declaration and no `<meta>`: the standard's "get an XML encoding" when the prescan finds nothing (html5lib has no such step)
            it["scenario"] = "xml declaration " + label
            t = '<?xml version="1.0" encoding="%s"?>' % label + t
        else:
            t = put_meta(rng, t, meta)
        it["bytes"] = encode(t, codec)
        sub = rng.random()
        if sub < 0.1:  # a transport label that overrides the meta
            lab2, codec2, _ = rng.choice(LANGS)
            it["scenario"] += " + certain transport " + lab2
            it["transport"] = lab2.encode()
        elif sub < 0.2:
            it["scenario"] += " + tentative transport"
            it["transport"] = rng.choice(LANGS)[0].encode()
            it["certain"] = False
        elif sub < 0.25:
            it["scenario"] += " + ignore_meta"
            it["ignore"] = True
            it["default"] = rng.choice(LANGS)[0].encode()
        elif sub < 0.3:
            it["scenario"] += " + default"
            it["default"] = rng.choice(LANGS)[0].encode()
    elif r < 0.92:
        label, codec, snippet = rng.choice(LANGS)
        it["scenario"] = "default " + label
        it["default"] = label.encode()
        it["bytes"] = encode(splice(rng, text, snippet, rng.randint(1, 3)), codec)
    else:
        it["scenario"] = "invalid bytes"
        b = bytearray(splice(rng, text, rng.choice(UTF8_SNIPPETS), 2).encode("utf-8", "surrogatepass"))
        for _ in range(rng.randint(1, 4)):
            if b:
                b.insert(rng.randrange(len(b)), rng.choice([0x80, 0xBF, 0xC0, 0xC3, 0xE2, 0xF0, 0xF8, 0xFE, 0xFF, 0x81, 0x9F]))
        it["bytes"] = bytes(b)
        if rng.random() < 0.3:
            it["default"] = rng.choice(LANGS)[0].encode()
            it["scenario"] += " + default"
    return it


def population(count, seed, corpus):
    rng = random.Random(seed * 7919 + 13)
    base = tree_diff.population(None, count, seed, corpus)
    items = []
    for it in base:
        it = dict(it)
        if it.get("context"):
            it["kind"] = "frag"
            it["scenario"] = "fragment"
            it["bytes"] = it["html"].encode("utf-8", "surrogatepass")
            it["certain"], it["transport"], it["default"], it["ignore"] = True, None, None, False
        else:
            it["kind"] = "doc"
            make_bytes(rng, it)
        items.append(it)
    return items


# ---- reducing a divergence -------------------------------------------------------------------------

TOKEN_RE = tree_diff.TOKEN_RE


def reduce_items(runner, entries, ref_name, rounds=200):
    """Delta debugging over the tokens: for each entry {text, scripting, context} that this library and `ref_name` disagree on, the smallest
    list of tokens (1-minimal for deletion of a token, the text of what remains simplified where that keeps the disagreement) on which they
    still do. The reference is asked each step; a step that makes either side fail to answer does not count."""
    state = []
    for e in entries:
        toks = TOKEN_RE.findall(e["text"]) or [e["text"]]
        state.append({"toks": toks, "n": 2, "done": False, "e": e, "steps": 0})
    for _ in range(rounds):
        queries = []
        for si, s in enumerate(state):
            if s["done"]:
                continue
            toks = s["toks"]
            n = min(s["n"], len(toks))
            size = max(1, len(toks) // n)
            s["cands"] = []
            for i in range(0, len(toks), size):
                cand = toks[:i] + toks[i + size:]
                if cand and cand != toks:
                    s["cands"].append(("del", cand))
            if size == 1:
                for i, t in enumerate(toks):
                    for simple in simplifications(t):
                        s["cands"].append(("simp", toks[:i] + [simple] + toks[i + 1:]))
            for kind, cand in s["cands"]:
                queries.append((si, "".join(cand)))
        if not queries:
            break
        asked = ours_texts(runner, [(q[1], state[q[0]]["e"]["scripting"], state[q[0]]["e"]["context"]) for q in queries])
        reqs = [string_request(q[1], state[q[0]]["e"]["scripting"], state[q[0]]["e"]["context"]) for q in queries]
        theirs = ask_html5lib(reqs) if ref_name == "html5lib" else ask_parse5(reqs)
        # first success per entry
        won = {}
        for (si, text), (st, dump, _a), t in zip(queries, asked, theirs):
            if si in won:
                continue
            if st == "OK" and "dump" in t and t["dump"] != dump:
                won[si] = text
        k = 0
        by_entry = collections.defaultdict(list)
        for (si, text) in queries:
            by_entry[si].append(text)
        for si, s in enumerate(state):
            if s["done"]:
                continue
            if si in won:
                new = None
                for kind, cand in s["cands"]:
                    if "".join(cand) == won[si]:
                        new = cand
                        break
                s["toks"] = new
                s["n"] = max(s["n"] - 1, 2)
                s["steps"] += 1
            else:
                if min(s["n"], len(s["toks"])) >= len(s["toks"]):
                    s["done"] = True
                else:
                    s["n"] = min(s["n"] * 2, len(s["toks"]))
        if all(s["done"] for s in state):
            break
    return ["".join(s["toks"]) for s in state]


ATTR_SPLIT = re.compile(r"""\s+(?=[^\s=/>]+(?:\s*=|\s|/|>|$))""")


def simplifications(tok):
    """The simpler tokens to try in place of `tok`: with one attribute fewer (a start tag), with none, or the plain form."""
    out = []
    if tok.startswith("<") and tok.endswith(">") and not tok.startswith("<!") and not tok.startswith("<?") and not tok.startswith("</"):
        m = re.match(r"<([^\s/>]+)", tok)
        if m:
            name = m.group(1)
            attrs = [mm.group(0) for mm in re.finditer(r"""\s+[^\s=/>]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>"']*))?""", tok[m.end():-1])]
            if len(attrs) > 1:
                for i in range(len(attrs)):
                    out.append("<" + name + "".join(attrs[:i] + attrs[i + 1:]) + ">")
            elif len(attrs) == 1:
                out.append("<" + name + ">")
    simple = simplify(tok)
    if simple != tok:
        out.append(simple)
    return out


def simplify(tok):
    if tok.startswith("<!--"):
        return "<!--c-->" if tok != "<!--c-->" else tok
    if tok.startswith("<") and tok.endswith(">") and not tok.startswith("<!") and not tok.startswith("<?"):
        m = re.match(r"<(/?)([^\s/>]+)", tok)
        if m and tok != "<%s%s>" % (m.group(1), m.group(2)):
            return "<%s%s>" % (m.group(1), m.group(2))
        return tok
    if not tok.startswith("<") and tok != "x":
        return "x" if tok.strip() else tok
    return tok


# ---- classes -----------------------------------------------------------------------------------------

def load_list(path):
    entries = {}
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            if raw.startswith("#") or not raw.strip():
                continue
            cols = raw.rstrip("\n").split("\t")
            if cols[0].startswith("@"):
                continue
            if len(cols) < 4:
                raise SystemExit("%s: a line has %d columns, not 4: %r" % (path, len(cols), raw[:80]))
            entries[cols[0]] = (cols[1], cols[2], cols[3])
    return entries


def first_difference(a, b):
    i = 0
    while i < len(a) and i < len(b) and a[i] == b[i]:
        i += 1
    ea, eb = len(a), len(b)
    while ea > i and eb > i and a[ea - 1] == b[eb - 1]:
        ea -= 1
        eb -= 1
    return a[i:ea], b[i:eb], i


def shape(s):
    """A string with its letters and digits collapsed: the class of a difference, not the instance."""
    out = []
    for ch in s[:40]:
        c = "a" if ch.isalpha() and ord(ch) < 128 else "0" if ch.isdigit() else ch
        if out and out[-1] == c and c in "a0":
            continue
        out.append(c)
    return "".join(out) + ("..." if len(s) > 40 else "")


def cp_name(ch):
    return "U+%04X" % ord(ch)


def model_text(enc, data):
    """What the Encoding Standard says `data` decodes to (replacement mode): the multi-byte handlers written out again in Python
    (encoding_model.py), the single-byte encodings from the standard's index, the rest from Python's codecs, which agree with the
    standard for UTF-8 and UTF-16."""
    if enc == "utf-8":
        return data.decode("utf-8", "replace")
    if enc in ("utf-16le", "utf-16be"):
        return data.decode(enc.replace("utf-16", "utf-16-"), "replace")
    if enc == "replacement":
        return "\ufffd" if data else ""
    if enc == "x-user-defined":
        return "".join(chr(b) if b < 0x80 else chr(0xF780 + b - 0x80) for b in data)
    if enc in encoding_model.DECODERS:
        cps, _ = encoding_model.decode(enc, data)
        return "".join(chr(c) for c in cps)
    name = "iso-8859-8" if enc == "iso-8859-8-i" else enc
    index = encoding_model.indexes()[name]
    return "".join(chr(b) if b < 0x80 else (chr(index[b - 0x80]) if index[b - 0x80] is not None else "\ufffd") for b in data)


def decode_kind(ours, theirs):
    """How two readings of the same bytes differ at the first place they do."""
    a, b, _i = first_difference(ours, theirs)
    if not a or not b:
        return "different-replacement-count"
    ca, cb = a[0], b[0]
    if cb == "\ufffd" and ca != "\ufffd":
        return "standard-maps-python-errors" if not ("\u0080" <= ca <= "\u009f") else "standard-maps-C1-python-errors"
    if ca == "\ufffd" and cb != "\ufffd":
        return "standard-errors-python-maps"
    return "different-mapping"


def decode_class(enc, ours, theirs):
    a, b, i = first_difference(ours, theirs)
    pa = cp_name(a[0]) if a else "none"
    pb = cp_name(b[0]) if b else "none"
    if len(a) > 3 or len(b) > 3:
        return "%s: %s... vs %s..." % (enc, pa, pb)
    return "%s: %s vs %s" % (enc, " ".join(cp_name(c) for c in a) or "none", " ".join(cp_name(c) for c in b) or "none")


VOID_DIFF = {"wbr", "keygen", "basefont", "bgsound", "frame", "command", "event-source"}
RAW_NAMES = ("script", "style", "xmp", "iframe", "noembed", "noframes", "plaintext", "noscript")
ATTR_RE = re.compile(r'([^\s=/>]+)(?:\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|([^\s>]*)))?')
TAG_NAME_RE = re.compile(r"</?([A-Za-z][^\s/>]*)")


def tag_attrs(tok):
    m = TAG_NAME_RE.match(tok)
    if not m:
        return None, []
    rest = tok[m.end():-1]
    attrs = []
    for am in ATTR_RE.finditer(rest):
        v = am.group(2) if am.group(2) is not None else am.group(3) if am.group(3) is not None else am.group(4) or ""
        attrs.append((am.group(1), v))
    return m.group(1).lower(), attrs


SER_TOKEN = re.compile(r"""<(?:=[ \t\n\f\r]*"[^"]*"|=[ \t\n\f\r]*'[^']*'|[^<>])*>|[^<]+|<""")


ESC_RE = re.compile(r"&(lt|gt|amp|nbsp);")
ESC_MAP = {"lt": "<", "gt": ">", "amp": "&", "nbsp": "\u00a0"}


def esc_norm(x):
    """All the escaping off (`&amp;lt;` and `&lt;` and `<` are one text): what two spellings of a text have in common."""
    while True:
        y = ESC_RE.sub(lambda m: ESC_MAP[m.group(1)], x)
        if y == x:
            return y
        x = y


def enclosing_raw(tokens, i):
    """The raw text element (and whether an svg or math element is open around it) whose start tag in `tokens[:i]` has not been closed."""
    stack = []
    for t in tokens[:i]:
        if not t.startswith("<") or t.startswith("<!") or t.startswith("<?"):
            continue
        nm, _ = tag_attrs(t)
        if nm is None:
            continue
        if t.startswith("</"):
            for k in range(len(stack) - 1, -1, -1):
                if stack[k] == nm:
                    del stack[k:]
                    break
        elif nm not in ("br", "img", "input", "hr", "meta", "link", "area", "base", "col", "embed", "param", "source", "track", "wbr"):
            stack.append(nm)
    for k in range(len(stack) - 1, -1, -1):
        if stack[k] in RAW_NAMES:
            return stack[k], any(x in ("svg", "math") for x in stack[:k]), stack[k + 1:]
    return None, False, []


def tag_state(prefix):
    """Where the end of `prefix` is in the markup: text, tag (between `<` and `>`), dq or sq (inside a quoted attribute value), doctype, comment."""
    state = "text"
    last = ""
    i = 0
    n = len(prefix)
    while i < n:
        c = prefix[i]
        if state == "text":
            if prefix.startswith("<!--", i):
                state, i = "comment", i + 4
                continue
            if prefix.startswith("<!DOCTYPE", i):
                state, i = "doctype", i + 9
                continue
            if c == "<" and i + 1 < n and (prefix[i + 1].isalpha() or prefix[i + 1] in "/?"):
                state = "tag"
        elif state == "tag":
            if c in "\"'" and last == "=":
                state = "dq" if c == '"' else "sq"
            elif c == ">":
                state = "text"
            if c not in " \t\n\f\r":
                last = c
        elif state == "dq" and c == '"' or state == "sq" and c == "'":
            state = "tag"
        elif state == "doctype" and c == ">":
            state = "text"
        elif state == "comment" and prefix.startswith("-->", i):
            state, i = "text", i + 3
            continue
        i += 1
    return state


UNIT_NORM = {"&lt;": "<", "&gt;": ">", "&amp;": "&", "&nbsp;": "\u00a0", "&quot;": '"', "&#39;": "'"}


def unit_norm(u):
    """A unit of markup with its escaping taken off one level: what the same text is when it is not escaped."""
    if u.startswith("&amp;") and len(u) > 5:
        return "&" + u[5:]
    return UNIT_NORM.get(u, u)


TAG_PREFIX_RE = re.compile(r"(\s)(?:xlink|xml|xmlns):")


def unit_key(u):
    """The unit with every level of escaping off, for finding the places two spellings of one text correspond."""
    if u.startswith("<") and not u.startswith("</") and not u.startswith("<!") and u.endswith(">"):
        # a start tag: the escaping of its attribute values and the prefixes of the attribute names are what the references differ in
        return TAG_PREFIX_RE.sub(r"\1", esc_norm(u))
    for _ in range(4):
        v = unit_norm(u)
        if v == u:
            break
        u = v
    return u


UNIT_RE = re.compile(r"""<(?:=[ \t\n\f\r]*"[^"]*"|=[ \t\n\f\r]*'[^']*'|[^<>])*>|&amp;(?:#[0-9]+|#[xX][0-9a-fA-F]+|[A-Za-z][A-Za-z0-9]*);|"""
                     r"""&(?:#[0-9]+|#[xX][0-9a-fA-F]+|[A-Za-z][A-Za-z0-9]*);|[A-Za-z_:][-A-Za-z0-9_:.]*|[ \t\r\n\f]+|.""", re.S)
RAW_START_RE = re.compile(r"<(plaintext|noscript|script|style|xmp|iframe|noembed|noframes)\b", re.I)
VOID_END_RE = re.compile(r"</(?:%s)>$" % "|".join(sorted(VOID_DIFF)))


def piece_features(prefix, pa, pb, tokens_before):
    """The features of html_serialize_divergences.txt that turn what this library wrote (pa) into what the reference wrote (pb) after `prefix`:
    a list, empty if none accounts for the pair."""
    state = tag_state(prefix)
    in_tag = state in ("tag", "dq", "sq")
    # two start tags of one element: attribute by attribute
    if pa.startswith("<") and pb.startswith("<") and not pa.startswith("</") and not pa.startswith("<!") and not pa.startswith("<?"):
        na, aa = tag_attrs(pa)
        nb, ab = tag_attrs(pb)
        if na == nb and len(aa) == len(ab):
            out = []
            for (xn, xv), (yn, yv) in zip(aa, ab):
                if xn != yn:
                    if re.fullmatch(r"(?:xlink|xml|xmlns):[^\s=]*", xn) and yn == xn.split(":", 1)[1]:
                        out.append("attribute-namespace")
                    else:
                        return []
                if xv != yv:
                    if xv.replace("&nbsp;", "\u00a0") == yv and "&nbsp;" in xv:
                        out.append("nbsp-in-attribute")
                    elif esc_norm(xv.replace("&nbsp;", "\u00a0")) == esc_norm(yv):
                        out.append("angle-in-attribute")
                    else:
                        return []
            return sorted(set(out))
        return []
    if pa.replace("&nbsp;", "\u00a0") == pb and "&nbsp;" in pa:
        return ["nbsp-in-attribute" if in_tag else "nbsp-in-text"]
    if state == "doctype" or pa.startswith("<!DOCTYPE") or pb.startswith("<!DOCTYPE"):
        return ["doctype-identifiers"]
    if pa.startswith("<?") and not pb:
        return ["processing-instruction"]
    if pa == "" and pb == "undefined:":
        return ["attribute-namespace-without-prefix"]
    if in_tag and re.fullmatch(r"(?:xlink|xml|xmlns):[^\s=]*", pa) and (pb == pa.split(":", 1)[1] or pb == ""):
        return ["attribute-namespace"]
    if (not pa and VOID_END_RE.fullmatch(pb)) or (not pb and VOID_END_RE.fullmatch(pa)):
        return ["void-set"]
    if esc_norm(pa) == esc_norm(pb) and pa != pb:
        if in_tag:
            return ["angle-in-attribute"]
        raw, foreign, inner = enclosing_raw(tokens_before, len(tokens_before))
        if raw is None:
            # (a tag whose name holds a quote or an escape byte defeats the tokens: read the raw text elements off the text itself)
            text = "".join(tokens_before)
            m = list(RAW_START_RE.finditer(text))
            if m:
                name = m[-1].group(1).lower()
                if name == "plaintext" or not re.search(r"</%s[\s/>]" % name, text[m[-1].end():], re.I):
                    raw, foreign, inner = name, False, []
        if raw == "plaintext":
            return ["plaintext"]
        if raw == "noscript":
            return ["noscript"]
        if raw:
            return ["raw-name-in-foreign-namespace" if foreign else "raw-text-with-elements"]
    return []


def ser_classes(ours, theirs):
    """The features that make two serializations of one tree differ: for each place the token lists differ, and in each place for each piece
    of it, the feature the piece shows (the names of html_serialize_divergences.txt), or `other:` and the two pieces, which no listed feature
    accounts for."""
    import difflib
    a = SER_TOKEN.findall(ours)
    b = SER_TOKEN.findall(theirs)
    found = {}
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        sa, sb = "".join(a[i1:i2]), "".join(b[j1:j2])
        base = "".join(a[:i1])
        ua, ub = UNIT_RE.findall(sa), UNIT_RE.findall(sb)
        na_, nb_ = sa.replace("&nbsp;", "\u00a0"), sb.replace("&nbsp;", "\u00a0")
        if esc_norm(na_) == esc_norm(nb_) and tag_state(base) in ("text", "comment"):
            # the two differ only in escaping, and the block is text: the escapes of each kind found in it are the features
            feats = []
            if "&nbsp;" in sa and "\u00a0" in sb:
                for t in a[i1:i2]:
                    if "&nbsp;" in t:
                        feats.append("nbsp-in-attribute" if t.startswith("<") and not t.startswith("<!--") else "nbsp-in-text")
            if sa.replace("&nbsp;", "\u00a0") != sb:
                # the block can hold a tag the reference wrote and this library escaped (or the other way): what holds it is its raw text element
                feats.extend(piece_features(base, sa.replace("&nbsp;", "\u00a0"), sb, SER_TOKEN.findall(base)))
            if feats:
                for f in feats:
                    found.setdefault(f, {"ours": sa[:80], "theirs": sb[:80]})
                continue
        cm = difflib.SequenceMatcher(None, [unit_key(u) for u in ua], [unit_key(u) for u in ub], autojunk=False)
        best = (0, cm.get_opcodes())
        offs = [0]
        for u in ua:
            offs.append(offs[-1] + len(u))
        pieces = []
        for t, x1, x2, y1, y2 in best[1]:
            if t == "equal":
                pieces.extend((ua[x1 + k], ub[y1 + k], offs[x1 + k]) for k in range(x2 - x1) if ua[x1 + k] != ub[y1 + k])
            else:
                pieces.append(("".join(ua[x1:x2]), "".join(ub[y1:y2]), offs[x1]))
        for pa, pb, off in pieces:
            prefix = base + sa[:off]
            feats = piece_features(prefix, pa, pb, SER_TOKEN.findall(prefix))
            if not feats:
                feats = ["other:%s -> %s" % (shape(pa), shape(pb))]
            for f in feats:
                found.setdefault(f, {"ours": pa[:80], "theirs": pb[:80]})
    return list(found.items())


FORMATTING_ENDS = re.compile(r"</(?:a|b|big|code|em|font|i|nobr|s|small|strike|strong|tt|u)[\s/>]", re.I)


# ---- what each class is (the list's class and reason columns) ---------------------------------------

NEW_TREE_CAUSES = {
    "older-adoption-agency": (
        "reference-old",
        "html5lib 1.1 (2020) alone differs and parse5 agrees with this library, in a reproducer of formatting elements with an end tag that "
        "misnests them: its adoption agency algorithm and Noah's Ark clause are the older ones (the tree differential's `html5lib:alone:unverified`)."),
    "context:colgroup": (
        "reference-old",
        "html5lib 1.1 drops white space in a fragment whose context is colgroup. The served standard (13.2.6.4.12, 'in column group'): "
        "\"A character token that is one of U+0009 CHARACTER TABULATION, U+000A LINE FEED (LF), U+000C FORM FEED (FF), U+000D CARRIAGE RETURN (CR), "
        "or U+0020 SPACE: Insert the character.\" This library and parse5 insert it."),
}
NEW_TREE_CAUSES["context:form"] = (
    "reference-old",
    "html5lib 1.1 ignores the form element in a fragment's context: the served standard (13.4, the HTML fragment parsing algorithm) says \"Set the HTML "
    "parser's form element pointer to the nearest node to context that is a form element (going straight up the ancestor chain, and including the element "
    "itself, if it is a form element), if any\", and 'in body' for a start tag form: \"If the form element pointer is not null, and the parser is not "
    "parsing template contents, then this is a parse error; ignore the token.\" So a form start tag in a form context is ignored (this library and parse5).")
REPRODUCER_REASONS = {}
REPRODUCER_REASONS["<table><table>"] = (
    "reference-old",
    "html5lib 1.1 differs on a `<table>` start tag that 'in table' meets when no table element is in table scope or when one is: a second `<table>` start tag in a "
    "fragment (whatever its context element), or one in a table part of a fragment whose context is a table (the context table is not on the stack). The served "
    "standard (13.2.6.4.9, 'in table'): \"A start tag whose tag name is \"table\": Parse error. If the stack of open elements does not have a table element in table "
    "scope, ignore the token. Otherwise: Pop elements from this stack until a table element has been popped from the stack. Reset the insertion mode "
    "appropriately. Reprocess the token.\" (this library and parse5; html5lib 1.1 ignores the second one in the first case and closes the section and opens a "
    "body in the second).")
REPRODUCER_REASONS["<table><li><li>"] = (
    "reference-wrong",
    "html5lib 1.1 fails the standard's own vector for it (web-platform-tests tests8.dat#6, listed in html_deviations.txt): after foster-parenting the first `li` "
    "out of the table it puts the second inside the table. The served standard (13.2.6.4.9 'in table', 'Anything else': \"Parse error. Enable foster parenting, "
    "process the token using the rules for the \"in body\" insertion mode, and then disable foster parenting.\") foster-parents both (this library and parse5).")
ATTRIBUTE_COLLISION = (
    "html5lib 1.1 loses one of two attributes whose names differ only in a prefix (`href` and `xlink:href`, `lang` and `xml:lang`, `x` and `xmlns:x`, or one "
    "with a stray character in the prefix): in each reproducer the element has both, and the dump of html5lib's DOM tree has one. The served standard keeps every "
    "attribute the tokenizer produced (the attribute name state drops only a duplicate of the same name: \"be compared to the other attributes on the same token; if there is already an attribute on the token with the exact same name, then this is a duplicate-attribute parse error and the new attribute must be removed from the token. If an attribute is s\"), and adjusts foreign attributes "
    "only for the eleven names of its table, in foreign content; this library and parse5 keep both.")
REPRODUCER_REASONS["<table><svg><foreignobject>x<!--c-->"] = (
    "reference-wrong",
    "html5lib 1.1 inserts a comment before the text that is pending in a table. The served standard (13.2.6.4.10 'in table text'): \"Anything else: If any of the tokens in "
    "the pending table character tokens list are character tokens that are not ASCII whitespace, then this is a parse error: reprocess the character tokens in the "
    "pending table character tokens list using the rules given in the \"anything else\" entry in the \"in table\" insertion mode. Otherwise, insert the characters given "
    "by the pending table character tokens list. Switch the insertion mode to the original insertion mode and reprocess the token.\" - the text first, then the "
    "comment that ended the list (here with the text inside an HTML integration point of an SVG element that is itself in a table); this library and parse5 do.")
NEW_ATTRIBUTE_REASONS = {
    "xlink:href": "html5lib 1.1 makes `xlink:href` on an HTML element a namespaced attribute (or loses it, when the element also has `href`); the served standard "
                  "adjusts foreign attributes only for a token 'in foreign content' (an SVG or MathML start tag), so on an HTML element it is an attribute named "
                  "`xlink:href` (this library and parse5).",
    "link:href": "an attribute whose name has a colon that is not one of the eleven of the standard's table (here a name that ends in `link:href`, with a stray character before "
                 "`link`) is an attribute of that name on a MathML or SVG element; html5lib 1.1's DOM tree builder loses it. The served standard ('adjust foreign attributes') "
                 "matches the whole names `xlink:actuate`, `xlink:arcrole`, `xlink:href`, `xlink:role`, `xlink:show`, `xlink:title`, `xlink:type`, `xml:lang`, `xml:space`, "
                 "`xmlns` and `xmlns:xlink` and nothing else (this library and parse5 keep it).",
    "xml:base": "html5lib 1.1's table of foreign attributes has xml:base (as a namespaced attribute of the XML namespace); the served standard's table "
                "('adjust foreign attributes') has xlink:actuate, xlink:arcrole, xlink:href, xlink:role, xlink:show, xlink:title, xlink:type, xml:lang, "
                "xml:space, xmlns and xmlns:xlink and no xml:base, so it stays an attribute named `xml:base` (this library and parse5).",
    "xmlns:x": "html5lib 1.1 makes `xmlns:x` on an HTML element a namespaced attribute; the standard adjusts foreign attributes only for a token "
               "'in foreign content' (the SVG and MathML start tags), and only the eleven names of its table, so on an HTML element it is an attribute "
               "named `xmlns:x` (this library and parse5).",
    "filterres": "html5lib 1.1's table of SVG attributes whose case is restored has filterRes (and contentScriptType, contentStyleType, "
                 "externalResourcesRequired); the served standard's table ('adjust SVG attributes') does not, so `filterres` stays lower case "
                 "(this library and parse5).",
}


NEWLINE_AFTER_PRE = ("reference-old",
    "html5lib 1.1 keeps the line feed after a `pre`, `listing` or `textarea` start tag when the element is foster parented (in a table or a table-part context), "
    "where the served standard says (13.2.6.4.7 'in body') \"A start tag whose tag name is one of: \"pre\", \"listing\" ... Insert an HTML element for the token. If "
    "the next token is a U+000A LINE FEED (LF) character token, then ignore that token and move on to the next one.\" (and the same for \"textarea\"), foster "
    "parenting ('in table', 'Anything else') changing nothing of it. This library and parse5 drop the line feed.")
NEWLINE_NOT_NEXT = ("reference-old",
    "html5lib 1.1 drops a line feed that follows a `pre`, `listing` or `textarea` start tag after other tokens have come between (here an end tag that is ignored). "
    "The served standard (13.2.6.4.7 'in body'): \"If the next token is a U+000A LINE FEED (LF) character token, then ignore that token and move on to the next one.\" "
    "- the *next token*, which here is the end tag; the line feed that comes after it is text (this library and parse5 keep it).")
FOSTER_LIST_NAMES = {"li", "dd", "dt", "button", "option", "optgroup"}
FORMATTING_NAMES = {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"}


def effect_reason(sig):
    """(class, reason) for an effect signature, where its shape and a quoted rule account for it."""
    ours, _, theirs = sig.partition("/")
    ko, _, no = ours.partition(":")
    kt, _, nt = theirs.partition(":")
    if ko == "element" and no in FORMATTING_NAMES and kt == "text":
        return ("reference-old",
                "html5lib 1.1 does not reopen a formatting element (here `%s`) for the text that follows, though the standard's list of active formatting elements still holds it. "
                "The served standard, 'in body': \"Any other character token: Reconstruct the active formatting elements, if any. Insert the token's character.\"; and "
                "the adoption agency algorithm, when there is no furthest block, \"pop all the nodes from the bottom of the stack of open elements, from the current node up "
                "to and including formatting element, then remove formatting element from the list of active formatting elements\" and no other entry of the list. This library "
                "and parse5 reopen it; the reproducer in the class's examples is the witness." % no)
    if ko == "text" and kt == "element" and nt in FORMATTING_NAMES:
        return ("reference-old",
                "html5lib 1.1 reopens a formatting element (here `%s`) inside the element whose text follows, where the standard inserts the text as it is: for "
                "`textarea` the served standard (13.2.6.4.7 'in body') says \"Run these steps: ... Switch the tokenizer to the RCDATA state ... Switch the insertion mode to "
                "\"text\"\", and \"text\" handles \"A character token: Insert the token's character.\", with no reconstruction. This library and parse5 insert it as it is." % nt)
    if ko == "element" and no in FOSTER_LIST_NAMES and kt == "element" and nt == "table":
        rule = {"li": "\"A start tag whose tag name is \"li\": ... Loop: If node is an li element, then run these substeps: Generate implied end tags, except for li "
                      "elements. ... Pop elements from the stack of open elements until an li element has been popped\"",
                "dd": "\"A start tag whose tag name is one of: \"dd\", \"dt\": ... Loop: If node is a dd element ... Pop elements from the stack of open elements "
                      "until a dd element has been popped\" (and the same for dt)",
                "dt": "\"A start tag whose tag name is one of: \"dd\", \"dt\": ... Loop: If node is a dt element ... Pop elements from the stack of open elements "
                      "until a dt element has been popped\"",
                "button": "\"A start tag whose tag name is \"button\": 1. If the stack of open elements has a button element in scope, then run these substeps: "
                          "Parse error. Generate implied end tags. Pop elements from the stack of open elements until a button element has been popped\"",
                "option": "\"A start tag whose tag name is one of: \"optgroup\", \"option\": If the current node is an option element, then pop the current node off "
                          "the stack of open elements.\"",
                "optgroup": "\"A start tag whose tag name is one of: \"optgroup\", \"option\": If the current node is an option element, then pop the current node off "
                            "the stack of open elements.\""}[no]
        return ("reference-wrong",
                "html5lib 1.1 puts the second of two such elements (here `%s`) inside the table after foster-parenting the first out of it (compare the vector "
                "tests8.dat#6 in html_deviations.txt). The served standard: 'in table', 'Anything else': \"Parse error. Enable foster parenting, process the token using "
                "the rules for the \"in body\" insertion mode, and then disable foster parenting.\" and in body %s: the current node is the first element, "
                "which the rule closes, so both are foster parented (this library and parse5)." % (no, rule))
    return None


def tree_reason(rest, tree_list):
    """(class, reason) of a tree class key `<who>:<cause>` from the tree differential's list and the causes this stage found."""
    who, _, cause = rest.partition(":") if not rest.startswith("html5lib:alone:") else ("html5lib:alone", ":", rest[len("html5lib:alone:"):])
    if cause in NEW_TREE_CAUSES:
        return NEW_TREE_CAUSES[cause]
    if cause == "newline-after-pre":
        return NEWLINE_AFTER_PRE
    if cause == "newline-after-pre-not-next":
        return NEWLINE_NOT_NEXT
    if cause.startswith("reproducer:"):
        return REPRODUCER_REASONS.get(cause[len("reproducer:"):])
    if cause.startswith("fragment:"):
        return {"fragment:hidden-input-in-a-table-part": ("reference-old",
                    "html5lib 1.1 puts a hidden input a row follows in the section instead of the row. The served standard (13.2.6.4.9 'in table'): \"A start tag whose tag name "
                    "is \"input\": If the token does not have an attribute with the name \"type\", or if it does, but that attribute's value is not an ASCII case-insensitive "
                    "match for the string \"hidden\", then: act as described in the \"anything else\" entry below. Otherwise: Parse error. Insert an HTML element for the token. "
                    "Pop that element immediately off the stack of open elements.\" - inserted at the current node, here the row (this library and parse5)."),
                "fragment:<table><table>": REPRODUCER_REASONS["<table><table>"], "fragment:colgroup:text": NEW_TREE_CAUSES["context:colgroup"],
                "fragment:form:<form>": NEW_TREE_CAUSES["context:form"]}.get(cause)
    if cause.startswith("effect:"):
        return effect_reason(cause[len("effect:"):])
    if cause.startswith("attribute:"):
        ours = cause.split(":", 1)[1].split("/")[0]
        if any(n in cause.lower() for n in ("filterres", "contentstyletype", "contentscripttype", "externalresourcesrequired")):
            return ("reference-old", NEW_ATTRIBUTE_REASONS["filterres"])
        if "xml:base" not in cause:
            ab = cause.split(":", 1)[1].split("/")
            if len(ab) == 2 and (ab[0] == "missing" or ab[1] == "missing" or (ab[0] != ab[1] and ab[0].split(":")[-1] == ab[1].split(":")[-1])):
                return ("reference-wrong", ATTRIBUTE_COLLISION)
        if "\ufffd" not in cause:
            NEW_ATTRIBUTE_REASONS_ORDER = [k for k in NEW_ATTRIBUTE_REASONS if k != "link:href"]
        else:
            NEW_ATTRIBUTE_REASONS_ORDER = ["link:href"]
        for k in NEW_ATTRIBUTE_REASONS_ORDER:
            text = NEW_ATTRIBUTE_REASONS[k]
            if ours.startswith(k.replace(":", ":")) or cause.lower().find(k) >= 0:
                return ("reference-wrong" if k != "filterres" else "reference-old", text)
        return None
    parts = cause.split("+")
    classes, reasons = [], []
    for name in parts:
        name = {"rp": "rt"}.get(name, name)  # (the tree differential's list names rt, rb, rtc and ruby; rp is read with them)
        entry = tree_list.get("%s:%s" % (who, name))
        if entry is None:
            for other in ("both", "three-way", "parse5", "html5lib:alone"):
                entry = tree_list.get("%s:%s" % (other, name))
                if entry:
                    break
        if entry is None:
            return None
        classes.append(entry[1])
        reasons.append(entry[2])
    reason = reasons[0] if len(parts) == 1 else "The reproducer holds several constructs that a reference reads by older rules, each as in the tree differential: " + " | ".join(
        "(%s) %s" % (n, r) for n, r in zip(parts, reasons))
    return classes[0], reason


def describe(key):
    """(class, reason) for a class key, or None where nothing says what it is."""
    tree_list = load_list(TREE_LIST)
    ser_list = load_list(SER_LIST)
    if key.startswith("tree:"):
        _, via, rest = key.split(":", 2)
        found = tree_reason(rest, tree_list)
        if found and via == "reread":
            found = (found[1] and found[0], "On the serialization this library wrote, read again: " + found[1])
        return found
    if key.startswith("serialization:"):
        _, ref, feature = key.split(":", 2)
        entry = ser_list.get("%s:%s" % (ref, feature))
        return (entry[1], entry[2]) if entry else None
    if key.startswith("reading:html5lib-stream"):
        return ("reference-wrong", "html5lib reads through a codecs StreamReader, which is never told the input has ended, so an incomplete UTF-8 sequence "
                "at the end of the input is dropped; the Encoding Standard's decoder, at the end of the queue, 'If utf-8 bytes needed is not 0, set "
                "utf-8 bytes needed to 0 and return error' (one U+FFFD), which this library writes.")
    if key.startswith("reading:"):
        _, enc, kind = key.split(":", 2)
        why = {
            "standard-maps-C1-python-errors": "the Encoding Standard's index for %s maps the byte to the C1 control of the same value (indexes.json; 'index single-byte "
                "%s'), and Python's codec for it has no mapping, so html5lib (webencodings) replaces it with U+FFFD. The model, written from the standard, agrees with this library.",
            "standard-maps-python-errors": "the standard's %s decoder maps the sequence (indexes.json) and Python's codec reports an error; the model agrees with this library.",
            "standard-errors-python-maps": "the standard's %s decoder returns an error (for a pointer the index does not map) where Python's codec maps it; the model agrees with this library.",
            "different-mapping": "the standard's %s index maps the pointer to a different code point than Python's codec; the model agrees with this library.",
            "different-replacement-count": "the standard's %s decoder restores an ASCII byte after a bad trail byte ('If byte is an ASCII byte, restore byte to ioQueue') and "
                "replaces the lead alone, where Python's codec replaces more or fewer bytes; the model agrees with this library.",
        }.get(kind)
        if why is None:
            return None
        return ("reference-old", (why % ((enc, enc) if why.count("%s") == 2 else (enc,))))
    if key.startswith("decision:"):
        names = key.split(":", 1)[1].split("+")
        sniff = sniff_diff.read_list(sniff_diff.LIST)
        if all(n in sniff for n in names):
            return ("reference-old", " | ".join("(%s) %s" % (n, sniff[n]["rule"]) for n in names))
        return None
    if key.startswith("stability:") or key.startswith("html5lib:"):
        return None
    return None



def main(argv):
    args = argv[1:]
    runner = None
    count = int(os.environ.get("HTML_PIPELINE_ORACLE_COUNT") or DEFAULT_COUNT)
    seed = int(os.environ.get("HTML_PIPELINE_ORACLE_SEED") or DEFAULT_SEED)
    chunk = int(os.environ.get("HTML_PIPELINE_ORACLE_CHUNK") or 4000)
    plant = None
    write_list = False
    show = 0
    corpus = True
    save_classes = against = None
    show_filter = ""
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--chunk":
            chunk = int(args.pop(0))
        elif a == "--plant":
            plant = args.pop(0)
            if plant not in PLANTS:
                raise SystemExit("--plant: one of %s" % ", ".join(PLANTS))
        elif a == "--write-list":
            write_list = True
        elif a == "--show":
            show = int(args.pop(0))
        elif a == "--filter":
            show_filter = args.pop(0)
        elif a == "--no-corpus":
            corpus = False
        elif a == "--save-classes":
            save_classes = args.pop(0)
        elif a == "--against":
            against = args.pop(0)
        elif runner is None:
            runner = a
        else:
            raise SystemExit("unexpected argument %r" % a)
    if runner is None:
        raise SystemExit(__doc__)
    # A control that runs a smaller population than the list describes is held to a saved summary of the unplanted library on that same
    # population (--save-classes writes it, --against reads it): the planted library must give exactly the same classes and counts.
    hunting = (count, seed, corpus) != (DEFAULT_COUNT, DEFAULT_SEED, True)
    started = time.time()
    items = population(count, seed, corpus)
    print("population: %d items (count %d, seed %d)" % (len(items), count, seed), flush=True)

    failures = []
    # The two drivers of this gate dump trees themselves (their code is copied from the pinned drivers'): on a sample of the population they must
    # give the pinned drivers' dump, or the gate would compare this library with a reference through a dump writer nobody checked.
    sample = [it for it in items if it.get("html") and not any(ord(c) > 0xFFFF for c in it["html"]) and not foreign_context(it.get("context"))][:300:2]
    pinned = html_ref.ask_all([html_ref.request(it["html"], it["scripting"], it.get("context")) for it in sample])
    mine_h, mine_p = ask_both([string_request(it["html"], it["scripting"], it.get("context")) for it in sample])
    differ = 0
    for i, it in enumerate(sample):
        for name, mine in (("html5lib", mine_h[i]), ("parse5", mine_p[i])):
            theirs = html_ref.normalize_dump(pinned[name][i].get("dump")) if "dump" in pinned[name][i] else None
            if "dump" in mine and theirs is not None and mine["dump"] != theirs:
                differ += 1
            if plant == "driver-wrong" and i == 0:
                differ += 1
    print("drivers: %d documents asked of the pinned drivers and of this gate's, each of both references; differing: %d" % (len(sample), differ))
    if differ:
        failures.append(("drivers", sample[0], "the gate's drivers dump a tree differently from the pinned drivers on %d answers" % differ))
    tables = sniff_diff.LabelTables(runner)
    classes = collections.defaultdict(list)  # key -> [(item id, summary)]
    stats = collections.Counter()
    refs_failed = collections.Counter()
    ref_failed_examples = []
    witnesses = {}

    def record(key, it, detail):
        classes[key].append((it["id"], detail))
        witnesses.setdefault(key, (it, detail))

    pending_tree = []  # divergences to reduce: dict(text, scripting, context, ref, who, ours, theirs, item, via)

    for lo in range(0, len(items), chunk):
        part = items[lo:lo + chunk]
        answers = ours_docs(runner, part)
        # the second wave needs this library's text
        good = []
        for it, a in zip(part, answers):
            if "error" in a:
                stats["stopped: " + a["error"][:40]] += 1
                continue
            it["o"] = a
            good.append(it)
        if plant == "ours-wrong-encoding" and lo == 0:
            for it in good:
                if it["kind"] == "doc":
                    it["o"]["enc"] = "utf-7"
                    break
        if plant == "ours-extra-line" and lo == 0 and good:
            good[0]["o"]["dump"] += "\n| <planted>"
        # integration: this library's tree for the text parsed as UTF-8
        again = ours_texts(runner, [(it["o"]["text"], it["scripting"], it.get("context")) for it in good])
        for n, (it, (st, dump, a2)) in enumerate(zip(good, again)):
            if plant == "ours-bytes-differ" and lo == 0 and n == 0:
                dump += "\n| <planted>"
            it["o_text"] = (st, dump)
            if it["kind"] == "frag":
                continue
            # (a document in UTF-8 whose text starts with U+FEFF would be a different question: the text has no BOM removed twice)
            if st != "OK" or dump != it["o"]["dump"]:
                failures.append(("integration", it, "this library's tree for the bytes differs from its tree for their text: %r vs %r" % (
                    it["o"]["dump"][:200], dump[:200])))
        # the references
        reqs_h, reqs_p, idx = [], [], []
        byte_reqs, byte_idx = [], []
        for n, it in enumerate(good):
            ctx = it.get("context")
            if it["kind"] == "frag" and foreign_context(ctx):
                reqs_p.append(string_request(it["o"]["text"], it["scripting"], ctx))
                reqs_h.append(string_request(it["o"]["text"], it["scripting"], ctx))
            else:
                reqs_p.append(string_request(it["o"]["text"], it["scripting"], ctx))
                reqs_h.append(string_request(it["o"]["text"], it["scripting"], ctx))
            idx.append(n)
            if it["kind"] == "doc":
                byte_idx.append(n)
                byte_reqs.append({"op": "bytes", "hex": it["bytes"].hex(), "transport": it["transport"].decode("latin-1") if it["transport"] is not None and it["certain"] else None,
                                  "likely": it["transport"].decode("latin-1") if it["transport"] is not None and not it["certain"] else None,
                                  "default": it["default"].decode("latin-1") if it["default"] is not None else "utf-8",
                                  "ignore_meta": it["ignore"], "scripting": it["scripting"]})
        hs, ps = ask_both(reqs_h)
        hb = ask_html5lib(byte_reqs) if byte_reqs else []
        for n, h, p in zip(idx, hs, ps):
            good[n]["h"], good[n]["p"] = h, p
        for n, h in zip(byte_idx, hb):
            good[n]["hb"] = h
        if plant == "theirs-wrong" and lo == 0:
            for it in good:
                if "dump" in it["p"] and it["kind"] == "doc":
                    it["p"]["dump"] += "\n| <planted>"
                    break
        # second wave: the serialization read again by the references, and the references' own serializations read again
        reqs2_h, reqs2_p, own = [], [], []
        for n, it in enumerate(good):
            ctx = it.get("context")
            o = it["o"]
            if plant == "ours-unstable" and lo == 0 and n == 0:
                o["ser"] = o["ser"] + "<planted>"
                o["dump2"] = None
            reqs2_h.append(string_request(o["ser"], it["scripting"], ctx))
            reqs2_p.append(string_request(o["ser"], it["scripting"], ctx))
            own.append(n)
        h2, p2 = ask_both(reqs2_h)
        for n, h, p in zip(own, h2, p2):
            good[n]["h_read"], good[n]["p_read"] = h, p
        own_h_reqs, own_h_idx, own_p_reqs, own_p_idx = [], [], [], []
        for n, it in enumerate(good):
            ctx = it.get("context")
            if "ser" in it["h"] and "ser" in it["p"]:
                pass
            if "ser" in it["h"] and not foreign_context(ctx):
                own_h_reqs.append(string_request(it["h"]["ser"], it["scripting"], ctx))
                own_h_idx.append(n)
            if "ser" in it["p"]:
                own_p_reqs.append(string_request(it["p"]["ser"], it["scripting"], ctx))
                own_p_idx.append(n)
        h3 = ask_html5lib(own_h_reqs) if own_h_reqs else []
        p3 = ask_parse5(own_p_reqs) if own_p_reqs else []
        for n, a in zip(own_h_idx, h3):
            good[n]["h_own"] = a
        for n, a in zip(own_p_idx, p3):
            good[n]["p_own"] = a
        # ---- compare ----
        for it in good:
            judge(it, tables, record, stats, pending_tree, refs_failed, ref_failed_examples, plant)
        print("  ... %d of %d items (%.0f s)" % (min(lo + chunk, len(items)), len(items), time.time() - started), flush=True)
        for it in good:  # free what the next chunk does not need
            for k in ("h", "p", "hb", "h_read", "p_read", "h_own", "p_own"):
                it.pop(k, None)

    # ---- reduce and classify the tree divergences ----
    print("tree divergences to reduce: %d" % len(pending_tree), flush=True)
    reduced = {}
    for ref_name in ("html5lib", "parse5"):
        group = [d for d in pending_tree if d["ref"] == ref_name]
        if group:
            mins = reduce_items(runner, group, ref_name)
            for d, m in zip(group, mins):
                reduced[id(d)] = m
    min_ours = {}
    keys_for = []
    asked = ours_texts(runner, [(reduced[id(d)], d["scripting"], d["context"]) for d in pending_tree])
    reqs = [string_request(reduced[id(d)], d["scripting"], d["context"]) for d in pending_tree]
    h_min = ask_html5lib([r for r, d in zip(reqs, pending_tree) if d["ref"] == "html5lib"])
    p_min = ask_parse5([r for r, d in zip(reqs, pending_tree) if d["ref"] == "parse5"])
    hi = pi = 0
    for d, (st, dump, _a) in zip(pending_tree, asked):
        d["min"] = reduced[id(d)]
        d["min_ours"] = dump
        if d["ref"] == "html5lib":
            d["min_theirs"] = h_min[hi].get("dump", "")
            hi += 1
        else:
            d["min_theirs"] = p_min[pi].get("dump", "")
            pi += 1
        cause = tree_cause(d)
        record("tree:%s:%s:%s" % (d["via"], d["who"], cause), d["item"], {"min": d["min"], "context": d["context"]})
    tables.close()
    return finish(classes, witnesses, stats, failures, refs_failed, ref_failed_examples, hunting, plant, write_list, show, items, started, count, seed,
        runner, save_classes, against, show_filter)


def normalize_enc(name):
    return (name or "").lower()


def tree_compare(it, text, ours_tree, h, p, via, pending_tree, stats):
    """Compare this library's tree with each reference's tree of `text`; a difference is queued for reduction, with who differs: both
    references (and agree), html5lib alone, parse5 alone, or three-way (both differ and disagree). `h` is None where html5lib cannot be
    asked (a foreign context) or stopped on an assertion."""
    ctx = it.get("context")
    d_h = h is not None and h["dump"] != ours_tree
    d_p = p["dump"] != ours_tree
    if h is not None and not d_h:
        stats["%s: tree equals html5lib" % via] += 1
    if not d_p:
        stats["%s: tree equals parse5" % via] += 1
    if not (d_h or d_p):
        return
    if h is not None:
        if d_h and d_p and h["dump"] == p["dump"]:
            who, ref = "both", "parse5"
        elif d_h and not d_p:
            who, ref = "html5lib:alone", "html5lib"
        elif d_h and d_p:
            who, ref = "three-way", "parse5"
        else:
            who, ref = "parse5", "parse5"
    else:
        who, ref = "parse5", "parse5"
    pending_tree.append({"text": text, "scripting": it["scripting"], "context": ctx, "ref": ref, "who": who, "ours": ours_tree,
                         "theirs": (p if ref == "parse5" else h)["dump"], "item": it, "via": via})


def judge(it, tables, record, stats, pending_tree, refs_failed, ref_examples, plant):
    o, h, p = it["o"], it["h"], it["p"]
    ctx = it.get("context")
    doc = it["kind"] == "doc"
    stats["items " + it["kind"]] += 1
    stats["scenario " + it["scenario"].split(" ")[0]] += 1
    h_ok = "dump" in h
    p_ok = "dump" in p
    foreign = foreign_context(ctx)
    if foreign:
        if h_ok:
            refs_failed["html5lib answered a foreign context"] += 1
        h_ok = False
    elif not h_ok and h.get("error", "").startswith("AssertionError"):
        stats["html5lib stopped on an assertion"] += 1
    elif not h_ok:
        refs_failed["html5lib: " + h.get("error", "?")[:60]] += 1
        ref_examples.append((it, "html5lib", h.get("error")))
    if not p_ok:
        refs_failed["parse5: " + p.get("error", "?")[:60]] += 1
        ref_examples.append((it, "parse5", p.get("error")))
        return
    hb_trusted = True
    # -- the decision, and the reading
    if doc and "hb" in it:
        hb = it["hb"]
        if "enc" not in hb:
            if hb.get("error", "").startswith("AssertionError"):
                stats["html5lib bytes: stopped on an assertion"] += 1
            else:
                refs_failed["html5lib bytes: " + hb.get("error", "?")[:60]] += 1
        else:
            same = normalize_enc(o["enc"]) == normalize_enc(hb["enc"])
            if not same:
                item = (it["bytes"], it["transport"], it["certain"], it["default"] if it["default"] is not None else b"utf-8", it["ignore"])
                chosen, _ties = sniff_diff.explain((item[0], item[1], item[2], it["default"], item[4]), hb["enc"], tables)
                if chosen is None:
                    record("decision:unexplained", it, {"ours": o["enc"], "theirs": hb["enc"], "scenario": it["scenario"]})
                else:
                    record("decision:" + "+".join(chosen), it, {"ours": o["enc"], "theirs": hb["enc"]})
            else:
                stats["decision agrees"] += 1
                ours_text = o["text"]
                theirs_text = bytes.fromhex(hb["text"]).decode("utf-8", "surrogatepass")
                stream_text = bytes.fromhex(hb["stream_text"]).decode("utf-8", "surrogatepass") if hb.get("stream_text") is not None else theirs_text
                model = model_text(normalize_enc(o["enc"]), it["bytes"][o["bom"]:])
                if ours_text != model:
                    record("reading:differs-from-the-standard's-model", it, {"enc": o["enc"], "ours": ours_text[:80], "model": model[:80]})
                if ours_text != theirs_text:
                    record("reading:%s:%s" % (normalize_enc(o["enc"]), decode_kind(ours_text, theirs_text)), it, {"scenario": it["scenario"]})
                if stream_text != theirs_text:
                    # html5lib reads through a codecs StreamReader, which never tells the decoder the input has ended: an incomplete
                    # sequence at the end of the input is dropped, not replaced
                    record("reading:html5lib-stream-drops-the-incomplete-sequence-at-the-end", it, {"enc": o["enc"]})
                    hb_trusted = False
                if hb_trusted and h_ok and hb["dump"] != h["dump"] and ours_text == theirs_text:
                    record("html5lib:bytes-differ-from-text:unexplained", it, {"scenario": it["scenario"]})
    # -- the trees of the text
    if not h_ok and not foreign and not h.get("error", "").startswith("AssertionError"):
        return
    ours_tree = o["dump"]
    tree_compare(it, o["text"], ours_tree, h if h_ok else None, p, "text", pending_tree, stats)
    # -- the serialization, where the trees agree
    refs = []
    if h_ok:
        refs.append(("html5lib", h))
    refs.append(("parse5", p))
    for name, r in refs:
        if r["dump"] == ours_tree and "ser" in r:
            if o["ser"] == r["ser"]:
                stats["serialization equals " + name] += 1
            else:
                for feature, detail in ser_classes(o["ser"], r["ser"]):
                    record("serialization:%s:%s" % (name, feature), it, dict(detail, text=o["text"][:300], ser=o["ser"][:300]))
    # -- the serialization read again
    t2 = o["dump2"]
    hr = it["h_read"] if "dump" in it.get("h_read", {}) and h_ok else None
    pr = it["p_read"]
    if "dump" in pr:
        if t2 is None:
            record("reread:ours-failed:unexplained", it, {})
        else:
            tree_compare(it, o["ser"], t2, hr, pr, "reread", pending_tree, stats)
    # -- stability
    if t2 is not None:
        ours_stable = t2 == ours_tree
        stats["ours stable" if ours_stable else "ours unstable"] += 1
        if "dump" in it.get("p_own", {}):
            p_stable = it["p_own"]["dump"] == p["dump"]
            if p_stable != ours_stable and p["dump"] == ours_tree and p.get("ser") == o["ser"]:
                record("stability:ours-%s,parse5-%s" % ("stable" if ours_stable else "unstable", "stable" if p_stable else "unstable"), it, {})
            elif p["dump"] == ours_tree and p.get("ser") == o["ser"]:
                stats["stability verdict agrees with parse5"] += 1


ATTR_LINE = re.compile(r'^\| +[^ "<=][^=]*=".*"$')


def line_kind(lines, i):
    """tree_diff.line_info(lines, i) with the lines of a text node that holds a line feed read as text (they are continuation lines of the
    dump, and the first of them is not an attribute because its text happens to look like one)."""
    kind, path, own = tree_diff.line_info(lines, i)
    if kind == "attribute" and i < len(lines) and not ATTR_LINE.match(lines[i]):
        return "text", path, ""
    return kind, path, own


def effect_signature(d):
    """What the first differing line of the two minimal trees is, on each side: `element:u/text` is a line that is the element u here and text
    there. Nothing but the shape of the difference, so a class named by it is held to its reproducer and its reason."""
    i, la, lb = tree_diff.first_diff(d["min_ours"], d["min_theirs"])
    if i is None:
        return None
    ka, _pa, na = line_kind(la, i)
    kb, _pb, nb = line_kind(lb, i)
    return "%s/%s" % (ka + (":" + na if na else ""), kb + (":" + nb if nb else ""))


def tree_cause(d):
    ref_who = d["who"]
    who = ref_who
    names = tree_diff.candidates("parse5" if d["ref"] == "parse5" else "html5lib", d["min"], d["context"], d["scripting"], d["min_ours"])
    if d["context"] in tree_diff.KNOWN_OLD["parse5" if d["ref"] == "parse5" else "html5lib"]:
        names = sorted(set(names) | {d["context"]})
    if not names:
        # the first line the two trees differ on, if it is an attribute of the same element: the attribute is the cause
        i, la, lb = tree_diff.first_diff(d["min_ours"], d["min_theirs"])
        if i is not None:
            ka, pa, na = line_kind(la, i)
            kb, pb, nb = line_kind(lb, i)
            if ka == "attribute" and kb == "attribute" and na and nb:
                return "attribute:%s/%s" % (na.replace(" ", ":"), nb.replace(" ", ":"))
            if ka == "attribute" and kb != "attribute" and na and la[i].strip():
                return "attribute:%s/missing" % na.replace(" ", ":")
            if kb == "attribute" and ka != "attribute" and nb and lb[i].strip():
                return "attribute:missing/%s" % nb.replace(" ", ":")
        sig = effect_signature(d) if d["who"] == "html5lib:alone" else None
        if d["min"] in REPRODUCER_REASONS:
            return "reproducer:" + d["min"]
        if d["who"] == "html5lib:alone" and re.search(r"<(?:pre|listing|textarea)>[\n\r]", d["min"]):
            return "newline-after-pre"
        if d["who"] == "html5lib:alone" and re.search(r"<(?:pre|listing|textarea)>(?:<[^>]*>)+[\n\r]", d["min"]):
            return "newline-after-pre-not-next"
        if d["who"] == "html5lib:alone" and re.search(r"<col(?:group)?[\s/>]", d["min"]) and not re.search(r"(?:^|>)[^<]*[^<\s][^<]*(?:<|$)", d["min"]):
            return "fragment:colgroup:text"
        if sig and effect_reason(sig):
            return "effect:" + sig
        if sig and (FORMATTING_ENDS.search(d["min"]) or re.search(r"<nobr\b.*<nobr\b", d["min"], re.S)) and sig.startswith("element:") \
                and sig.split("/")[1].startswith("element:"):
            return "older-adoption-agency"
        if d["context"] and not foreign_context(d["context"]):
            if d["min"] == "<table><table>" or (d["context"] in ("table", "tbody", "thead", "tfoot", "tr") and re.search(r"<table>", d["min"])):
                return "fragment:<table><table>"
            if d["context"] == "colgroup" and "<" not in d["min"]:
                return "fragment:colgroup:text"
            if d["context"] == "form" and d["min"] == "<form>":
                return "fragment:form:<form>"
            if "type=hidden" in d["min"] and re.search(r"<input\b", d["min"]) and d["context"] in ("table", "tbody", "thead", "tfoot", "tr"):
                return "fragment:hidden-input-in-a-table-part"
            return "unexplained"
        if sig:
            return "effect:" + sig
        return "unexplained"
    return "+".join(sorted(names))


def finish(classes, witnesses, stats, failures, refs_failed, ref_examples, hunting, plant, write_list, show, items, started, count, seed, runner,
        save_classes=None, against=None, show_filter=""):
    listed = {} if plant == "no-list" else load_list(LIST)
    known = set(listed)
    if against:
        with open(against) as fh:
            saved = json.load(fh)
        listed = {k: (str(v), "saved", "the unplanted library on this population") for k, v in saved.items()}
        hunting = False
        if plant == "no-list":
            listed = {}
    if save_classes:
        hunting = True  # the saved summary is for a population the list does not describe: counts are not compared with it
    if plant == "stale-entry":
        listed["tree:parse5:planted-gone"] = ("1", "reference-old", "planted")
    if plant == "change-count":
        for k in sorted(listed):
            if listed[k][0] != "hunt":
                listed[k] = (str(int(listed[k][0]) + 1),) + listed[k][1:]
                break
    print("\nitems: %d; %s" % (len(items), "; ".join("%s %d" % (k, v) for k, v in sorted(stats.items()))))
    print("classes: %d, documents in them: %d" % (len(classes), sum(len(v) for v in classes.values())))
    if write_list:
        for k in sorted(classes):
            d = describe(k)
            print("%s\t%d\t%s\t%s" % (k, len(classes[k]), d[0] if d else "UNREAD", d[1] if d else "UNREAD"))
    if show:
        for k in sorted(classes):
            if show_filter in k:
                for iid, detail in classes[k][:show]:
                    print("== %s %s %r" % (k, iid, detail))
    failed = False
    for kind, it, why in failures:
        failed = True
        sys.stderr.write("\n### FAIL (%s): %s\n  %r\n" % (kind, why, it["html"][:200]))
    if refs_failed:
        failed = True
        sys.stderr.write("\n### A reference could not answer ###\n")
        for k, v in refs_failed.most_common(10):
            sys.stderr.write("  %s x%d\n" % (k, v))
        for it, who, err in ref_examples[:5]:
            sys.stderr.write("  e.g. %s %r: %s\n" % (who, it["html"][:100], err))
    unexplained = [k for k in classes if k.endswith(":unexplained") or k == "decision:unexplained" or k.startswith("reading:differs")
                   or k.startswith("serialization:") and ":other:" in k or k == "reread:ours-failed"]
    for k in sorted(unexplained):
        failed = True
        sys.stderr.write("\n### Divergences with no known cause: %s x%d ###\n" % (k, len(classes[k])))
        for iid, detail in classes[k][:5]:
            sys.stderr.write("  %s %r\n" % (iid, detail))
    unlisted = sorted(k for k in classes if k not in listed and k not in unexplained)
    if hunting and not against:
        # A hunt reaches combinations of constructs the default population does not (a reproducer with two elements that a reference reads by older rules, a
        # context): a class that describe() can name from the minimal reproducer is accepted and counted, one it cannot name is a failure.
        described = [k for k in unlisted if describe(k) is not None]
        print("classes described but not in the list (new combinations): %d" % len(described))
        unlisted = [k for k in unlisted if describe(k) is None]
    if save_classes:
        unlisted = sorted(k for k in classes if k not in known and describe(k) is None and k not in unexplained)
    if unlisted:
        failed = True
        sys.stderr.write("\n### Divergence classes with no entry (tools/oracle/html_pipeline_divergences.txt) ###\n")
        for k in unlisted[:60]:
            sys.stderr.write("  %s  x%d\n" % (k, len(classes[k])))
            seen = set()
            for iid, detail in classes[k]:
                shown = repr(detail.get("min", detail) if isinstance(detail, dict) else detail)
                if shown not in seen and len(seen) < 3:
                    seen.add(shown)
                    sys.stderr.write("    e.g. %s %s\n" % (iid, shown[:300]))
    if not hunting:
        gone = sorted(k for k in listed if k not in classes and listed[k][0] != "hunt")
        if gone:
            failed = True
            sys.stderr.write("\n### Listed classes that are gone ###\n")
            for k in gone:
                sys.stderr.write("  %s\n" % k)
        changed = sorted(k for k in listed if k in classes and listed[k][0] != "hunt" and int(listed[k][0]) != len(classes[k]))
        if changed:
            failed = True
            sys.stderr.write("\n### Listed classes whose count has changed ###\n")
            for k in changed:
                sys.stderr.write("  %s  listed %s, now %d\n" % (k, listed[k][0], len(classes[k])))
        if len(items) < MIN_DOCUMENTS and not against:
            failed = True
            sys.stderr.write("\n### The population has shrunk: %d items, floor %d ###\n" % (len(items), MIN_DOCUMENTS))
    print("elapsed %.0f s" % (time.time() - started))
    if save_classes and not failed:
        with open(save_classes, "w") as fh:
            json.dump({k: len(v) for k, v in sorted(classes.items())}, fh, indent=1)
    if failed:
        return EXIT_FAIL
    print("html pipeline differential: every class is listed, none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
