#!/usr/bin/env python3
"""Classify a disagreement from evidence, for `xml_diff.py --propose`.

A class on a disagreement is a claim ("the disagreement is the DTD", "expat's
tables are the Fourth Edition's"), and a claim made by a regular expression over
the input is a guess. These rules do not guess: each one asks for **evidence that
is itself a measurement**, taken from the programs that disagreed.

  scope-dtd         a *control*: the same document with its document type
                    declaration removed is put to all three programs, and they
                    agree. The disagreement is then the declaration and nothing
                    else in the document.
  reference-scope   a control: the same document converted to UTF-8 (with the
                    encoding label removed) is put to all three, and they agree.
                    The disagreement is then the encoding and nothing else.
  version-specific  expat refuses with "invalid token" at a byte that is not
                    ASCII, in a document libxml2 and this parser both accept. The
                    offending character is one of the Fifth Edition's, which
                    expat's Fourth Edition tables do not have.
  scope-unsupported
  reference-wrong   a control again: the same document with the one thing the specification
                    forbids put right (the declared version made `1.0`; the bytes after a NUL
                    cut off, which libxml2 does by itself) is agreed on, or the encoding
                    label contradicts the bytes and the reference accepts it.
  spec-ambiguity    UTF-16 or UTF-32 with no mark and no `<?` to start, which expat guesses
                    and 4.3.3 does not allow; the same document as UTF-8 is agreed on.
  representation    libxml2 reports the same information differently (an `xml` prefix
                    declaration it drops, a public identifier it does not normalize, `&#38;`
                    left in a namespace name): the dump agrees once each is undone.

A document can have two causes. When removing its DTD leaves a disagreement, the document
without the DTD is classified in its own right and that is the answer (`nodtd-residue`).

A disagreement no rule explains is returned as `unclassified`. Those are the ones
to read: each is either a defect of this parser or a rule not yet written.

Copyright 2026 by Corey Pennycuff
"""

import re

# Expat's error codes as numbered in the pinned 2.8.3 (containers/IMAGES); the driver prints the
# number and then the message. They are version-specific: a new expat may renumber them, and the
# pin check is what says so. 4 is XML_ERROR_INVALID_TOKEN, 19 is XML_ERROR_INCORRECT_ENCODING.
EXPAT_INVALID_TOKEN = "4 "
EXPAT_INCORRECT_ENCODING = "19 "

SUPPORTED_LABELS = (b"utf-8", b"utf-16", b"utf-32", b"iso-8859-1", b"us-ascii")


def decode_text(data, loose=False):
    """(text, family) for a document in an encoding this parser reads, or None.

    The family is what Appendix F's byte-order sniffing says; a document with no
    mark and no `<?` pattern is UTF-8. `loose` also guesses UTF-16 and UTF-32
    from a NUL beside the first character, which is what expat does and what
    Appendix F does not ask of anyone: it is for building a control only.
    """
    boms = ((b"\xff\xfe\x00\x00", "utf-32-le", 4), (b"\x00\x00\xfe\xff", "utf-32-be", 4),
            (b"\xff\xfe", "utf-16-le", 2), (b"\xfe\xff", "utf-16-be", 2),
            (b"\xef\xbb\xbf", "utf-8", 3))
    for bom, codec, n in boms:
        if data.startswith(bom):
            try:
                return data[n:].decode(codec), codec
            except UnicodeDecodeError:
                return None
    sniff = ((b"\x00\x00\x00<", "utf-32-be"), (b"<\x00\x00\x00", "utf-32-le"),
             (b"\x00<\x00?", "utf-16-be"), (b"<\x00?\x00", "utf-16-le"))
    for pat, codec in sniff:
        if data.startswith(pat):
            try:
                return data.decode(codec), codec
            except UnicodeDecodeError:
                return None
    if loose and len(data) >= 4:
        if data[0] == 0 and data[1] == 0 and data[2] == 0 and data[3] != 0:
            codec = "utf-32-be"
        elif data[0] != 0 and data[1] == 0 and data[2] == 0 and data[3] == 0:
            codec = "utf-32-le"
        elif data[0] == 0 and data[1] != 0:
            codec = "utf-16-be"
        elif data[0] != 0 and data[1] == 0:
            codec = "utf-16-le"
        else:
            codec = None
        if codec:
            try:
                return data.decode(codec), codec
            except UnicodeDecodeError:
                return None
    try:
        return data.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        m = re.match(rb"<\?xml[^>]*encoding=[\"'](iso-8859-1|us-ascii)[\"']", data, re.I)
        if m:
            return data.decode("latin-1"), "latin-1"
        return None


DECL = re.compile(r"^<\?xml\s+version\s*=\s*([\"'])(.*?)\1")
LABEL = re.compile(r"(<\?xml[^>]*?)\s+encoding\s*=\s*([\"'])[^\"']*\2")


def has_mark_or_decl_pattern(data):
    """Whether Appendix F's detection identifies this document's encoding."""
    return (data[:4] in (b"\xff\xfe\x00\x00", b"\x00\x00\xfe\xff", b"\x00\x00\x00<", b"<\x00\x00\x00")
            or data[:2] in (b"\xff\xfe", b"\xfe\xff") or data[:4] in (b"\x00<\x00?", b"<\x00?\x00"))


def declared_version(text):
    m = DECL.match(text)
    return m.group(2) if m else None


def declared_label(text):
    m = re.match(r"<\?xml[^>]*?\s+encoding\s*=\s*([\"'])([^\"']*)\1", text)
    return m.group(2) if m else None


def utf8_control(data):
    """The document in UTF-8 with its encoding label removed, or None when that
    would be the same document (plain UTF-8 with no label) or cannot be made."""
    got = decode_text(data, loose=True)
    if got is None:
        return None
    text, codec = got
    stripped = LABEL.sub(r"\1", text)
    out = stripped.encode("utf-8", "surrogatepass")
    if out == data:
        return None
    return out


def strip_doctype(text):
    """The text without its document type declaration, or None if it has none.

    Finds the end by the same rule the declaration's grammar has: the first `>`
    outside a quoted literal and outside a bracketed internal subset, where a
    quote inside a comment or instruction in the subset does not count either.
    Good enough for a *control*: if it is wrong, the control fails to agree and
    the disagreement stays unclassified, which is the safe direction.
    """
    start = text.find("<!DOCTYPE")
    if start < 0:
        return None
    i = start + len("<!DOCTYPE")
    depth = 0
    quote = None
    while i < len(text):
        c = text[i]
        if quote:
            if c == quote:
                quote = None
        elif depth:
            if text.startswith("<!--", i):
                j = text.find("-->", i + 4)
                if j < 0:
                    return None
                i = j + 2
            elif text.startswith("<?", i):
                j = text.find("?>", i + 2)
                if j < 0:
                    return None
                i = j + 1
            elif c in "\"'":
                quote = c
            elif c == "]":
                depth = 0
        else:
            if c in "\"'":
                quote = c
            elif c == "[":
                depth = 1
            elif c == ">":
                return text[:start] + text[i + 1:]
        i += 1
    return None


def nodtd_control(data):
    got = decode_text(data)
    if got is None:
        return None
    text, codec = got
    out = strip_doctype(text)
    if out is None:
        return None
    if codec == "latin-1":
        return out.encode("latin-1", "replace")
    return out.encode("utf-8", "surrogatepass")


def truncnul_control(data):
    """The document cut at its first NUL character and converted to UTF-8 with its
    label removed, or None if it has none. In UTF-16 and UTF-32 the NUL is a
    character (U+0000), not a byte, so the document is decoded first."""
    got = decode_text(data, loose=True)
    if got is None or "\x00" not in got[0]:
        return None
    text = LABEL.sub(r"\1", got[0].split("\x00", 1)[0])
    return text.encode("utf-8", "surrogatepass")


def fixversion_control(data):
    """The document, as UTF-8 with its encoding label removed, with the declared
    version replaced by 1.0; None when the declaration is absent or the version is
    already a VersionNum."""
    got = decode_text(data, loose=True)
    if got is None:
        return None
    text = LABEL.sub(r"\1", got[0])
    m = DECL.match(text)
    if not m or re.fullmatch(r"1\.[0-9]+", m.group(2)):
        return None
    text = text[:m.start(2)] + "1.0" + text[m.end(2):]
    return text.encode("utf-8", "surrogatepass")


def doctypespace_control(data):
    """The document, as UTF-8 with its label removed, with the white space after `<!DOCTYPE`
    that production [28] requires put in; None if it is there or there is no declaration."""
    got = decode_text(data, loose=True)
    if got is None:
        return None
    text = LABEL.sub(r"\1", got[0])
    m = re.search(r"<!DOCTYPE(?=[^ \t\r\n])", text)
    if not m:
        return None
    return (text[:m.end()] + " " + text[m.end():]).encode("utf-8", "surrogatepass")


def make_controls(data):
    controls = {}
    c = doctypespace_control(data)
    if c is not None:
        controls["doctypespace"] = c
    c = truncnul_control(data)
    if c is not None:
        controls["truncnul"] = c
    c = fixversion_control(data)
    if c is not None:
        controls["fixversion"] = c
    c = nodtd_control(data)
    if c is not None:
        controls["nodtd"] = c
    c = utf8_control(data)
    if c is not None:
        controls["utf8"] = c
    return controls


XMLNS_XML = b"http://www.w3.org/XML/1998/namespace"


def dump_difference(ours, ref, normalize):
    """(class, reason) if libxml2's dump differs from ours in one known way only.

    "Only" is checked: the transformation is applied to both lists and they must
    then be equal, so a document that has this difference *and another* stays
    unclassified.
    """
    o = normalize(ours["dump"])
    x = normalize(ref["xml2"]["dump"])

    def drop_xml_decl(events):
        return [e for e in events if not (e[0] == "N" and e[1] == b"xml" and e[2] == XMLNS_XML)]

    def collapse_public(events):
        out = []
        for e in events:
            if e[0] == "D" and e[2] is not None:
                e = (e[0], e[1], b" ".join(e[2].split()), e[3])
            out.append(e)
        return out

    def unescape_amp(events):
        # A namespace name, wherever it is reported: in a declaration, on an element, on an attribute.
        out = []
        for e in events:
            if e[0] == "N" and e[2]:
                e = (e[0], e[1], e[2].replace(b"&#38;", b"&")) + e[3:]
            elif e[0] in ("S", "A") and e[1]:
                e = (e[0], e[1].replace(b"&#38;", b"&")) + e[2:]
            out.append(e)
        return out

    # Each known difference alone, then together: a document can have more than one.
    known = (("xml", drop_xml_decl, "libxml2 does not report a declaration of the reserved xml prefix; expat and this "
                                    "parser do"),
             ("public", collapse_public, "libxml2 keeps the white space of a public identifier as written; expat and this "
                                         "parser normalize it as XML 4.2.2 asks"),
             ("amp", unescape_amp, "libxml2 reports '&#38;' where a namespace name is '&'"))
    from itertools import combinations
    for size in (1, 2, 3):
        for chosen in combinations(known, size):
            def apply(events, chosen=chosen):
                for _name, fn, _why in chosen:
                    events = fn(events)
                return events
            if apply(o) == apply(x):
                cls = "reference-wrong" if any(n == "amp" for n, _f, _w in chosen) else "representation"
                return cls, "; ".join(why for _n, _f, why in chosen)
    return None


def xml11_line_end_difference(data, ours, ref, normalize):
    """Whether XML 1.1 line-end normalization alone explains both event diffs."""
    decoded = decode_text(data)
    if not decoded or declared_version(decoded[0]) != "1.1":
        return None
    ours_events = normalize(ours["dump"])

    def normalize_reference(events):
        # The reference normalizes CR to LF but leaves the following XML 1.1
        # separator visible. Collapse only the pairs that were CR+NEL or CR+LS
        # in the input; an ordinary LF followed by either character is two ends.
        remaining_pairs = {
            b"\n\xc2\x85": decoded[0].count("\r\u0085"),
            b"\n\xe2\x80\xa8": decoded[0].count("\r\u2028"),
        }
        out = []
        for event in events:
            fields = []
            for index, field in enumerate(event):
                if isinstance(field, bytes):
                    replacement = b" " if event[0] == "A" and index == 4 else b"\n"
                    for pair in remaining_pairs:
                        while remaining_pairs[pair] and pair in field:
                            field = field.replace(pair, replacement, 1)
                            remaining_pairs[pair] -= 1
                    field = field.replace(b"\xe2\x80\xa8", replacement).replace(b"\xc2\x85", replacement)
                fields.append(field)
            out.append(tuple(fields))
        return out

    for name in ("xml2", "expat"):
        if not ref[name].get("dump"):
            return None
        reference_events = normalize(ref[name]["dump"])
        if normalize_reference(reference_events) != ours_events:
            return None
    if not any(ch in decoded[0] for ch in ("\u0085", "\u2028")):
        return None
    return ("reference-wrong", "XML 1.1 section 2.11 requires literal NEL and LINE SEPARATOR line ends "
            "to normalize to LF; both references retain the literal character")


def classify(data, ours, ref, sig, detail, ctl, normalize, tags=()):
    """(class, reason) for a disagreement, or ("unclassified", detail).

    `ctl` maps a control's name to (agrees, ours_answer, ref_answers) for the
    controls that exist for this document.
    """
    status = (ours.get("status") or "").replace("GTEXT_XML_E_", "")
    got = decode_text(data)
    text = got[0] if got else None

    # xmlconf TYPE is normative for well-formedness. An independent reference
    # that disagrees with the selected class is evidence of a reference gap,
    # provided our own verdict matches the W3C expectation.
    expected_type = next((tag.split(":", 1)[1] for tag in tags if tag.startswith("type:")), None)
    if expected_type in ("not-wf", "valid", "invalid"):
        expected = "REFUSE" if expected_type == "not-wf" else "ACCEPT"
        if ours["verdict"] == expected:
            xml2 = "A" if ref["xml2"]["wf"] and ref["xml2"]["ns"] else "R"
            expat = "A" if ref["expat"]["ok"] else "R"
            if xml2 != expected[0] or expat != expected[0]:
                return "reference-wrong", ("pinned W3C xmlconf TYPE=%s expects %s under its XML/Namespaces "
                                            "Recommendation; the pinned references disagree" %
                                            (expected_type, expected.lower()))

    if text is not None and declared_version(text) == "1.1" and ours["verdict"] == "REFUSE":
        restricted = any((1 <= ord(c) <= 8) or 11 <= ord(c) <= 12 or 14 <= ord(c) <= 31
                         or 0x7f <= ord(c) <= 0x84 or 0x86 <= ord(c) <= 0x9f for c in text)
        xml2 = "A" if ref["xml2"]["wf"] and ref["xml2"]["ns"] else "R"
        expat = "A" if ref["expat"]["ok"] else "R"
        if restricted and xml2 == expat == "A":
            return "reference-wrong", ("XML 1.1 section 2.2 forbids RestrictedChar literals; the character must "
                                        "be written as a reference, but both pinned references accept it literally")

    if ours["verdict"] == "ACCEPT" and " dump=" in sig:
        found = xml11_line_end_difference(data, ours, ref, normalize)
        if found:
            return found

    if ours["verdict"] == "DECLINE":
        if status == "UNSUPPORTED_VERSION" and text is not None:
            v = declared_version(text) or ""
            if v == "1.1":
                return "unclassified", "XML 1.1 is a supported parser profile; this refusal must be investigated"
            if re.fullmatch(r"1\.[0-9]+", v):
                return "scope-unsupported", "declaration version %s is declined" % v
        if status == "UNSUPPORTED_ENCODING":
            m = re.match(rb"(?:\xef\xbb\xbf)?<\?xml[^>]*?\sencoding\s*=\s*[\"']([^\"']*)[\"']", data)
            label = m.group(1).decode("latin-1") if m else None
            if label is None:
                loose = decode_text(data, loose=True)
                label = declared_label(loose[0]) if loose else None
            if label is not None and label.lower().encode() not in SUPPORTED_LABELS:
                return "scope-unsupported", "the encoding label %r is declined: only UTF-8, UTF-16, UTF-32, ISO-8859-1 and US-ASCII are read" % label
        if status.startswith("LIMIT"):
            return "scope-limit", "a limit this parser enforces and the references do not"

    if status == "ENCODING_MISMATCH":
        c = ctl.get("utf8")
        if ref["xml2"]["wf"] and (ref["expat"]["ok"] or (ref["expat"]["error"] or "").startswith(EXPAT_INCORRECT_ENCODING)):
            return "reference-wrong", ("the encoding label contradicts the document's bytes, which XML 4.3.3 makes "
                                       "an error; libxml2 accepts it" + ("" if not ref["expat"]["ok"] else ", and so does expat")
                                       + (", where expat refuses it as this parser does" if not ref["expat"]["ok"] else ""))
        if c and c[0]:
            return "reference-wrong", ("the encoding label contradicts the document's bytes, which XML 4.3.3 makes "
                                       "an error; the same document without the label agrees with everyone")

    # Every rule below asks for two things: the control agrees (the cause is isolated) and
    # the reference-side evidence points the way the class says. A control alone is not
    # enough: a regression of this parser in UTF-16 decoding or in skipping a DTD makes the
    # UTF-8 or the no-DTD control agree too, and would be filed as a reference's quirk.
    ours_reads = ours["verdict"] == "ACCEPT"
    a_reference_refused = not (ref["xml2"]["wf"] and ref["xml2"]["ns"]) or not ref["expat"]["ok"]

    c = ctl.get("nodtd")
    # This parser skips a declaration's grammar and processes none of it, so the way it
    # can differ because of a DTD is: it accepts what a reference refuses, it refuses a
    # reference to an entity the DTD declares, it declines, or the tree differs (a type
    # normalization, a default). A refusal for any other reason is not the DTD's.
    dtd_direction = ((ours_reads and a_reference_refused) or status == "ENTITY"
                     or ours["verdict"] == "DECLINE" or "dump=" in sig)
    if c and c[0] and dtd_direction:
        return "scope-dtd", ("needs DTD processing: with the document type declaration removed all three "
                             "programs agree")

    c = ctl.get("utf8")
    if c and c[0] and status != "ENCODING_MISMATCH":
        guessed = decode_text(data, loose=True)
        if (guessed is not None and guessed[1] != "utf-8" and not has_mark_or_decl_pattern(data)
                and ours["verdict"] == "REFUSE" and ref["expat"]["ok"] and not ref["xml2"]["wf"]):
            return "spec-ambiguity", ("UTF-16 or UTF-32 with no byte order mark and no '<?' at the start: XML 4.3.3 "
                                      "requires the mark and Appendix F's detection does not cover it; expat guesses "
                                      "from a NUL beside the first character. The same document as UTF-8 agrees")
        if ours_reads and a_reference_refused:
            return "reference-scope", ("a reference does not read this encoding or its presentation: the same "
                                       "document as UTF-8, without the label, agrees")

    # Fifth Edition names that expat's Fourth Edition tables refuse. The byte index of
    # expat's error is into the bytes it was given, so for any encoding but UTF-8 the
    # UTF-8 control is the document to look at.
    seen = [(data, ours, ref)]
    if "utf8" in ctl:
        seen.append((ctl["utf8"][3], ctl["utf8"][1], ctl["utf8"][2]))
    for d, o, r in seen:
        if (o["verdict"] == "ACCEPT" and r["xml2"]["wf"] and r["xml2"]["ns"] and not r["expat"]["ok"]
                and r["expat"]["error"].startswith(EXPAT_INVALID_TOKEN) and decode_text(d) is not None
                and decode_text(d)[1] == "utf-8" and 0 <= r["expat"]["at"] < len(d) and d[r["expat"]["at"]] >= 0x80):
            return "version-specific", ("expat's name-character tables are XML 1.0 Fourth Edition's: it refuses a "
                                        "character that Fifth Edition makes a name character")

    c = ctl.get("fixversion")
    who = [n for n, ok in (("libxml2", ref["xml2"]["wf"] and ref["xml2"]["ns"]), ("expat", ref["expat"]["ok"])) if ok]
    if c and c[0] and ours["verdict"] == "REFUSE" and who:
        v = declared_version(LABEL.sub(r"\1", decode_text(data, loose=True)[0]))
        return "reference-wrong", ("the version %r is not a VersionNum (1.[0-9]+) and this parser refuses it; %s accept%s it. "
                                   "With the version replaced by 1.0 everyone agrees" % (v, " and ".join(who),
                                                                                         "s" if len(who) == 1 else ""))

    c = ctl.get("doctypespace")
    if c and c[0] and status == "DOCTYPE" and ref["xml2"]["wf"] and ref["xml2"]["ns"]:
        return "reference-wrong", ("a document type declaration needs white space after DOCTYPE (production 28) and "
                                   "this parser refuses its absence; libxml2 accepts it. With the space put in "
                                   "everyone agrees")

    c = ctl.get("truncnul")
    if c and ours["verdict"] == "REFUSE" and status == "CHARACTER" and ref["xml2"]["wf"] and ref["xml2"]["ns"]:
        # libxml2 answered as if the document ended at the NUL, and this parser agrees
        # with it about the document that does: the NUL is the whole difference.
        t_ours, t_ref = c[1], c[2]
        t_xml2 = "A" if t_ref["xml2"]["wf"] and t_ref["xml2"]["ns"] else "R"
        t_o = "A" if t_ours["verdict"] == "ACCEPT" else "R"
        if t_xml2 == "A" and t_o == "A":
            return "reference-wrong", ("a NUL is not an XML character; libxml2 treats one as the end of the input "
                                       "(the document cut at the NUL is accepted by libxml2 and by this parser)")

    if ours["verdict"] == "ACCEPT" and re.search(r" dump=xml2(#|$)", sig):
        found = dump_difference(ours, ref, normalize)
        if found:
            return found

    residue = ctl.get("nodtd-residue")
    if residue:
        return residue[0], "with the document type declaration removed: " + residue[1]

    return "unclassified", detail
