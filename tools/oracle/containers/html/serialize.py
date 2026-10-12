#!/usr/bin/env python3
"""html5lib's serializer, asked about a tree (the serializer differential, tools/oracle/html_serialize_diff.py).

    serialize.py                 read requests on stdin, answer on stdout

Each request is a line of JSON, {"tree": <node>, "mode": "children" | "outer", "scripting": <bool>}, and each answer
a line of JSON, {"out": "<html>"} or {"error": "<why>"}. The tree is the one the differential built (see
html_serialize_diff.py): nodes {"k": "doc" | "frag", "c": [...]}, {"k": "e", "ns", "p", "n", "a": [{"ns", "p", "n", "v"}],
"c": [...], "t": [...] or null}, {"k": "t" | "k" | "c", "d"}, {"k": "p", "t", "d"}, {"k": "d", "n", "pub", "sys"}. With
"children" the root's children are written; with "outer" the root's first child itself.

html5lib has no tree of its own that this could be given (its serializer takes a stream of tokens), so the tree is
turned into tokens the way html5lib's tree walkers turn one: an element that html5lib calls void (in the HTML
namespace) is one EmptyTag and its children are not visited, any other element a StartTag, its children and an EndTag, and
text a Characters token. A template's contents are written in place of its children, as the standard does (html5lib has
no such thing, and would otherwise have nothing to write). Processing instructions are not written (html5lib's serializer
has no token for one). The serializer is run with the standard's settings, every option that html5lib has that the
standard does not at its off: attribute values always quoted with the double quote, no omitted tags, no minimized
boolean attributes, no solidus, no meta charset injected, white space left alone.

Not baked into the image: the image (containers/html/Dockerfile) holds html5lib and parse5 and the two drivers that
answer parse requests, and this file is read from the tree, on the bind mount that makes the tree visible to the
reference at the same path.

Copyright 2026 by Corey Pennycuff
"""

import json
import sys

sys.setrecursionlimit(6000)  # the tree is written by recursion, and a deep one is part of the population

from html5lib import constants
from html5lib.serializer import HTMLSerializer

NS_HTML = "http://www.w3.org/1999/xhtml"
NS_SVG = "http://www.w3.org/2000/svg"
NS_MATH = "http://www.w3.org/1998/Math/MathML"


def tokens(nodes):
    for node in nodes:
        k = node["k"]
        if k == "e":
            # The name a DOM gives the element: the local name in the three namespaces the standard names, the
            # qualified name in any other (the algorithm's rule is a statement about those names).
            name = node["n"] if node["ns"] in (NS_HTML, NS_SVG, NS_MATH) else (
                node["p"] + ":" + node["n"] if node["p"] else node["n"])
            data = {}
            for a in node["a"]:
                data[(a["ns"], a["n"])] = a["v"]
            if node["ns"] == NS_HTML and node["n"] in constants.voidElements:
                yield {"type": "EmptyTag", "name": name, "data": data}
                continue
            yield {"type": "StartTag", "name": name, "namespace": node["ns"], "data": data}
            inner = node["t"] if (node["ns"] == NS_HTML and node["n"] == "template" and node["t"] is not None) else node["c"]
            if node["ns"] == NS_HTML and node["n"] == "template" and node["t"] is None:
                inner = []
            yield from tokens(inner)
            yield {"type": "EndTag", "name": name, "namespace": node["ns"]}
        elif k in ("t", "k"):
            if node["d"]:
                yield {"type": "Characters", "data": node["d"]}
        elif k == "c":
            yield {"type": "Comment", "data": node["d"]}
        elif k == "d":
            yield {"type": "Doctype", "name": node["n"], "publicId": node["pub"], "systemId": node["sys"]}
        # a processing instruction: no token


def serialize(request):
    tree = request["tree"]
    nodes = tree["c"][:1] if request["mode"] == "outer" else tree["c"]
    s = HTMLSerializer(quote_attr_values="always", quote_char='"', use_best_quote_char=False,
                       omit_optional_tags=False, minimize_boolean_attributes=False, use_trailing_solidus=False,
                       escape_lt_in_attrs=False, escape_rcdata=False, resolve_entities=True,
                       alphabetical_attributes=False, inject_meta_charset=False, strip_whitespace=False,
                       sanitize=False)
    return s.render(iter(list(tokens(nodes))))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--version":
        import html5lib
        print("html5lib %s serialize" % html5lib.__version__)
        return 0
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            answer = {"out": serialize(json.loads(line))}
        except Exception as e:  # noqa: BLE001 - a reference that cannot answer says so, the caller counts it
            answer = {"error": "%s: %s" % (type(e).__name__, e)}
        sys.stdout.write(json.dumps(answer, ensure_ascii=True) + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
