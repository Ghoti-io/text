#!/usr/bin/env python3
"""html5lib for the whole-pipeline differential (tools/oracle/html_pipeline_diff.py).

    /opt/html-oracle/venv/bin/python3 pipeline.py        one JSON request per line on stdin, one JSON answer per line on stdout

Two operations, both answered with the tree in the tree-construction dump format (driver.py's, copied here: the harness asks the pinned
driver the same questions on a sample and requires the same answers, so the copy cannot drift), the number of parse errors and the
tree written by html5lib's serializer at the standard's settings (serialize.py's):

    {"op": "bytes", "hex": "<document bytes>", "transport": <label|null>, "likely": <label|null>, "default": <label|null>,
     "ignore_meta": <bool>, "scripting": <bool>}
        html5lib's own byte pipeline: HTMLParser.parse(bytes, useChardet=False, ...), a restart on a late `<meta>` included. The answer adds
        "enc" (documentEncoding) and "text", what its decoder (the codec of the encoding it chose, errors replaced) makes of the bytes.
    {"op": "string", "hex": "<UTF-8 of the text>", "context": <HTML element name|null>, "scripting": <bool>}
        html5lib's parser on a string, as a document or as a fragment in the context element.

An answer is {"dump", "errors", "ser", ...} or {"error": "<why>"}; a reference that cannot answer says so and the caller counts it.
Read from the tree on the bind mount that makes the tree visible in the container; the image is not rebuilt for it.

Copyright 2026 by Corey Pennycuff
"""

import io
import json
import sys

sys.setrecursionlimit(8000)

import html5lib
from html5lib import _inputstream, html5parser
from html5lib.serializer import HTMLSerializer

NS_HTML = "http://www.w3.org/1999/xhtml"
NS_SVG = "http://www.w3.org/2000/svg"
NS_MATH = "http://www.w3.org/1998/Math/MathML"
NS_XLINK = "http://www.w3.org/1999/xlink"
NS_XML = "http://www.w3.org/XML/1998/namespace"
NS_XMLNS = "http://www.w3.org/2000/xmlns/"
PREFIX = {NS_XLINK: "xlink", NS_XML: "xml", NS_XMLNS: "xmlns"}


def codepoints(s):
    return [ord(c) for c in s]


def dump_tree(root):
    lines = []
    walker = html5lib.getTreeWalker("dom")
    depth = 0
    text = []
    template_stack = []

    def emit(d, s):
        lines.append("| " + "  " * d + s)

    def flush():
        if text:
            emit(depth, '"' + "".join(text) + '"')
            del text[:]

    for tok in walker(root):
        kind = tok["type"]
        if kind in ("Characters", "SpaceCharacters"):
            text.append(tok["data"])
            continue
        flush()
        if kind == "Doctype":
            name, pub, sysid = tok["name"] or "", tok.get("publicId"), tok.get("systemId")
            if pub or sysid:
                emit(depth, '<!DOCTYPE %s "%s" "%s">' % (name, pub or "", sysid or ""))
            else:
                emit(depth, "<!DOCTYPE %s>" % name)
        elif kind == "Comment":
            emit(depth, "<!-- %s -->" % tok["data"])
        elif kind in ("StartTag", "EmptyTag"):
            ns, name = tok["namespace"], tok["name"]
            shown = name if ns == NS_HTML or ns is None else ("svg " if ns == NS_SVG else "math " if ns == NS_MATH else "? ") + name
            emit(depth, "<%s>" % shown)
            attrs = []
            for (ans, aname), value in tok["data"].items():
                if ans is None:
                    label = aname
                elif ans == NS_XMLNS and aname == "xmlns":
                    label = "xmlns"
                else:
                    label = PREFIX.get(ans, "?") + " " + aname
                attrs.append((label, value))
            attrs.sort(key=lambda a: a[0].encode("utf-16-be", "surrogatepass"))  # the dump format's order: UTF-16 code units (as a JavaScript sort has it)
            for label, value in attrs:
                emit(depth + 1, '%s="%s"' % (label, value))
            if kind == "StartTag":
                depth += 1
                if ns == NS_HTML and name == "template":
                    emit(depth, "content")
                    depth += 1
                    template_stack.append(True)
                else:
                    template_stack.append(False)
        elif kind == "EndTag":
            depth -= 1
            if template_stack.pop():
                depth -= 1
        elif kind == "Entity":
            pass
        else:
            raise ValueError("unknown token %r" % kind)
    flush()
    return "\n".join(lines)


def serialize_tree(root):
    walker = html5lib.getTreeWalker("dom")
    s = HTMLSerializer(quote_attr_values="always", quote_char='"', use_best_quote_char=False,
                       omit_optional_tags=False, minimize_boolean_attributes=False, use_trailing_solidus=False,
                       escape_lt_in_attrs=False, escape_rcdata=False, resolve_entities=True,
                       alphabetical_attributes=False, inject_meta_charset=False, strip_whitespace=False, sanitize=False)
    return s.render(walker(root))


PRESCAN = _inputstream.HTMLBinaryInputStream.detectEncodingMeta
CHANGE = _inputstream.HTMLBinaryInputStream.changeEncoding


def ask(req):
    scripting = bool(req.get("scripting"))
    parser = html5parser.HTMLParser(tree=html5lib.getTreeBuilder("dom"), namespaceHTMLElements=True)
    out = {}
    if req["op"] == "string":
        text = bytes.fromhex(req["hex"]).decode("utf-8")
        if req.get("context") is None:
            root = parser.parse(text, scripting=scripting)
        else:
            if len(req["context"].split(" ")) != 1:
                raise ValueError("html5lib takes a fragment context by HTML name only, not %r" % req["context"])
            root = parser.parseFragment(text, container=req["context"], scripting=scripting)
    else:
        data = bytes.fromhex(req["hex"])
        if req.get("ignore_meta"):
            _inputstream.HTMLBinaryInputStream.detectEncodingMeta = lambda self: None
            _inputstream.HTMLBinaryInputStream.changeEncoding = lambda self, new: None
        else:
            _inputstream.HTMLBinaryInputStream.detectEncodingMeta = PRESCAN
            _inputstream.HTMLBinaryInputStream.changeEncoding = CHANGE
        kwargs = {"transport_encoding": req.get("transport"), "likely_encoding": req.get("likely"),
                  "default_encoding": req.get("default") or "windows-1252", "useChardet": False}
        root = parser.parse(data, scripting=scripting, **kwargs)
        out["enc"] = parser.documentEncoding
        # What its decoder makes of the bytes under the encoding it ended with: the input stream on its own gives the length of a byte
        # order mark (where it begins reading) and the encoding it began with; the codec of the one the parse ended with reads the rest.
        import webencodings
        stream = _inputstream.HTMLBinaryInputStream(data, **kwargs)
        out["first_enc"] = stream.charEncoding[0].name
        bom = stream.rawStream.tell()
        final = webencodings.lookup(parser.documentEncoding) if parser.documentEncoding else stream.charEncoding[0]
        out["text"] = final.codec_info.decode(data[bom:], "replace")[0].encode("utf-8", "surrogatepass").hex()
        # ... and what html5lib's own stream (a codecs StreamReader over the same bytes) reads, when it began in the encoding it ended in
        out["stream_text"] = None
        if final.name == stream.charEncoding[0].name:
            out["stream_text"] = stream.dataStream.read().encode("utf-8", "surrogatepass").hex()
    out["dump"] = dump_tree(root)
    out["errors"] = len(parser.errors)
    out["ser"] = serialize_tree(root)
    return out


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            answer = ask(json.loads(line))
        except Exception as e:  # noqa: BLE001 - a reference that cannot answer says so
            answer = {"error": "%s: %s" % (type(e).__name__, e)}
        sys.stdout.write(json.dumps(answer, ensure_ascii=True) + "\n")
    sys.stdout.flush()


if __name__ == "__main__":
    main()
