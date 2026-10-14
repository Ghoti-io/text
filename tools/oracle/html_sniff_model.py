"""A second reading of the HTML encoding sniffing algorithm, for tools/oracle/html_sniff_diff.py.

This is the standard's section 13.2.3.2 ("Determining the character encoding" and its prescan, get-an-attribute and
get-an-XML-encoding steps, and the extraction of a charset from a `content` value in the fetch standard) written out again in
Python, from the text and not from the C that this library runs. It exists for one purpose: to say *why* this library and html5lib
disagree. The library's C and html5lib are two readers of the standard that differ in places; this is a third reader whose switches
(`QUIRKS`) each name one place where html5lib is known to read the standard differently. With every switch off the model must give the
library's answer on every document (else one of the two is wrong, and the run fails); with a set of switches on it must give
html5lib's (else the divergence is unexplained, and the run fails). A divergence is explained by the smallest set of switches that
reproduces html5lib's answer. Nothing here looks at an answer to decide what a document "has": the switches are behaviours of an
algorithm, not tags on an outcome.

Copyright 2026 by Corey Pennycuff
"""

WS = b"\t\n\x0c\r "
WS_SLASH = WS + b"/"
LETTERS = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

# One switch per place where html5lib (1.1, `useChardet` off) reads the algorithm differently. The text of each is in
# html_sniff_divergences.txt with the sentence of the standard it departs from and a document that shows it.
QUIRKS = [
    "bom32",      # a four-byte UTF-32 byte order mark is recognised before UTF-16 is; with no UTF-32 to give, nothing is returned but the four bytes are
                  # passed over, so the prescan begins after them
    "noxml",      # no XML-declaration fallback when the prescan finds nothing
    "noutf16",    # no prescan for a UTF-16 "<?x"
    "dflt",       # an unusable caller default becomes windows-1252 rather than UTF-8
    "xud",        # a declared x-user-defined stays x-user-defined (the standard says windows-1252)
    "slash",      # "<meta" must be followed by white space (the standard also accepts "/")
    "comment",    # a comment's "-->" is looked for after the "<!--" (the standard lets the dashes of "<!--" count)
    "lt",         # "<" ends a tag name and an unquoted attribute value
    "metax",      # "<meta" followed by anything else drops the next byte from the scan instead of reading a tag
    "skip1",      # "<" and a byte that starts nothing: the byte is dropped from the scan too
    "endskip",    # "</" drops the byte after it before asking whether a tag name follows
    "dups",       # a repeated attribute is not ignored (it replaces the pragma flag, a charset the first one failed to supply, or the one a content had)
    "cic",        # a charset attribute that names nothing is skipped, and the tag goes on (the standard abandons the tag)
    "imm",        # the decision is taken as soon as an attribute makes it, not at the end of the tag
    "content1",   # in a content value only the first "charset" is looked at, and an unquoted value ends only at white space
]


class Out(Exception):
    """The prescan ran out of bytes (or reached its end condition)."""


def lower(c):
    return c + 32 if 0x41 <= c <= 0x5A else c


def extract_charset(value, label, q):
    """The fetch standard's "extract a character encoding from a meta element" over `value`. Returns an encoding name or None."""
    s = value
    lows = s.lower()
    pos = 0
    n = len(s)
    while True:
        i = lows.find(b"charset", pos)
        if i < 0:
            return None
        j = i + 7
        while j < n and s[j] in WS:
            j += 1
        if j >= n or s[j] != 0x3D:
            if "content1" in q:
                return None
            pos = j  # (just before that next character)
            if pos <= i:
                pos = i + 1
            continue
        j += 1
        while j < n and s[j] in WS:
            j += 1
        if j >= n:
            return None
        c = s[j]
        if c in b"\"'":
            k = s.find(bytes([c]), j + 1)
            if k < 0:
                return None
            return label(s[j + 1:k])
        k = j
        stops = WS if "content1" in q else WS + b";"
        while k < n and s[k] not in stops:
            k += 1
        return label(s[j:k])


def get_attribute(d, p, q):
    """(name, value, new position); name None when there is no attribute. Raises Out."""
    n = len(d)
    while True:
        if p >= n:
            raise Out
        if d[p] in WS_SLASH:
            p += 1
            continue
        break
    if d[p] == 0x3E:
        return None, None, p
    name = bytearray()
    while True:
        if p >= n:
            raise Out
        c = d[p]
        if c == 0x3D and name:
            p += 1
            break
        if c in WS:
            while True:  # spaces
                p += 1
                if p >= n:
                    raise Out
                if d[p] not in WS:
                    break
            if d[p] != 0x3D:
                return bytes(name), b"", p
            p += 1
            break
        if c in b"/>":
            return bytes(name), b"", p
        name.append(lower(c))
        p += 1
    # value
    while True:
        if p >= n:
            raise Out
        if d[p] in WS:
            p += 1
            continue
        break
    c = d[p]
    value = bytearray()
    if c in b"\"'":
        while True:
            p += 1
            if p >= n:
                raise Out
            if d[p] == c:
                return bytes(name), bytes(value), p + 1
            value.append(lower(d[p]))
    if c == 0x3E:
        return bytes(name), b"", p
    value.append(lower(c))
    p += 1
    stops = WS + b">" + (b"<" if "lt" in q else b"")
    while True:
        if p >= n:
            raise Out
        c = d[p]
        if c in stops:
            return bytes(name), bytes(value), p
        value.append(lower(c))
        p += 1


def meta_tag(d, p, label, q):
    """From the separator after "<meta": the encoding the tag declares (a name) or None, and the position after it."""
    attrs = set()
    got_pragma = False
    need = None  # None | True | False
    charset = None  # None = null, "" = failure, else a name
    FAIL = ""
    while True:
        name, value, p = get_attribute(d, p, q)
        if name is None:
            break
        if name in attrs and "dups" not in q:
            continue
        attrs.add(name)
        if name == b"http-equiv":
            if "dups" in q:
                got_pragma = value == b"content-type"
            elif value == b"content-type":
                got_pragma = True
        elif name == b"content":
            e = extract_charset(value, label, q)
            if e is not None and (charset is None or ("dups" in q and need)):
                charset = e
                need = True
        elif name == b"charset":
            e = label(value)
            if e is None and "cic" in q:
                pass
            else:
                charset = e if e is not None else FAIL
                need = False
        if "imm" in q and decided(need, got_pragma, charset):
            return charset, p
    if decided(need, got_pragma, charset):
        return charset, p
    return None, p


def decided(need, got_pragma, charset):
    if need is None:
        return False
    if need and not got_pragma:
        return False
    return charset not in (None, "")


def prescan(data, label, q, window=1024):
    """The prescan over the first `window` bytes. Returns a name, or None for "aborted" (ran out of bytes)."""
    d = data[:window]
    n = len(d)
    if "noutf16" not in q:
        if d[:6] == b"<\x00?\x00x\x00":
            return "utf-16le"
        if d[:6] == b"\x00<\x00?\x00x":
            return "utf-16be"
    p = 0
    try:
        while p < n:
            if d[p] != 0x3C:
                p += 1
                continue
            if d.startswith(b"<!--", p):
                start = p + 6 if "comment" in q else p + 3
                j = start
                while True:
                    j = d.find(b">", j)
                    if j < 0:
                        raise Out
                    if d[j - 2:j] == b"--" and j - 2 >= p + 2:
                        break
                    j += 1
                p = j + 1
                continue
            if d[p + 1:p + 5].lower() == b"meta" and p + 5 < n and d[p + 5] in (WS if "slash" in q else WS_SLASH):
                name, p = meta_tag(d, p + 5, label, q)
                if name is not None:
                    if name in ("utf-16be", "utf-16le"):
                        name = "utf-8"
                    if name == "x-user-defined" and "xud" not in q:
                        name = "windows-1252"
                    return name
                p += 1
                continue
            if "metax" in q and d[p + 1:p + 5].lower() == b"meta":
                p += 6
                continue
            k = p + 1
            if k < n and d[k] == 0x2F:
                k += 2 if "endskip" in q else 1
            if k < n and d[k] in LETTERS:
                stops = WS + b">" + (b"<" if "lt" in q else b"")
                while True:
                    if k >= n:
                        raise Out
                    if d[k] in stops:
                        break
                    k += 1
                if d[k] == 0x3C:
                    p = k
                    continue
                p = k
                while True:
                    name, _v, p = get_attribute(d, p, q)
                    if name is None:
                        break
                p += 1
                continue
            if d[p + 1:p + 2] in (b"!", b"/", b"?"):
                j = d.find(b">", p)
                if j < 0:
                    raise Out
                p = j + 1
                continue
            if "skip1" in q and d[p + 1:p + 2] not in (b"!", b"/", b"?"):
                p += 2
                continue
            p += 1
        return None
    except Out:
        return None


def xml_encoding(d, label):
    if not d.startswith(b"<?xml"):
        return None
    end = d.find(b">")
    if end < 0:
        return None
    i = d.find(b"encoding", 0, end)
    if i < 0:
        return None
    i += 8
    n = len(d)
    while i < n and d[i] <= 0x20:
        i += 1
    if i >= n or d[i] != 0x3D:
        return None
    i += 1
    while i < n and d[i] <= 0x20:
        i += 1
    if i >= n or d[i] not in b"\"'":
        return None
    qm = d[i]
    i += 1
    j = d.find(bytes([qm]), i)
    if j < 0:
        return None
    pot = d[i:j]
    if any(b <= 0x20 for b in pot):
        return None
    e = label(pot)
    if e in ("utf-16be", "utf-16le"):
        return "utf-8"
    return e


def bom(d, q):
    if d[:3] == b"\xef\xbb\xbf":
        return "utf-8"
    if "bom32" in q and (d[:4] == b"\xff\xfe\x00\x00" or d[:4] == b"\x00\x00\xfe\xff"):
        return None
    if d[:2] == b"\xfe\xff":
        return "utf-16be"
    if d[:2] == b"\xff\xfe":
        return "utf-16le"
    return None


def sniff(doc, transport, certain, default, ignore, q, label, window=1024):
    """The encoding name the algorithm ends with. `label(bytes)` -> name or None is the label table in force."""
    b = bom(doc, q)
    if b is not None:
        return b
    if "bom32" in q and (doc[:4] == b"\xff\xfe\x00\x00" or doc[:4] == b"\x00\x00\xfe\xff"):
        doc = doc[4:]  # (the mark was read, and is not an encoding; the stream goes on after it)
    t = label(transport) if transport is not None else None
    if t is not None and certain:
        return t
    if not ignore:
        r = prescan(doc, label, q, window)
        if r is None and "noxml" not in q:
            r = xml_encoding(doc[:window], label)
        if r is not None:
            return r
    if t is not None:
        return t
    if default is not None:
        e = label(default)
        if e is not None:
            return e
        return "windows-1252" if "dflt" in q else "utf-8"
    return "utf-8"
