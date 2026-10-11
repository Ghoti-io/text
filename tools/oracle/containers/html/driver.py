#!/usr/bin/env python3
"""The HTML reference drivers: html5lib (here) and parse5 (driver.mjs).

    html-driver --version        one line naming both references and the driver
    html-driver html5lib         read requests on stdin, answer on stdout
    html-driver parse5           the same, answered by parse5

**One format for both references.** Each request is a line of JSON

    {"html": "<text>", "scripting": false, "context": null}

(`context` is null for a document, or the fragment parsing algorithm's context
element: `td`, `svg path`, `math mi` - a namespace word and a name, or a bare
HTML name) and each answer is a line of JSON, either

    {"dump": "<tree>", "errors": <parse error count>}
    {"error": "<why the reference could not answer>"}

where the tree is in the html5lib-tests / web-platform-tests tree-construction
format, byte for byte: `| ` then two spaces of indent per level, `<tag>` (with an
`svg ` or `math ` namespace word for foreign elements), attributes as
`name="value"` sorted by name (namespaced ones as `xlink href`), `"text"`,
`<!-- comment -->`, `<!DOCTYPE name "public" "system">`, and `content` under an
HTML `template`. Both references are asked through this one serialization, so a
difference between their answers is a difference between the parsers and not
between two dump writers. No newline is added after the last line; the caller
joins with LF.

This file is the html5lib half; the Dockerfile copies both halves and the
version line carries the SHA-256 of the pair, so that an image built from an
older copy of either answers under a name that no longer matches the pin.

Copyright 2026 by Corey Pennycuff
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

NS_HTML = "http://www.w3.org/1999/xhtml"
NS_SVG = "http://www.w3.org/2000/svg"
NS_MATH = "http://www.w3.org/1998/Math/MathML"
NS_XLINK = "http://www.w3.org/1999/xlink"
NS_XML = "http://www.w3.org/XML/1998/namespace"
NS_XMLNS = "http://www.w3.org/2000/xmlns/"
PREFIX = {NS_XLINK: "xlink", NS_XML: "xml", NS_XMLNS: "xmlns"}


def version_line():
    import html5lib
    node = subprocess.run(["node", "--version"], capture_output=True, text=True).stdout.strip()
    # parse5 is an ES module: its version is read from its package.json.
    with open("/opt/html-oracle/node_modules/parse5/package.json", encoding="utf-8") as fh:
        p5 = json.load(fh)["version"]
    with open("/opt/html-oracle/DRIVER_SHA", encoding="utf-8") as fh:
        sha = fh.read().strip()
    with open("/opt/html-oracle/PACKAGES", encoding="utf-8") as fh:
        pkgs = fh.read().strip()
    return "html5lib %s, parse5 %s, %s, node %s, driver %s" % (html5lib.__version__, p5, pkgs, node, sha)


def codepoints(s):
    return [ord(c) for c in s]


def ask(request):
    import html5lib
    html = request["html"]
    scripting = bool(request.get("scripting"))
    context = request.get("context")
    parser = html5lib.HTMLParser(tree=html5lib.getTreeBuilder("dom"), namespaceHTMLElements=True)
    if context is not None:
        words = context.split(" ")
        if len(words) != 1:
            raise ValueError("html5lib takes a fragment context by HTML name only, not %r" % context)
        root = parser.parseFragment(html, container=words[0], scripting=scripting)
    else:
        root = parser.parse(html, scripting=scripting)
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
            attrs.sort(key=lambda a: codepoints(a[0]))
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
    return {"dump": "\n".join(lines), "errors": len(parser.errors)}


def serve():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            out = ask(json.loads(line))
        except Exception as why:  # a crash is an answer, not a missing one
            out = {"error": "%s: %s" % (type(why).__name__, why)}
        sys.stdout.write(json.dumps(out, ensure_ascii=True) + "\n")
    sys.stdout.flush()


def main(argv):
    if len(argv) == 2 and argv[1] == "--version":
        print(version_line())
        return 0
    if len(argv) == 2 and argv[1] == "html5lib":
        serve()
        return 0
    if len(argv) == 2 and argv[1] == "parse5":
        os.execvp("node", ["node", os.path.join(HERE, "driver.mjs")])
    sys.stderr.write("usage: html-driver --version | html5lib | parse5\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
