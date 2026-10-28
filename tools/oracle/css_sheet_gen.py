#!/usr/bin/env python3
"""The population of the style sheet and selector differential (tools/oracle/css_sheet_diff.py): which documents are asked.

    css_sheet_gen.py --seed N --count M [--out FILE]

A population is a pure function of the seed and the count: the same arguments make the same file on every host. One line of the output is a document, as JSON:

    {"name", "text", "shadows": [[host id, html], ...], "queries": [[id, [property, ...]], ...], "selectors": [selector, ...]}

`text` is an HTML document in standards mode with a `<style>` element in its head and a tree of ten to twenty elements in a wrapper; `shadows` are the shadow trees of
some of the elements (a custom element `x-host`, whose shadow tree has slots, parts and its own `<style>`); `queries` are the five properties asked of each element
(`color`, `background-color`, `margin-top`, `margin-left`, `padding-left`: none has a user agent value on the tags the tree uses, and the first inherits); `selectors` are
the selectors `document.querySelectorAll()` is asked, valid and not.

What the sheets hold: rules over selectors of every form (types, ids, classes, attribute selectors, the structural, logical and relational pseudo-classes), nested rules
with and without `&`, `@layer` statements and blocks, `@scope` with and without a limit, conditional groups whose conditions are in the table of css_sheet_ours.cpp,
`!important`, and in a shadow tree `:host`, `:host()` and `::slotted()`, and in the outer sheet `::part()`.
"""

import argparse
import json
import random
import sys

TAGS = ["div", "span", "section", "b", "i", "article", "em", "li"]
CLASSES = ["c0", "c1", "c2", "c3"]
DATA = ["x", "y", "x y", "x-y"]
LANGS = ["en", "de", "de-CH"]
COLORS = ["red", "blue", "green", "orange", "purple"]
BACKGROUNDS = ["pink", "teal", "gold", "navy"]
PX = ["1px", "2px", "3px", "5px", "8px"]
PROPS = ["color", "background-color", "margin-top", "margin-left", "padding-left"]
MEDIA = ["all", "screen", "print", "(min-width: 1px)", "(max-width: 10px)"]
SUPPORTS = ["(display: block)", "(foo: bar)", "(color: red)"]


class Gen:
    def __init__(self, seed, index):
        self.r = random.Random("%d/%d" % (seed, index))
        self.index = index
        self.elements = []  # (id, tag, classes, ...)
        self.next_id = 0

    # ---- selectors ----------------------------------------------------------------------------------------------------
    def simple(self, depth):
        r = self.r
        k = r.randrange(100)
        ids = [e["id"] for e in self.elements] or ["e0"]
        if k < 24:
            return r.choice(TAGS)
        if k < 50:
            return "." + r.choice(CLASSES)
        if k < 54:
            return "#" + r.choice(ids)
        if k < 62:
            return r.choice(["[data-k]", "[data-k=x]", "[data-k~=y]", "[data-k^=x]", "[data-k$=y]", "[data-k|=x]", "[data-k*=-]", "[data-k=X i]"])
        if k < 68:
            return r.choice([":first-child", ":last-child", ":only-child", ":empty", ":root"])
        if k < 74:
            return r.choice([":nth-child(2n+1)", ":nth-child(even)", ":nth-last-child(2)", ":nth-of-type(2)", ":nth-last-of-type(1)", ":first-of-type", ":last-of-type",
                             ":only-of-type", ":nth-child(odd of .c1)", ":nth-child(-n+3)", ":nth-last-child(2n of .c0, .c2)"])
        if k < 77:
            return r.choice([":lang(en)", ":lang(de)", ":lang(de-CH)", ":lang(\"*-CH\")", ":checked", ":disabled", ":enabled", ":required"])
        if depth == 0 and k < 82:
            return ":not(%s)" % self.compound(1)
        if depth == 0 and k < 87:
            return ":is(%s)" % self.selector_list(1, 2)
        if depth == 0 and k < 89:
            return ":where(%s)" % self.selector_list(1, 2)
        if depth == 0 and k < 95:
            return ":has(%s%s)" % (r.choice(["", "> ", "+ ", "~ "]), self.compound(1))
        return "*"

    def compound(self, depth):
        r = self.r
        n = r.choice([1, 1, 1, 2, 2, 3])
        parts = [self.simple(depth) for _ in range(n)]
        # a type or universal selector comes first
        types = [p for p in parts if p in TAGS or p == "*"]
        rest = [p for p in parts if not (p in TAGS or p == "*")]
        if len(types) > 1:
            types = types[:1]
        return "".join(types + rest) or "*"

    def complex(self, depth):
        r = self.r
        out = self.compound(depth)
        for _ in range(r.choice([0, 0, 0, 1, 1, 2])):
            out += r.choice([" ", " > ", " + ", " ~ "]) + self.compound(depth)
        return out

    def selector_list(self, depth, most=2):
        return ", ".join(self.complex(depth) for _ in range(self.r.randrange(1, most + 1)))

    # ---- sheets -------------------------------------------------------------------------------------------------------
    def declaration(self):
        r = self.r
        p = r.choice(PROPS)
        v = r.choice(COLORS) if p == "color" else r.choice(BACKGROUNDS) if p == "background-color" else r.choice(PX)
        return "%s: %s%s" % (p, v, " !important" if r.randrange(5) == 0 else "")

    def declarations(self):
        return "; ".join(self.declaration() for _ in range(self.r.randrange(1, 3)))

    def rule(self, depth, host_rules=False, nested=False):
        r = self.r
        if host_rules:
            sel = r.choice([":host", ":host(.c0)", ":host(.c1)", "::slotted(span)", "::slotted(.c1)", "::slotted(*)", ".in", "span", ".in + .in", ":host > *", "slot",
                            ":host.c0", ":host:not(.c1)", "div :host", ":host(.c0).c1", ":host :host-context(div)", ":host .in", ":host > span", "x-host"])
        elif nested:
            sel = r.choice(["& .c1", "& > span", ".c2 &", "&.c0", "> i", "+ div", "~ .c3", ".c1", "span", ":is(.c0, .c2) &", "& & ", "&:not(.c1)", "div &"])
        else:
            sel = self.selector_list(0, 2)
        body = self.declarations()
        if depth < 2 and r.randrange(5) == 0:
            kids = []
            for _ in range(r.randrange(1, 3)):
                kids.append(self.rule(depth + 1, nested=True))
            body += "; " + " ".join(kids) + (" " + self.declaration() if r.randrange(3) == 0 else "")
            return "%s { %s }" % (sel, body.replace("; }", " }"))
        return "%s { %s }" % (sel, body)

    def group(self, depth, host_rules=False):
        r = self.r
        k = r.randrange(100)
        if depth > 1 or k < 55:
            return self.rule(0, host_rules)
        inner = " ".join(self.group(depth + 1, host_rules) for _ in range(r.randrange(1, 3)))
        if k < 68:
            return "@layer %s { %s }" % (r.choice(["a", "b", "c", "a.x"]), inner)
        if k < 74:
            return "@layer { %s }" % inner
        if k < 82 and not host_rules:
            start = self.complex(1)
            if r.randrange(2):
                return "@scope (%s) to (%s) { %s }" % (self.compound(1), self.compound(1), inner)
            return "@scope (%s) { %s }" % (start if len(start) < 40 else ".c0", inner)
        if k < 90:
            return "@media %s { %s }" % (r.choice(MEDIA), inner)
        return "@supports %s { %s }" % (r.choice(SUPPORTS), inner)

    def sheet(self, host_rules=False):
        r = self.r
        parts = []
        if r.randrange(3) == 0:
            parts.append("@layer %s;" % ", ".join(r.sample(["a", "b", "c"], r.randrange(1, 4))))
        for _ in range(r.randrange(3, 9)):
            parts.append(self.group(0, host_rules))
        return "\n".join(parts)

    # ---- the tree -----------------------------------------------------------------------------------------------------
    def new_id(self):
        self.next_id += 1
        return "e%d" % self.next_id

    def element_attrs(self, tag, e):
        r = self.r
        attrs = ' id="%s"' % e["id"]
        if e["classes"]:
            attrs += ' class="%s"' % " ".join(e["classes"])
        if r.randrange(4) == 0:
            e["data"] = r.choice(DATA)
            attrs += ' data-k="%s"' % e["data"]
        if r.randrange(8) == 0:
            attrs += ' lang="%s"' % r.choice(LANGS)
        return attrs

    def make_children(self, parent_ids, depth, slots=None):
        r = self.r
        out = ""
        for _ in range(r.randrange(0, 4 if depth else 5)):
            e = {"id": self.new_id(), "tag": r.choice(TAGS), "classes": r.sample(CLASSES, r.randrange(0, 3))}
            self.elements.append(e)
            body = ""
            if depth < 3 and r.randrange(10) < 5:
                body = self.make_children(parent_ids + [e["id"]], depth + 1)
            elif r.randrange(3) == 0:
                body = "t"
            extra = ""
            if slots and r.randrange(3) == 0:
                extra = ' slot="%s"' % r.choice(slots)
            out += "<%s%s%s>%s</%s>" % (e["tag"], self.element_attrs(e["tag"], e), extra, body, e["tag"])
            if r.randrange(12) == 0:
                inp = {"id": self.new_id(), "tag": "input", "classes": [], "no_query": True}
                self.elements.append(inp)
                state = r.choice(["", " checked", " disabled", " required"])
                out += '<input id="%s" type="%s"%s>' % (inp["id"], r.choice(["checkbox", "text"]), state)
        return out

    def document(self):
        r = self.r
        # the tree, with up to two hosts: an element that is replaced by an x-host with a shadow tree
        shadows = []
        tree = ""
        n_hosts = r.choice([0, 0, 1, 1, 2])
        hosts = []
        body = self.make_children([], 0)
        if not self.elements:
            body = '<div id="e0">t</div>'
            self.elements.append({"id": "e0", "tag": "div", "classes": []})
        # hosts: elements appended after the generated tree, each with light children (some slotted) and a shadow tree
        host_html = ""
        for _ in range(n_hosts):
            hid = self.new_id()
            e = {"id": hid, "tag": "x-host", "classes": r.sample(CLASSES, r.randrange(0, 3)), "host": True}
            self.elements.append(e)
            names = ["n1"] if r.randrange(2) else []
            light = self.make_children([hid], 2, slots=names or None)
            # shadow tree: a default slot, maybe a named one, spans with parts, and a style
            inner = ""
            sr = []
            inner += "<slot></slot>"
            if names:
                inner += '<slot name="n1"></slot>'
            for _ in range(r.randrange(1, 4)):
                sid = self.new_id()
                part = r.choice(["", ' part="a"', ' part="a b"', ' part="b"'])
                cls = r.choice(["in", "in c0", "in c1", ""])
                el = {"id": sid, "tag": "span", "classes": cls.split() if cls else [], "shadow": True}
                self.elements.append(el)
                inner += '<span id="%s"%s%s>%s</span>' % (sid, ' class="%s"' % cls if cls else "", part, r.choice(["t", ""]))
            style = "<style>%s</style>" % self.sheet(host_rules=True)
            shadows.append([hid, style + inner])
            host_html += '<x-host id="%s"%s>%s</x-host>' % (hid, ' class="%s"' % " ".join(e["classes"]) if e["classes"] else "", light)
        sheet = self.sheet()
        if n_hosts and r.randrange(2):
            sheet += "\nx-host::part(%s) { %s }" % (r.choice(["a", "b", "a b"]), self.declarations())
        text = ("<!doctype html><html><head><title>t</title><style>\n%s\n</style></head><body><div id=\"w\">%s%s</div></body></html>" % (sheet, body, host_html))
        queries = []
        for e in self.elements:
            if e.get("no_query"):
                continue
            queries.append([e["id"], list(PROPS)])
        selectors = []
        for _ in range(24):
            selectors.append(self.selector_list(0, 2))
        selectors += r.sample(["div:", ":is(", "a:nonesuch", "::before", "x-host::part(a)", ":host", "#", ".", "[", "a >", "> a", ":nth-child(2n+)", "&", ":scope", "*|*", "p:has()"], 4)
        return {"name": "sheet-%d" % self.index, "text": text, "shadows": shadows, "queries": queries, "selectors": selectors}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--out", default="-")
    a = ap.parse_args()
    out = sys.stdout if a.out == "-" else open(a.out, "w", encoding="utf-8")
    for i in range(a.count):
        out.write(json.dumps(Gen(a.seed, i).document(), ensure_ascii=False) + "\n")
    if out is not sys.stdout:
        out.close()


if __name__ == "__main__":
    main()
