"""The documents of the sniffing differential (tools/oracle/html_sniff_diff.py): bytes that put the encoding sniffing algorithm
through its cases, each tagged with the features it was built with.

The tags are what the classification of a divergence rests on: a document is built from named parts, so what a document
*has* is known from how it was made and not guessed from what it does. A part with a tag is one the standard says something
about (a byte order mark, an XML declaration, a charset attribute that names nothing, a meta that ends at the edge of the
1024 bytes); a document made of parts none of which has a tag is plain, and a disagreement on a plain document is a finding.

Copyright 2026 by Corey Pennycuff
"""

import random

# Labels: names the Encoding Standard knows (a sample of every family and of the spellings of one), and names it does not.
LABELS = [
    "utf-8", "UTF-8", "Utf8", "unicode-1-1-utf-8", "windows-1252", "WINDOWS-1252", "latin1", "iso-8859-1", "ascii", "us-ascii",
    "iso-8859-2", "iso8859-2", "iso_8859-2", "latin2", "l2", "iso-8859-3", "iso-8859-4", "iso-8859-5", "cyrillic", "iso-8859-6",
    "arabic", "iso-8859-7", "greek", "iso-8859-8", "hebrew", "iso-8859-8-i", "iso-8859-10", "iso-8859-13", "iso-8859-14", "iso-8859-15",
    "iso-8859-16", "koi8-r", "koi8", "koi8-u", "macintosh", "mac", "x-mac-roman", "windows-874", "windows-1250", "windows-1251",
    "windows-1253", "windows-1254", "windows-1255", "windows-1256", "windows-1257", "windows-1258", "x-mac-cyrillic", "ibm866", "cp866",
    "gbk", "gb2312", "chinese", "gb18030", "big5", "big5-hkscs", "cn-big5", "euc-jp", "x-euc-jp", "iso-2022-jp", "csiso2022jp",
    "shift_jis", "shift-jis", "sjis", "ms_kanji", "windows-31j", "x-sjis", "euc-kr", "korean", "ks_c_5601-1987", "windows-949",
    "utf-16", "utf-16le", "utf-16be", "unicode", "unicodefffe", "ucs-2", "x-user-defined", "replacement", "hz-gb-2312", "iso-2022-kr",
    "utf-7", "utf-32", "ucs-4", "cp1252", "cp1251", "windows-1251 ", " windows-1251", "x-unicode20utf8", "tis-620", "dos-874",
]
INVALID = ["utf-8x", "nonsense", "", " ", "utf 8", "windows", "iso-8859", "utf-80", "x", "charset", "é", "\xff", "utf8;", "'utf-8'"]

BOMS = {"bom8": b"\xef\xbb\xbf", "bom16le": b"\xff\xfe", "bom16be": b"\xfe\xff", "bom32le": b"\xff\xfe\x00\x00", "bom32be": b"\x00\x00\xfe\xff"}
WS = [b" ", b"\t", b"\n", b"\x0c", b"\r", b"  ", b" \n "]
SEPS = [b" ", b"\t", b"\n", b"\x0c", b"\r", b"/", b" /", b"/ ", b"  "]


def label_bytes(rng, tags, allow_invalid=True):
    r = rng.random()
    if allow_invalid and r < 0.12:
        tags.add("invalid-label")
        return rng.choice(INVALID).encode("latin-1", "replace")
    s = rng.choice(LABELS)
    if rng.random() < 0.3:
        s = s.upper() if rng.random() < 0.5 else s.title()
    return s.encode("latin-1", "replace")


def quote(rng, value, tags):
    q = rng.choice([b'"', b"'", b"", b"", b'"'])
    if q == b"" and (b" " in value or b">" in value or not value):
        q = b'"'
    if rng.random() < 0.05:
        tags.add("unmatched-quote")
        return q + value  # the quote is never closed
    return q + value + q


def attr_name(rng, name, tags):
    if rng.random() < 0.25:
        name = name.upper() if rng.random() < 0.7 else name.title()
    return name.encode()


def content_value(rng, label, tags):
    forms = [
        b"text/html; charset=" + label,
        b"text/html;charset=" + label,
        b"charset=" + label,
        b"text/html; charset = " + label,
        b"text/html; charset='" + label + b"'",
        b'text/html; charset="' + label + b'"',
        b"text/html; charset=" + label + b"; q=1",
        b"text/html; Charset=" + label,
        b"text/html; CHARSET=" + label,
        b"text/html; xcharset=" + label,
        b"text/html; charset",
        b"text/html; charset=",
        b"charset=" + label + b" charset=utf-8",
        b"text/html",
        b"text/html; charset = '" + label,
        b'text/html; charset="' + label,
        b"; charset=" + label,
        b"charsetcharset=" + label,
    ]
    f = rng.choice(forms)
    if b"xcharset" in f:
        tags.add("charset-substring")
    return f


def meta(rng, tags):
    """One meta tag (or something that looks like one), as bytes."""
    kind = rng.choice(["charset"] * 5 + ["http"] * 5 + ["both", "refresh", "content-only", "empty", "name"])
    lab = label_bytes(rng, tags)
    sep = rng.choice(SEPS)
    tag = rng.choice([b"<meta", b"<META", b"<Meta"])
    parts = []
    if kind in ("charset", "both"):
        eq = rng.choice([b"=", b" =", b"= ", b" = ", b"\t=\n"])
        parts.append(attr_name(rng, "charset", tags) + eq + quote(rng, lab, tags))
    if kind in ("http", "both"):
        he = rng.choice([b"content-type", b"Content-Type", b"CONTENT-TYPE", b"content-type ", b"content-style-type", b"refresh"])
        if he.strip() != he or he == b"content-style-type" or he == b"refresh":
            tags.add("wrong-pragma")
        h = attr_name(rng, "http-equiv", tags) + b"=" + quote(rng, he, tags)
        c = attr_name(rng, "content", tags) + b"=" + quote(rng, content_value(rng, lab, tags), tags)
        parts.extend([h, c] if rng.random() < 0.5 else [c, h])
    if kind == "refresh":
        parts.append(b'http-equiv=refresh content="0; url=x"')
    if kind == "content-only":
        parts.append(b"content=" + quote(rng, content_value(rng, lab, tags), tags))
    if kind == "name":
        parts.append(b"name=viewport content=width")
    if rng.random() < 0.25:
        parts.insert(rng.randrange(len(parts) + 1), b"x=y")
    if rng.random() < 0.15 and parts:
        parts.append(parts[0])  # a duplicate attribute
        tags.add("duplicate-attribute")
    out = tag + sep + (sep if rng.random() < 0.3 else b" ").join(parts)
    out += rng.choice([b">", b" >", b"/>", b" />", b">", b">", b""])
    if kind != "empty" and len(parts) == 0:
        out = tag + sep + b">"
    return out


def filler(rng, tags):
    r = rng.random()
    if r < 0.15:
        return rng.choice([b"<!-- a -->", b"<!---->", b"<!-->", b"<!--->", b"<!-- <meta charset=koi8-r> -->", b"<!-- -- --!>", b"<!--"])
    if r < 0.3:
        return rng.choice([b"<!DOCTYPE html>", b"<html>", b"<head>", b"<title>t</title>", b"<p>", b"</p>", b"<br/>", b"<a href=x>"])
    if r < 0.4:
        return rng.choice([b'<a title="<meta charset=koi8-r>">', b"<a title='>' >", b"<a x=\">\"><meta charset=koi8-r>", b"<script>x</script>"])
    if r < 0.5:
        return rng.choice([b"<?php echo 1 ?>", b"<?x>", b"<!bogus>", b"</x y=z>", b"</>", b"</ x>", b"<", b"<<", b"<!", b"</", b"<?"])
    if r < 0.58:
        return rng.choice([b"<metax charset=koi8-r>", b"<meta", b"<meta charset=koi8-r", b"<meta/", b"<meta\tcharset=iso-8859-2>", b"<meta>"])
    if r < 0.66:
        return rng.choice([b"\xe3\x81\x82", b"\xff\xfe", b"\x00", b"\x80\x81", b"caf\xc3\xa9", b"\xef\xbb\xbf"])
    return rng.choice([b" ", b"\n", b"text", b"  ", b"\r\n", b"x" * rng.randrange(1, 40)])


def make(rng):
    """(document bytes, tags)."""
    tags = set()
    doc = b""
    r = rng.random()
    if r < 0.12:
        name = rng.choice(sorted(BOMS))
        tags.add(name)
        doc += BOMS[name]
    elif r < 0.2:
        lab = label_bytes(rng, tags)
        form = rng.choice([b'<?xml version="1.0" encoding="%s"?>', b"<?xml encoding='%s'?>", b'<?xml version="1.0" encoding = "%s" standalone="yes"?>',
                           b'<?xml version="1.0" encoding=%s?>', b'<?xml  encoding="%s "?>', b'<?XML encoding="%s"?>', b'<?xml version="1.0"?><x encoding="%s">'])
        doc += form % lab
        tags.add("xml-declaration")
    elif r < 0.23:
        doc += rng.choice([b"<\x00?\x00x\x00m\x00l\x00", b"\x00<\x00?\x00x\x00m\x00l", b"<\x00?\x00y\x00", b"<\x00?\x00x"])
        tags.add("utf16-xml-prescan")
    n = rng.choice([0, 0, 1, 2, 3, 6])
    for _ in range(n):
        doc += filler(rng, tags)
    metas = rng.choice([0, 1, 1, 1, 1, 2, 3])
    for i in range(metas):
        doc += meta(rng, tags)
        if rng.random() < 0.5:
            doc += filler(rng, tags)
    # the edge of the window: pad so that the end of a meta (or of the document) falls near byte 1024
    if rng.random() < 0.25:
        m = meta(rng, tags)
        target = rng.choice([1021, 1022, 1023, 1024, 1025, 1026, 1030, 1100])
        pad = target - len(doc) - len(m)
        if pad > 0:
            if pad >= 7 and rng.random() < 0.4:
                doc += b"<!--" + b" " * (pad - 7) + b"-->"
            else:
                doc += b" " * pad
        doc += m
        tags.add("at-the-edge")
    if rng.random() < 0.4:
        for _ in range(rng.choice([1, 2, 4])):
            doc += filler(rng, tags)
    if rng.random() < 0.08:
        i = rng.randrange(len(doc) + 1)
        doc = doc[:i] + bytes([rng.choice([0x80, 0xff, 0x00, 0x0d, 0x3c, 0x3e, 0x22, 0x27, 0x3d])]) + doc[i:]
        tags.add("stray-byte")
    return doc, tags


def options(rng, tags):
    """(transport label bytes or None, certain, default label bytes or None, ignore_meta)."""
    r = rng.random()
    transport, certain = None, True
    if r < 0.12:
        transport = label_bytes(rng, tags)
        tags.add("transport")
    elif r < 0.2:
        transport = label_bytes(rng, tags)
        certain = False
        tags.add("transport-tentative")
    default = None
    if rng.random() < 0.3:
        default = label_bytes(rng, tags, allow_invalid=False)
        tags.add("default")
    ignore = rng.random() < 0.05
    if ignore:
        tags.add("ignore-meta")
    return transport, certain, default, ignore


def population(count, seed):
    rng = random.Random(seed)
    out = []
    for _ in range(count):
        doc, tags = make(rng)
        transport, certain, default, ignore = options(rng, tags)
        out.append((doc, transport, certain, default, ignore, frozenset(tags)))
    return out
