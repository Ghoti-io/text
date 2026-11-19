#!/usr/bin/env python3
"""The inputs of the XML differential: the W3C suite, the fuzz seeds, and a
generated set, each tagged with where it came from and what axis it varies.

    xml_gen.py [--count N] [--seed S]     print a summary of the population

**Invalid input is the point.** A differential over well-formed XML measures
nothing about the refusals, and the refusals are most of what a parser is for. The
not-well-formed half of the W3C suite is in, every `valid` and `invalid` case is in
(whether it is *valid* is never asked; they are well-formed documents and the
references are asked whether they are), and the generated set is built to be
mostly refused (it is built from grammar with defects mixed in, and this parser accepts about 6% of it; xml_diff.py bounds that).

**The deciding axes are varied on purpose** and every one must appear in the run
and be counted:

  namespaces   prefixes bound, unbound and reused; the reserved `xml` and `xmlns`;
               a default namespace undeclared with `xmlns=""`; a name with two
               colons; the same attribute under two prefixes bound to one URI
  encodings    UTF-8 with and without a mark; UTF-16 and UTF-32, both byte orders,
               with and without a mark; ISO-8859-1 and US-ASCII by declaration;
               labels that disagree with the bytes; units that are not characters;
               and a second population (legacy_generated) of documents in the
               other encodings the library decodes (Windows-1251, KOI8-R,
               Shift_JIS, EUC-JP, ISO-2022-JP, EUC-KR, GBK, Big5, ...): declared,
               corrupted, labeled over a mark, labeled over UTF-16
  references   the five predefined entities, decimal and hexadecimal character
               references, ones that name a forbidden character, ones that do not
               end, and a name no declaration could give a meaning to
  structure    unclosed, mismatched, doubled and misplaced tags; text outside the
               root; a second root; comments and instructions where they may and
               may not be; CDATA that ends early or never

The generator uses its own generator of random numbers (splitmix64) and nothing
from the `random` module, because the population is named by content hash in the
disagreement list and a standard library that changed `choice` between releases
would turn every entry stale.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SUITE = os.path.join(ROOT, "third_party", "xml", "xmlconf")
SEED_DIR = os.path.join(ROOT, "tests", "fuzz", "corpus", "xml")

XML_BASE = "{http://www.w3.org/XML/1998/namespace}base"


# ---- the W3C suite ------------------------------------------------------------

def _read_text(path):
    with open(path, "rb") as fh:
        data = fh.read()
    m = re.match(rb"<\?xml[^>]*encoding=[\"']([A-Za-z0-9._-]+)[\"']", data)
    return data.decode(m.group(1).decode() if m else "utf-8")


def _strip_declaration(text):
    return re.sub(r"^\s*<\?xml[^>]*\?>", "", text)


def _catalogue(suite):
    """xmlconf.xml with its external entities inlined (see xmlconf_suite.py,
    which this follows: no XML library here will read them, and one that did would
    be doing exactly what the parser under test refuses to)."""
    top = _read_text(os.path.join(suite, "xmlconf.xml"))
    entities = dict(re.findall(r"<!ENTITY\s+(\S+)\s+SYSTEM\s+\"([^\"]+)\"", top))
    if not entities:
        raise SystemExit("xmlconf.xml names no sub-catalogues; is this the suite?")
    body = re.sub(r"<!DOCTYPE.*?\n\]>", "", top, flags=re.S)
    body = re.sub(r"<\?xml-stylesheet[^>]*\?>", "", _strip_declaration(body))
    for name, rel in entities.items():
        sub = _strip_declaration(_read_text(os.path.join(suite, rel)))
        sub = re.sub(r"<!DOCTYPE[^>]*>", "", sub)
        folder = os.path.dirname(rel)
        sub = re.sub(r"(<TESTCASES\b)", r'\1 SUBDIR="%s"' % folder, sub, count=1)
        body = body.replace("&%s;" % name, sub)
    return ET.fromstring(body)


def _walk(node, base, subdir, out, suite):
    if node.tag == "TESTCASES":
        if node.get(XML_BASE):
            base = os.path.join(base, node.get(XML_BASE))
        if node.get("SUBDIR"):
            subdir = node.get("SUBDIR")
    for child in node:
        if child.tag == "TESTCASES":
            _walk(child, base, subdir, out, suite)
        elif child.tag == "TEST":
            own = child.get(XML_BASE)
            path = os.path.join(base, own) if own else base
            where = os.path.normpath(os.path.join(suite, path, child.get("URI")))
            beside = os.path.normpath(os.path.join(suite, subdir, child.get("URI")))
            out.append((child, where, beside))


def xmlconf_cases(suite=SUITE, include_excluded=False):
    """[(id, bytes, tags)] for every case in the catalogue.

    Every type: `not-wf` and `error` are in the denominator along with `valid` and
    `invalid`. The tags carry the type and the edition/version/namespace
    attributes, which the report uses to separate the two supported XML profiles.
    """
    if not os.path.exists(os.path.join(suite, "xmlconf.xml")):
        raise SystemExit("%s is not here. Fetch the pinned suite: tools/xml/fetch.sh"
                         % os.path.join(suite, "xmlconf.xml"))
    cases = []
    _walk(_catalogue(suite), "", "", cases, suite)
    exclusions_path = os.path.join(os.path.dirname(HERE), "conformance", "XMLCONF_EXCLUSIONS")
    excluded = set()
    with open(exclusions_path, encoding="utf-8") as fh:
        for line in fh:
            if line.strip() and not line.startswith("#"):
                excluded.add(line.split("\t", 1)[0])
    out = []
    for t, where, beside in cases:
        path = where if os.path.exists(where) else beside
        if not os.path.exists(path):
            raise SystemExit("catalogue case %s has no file at %s" % (t.get("ID"), where))
        if not include_excluded and t.get("ID") in excluded:
            continue
        with open(path, "rb") as fh:
            data = fh.read()
        tags = ["xmlconf", "type:" + (t.get("TYPE") or "?")]
        if t.get("VERSION"):
            tags.append("version:" + t.get("VERSION").replace(" ", ","))
        if t.get("NAMESPACE") == "no":
            tags.append("not-namespace-aware")
        if t.get("RECOMMENDATION"):
            tags.append("rec:" + t.get("RECOMMENDATION"))
        out.append((t.get("ID"), data, tags))
    return out


def seed_cases(directory=SEED_DIR):
    """The fuzz seeds, each four option bytes and then a document (tests/fuzz/README.md)."""
    out = []
    if not os.path.isdir(directory):
        return out
    exclusions_path = os.path.join(os.path.dirname(HERE), "conformance", "XMLCONF_EXCLUSIONS")
    with open(exclusions_path, encoding="utf-8") as fh:
        excluded = {line.split("\t", 1)[0] for line in fh
                    if line.strip() and not line.startswith("#")}
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".seed"):
            continue
        if name.startswith("xmlconf-") and name[len("xmlconf-"):-len(".seed")] in excluded:
            continue
        with open(os.path.join(directory, name), "rb") as fh:
            data = fh.read()
        if not data:
            continue
        out.append(("seed:" + name, data[4:], ["fuzz-seed"]))
    return out


# ---- a generator of random numbers that will not change under us ----------------

class Rng:
    """splitmix64. Small, fixed, and the same on every Python."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFFFFFFFFFF

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return z ^ (z >> 31)

    def below(self, n):
        return self.next() % n

    def chance(self, num, den):
        return self.below(den) < num

    def pick(self, seq):
        return seq[self.below(len(seq))]


NAMES = ["a", "b", "item", "x1", "_u", "a-b", "a.b", "e\u00e9", "\u65e5\u672c", "A", "doc", "r"]
# Names that XML 1.0 Fifth Edition allows and the Fourth did not: expat's tables are the
# Fourth's, so every document using one disagrees with it, and a population where that is
# one name in a dozen is mostly that disagreement. Rare on purpose, and still present.
NAMES_FIFTH = ["\U00010000n", "a\U0001d7ce", "a\u0300"]
PREFIXES = ["p", "q", "ns", "xml", "xmlns", "a", "xmlnsx", "XML", "x"]
# Namespace names that are not URIs ("a b", an accented letter) are refused by libxml2 and
# accepted by expat and by this parser, which does not check; rare for the reason above.
URIS = ["u", "urn:a", "http://example.com/ns", "", "http://www.w3.org/XML/1998/namespace",
        "http://www.w3.org/2000/xmlns/", "v", "urn:b", "x:y", "http://example.com/ns"]
URIS_ODD = ["a b", "\u00e9"]


TEXTS = ["t", "hello world", " ", "a&amp;b", "&lt;&gt;&quot;&apos;", "&#65;", "&#x1F600;",
         "&#x0;", "&#xD800;", "&#38;", "&#xFFFE;", "&amp", "&nosuch;", "a]]>b", "x\r\ny", "\r", "\t",
         "&#65", "&#;", "&#x;", "&", "<", ">", "\u00e9", "\U0001F600"]
SPECIALS = ["<", ">", "&", ";", "\"", "'", "=", "/", "!", "[", "]", "-", "--", "?", ":", "::",
            "\x00", "\x01", "\x0b", "\xff", " ", "\t", "\r", "\n", "<!--", "-->", "<![CDATA[",
            "]]>", "<?", "?>", "xmlns", "xmlns:", "xml:", "&#", "<!DOCTYPE", "\ufeff", "\ufffe"]


def _name(rng):
    if rng.chance(1, 24):
        return rng.pick(NAMES_FIFTH)
    return rng.pick(NAMES)


def _uri(rng):
    if rng.chance(1, 24):
        return rng.pick(URIS_ODD)
    return rng.pick(URIS)


def _attrs(rng):
    out = []
    for _ in range(rng.below(4)):
        if rng.chance(1, 3):
            pre = rng.pick(PREFIXES) if rng.chance(1, 2) else ""
            name = ("xmlns:" + pre) if pre else "xmlns"
            out.append('%s=%s' % (name, _quote(rng, _uri(rng))))
        else:
            pre = rng.pick(PREFIXES) + ":" if rng.chance(1, 3) else ""
            out.append('%s%s=%s' % (pre, _name(rng), _quote(rng, rng.pick(TEXTS))))
    return out


def _quote(rng, text):
    q = '"' if rng.chance(3, 4) else "'"
    if q in text:
        q = "'" if q == '"' else '"'
    return q + text + q


def _element(rng, depth):
    pre = (rng.pick(PREFIXES) + ":") if rng.chance(1, 4) else ""
    name = pre + _name(rng)
    attrs = _attrs(rng)
    ws = " " if attrs or rng.chance(1, 8) else ""
    head = "<%s%s%s" % (name, ws, " ".join(attrs))
    if depth > 3 or rng.chance(1, 4):
        return head + "/>"
    body = []
    for _ in range(rng.below(4)):
        kind = rng.below(7)
        if kind <= 1:
            body.append(rng.pick(TEXTS))
        elif kind == 2:
            body.append("<![CDATA[%s]]>" % rng.pick(["x", "a<b>&c", "", "]]", "]"]))
        elif kind == 3:
            body.append("<!--%s-->" % rng.pick(["c", " c ", "", "a-b", "--", "-"]))
        elif kind == 4:
            body.append("<?%s%s?>" % (rng.pick(["pi", "xml", "XML", "a:b", "x"]),
                                      rng.pick([" d", "", " d?", "  x  "])))
        else:
            body.append(_element(rng, depth + 1))
    # A closing tag that sometimes does not match: a refused document, made cheaply.
    close = name if not rng.chance(1, 14) else rng.pick(NAMES)
    return "%s>%s</%s>" % (head, "".join(body), close)


def structured(rng):
    """One document built from the grammar, with defects mixed in at a low rate."""
    parts = []
    if rng.chance(1, 3):
        decl = '<?xml version="%s"' % rng.pick(["1.0", "1.0", "1.0", "1.1", "2.0", "1"])
        if rng.chance(1, 3):
            decl += ' encoding="%s"' % rng.pick(["UTF-8", "utf-8", "UTF-16", "ISO-8859-1",
                                                  "US-ASCII", "latin1", "x y", "windows-1252"])
        if rng.chance(1, 4):
            decl += ' standalone="%s"' % rng.pick(["yes", "no", "maybe"])
        parts.append(decl + "?>")
    for _ in range(rng.below(3)):
        parts.append(rng.pick(["<!--c-->", "<?pi d?>", " ", "\n", "<!--a--b-->"]))
    if rng.chance(1, 5):
        parts.append(rng.pick([
            "<!DOCTYPE a>", '<!DOCTYPE a SYSTEM "s.dtd">', '<!DOCTYPE a PUBLIC "p" "s">',
            '<!DOCTYPE a PUBLIC "p  q\r\n r" "s">', "<!DOCTYPE a [ ]>", "<!DOCTYPE a [<!-- ] -->]>",
            '<!DOCTYPE a [<!ENTITY e "v">]>', "<!DOCTYPE>", "<!DOCTYPE a", "<!DOCTYPE a [",
            '<!DOCTYPE a PUBLIC "p">']))
    parts.append(_element(rng, 0))
    for _ in range(rng.below(3)):
        parts.append(rng.pick(["<!--c-->", "<?pi d?>", " ", "\n", "text", "<b/>", "<!--", "&amp;"]))
    return "".join(parts).encode("utf-8")


def mutate(rng, data):
    """A few byte-level edits at the characters that matter."""
    data = bytearray(data)
    for _ in range(1 + rng.below(3)):
        if not data:
            data.extend(b"<a/>")
        kind = rng.below(6)
        at = rng.below(len(data) + 1)
        if kind == 0 and data:
            del data[min(at, len(data) - 1)]
        elif kind == 1:
            data[at:at] = rng.pick(SPECIALS).encode("utf-8", "surrogatepass")
        elif kind == 2 and data:
            data[min(at, len(data) - 1)] = rng.below(256)
        elif kind == 3 and data:
            lo = min(at, len(data) - 1)
            hi = min(len(data), lo + 1 + rng.below(8))
            data[lo:lo] = data[lo:hi]
        elif kind == 4:
            del data[at:]
        else:
            data[at:at] = rng.pick(SPECIALS).encode("utf-8", "surrogatepass")
    return bytes(data)


def encode(rng, text_bytes):
    """The same document in another encoding, sometimes with a label that lies.

    Returns (bytes, axis tag). `text_bytes` is UTF-8; one that does not decode is
    returned as it is (a mutation made it invalid and that is its own axis).
    """
    try:
        text = text_bytes.decode("utf-8")
    except UnicodeDecodeError:
        return text_bytes, "enc:invalid-utf8"
    kind = rng.below(12)
    if kind in (5, 6, 7) and not rng.chance(1, 3):
        kind = 0  # UTF-32: no reference reads it, so a third of its share

    if kind == 0:
        return b"\xef\xbb\xbf" + text_bytes, "enc:utf8-bom"
    if kind == 1:
        return text.encode("utf-16-le"), "enc:utf16le-nobom"
    if kind == 2:
        return text.encode("utf-16-be"), "enc:utf16be-nobom"
    if kind == 3:
        return b"\xff\xfe" + text.encode("utf-16-le", "surrogatepass"), "enc:utf16le-bom"
    if kind == 4:
        return b"\xfe\xff" + text.encode("utf-16-be", "surrogatepass"), "enc:utf16be-bom"
    if kind == 5:
        return b"\xff\xfe\x00\x00" + text.encode("utf-32-le", "surrogatepass"), "enc:utf32le-bom"
    if kind == 6:
        return b"\x00\x00\xfe\xff" + text.encode("utf-32-be", "surrogatepass"), "enc:utf32be-bom"
    if kind == 7:
        return text.encode("utf-32-be", "surrogatepass"), "enc:utf32be-nobom"
    if kind == 8:
        t = '<?xml version="1.0" encoding="ISO-8859-1"?>' + re.sub(r"^<\?xml[^>]*\?>", "", text)
        return t.encode("latin-1", "replace"), "enc:latin1-declared"
    if kind == 9:
        t = '<?xml version="1.0" encoding="US-ASCII"?>' + re.sub(r"^<\?xml[^>]*\?>", "", text)
        return t.encode("utf-8"), "enc:ascii-declared"
    if kind == 10:
        t = '<?xml version="1.0" encoding="UTF-16"?>' + re.sub(r"^<\?xml[^>]*\?>", "", text)
        return t.encode("utf-8"), "enc:utf8-labeled-utf16"
    t = '<?xml version="1.0" encoding="UTF-8"?>' + re.sub(r"^<\?xml[^>]*\?>", "", text)
    return b"\xff\xfe" + t.encode("utf-16-le", "surrogatepass"), "enc:utf16-labeled-utf8"


def generated(count, seed, xmlconf):
    """[(id, bytes, tags)]: structured documents, mutants of them, mutants of the
    suite's own documents, and re-encodings of all three."""
    rng = Rng(seed)
    small = [d for (_i, d, _t) in xmlconf if len(d) < 600]
    out = []
    seen = set()
    guard = 0
    while len(out) < count and guard < count * 20:
        guard += 1
        which = rng.below(10)
        if which < 4:
            data, tags = structured(rng), ["gen:structured"]
        elif which < 7:
            data, tags = mutate(rng, structured(rng)), ["gen:mutated"]
        elif which < 9 and small:
            data, tags = mutate(rng, rng.pick(small)), ["gen:mutated-xmlconf"]
        else:
            data, tags = structured(rng), ["gen:structured"]
        if rng.chance(1, 4):
            data, axis = encode(rng, data)
            tags.append(axis)
        digest = hashlib.sha256(data).hexdigest()[:12]
        if digest in seen:
            continue
        seen.add(digest)
        out.append(("gen:" + digest, data, tags))
    return out


# ---- documents in the other encodings the library decodes -----------------------

# label, Python's codec, text that the encoding has (and that is not the other scripts'). The
# population is made with Python's codecs, which are not the library's tables: a place where
# they differ is a disagreement to be read.
LEGACY_SAMPLES = [
    ("windows-1251", "cp1251", ["\u041f\u0440\u0438\u0432\u0435\u0442", "\u043c\u0438\u0440", "\u0416\u0436"]),
    ("KOI8-R", "koi8_r", ["\u041f\u0440\u0438\u0432\u0435\u0442", "\u043c\u0438\u0440"]),
    ("windows-1252", "cp1252", ["caf\u00e9", "\u20ac", "na\u00efve"]),
    ("ISO-8859-7", "iso8859_7", ["\u0395\u03bb\u03bb\u03ac\u03b4\u03b1", "\u03a9"]),
    ("Shift_JIS", "cp932", ["\u65e5\u672c\u8a9e", "\u3042\u3044", "\u30a2"]),
    ("EUC-JP", "euc_jp", ["\u65e5\u672c\u8a9e", "\u3042\u3044", "\u30a2"]),
    ("ISO-2022-JP", "iso2022_jp", ["\u65e5\u672c\u8a9e", "\u3042\u3044"]),
    ("EUC-KR", "cp949", ["\ud55c\uad6d\uc5b4", "\uac00\ub098"]),
    ("GBK", "gb18030", ["\u4e2d\u6587", "\u6c49\u5b57"]),
    ("Big5", "big5hkscs", ["\u4e2d\u6587", "\u6f22\u5b57"]),
]
# What a label may look like; the Encoding standard's labels are not case sensitive.
LEGACY_LABEL_FORMS = {
    "windows-1251": ["windows-1251", "Windows-1251", "cp1251", "x-cp1251"],
    "Shift_JIS": ["Shift_JIS", "shift_jis", "sjis", "windows-31j"],
    "EUC-KR": ["EUC-KR", "euc-kr"],
}


def _legacy_document(rng, label, words):
    """A well-formed document in which every non-ASCII character is one of `words`, as text."""
    w = lambda: rng.pick(words)
    body = rng.pick([
        "<r a=\"%s\">%s<c>%s</c></r>" % (w(), w(), w()),
        "<r>%s %s<!--%s--></r>" % (w(), w(), w()),
        "<%s a=\"%s\">%s</%s>" % ("r", w(), w(), "r"),
        "<r><![CDATA[%s]]><?p %s?>%s</r>" % (w(), w(), w()),
        "<r>\n  <c>%s</c>\r\n  <c b='%s'/>\n</r>" % (w(), w()),
        "<%s>%s</%s>" % (w(), w(), w()) if rng.chance(1, 3) else "<r>%s</r>" % w(),
    ])
    decl = '<?xml version="%s" encoding="%s"?>' % (rng.pick(["1.0", "1.0", "1.1"]), label)
    return decl + rng.pick(["", "\n", "<!--x-->"]) + body


def legacy_generated(count, seed):
    """[(id, bytes, tags)]: documents in the other encodings of the encoding module, valid ones
    and the ways they go wrong. A stream of random numbers of its own, so that adding them left
    the identity of every other generated document (a hash of its bytes) as it was."""
    rng = Rng(seed ^ 0x4C45474143590001)
    out = []
    seen = set()
    guard = 0
    while len(out) < count and guard < count * 20:
        guard += 1
        label, codec, words = rng.pick(LEGACY_SAMPLES)
        written = rng.pick(LEGACY_LABEL_FORMS.get(label, [label, label.lower()]))
        text = _legacy_document(rng, written, words)
        kind = rng.below(10)
        if kind < 5:
            data, axis = text.encode(codec), "enc:legacy-declared"
        elif kind == 5:
            # An ill-formed sequence: the last byte of a multi-byte character cut off, or a byte no
            # character of the encoding starts with.
            data = bytearray(text.encode(codec))
            high = [i for i, b in enumerate(data) if b >= 0x80]
            if high and codec not in ("iso2022_jp",):
                at = rng.pick(high)
                if rng.chance(1, 2):
                    del data[at]
                else:
                    data[at] = rng.pick([0x80, 0xFF, 0xFE, 0xA0])
            else:
                data += b"\xff"
            data, axis = bytes(data), "enc:legacy-invalid"
        elif kind == 6:
            data, axis = b"\xef\xbb\xbf" + text.encode("utf-8"), "enc:legacy-labeled-utf8bom"
        elif kind == 7:
            data, axis = b"\xff\xfe" + text.encode("utf-16-le"), "enc:legacy-labeled-utf16"
        elif kind == 8:
            # UTF-8 bytes under a legacy label: ASCII only, so every encoding reads them the same.
            data = _legacy_document(rng, written, ["t", "hello"]).encode("ascii")
            axis = "enc:legacy-declared-ascii"
        else:
            # A document that is cut inside its last multi-byte character.
            raw = text.encode(codec)
            high = [i for i, b in enumerate(raw) if b >= 0x80]
            multi = high and codec not in ("iso2022_jp", "cp1251", "koi8_r", "cp1252", "iso8859_7")
            data = raw[:high[-1] + 1] if multi else raw
            axis = "enc:legacy-cut" if multi else "enc:legacy-declared"
        digest = hashlib.sha256(data).hexdigest()[:12]
        if digest in seen:
            continue
        seen.add(digest)
        out.append(("gen:" + digest, data, ["gen:legacy", axis]))
    return out


def corpus(count, seed):
    """Every input: suite, seeds, generated. Deduplicated by content, the first
    name kept, so a seed that is a suite document is counted once."""
    suite = xmlconf_cases()
    source_suite = xmlconf_cases(include_excluded=True)
    items = (suite + seed_cases() + generated(count, seed, source_suite)
             + legacy_generated(max(60, count // 10), seed))
    seen = {}
    out = []
    for ident, data, tags in items:
        digest = hashlib.sha256(data).digest()
        if digest in seen:
            continue
        seen[digest] = ident
        out.append((ident, data, tags))
    return out


if __name__ == "__main__":
    n, s = 4000, 1
    args = sys.argv[1:]
    while args:
        if args[0] == "--count":
            n = int(args[1])
        elif args[0] == "--seed":
            s = int(args[1])
        args = args[2:]
    items = corpus(n, s)
    counts = {}
    for _i, _d, tags in items:
        for t in tags:
            counts[t] = counts.get(t, 0) + 1
    print("%d inputs" % len(items))
    for t in sorted(counts):
        print("  %-28s %d" % (t, counts[t]))
