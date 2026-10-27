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
"""Read the document each computed-value page of web-platform-tests asks its question in, as data.

    tools/css/extract-wpt-computed-pages.py WPT_ROOT VECTORS_DIR OUT.json

A `test_computed_value("width", "10px", "10px")` is asked of `#target` in the page's document: its markup (`<div id="container"><div id="target">`), the
rules of its `<style>` (`#container { font-size: 40px }`), and whatever its scripts did first. This program reads the markup and the style sheets of every page
that has computed-value vectors (VECTORS_DIR/computed.json, made by extract-wpt-props.py), and decides whether the question can be put to this library, which has no
selector matching yet (C3):

* the document is read as the browser builds it: an HTML page (html5lib's tree is not needed for these pages: a tag soup reader that opens, closes and nests
  elements, the void elements being void, with `html` and `body` around the body's content) or an XML page (an SVG one);
* the page's style sheets are accepted when every rule is a style rule whose selectors are exactly `#id` (of an element of the page), `*`, `body`, `html`, `:root` or the
  name of an element in the page, and every declaration list is read as it is: the rules are matched here (their specificity is the selector's, their order the
  page's) and handed to the library as entries of the author origin;
* the page's scripts must not change the document before the vectors are asked: a script that makes an element, or that sets an attribute or a style outside of a
  `test()` or a function (which runs only when the page calls it, in a test of its own), makes the page one this cannot model;
* a page that has any other rule (an at-rule, a nested rule, a selector this does not match) or an element this does not model is not scored, and says why.

Output: {file: {"scorable": bool, "reason": text, "elements": [...], "target": index}} where an element is
{"ns": "h"|"s", "local", "id", "parent": index or -1, "attrs": {name: value}, "rules": [[a, b, c, order, declarations], ...]} in document order, parents first.

The reader never evaluates a script and never reads another implementation's code.
"""

import html.parser
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr", "basefont", "bgsound", "frame", "keygen"}
SVG_NS = "http://www.w3.org/2000/svg"
MUTATION = re.compile(r"createElement|appendChild|innerHTML|outerHTML|insertAdjacent|setAttribute|removeAttribute|removeChild|\.append\(|\.prepend\(|cloneNode|\.remove\(|"
                      r"\.style\.\w+\s*=[^=]|\.style\[[^\]]+\]\s*=[^=]|style\.setProperty|style\.cssText|\.style\s*=[^=]|\.className|\.classList|\.id\s*=[^=]|"
                      r"CSS\.registerProperty|insertRule|adoptedStyleSheets|\.sheet\b|\.textContent\s*=|\.innerText\s*=|document\.write|attachShadow|requestAnimationFrame|setTimeout|addEventListener")


class Node:
    def __init__(self, ns, local, attrs, parent):
        self.ns, self.local, self.attrs, self.parent = ns, local, attrs, parent
        self.children = []


class Reader(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node("h", "html", {}, None)
        self.body = Node("h", "body", {}, self.root)
        self.root.children.append(self.body)
        self.stack = [self.body]
        self.in_body = False
        self.skip = 0
        self.styles = []
        self.style_open = False
        self.problems = set()

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "template", "noscript"):
            self.skip += 1
            return
        if tag == "style":
            self.style_open = True
            self.styles.append("")
            return
        if tag == "body":
            self.in_body = True
            for k, v in attrs:
                self.body.attrs[k] = v or ""
            return
        if tag == "html":
            for k, v in attrs:
                self.root.attrs[k] = v or ""
            return
        if tag in ("head", "title", "meta", "link", "base"):
            return
        if not self.in_body and tag not in ("div", "p", "span"):
            self.in_body = True
        if self.skip:
            return
        if tag in ("svg", "math", "table", "iframe", "input", "select", "img", "video", "canvas", "object", "embed", "form", "textarea", "button"):
            self.problems.add("element <%s>" % tag)
        n = Node("h", tag, {k: (v or "") for k, v in attrs}, self.stack[-1])
        self.stack[-1].children.append(n)
        if tag not in VOID:
            self.stack.append(n)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        self.handle_endtag(tag)

    def handle_endtag(self, tag):
        if tag in ("script", "template", "noscript"):
            self.skip = max(0, self.skip - 1)
            return
        if tag == "style":
            self.style_open = False
            return
        if self.skip or tag in VOID:
            return
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].local == tag:
                del self.stack[i:]
                return

    def handle_data(self, data):
        if self.style_open:
            self.styles[-1] += data


def flatten_html(root):
    elements = []

    def walk(n, parent):
        idx = len(elements)
        elements.append({"ns": n.ns, "local": n.local, "id": n.attrs.get("id", ""), "parent": parent, "attrs": dict(n.attrs), "rules": []})
        for c in n.children:
            walk(c, idx)

    walk(root, -1)
    return elements


def read_xml(text):
    elements = []
    styles = []

    def local(tag):
        return tag.split("}")[1] if "}" in tag else tag

    def ns_of(tag):
        return "s" if tag.startswith("{" + SVG_NS + "}") else "x"

    def walk(e, parent):
        name = local(e.tag)
        if name in ("title", "metadata", "script", "desc"):
            return
        if name == "style":
            styles.append("".join(e.itertext()))
            return
        if e.tag.startswith("{") and ns_of(e.tag) != "s":
            return
        idx = len(elements)
        attrs = {local(k) if not k.startswith("{") else k: v for k, v in e.attrib.items()}
        elements.append({"ns": "s", "local": name, "id": attrs.get("id", ""), "parent": parent, "attrs": attrs, "rules": []})
        for c in e:
            walk(c, idx)

    root = ET.fromstring(text.encode("utf-8"))
    walk(root, -1)
    return elements, styles


def parse_rules(sheet):
    """[(selectors, declarations)] or a reason it cannot be read."""
    sheet = re.sub(r"/\*.*?\*/", "", sheet, flags=re.S)
    rules = []
    i = 0
    while i < len(sheet):
        while i < len(sheet) and sheet[i].isspace():
            i += 1
        if i >= len(sheet):
            break
        if sheet[i] == "@":
            return None, "an at-rule in a style sheet"
        j = sheet.find("{", i)
        if j < 0:
            return None, "a rule with no block"
        k = sheet.find("}", j)
        if k < 0 or "{" in sheet[j + 1:k]:
            return None, "a nested rule"
        rules.append((sheet[i:j].strip(), sheet[j + 1:k].strip()))
        i = k + 1
    return rules, ""


def specificity(sel):
    if sel.startswith("#"):
        return (1, 0, 0)
    if sel == "*":
        return (0, 0, 0)
    if sel == ":root":
        return (0, 1, 0)
    return (0, 0, 1)


def matches(sel, el):
    if sel == "*":
        return True
    if sel.startswith("#"):
        return el["id"] == sel[1:]
    if sel == ":root":
        return el["parent"] == -1
    return el["local"] == sel


def model(root, file, text):
    """(elements, target, reason)"""
    is_xml = file.endswith((".svg", ".xml", ".xhtml"))
    scripts = "\n".join(re.findall(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", text, re.S))
    if is_xml:
        try:
            elements, styles = read_xml(text)
        except ET.ParseError as e:
            return None, None, "an XML page that does not parse here (%s)" % e
    else:
        r = Reader()
        r.feed(text)
        r.close()
        if r.problems:
            return None, None, "a page with " + sorted(r.problems)[0]
        elements, styles = flatten_html(r.root), r.styles
    scripts = re.sub(r"(?m)^\s*//.*$", "", scripts)
    scripts = remove_calls(scripts)
    m = MUTATION.search(scripts)
    if m:
        return None, None, "a script that changes the document before the vectors (%s)" % m.group(0).strip()
    ids = [i for i, e in enumerate(elements) if e["id"] == "target"]
    if len(ids) != 1:
        return None, None, "a page with %d elements with the id target" % len(ids)
    order = 0
    for sheet in styles:
        rules, why = parse_rules(sheet)
        if rules is None:
            return None, None, why
        for selectors, decls in rules:
            for sel in [s.strip() for s in selectors.split(",")]:
                if not (sel in ("*", "body", "html", ":root") or (sel.startswith("#") and re.fullmatch(r"#[A-Za-z_][\w-]*", sel)) or re.fullmatch(r"[a-z][a-z0-9]*", sel)):
                    return None, None, "a selector this cannot match (%s: selectors are C3)" % sel
                if sel.startswith("#") and not any(e["id"] == sel[1:] for e in elements):
                    continue
                if re.fullmatch(r"[a-z][a-z0-9]*", sel) and sel not in ("body", "html") and not any(e["local"] == sel for e in elements):
                    continue
                for i, el in enumerate(elements):
                    if matches(sel, el):
                        spec = specificity(sel)
                        el["rules"].append([spec[0], spec[1], spec[2], order, decls])
            order += 1
    return elements, ids[0], ""


def remove_calls(src):
    """The script without the bodies of `test(...)`, `promise_test(...)`, `async_test(...)` and of function definitions: what they do runs in a test of its own or when called."""
    out = []
    i = 0
    n = len(src)
    pat = re.compile(r"\b(?:promise_test|async_test|test|function\s*[\w$]*)\s*\(")
    while i < n:
        m = pat.search(src, i)
        if not m:
            out.append(src[i:])
            break
        out.append(src[i:m.start()])
        depth = 0
        j = m.end() - 1
        # match the parentheses, skipping strings
        while j < n:
            c = src[j]
            if c in "\"'`":
                q = c
                j += 1
                while j < n and src[j] != q:
                    j += 2 if src[j] == "\\" else 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        i = j + 1
        if m.group(0).startswith("function"):
            # a function definition has a body in braces after the parameters
            while i < n and src[i] in " \t\r\n":
                i += 1
            if i < n and src[i] == "{":
                depth = 0
                while i < n:
                    c = src[i]
                    if c in "\"'`":
                        q = c
                        i += 1
                        while i < n and src[i] != q:
                            i += 2 if src[i] == "\\" else 1
                    elif c == "{":
                        depth += 1
                    elif c == "}":
                        depth -= 1
                        if depth == 0:
                            i += 1
                            break
                    i += 1
    return "".join(out)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    wpt, vectors, out = argv[1:4]
    computed = json.load(open(os.path.join(vectors, "computed.json"), encoding="utf-8"))
    files = sorted({v["file"] for v in computed})
    result = {}
    for f in files:
        path = os.path.join(wpt, f)
        text = open(path, encoding="utf-8", errors="replace").read()
        elements, target, reason = model(wpt, f, text)
        if elements is None:
            result[f] = {"scorable": False, "reason": reason}
        else:
            result[f] = {"scorable": True, "reason": "", "elements": elements, "target": target}
    with open(out, "w", encoding="utf-8") as h:
        json.dump(result, h, ensure_ascii=False, sort_keys=True)
    scorable = sum(1 for r in result.values() if r["scorable"])
    print("computed pages: %d, readable %d" % (len(result), scorable))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
