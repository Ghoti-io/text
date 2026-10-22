#!/usr/bin/env python3
"""The population of the CSS differential: stylesheets nobody chose.

    css_gen.py [--count N] [--seed S] [--show K]

Deterministic: the same count and seed give the same items on every run and every machine
(`random.Random`, no clock, no hash order). Four sources, each item tagged with where it came from:

  corpus     every input of the pinned css-parsing-tests files in scope of the vector gate, and of the
             vectors tools/css/extract-wpt.py makes from web-platform-tests (the inputs the standard's
             authors thought of);
  soup       a random run of CSS fragments (identifiers with escapes, every shape of number, strings
             that close and strings that do not, urls good and bad, comments, at-keywords, the
             delimiters, blocks opened and closed in the wrong order, white space of every kind,
             NUL and ill-formed UTF-8): the tokenizer's whole alphabet in no order;
  nested     a random stylesheet of the shapes the parser's algorithms are about (rules, at-rules,
             declarations, `!important`, custom properties, nesting at several depths, `{}` values),
             with noise injected at random places;
  mutated    a corpus or nested input with one to four edits (delete, duplicate, swap, insert a
             fragment, flip a byte), the way a fuzzer would find the next interesting input;
  bytes      a nested input encoded in one of the encodings the Encoding module reads, with a byte
             order mark or an `@charset` or a protocol label or an environment encoding, some of them
             wrong: the inputs of CSS Syntax 3.2 (only tinycss2 has an entry point for bytes).

Items are dicts: id, tags, data (bytes), and for bytes items `proto` and `env` (labels or None).

Copyright 2026 by Corey Pennycuff
"""

import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SUITE = os.path.join(ROOT, "third_party", "css", "css-parsing-tests")
WPT_VECTORS = os.path.join(ROOT, "build", "css-syntax-wpt-vectors.json")

# Fragments, grouped; the soup draws from every group with the weights below.
IDENTS = ["a", "b", "color", "-x", "--x", "--", "-", "_a", "a-b", "a9", "\\41 ", "\\41", "a\\", "\\ ", "\u00e9", "\u65e5\u672c",
          "\u00b7", "x\u00b7", "\\0 ", "\\110000 ", "\\D800 ", "\\6e ", "\\\n", "-\\2d ", "i\u0307", "\U0001f600", "important", "IMPORTANT",
          "url", "calc", "u", "U", "Important", "media", "charset", "unicode-range", "not", "and"]
NUMBERS = ["0", "1", "-1", "+1", "-0", "1.5", ".5", "-.5", "+.5", "1.", "1e3", "1e+3", "1E-3", "1e", "1e+", "1ex", "12px", "50%", "-50%",
           "1e400", "1e-400", "0.1", "9007199254740993", "1.5e-320", "00012", "12\\65 5", "1\\65 5", "1e5px", "1.e3", "5e-324", "+-1"]
STRINGS = ['"x"', "'y'", '"a\\"b"', '"a\nb"', '"unclosed', "'x\\", '"\\\n"', '"\\"', '"a\\\r\nb"', "'\\0'", '""', "''", '"\u00e9\u65e5"', '"\x01"']
URLS = ["url(x)", "url( x )", 'url("x")', "url(x y)", 'url(a"b)', "url(a(b)", "url(a\\)b)", "url(", "url(\\0)", "url(a\nb)", "url( )", "url()",
        "url(\x01)", "url(x\\", "URL(x)", "url('x')", "url( 'x' )", "url(\u00e9)", "url(a\\\nb)", "url(a b)c)"]
HASHES = ["#a", "#1", "#-", "#\\31 x", "#--a", "#", "#\u00e9", "#a\\", "#-1", "#_"]
ATS = ["@a", "@media", "@-x", "@\\31 ", "@", "@1", "@import", "@charset", "@font-face", "@\u00e9", "@--x", "@a\\"]
DELIMS = ["!", "?", "<", ">", "=", "+", "-", "*", "/", "%", "&", "|", "^", "~", "$", ".", "`", "\\", "~=", "|=", "^=", "$=", "*=", "||", "\u00a0", "\u00ad"]
SPECIALS = ["<!--", "-->", "<!---->", "/*c*/", "/**/", "/* unclosed", "/*/", "/***/", "U+0", "u+1?", "U+0-7F", "u+", "u+110000", "U+4??", "U+??????",
            "u+0-7f,u+4??", "U+0025-00FF", "u+1-0", "U+10FFFF", "u+0000001", "u+1e5"]
PUNCT = ["(", ")", "[", "]", "{", "}", ":", ";", ",", "f(", "calc(", "var(", "{}", "()", "[]", ";;", "} ", " {"]
SPACE = [" ", " ", " ", "\t", "\n", "\r\n", "\f", "\r", "  ", "\n\n"]
CONTROL = ["\x00", "\x01", "\x7f", "\x08", "\x0b", "\x1f", "\ufffd", "\ufeff", "\u2028", "\u0085"]
BAD_UTF8 = [b"\xc3", b"\xff", b"\xed\xa0\x80", b"\xe2\x82", b"\xf0\x9f\x98", b"\x80", b"\xc0\xaf", b"\xf4\x90\x80\x80", b"\xe2\x82\xac"]

GROUPS = [(IDENTS, 30), (NUMBERS, 12), (STRINGS, 8), (URLS, 5), (HASHES, 4), (ATS, 6), (DELIMS, 8), (SPECIALS, 6), (PUNCT, 24), (SPACE, 24), (CONTROL, 1)]


def pick_fragment(rng):
    if rng.random() < 0.01:
        return rng.choice(BAD_UTF8)
    total = sum(w for _, w in GROUPS)
    r = rng.random() * total
    for group, weight in GROUPS:
        if r < weight:
            return rng.choice(group).encode("utf-8")
        r -= weight
    return b" "


def soup(rng):
    n = 1 + int(rng.expovariate(1 / 9))
    return b"".join(pick_fragment(rng) for _ in range(min(n, 80)))


PROPS = ["color", "margin", "--x", "--y", "background", "font", "unicode-range", "width", "a", "x-y", "COLOR", "\\63 olor", "src", "content", "--"]
VALUES = ["red", "1px", "1px 2px", "#fff", "rgb(1, 2, 3)", "calc(1px + 2px)", "url(x.png)", "'s'", '"s"', "{a}", "{ }", "{a:b}", "[x]", "(y)", "f(g(h))", "1e3",
          "U+0-7F", "u+4??, U+10FFFF", "var(--x, 1)", "a, b", "a b !important", "!important", "! important", "!IMPORTANT", "! x", "!", "1px!important",
          "{a} b", "b {a}", "{a}{b}", "a{", "}", ")", "]", "a ) b", "\"s\n", "url(a b)", "-->", "<!--", "/*c*/ x /*d*/", "\\41 b", "foo(", "x:y"]
SELECTORS = ["a", "a, b", ".c > d", "#e", "a:hover", "a::before", "[x=y]", "&", "& b", "&:hover", "@", "a b c", "*", "a{", "a:", "--x:", "--x: ", "a:b", "x:hover",
             "\\41", "a/**/b", "a\n", ""]
ATRULES = ["@media x", "@media (min-width: 1px)", "@supports (a:b)", "@import url(x)", "@charset 'x'", "@font-face", "@keyframes k", "@layer a, b", "@a", "@-x y",
           "@page :first", "@namespace x", "@media", "@"]


def decl(rng, depth):
    p = rng.choice(PROPS)
    v = rng.choice(VALUES)
    sep = rng.choice([":", ": ", " :", ":", ":", " : "])
    bang = rng.choice(["", "", "", "", " !important", "!important", " ! important", " !IMPORTANT"])
    return f"{p}{sep}{v}{bang}"


def block_items(rng, depth):
    items = []
    for _ in range(rng.randint(0, 4)):
        r = rng.random()
        if r < 0.45:
            items.append(decl(rng, depth))
        elif r < 0.75 and depth < 4:
            items.append(rule(rng, depth + 1, nested=True))
        elif r < 0.85 and depth < 4:
            items.append(atrule(rng, depth + 1))
        elif r < 0.9:
            items.append(rng.choice(["", ";", "; ;", " ", "/*c*/"]))
        else:
            items.append(rng.choice(VALUES))
    return rng.choice([";", "; ", ";\n", ";"]).join(items)


def rule(rng, depth, nested=False):
    gap = rng.choice(["", " ", "\n"])
    return f"{rng.choice(SELECTORS)}{gap}{{{block_items(rng, depth)}}}"


def atrule(rng, depth):
    head = rng.choice(ATRULES)
    r = rng.random()
    if r < 0.4:
        return head + ";"
    if r < 0.5:
        return head
    return head + " {" + block_items(rng, depth) + "}"


def nested(rng):
    parts = []
    for _ in range(rng.randint(1, 5)):
        r = rng.random()
        if r < 0.55:
            parts.append(rule(rng, 0))
        elif r < 0.8:
            parts.append(atrule(rng, 0))
        elif r < 0.9:
            parts.append(decl(rng, 0))
        else:
            parts.append(rng.choice(["<!--", "-->", "/*c*/", ";", "}", ")", "@", "a b c", ""]))
    text = rng.choice(["", " ", "\n"]).join(parts)
    out = text.encode("utf-8")
    # Noise at random places: a fragment, a deleted byte, a doubled one.
    for _ in range(rng.choice([0, 0, 1, 1, 2, 3])):
        out = mutate_once(rng, out)
    return out


def mutate_once(rng, data):
    if not data:
        return pick_fragment(rng)
    kind = rng.randrange(7)
    i = rng.randrange(len(data))
    j = min(len(data), i + 1 + rng.randrange(4))
    if kind == 0:
        return data[:i] + data[j:]
    if kind == 1:
        return data[:j] + data[i:j] + data[j:]
    if kind == 2:
        return data[:i] + pick_fragment(rng) + data[i:]
    if kind == 3:
        return data[:i] + bytes([rng.randrange(256)]) + data[i + 1:]
    if kind == 4 and len(data) > 2:
        a, b = sorted((rng.randrange(len(data)), rng.randrange(len(data))))
        return data[:a] + data[b:b + 1] + data[a + 1:b] + data[a:a + 1] + data[b + 1:]
    if kind == 5:
        return data[:i] + pick_fragment(rng) + data[j:]
    return data[:i] + rng.choice([b"{", b"}", b"(", b")", b"\"", b"'", b"\\", b"/*", b"*/", b";", b":", b"!"]) + data[i:]


def mutated(rng, base):
    data = base
    for _ in range(rng.randint(1, 4)):
        data = mutate_once(rng, data)
    return data


CHARSETS = ["windows-1251", "iso-8859-2", "windows-1252", "shift_jis", "gb18030", "euc-kr", "big5", "utf-8", "iso-8859-5", "koi8-r", "latin1",
            "utf-16le", "utf-16be", "UTF-8", " Windows-1251 ", "x-user-defined", "hz-gb-2312", "kamoulox", ""]
PY_CODEC = {"windows-1251": "cp1251", "iso-8859-2": "iso8859_2", "windows-1252": "cp1252", "shift_jis": "shift_jis", "gb18030": "gb18030",
            "euc-kr": "euc_kr", "big5": "big5", "utf-8": "utf-8", "iso-8859-5": "iso8859_5", "koi8-r": "koi8_r", "latin1": "latin1",
            "utf-16le": "utf-16-le", "utf-16be": "utf-16-be"}
TEXT_POOL = ["a{b:c}", "\u043f\u0440\u0438\u0432\u0435\u0442{\u0446\u0432\u0435\u0442:\u043a\u0440\u0430\u0441\u043d\u044b\u0439}", ".\u00e9{b:\"\u00fc\"}", "a{content:\"\u65e5\u672c\"}",
             ".\u4e2d\u6587 > x{y:z}", "@media x{a{b:c}}", "/*\u00e9*/a{b:c}", "a{b:url(\u00e9)}", "x{\u00e9:y}", "\u00e9{}"]


def bytes_item(rng):
    text = "".join(rng.choice(TEXT_POOL) for _ in range(rng.randint(1, 3)))
    label = rng.choice([c for c in CHARSETS if c in PY_CODEC] + ["utf-8"] * 3)
    codec = PY_CODEC[label]
    body = text.encode(codec, "replace")
    proto = env = None
    kind = rng.randrange(9)
    if kind == 0:
        data = body
    elif kind == 1:
        data = ('@charset "%s";' % rng.choice(CHARSETS)).encode("ascii", "replace") + body
    elif kind == 2:
        data = (rng.choice(['@charset  "%s";', "@charset '%s';", '@Charset "%s";', ' @charset "%s";', '@charset "%s" ;', '@charset "%s"', '/**/@charset "%s";']) % rng.choice(CHARSETS)).encode("ascii", "replace") + body
    elif kind == 3:
        data = {"utf-8": b"\xef\xbb\xbf", "utf-16le": b"\xff\xfe", "utf-16be": b"\xfe\xff"}.get(label, b"\xef\xbb\xbf") + body
        # (a label that contradicts the mark half of the time, chosen from the body's length, not from the generator: the
        # items after this one stay what they were. The mark wins over a transport label and an environment encoding.)
        proto = "windows-1251" if len(body) % 2 == 0 else None
        env = "iso-8859-2" if len(body) % 3 == 0 else None
    elif kind == 4:
        data = body
        proto = rng.choice(CHARSETS)
    elif kind == 5:
        data = body
        env = rng.choice([c for c in CHARSETS if c in PY_CODEC])
    elif kind == 6:
        data = ('@charset "%s";' % label).encode("ascii") + body
        proto = rng.choice(CHARSETS)
        env = rng.choice([c for c in CHARSETS if c in PY_CODEC])
    elif kind == 7:
        pad = rng.choice([1000, 1010, 1011, 1012, 1013, 1014, 1030])
        data = ('@charset "%s"' % label).encode("ascii") + b" " * max(0, pad - 20) + b'";' + body
    else:
        data = mutated(rng, body)
        proto = rng.choice([None, None, "windows-1251", "utf-8", "kamoulox"])
    return data, proto, env


def corpus_items():
    """(tag, bytes) for every input of the pinned vectors in scope. Missing data is an error, not an empty corpus."""
    out = []
    if not os.path.isdir(SUITE):
        raise SystemExit("the pinned css-parsing-tests are not in %s; run tools/css/fetch.sh" % SUITE)
    for name in sorted(os.listdir(SUITE)):
        if not name.endswith(".json") or name.startswith("color_") or name in ("stylesheet_bytes.json",):
            continue
        with open(os.path.join(SUITE, name), encoding="utf-8") as fh:
            data = json.load(fh)
        for k in range(0, len(data) - 1, 2):
            out.append(("corpus:" + name[:-5], data[k].encode("utf-8", "surrogatepass")))
    if os.path.isfile(WPT_VECTORS):
        with open(WPT_VECTORS, encoding="utf-8") as fh:
            for v in json.load(fh)["vectors"]:
                out.append(("corpus:wpt", v["input"].encode("utf-8", "surrogatepass")))
    return out


def bytes_corpus():
    out = []
    with open(os.path.join(SUITE, "stylesheet_bytes.json"), encoding="utf-8") as fh:
        data = json.load(fh)
    for k in range(0, len(data) - 1, 2):
        c = data[k]
        out.append({"data": c["css_bytes"].encode("latin-1"), "proto": c.get("protocol_encoding"), "env": c.get("environment_encoding"),
                    "tags": ["corpus:stylesheet_bytes", "bytes"]})
    return out


def population(count, seed, corpus=True):
    """A list of items. The corpus is not counted in `count`; the generated items are."""
    rng = random.Random(seed)
    items = []
    base = corpus_items() if corpus else []
    for i, (tag, data) in enumerate(base):
        items.append({"id": "c%d" % i, "tags": [tag], "data": data, "proto": None, "env": None})
    if corpus:
        for i, b in enumerate(bytes_corpus()):
            b["id"] = "cb%d" % i
            items.append(b)
    nested_pool = [nested(rng) for _ in range(64)]
    for i in range(count):
        r = rng.random()
        if r < 0.30:
            items.append({"id": "s%d" % i, "tags": ["soup"], "data": soup(rng), "proto": None, "env": None})
        elif r < 0.55:
            items.append({"id": "n%d" % i, "tags": ["nested"], "data": nested(rng), "proto": None, "env": None})
        elif r < 0.90:
            pool = base if base and rng.random() < 0.5 else None
            src = rng.choice(pool)[1] if pool else rng.choice(nested_pool)
            items.append({"id": "m%d" % i, "tags": ["mutated"], "data": mutated(rng, src), "proto": None, "env": None})
        else:
            data, proto, env = bytes_item(rng)
            items.append({"id": "b%d" % i, "tags": ["bytes"], "data": data, "proto": proto, "env": env})
    return items


def main():
    count, seed, show = 1000, 1, 0
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--show":
            show = int(args.pop(0))
    items = population(count, seed)
    print("%d items (%d generated)" % (len(items), count))
    tags = {}
    for it in items:
        key = it["tags"][0].split(":")[0]
        tags[key] = tags.get(key, 0) + 1
    print(tags)
    for it in items[len(items) - count:][:show]:
        print(it["id"], it["tags"], it["proto"], it["env"], it["data"][:100])


if __name__ == "__main__":
    main()
