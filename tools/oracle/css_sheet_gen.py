#!/usr/bin/env python3
"""The population of the style sheet and selector differential (tools/oracle/css_sheet_diff.py): which documents are asked.

    css_sheet_gen.py --seed N --count M [--out FILE]

A population is a pure function of the seed and the count: the same arguments make the same file on every host. One line of the output is a document, as JSON:

    {"name", "text", "shadows": [[host id, html], ...], "queries": [[id, [property, ...]], ...], "selectors": [selector, ...]}

`text` is an HTML document (in quirks mode for one in five: no doctype) with a `<style>` element in its head and a tree of ten to twenty elements in a wrapper; `shadows` are the shadow trees of
some of the elements (a custom element `x-host`, whose shadow tree has slots, parts and its own `<style>`); `queries` are the five properties asked of each element
(`color`, `background-color`, `margin-top`, `margin-left`, `padding-left`: none has a user agent value on the tags the tree uses, and the first inherits); `selectors` are
the selectors `document.querySelectorAll()` is asked, valid and not.

What the sheets hold: rules over selectors of every form (types, ids, classes, attribute selectors, the structural, logical and relational pseudo-classes), nested rules
with and without `&`, `@layer` statements and blocks, `@scope` with and without a limit, conditional groups whose conditions are in the table of css_sheet_ours.cpp,
`!important`, and in a shadow tree `:host`, `:host()`, `::slotted()`, `:is(:host)`, `:host::part()` and `&` under `:host`, and in the outer sheet `::part()`. Also: class and id
spellings that differ in case (they match in quirks mode only), `dir` attributes and `:dir()`, an inline `<svg>` with `@namespace` and `svg|a`, `:scope` inside the arguments of
pseudo-classes in an `@scope`, an `@scope` inside an `@scope` with a limit, shadow trees inside shadow trees, and slots that are assigned to slots.
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
MEDIA = ["all", "screen", "print", "(min-width: 1px)", "(max-width: 10px)", "(min-width: 1280px)", "(width > 1280px)", "(width >= 1280px)", "(orientation: landscape)",
         "(min-aspect-ratio: 16/10)", "not print", "screen and (min-width: 1000px)", "(min-width: 1281px) or (max-height: 800px)", "(min-resolution: 1dppx)", "(foo: bar)",
         "(min-width: foo)", "not (min-width: foo)", "(400px < width < 2000px)"]
SUPPORTS = ["(display: block)", "(foo: bar)", "(color: red)", "(display: grid) and (color: red)", "not (foo: bar)", "(display: flex) or (foo: bar)", "selector(:has(a))",
            "selector(:foo)", "(--x: 1)", "not ((foo: bar) or (color: nonesuch))", "(container-type: size)", "at-rule(@media)"]
# @container: the containers of a document are block elements with an explicit size, so that the browsers lay out the box the library is told about (css_sheet_ours.cpp reads
# data-cw and data-ch)
CONTAINER_WIDTHS = [100, 250, 300, 500, 800]
CONTAINER_HEIGHTS = [50, 100, 300]
CONTAINER_NAMES = ["", "n0", "n1"]
CONTAINER_CONDITIONS = ["(min-width: %dpx)", "(width > %dpx)", "(max-width: %dpx)", "(width <= %dpx)", "(min-height: %dpx)", "(inline-size > %dpx)", "(block-size < %dpx)",
                        "(width < %dpx) or (height > %dpx)", "not (min-width: %dpx)", "((min-width: %dpx) and (max-height: 400px))", "(%dpx < width < 900px)"]
CONTAINER_THRESHOLDS = [50, 100, 200, 299, 300, 301, 499, 500, 600]
CONTAINER_FIXED = ["(orientation: landscape)", "(orientation: portrait)", "(aspect-ratio > 1)", "(aspect-ratio = 1 / 1)", "(foo)", "(min-width: foo)", "style(--k: 1)", "not (width)"]


class Gen:
    def __init__(self, seed, index):
        self.r = random.Random("%d/%d" % (seed, index))
        self.index = index
        self.elements = []  # (id, tag, classes, ...)
        self.next_id = 0
        self.has_svg = self.r.randrange(4) == 0
        self.quirks = self.r.randrange(5) == 0

    # ---- selectors ----------------------------------------------------------------------------------------------------
    def simple(self, depth):
        r = self.r
        k = r.randrange(100)
        ids = [e["id"] for e in self.elements] or ["e0"]
        if k < 24:
            return r.choice(TAGS)
        if k < 50:
            return "." + (r.choice(CLASSES) if r.randrange(8) else r.choice(["C1", "C2", "c1", "C0"]))
        if k < 54:
            return "#" + (r.choice(ids) if r.randrange(8) else r.choice(ids).upper())
        if k < 62:
            return r.choice(["[data-k]", "[data-k=x]", "[data-k~=y]", "[data-k^=x]", "[data-k$=y]", "[data-k|=x]", "[data-k*=-]", "[data-k=X i]"])
        if k < 68:
            return r.choice([":first-child", ":last-child", ":only-child", ":empty", ":root"])
        if k < 74:
            return r.choice([":nth-child(2n+1)", ":nth-child(even)", ":nth-last-child(2)", ":nth-of-type(2)", ":nth-last-of-type(1)", ":first-of-type", ":last-of-type",
                             ":only-of-type", ":nth-child(odd of .c1)", ":nth-child(-n+3)", ":nth-last-child(2n of .c0, .c2)"])
        if k < 77:
            return r.choice([":lang(en)", ":lang(de)", ":lang(de-CH)", ":lang(\"*-CH\")", ":checked", ":disabled", ":enabled", ":required", ":dir(rtl)", ":dir(ltr)", ":dir(ltr)",
                             ":not(:dir(rtl))", ":dir(foo)"])
        if depth == 0 and k < 78:
            return r.choice(["svg|a", "*|a", "svg|*", "|div", "*|div", "a"])
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
                            ":host.c0", ":host:not(.c1)", "div :host", ":host(.c0).c1", ":host :host-context(div)", ":host .in", ":host > span", "x-host",
                            ":is(:host)", ":where(:host) > span", ":is(:host, .zz)", ":is(:host(.c0), .zz)", ":host::part(a)", ":host(.c0)::part(b)", ":host-context(.c0)",
                            ":is(:host) > *", ":host-context(.c1) .in"])
        elif nested:
            sel = r.choice(["& .c1", "& > span", "& span", "&::part(a)", "&", "& &", ".c2 &", "&.c0", "> i", "+ div", "~ .c3", ".c1", "span", ":is(.c0, .c2) &", "& & ", "&:not(.c1)", "div &"])
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
            if r.randrange(3) == 0:
                # a selector whose subject is beside the scoping root, not inside it: is it in scope? (Cascade 6, 2.3: "only the selector's subject must be in scope")
                inner += " " + r.choice([":scope + *", ":scope ~ .c1", "& + div", ":scope ~ *", "& + .c0 + *"]) + " { %s }" % self.declarations()
            if r.randrange(3) == 0:
                inner += " " + r.choice([":is(:scope) > *", ":not(:scope) + *", "*:has(:scope)", ":where(:scope) .c1", ":is(&, .c2) > span", "div:not(:scope)",
                                         "p:is(:scope *)", ":scope:is(.c0)", "& :is(:scope, .c3)"]) + " { %s }" % self.declarations()
            if r.randrange(4) == 0:
                # a scope inside a scope with a limit: the roots of the inner are inside the outer, and the outer limit applies to the subject
                inner += " @scope (%s) to (%s) { %s { %s } }" % (self.compound(1), self.compound(1), self.r.choice(["*", "span", ".c1", "& > *", ":scope > div"]), self.declarations())
                return "@scope (%s) to (%s) { %s }" % (self.compound(1), self.compound(1), inner)
            if r.randrange(2):
                return "@scope (%s) to (%s) { %s }" % (self.compound(1), self.compound(1), inner)
            return "@scope (%s) { %s }" % (start if len(start) < 40 else ".c0", inner)
        if k < 90 and k >= 82 and not host_rules:
            name = r.choice(CONTAINER_NAMES)
            if r.randrange(5) == 0:
                cond = r.choice(CONTAINER_FIXED)
            else:
                cond = r.choice(CONTAINER_CONDITIONS)
                cond = cond.replace("%d", str(r.choice(CONTAINER_THRESHOLDS)))
            return "@container %s%s { %s }" % (name + " " if name else "", cond, inner)
        if k < 95:
            return "@media %s { %s }" % (r.choice(MEDIA), inner)
        return "@supports %s { %s }" % (r.choice(SUPPORTS), inner)

    def sheet(self, host_rules=False):
        r = self.r
        parts = []
        if r.randrange(3) == 0:
            parts.append("@layer %s;" % ", ".join(r.sample(["a", "b", "c"], r.randrange(1, 4))))
        if not host_rules and self.has_svg and r.randrange(2):
            parts.append("@namespace svg url(http://www.w3.org/2000/svg);")
        for _ in range(r.randrange(3, 9)):
            parts.append(self.group(0, host_rules))
        if not host_rules and self.elements and r.randrange(3) == 0:
            # a rule whose list holds a general and a specific selector that both match one element, and a later rule for the general one: the rule has the specificity of the
            # most specific selector that matches, so the first wins at that element (Selectors 4, 17)
            e = r.choice([x for x in self.elements if not x.get("host") and not x.get("shadow") and not x.get("no_query")] or self.elements)
            parts.append("%s, #%s { %s: %s }" % (e["tag"], e["id"], "color", r.choice(COLORS)))
            parts.append("%s { %s: %s }" % (e["tag"], "color", r.choice(COLORS)))
        return "\n".join(parts)

    # ---- the tree -----------------------------------------------------------------------------------------------------
    def new_id(self):
        self.next_id += 1
        return "e%d" % self.next_id

    def element_attrs(self, tag, e):
        r = self.r
        attrs = ' id="%s"' % e["id"]
        if e["classes"]:
            # a spelling that differs in case matches `.c1` in quirks mode only
            attrs += ' class="%s"' % " ".join(c.upper() if r.randrange(6) == 0 else c for c in e["classes"])
        if r.randrange(4) == 0:
            e["data"] = r.choice(DATA)
            attrs += ' data-k="%s"' % e["data"]
        if r.randrange(8) == 0:
            attrs += ' lang="%s"' % r.choice(LANGS)
        if r.randrange(9) == 0:
            e["dir"] = r.choice(["rtl", "ltr", "auto", "auto", "bogus"])
            attrs += ' dir="%s"' % e["dir"]
        return attrs

    def make_children(self, parent_ids, depth, slots=None):
        r = self.r
        out = ""
        for _ in range(r.randrange(0, 4 if depth else 5)):
            e = {"id": self.new_id(), "tag": r.choice(TAGS), "classes": r.sample(CLASSES, r.randrange(0, 3))}
            self.elements.append(e)
            attrs = self.element_attrs(e["tag"], e)
            if e["tag"] in ("div", "section", "article") and not slots and r.randrange(4) == 0:
                # a query container with an explicit content box: its type, one or two names, and the size the browsers lay out and this library is told
                w, h = r.choice(CONTAINER_WIDTHS), r.choice(CONTAINER_HEIGHTS)
                kind = r.choice(["size", "inline-size", "size", "inline-size", "normal"])
                names = " ".join(r.sample(["n0", "n1"], r.choice([0, 1, 1, 2])))
                css = "container-type: %s; %swidth: %dpx; height: %dpx; --k: %d" % (kind, "container-name: %s; " % names if names else "", w, h, r.randrange(2))
                attrs += ' style="%s" data-cw="%d" data-ch="%d"' % (css, w, h)
            body = ""
            if depth < 3 and r.randrange(10) < 5:
                body = self.make_children(parent_ids + [e["id"]], depth + 1)
            elif r.randrange(3) == 0:
                body = "t"
            if e.get("dir") == "auto" and r.randrange(2):
                body = "\u05d0" + body  # a Hebrew letter: the first strong character, so the direction is rtl
            extra = ""
            if slots and r.randrange(3) == 0:
                extra = ' slot="%s"' % r.choice(slots)
            out += "<%s%s%s>%s</%s>" % (e["tag"], attrs, extra, body, e["tag"])
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
        nested_shadows = []
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
            # a shadow tree inside the shadow tree: an inner host whose light children are in this tree (some are slots, assigned to the slots of the inner tree)
            if r.randrange(3) == 0:
                iid = self.new_id()
                ie = {"id": iid, "tag": "x-host", "classes": r.sample(CLASSES, r.randrange(0, 2)), "host": True, "shadow": True}
                self.elements.append(ie)
                inames = ["n1"] if r.randrange(2) else []
                ilight = ""
                for _ in range(r.randrange(0, 3)):
                    lid = self.new_id()
                    kind = r.choice(["slot", "slot", "span"])
                    lel = {"id": lid, "tag": kind, "classes": r.sample(CLASSES, r.randrange(0, 2)), "shadow": True}
                    self.elements.append(lel)
                    lcls = ' class="%s"' % " ".join(lel["classes"]) if lel["classes"] else ""
                    lslot = ' slot="%s"' % r.choice(inames) if inames and r.randrange(2) else ""
                    lname = ' name="n1"' if kind == "slot" and names and r.randrange(2) else ""
                    ilight += '<%s id="%s"%s%s%s>%s</%s>' % (kind, lid, lcls, lslot, lname, "t" if kind == "span" else "", kind)
                inner += '<x-host id="%s"%s>%s</x-host>' % (iid, ' class="%s"' % " ".join(ie["classes"]) if ie["classes"] else "", ilight)
                iinner = "<slot></slot>" + ('<slot name="n1"></slot>' if inames else "")
                for _ in range(r.randrange(1, 3)):
                    sid = self.new_id()
                    part = r.choice(["", ' part="a"', ' part="b"'])
                    se = {"id": sid, "tag": "span", "classes": ["in"], "shadow": True}
                    self.elements.append(se)
                    iinner += '<span id="%s" class="in"%s>t</span>' % (sid, part)
                # every inner tree also asks the one question the host-climb rule decides: `div :host` does not reach the host's ancestors in the outer tree (a fixed rule, so that
                # the controls see the defect in 200 documents and the draw of every other document is unchanged)
                inner_sheet = "<style>%s\ndiv :host { margin-top: 7px !important }\nx-host { span { margin-top: 9px !important } }</style>" % self.sheet(host_rules=True)
                nested_shadows.append([iid, inner_sheet + iinner])
            style = "<style>%s</style>" % self.sheet(host_rules=True)
            shadows.append([hid, style + inner])
            host_html += '<x-host id="%s"%s>%s</x-host>' % (hid, ' class="%s"' % " ".join(e["classes"]) if e["classes"] else "", light)
        shadows += nested_shadows
        svg = ""
        if self.has_svg:
            sid, aid = self.new_id(), self.new_id()
            self.elements.append({"id": sid, "tag": "svg", "classes": ["c1"]})
            self.elements.append({"id": aid, "tag": "a", "classes": ["c0"]})
            svg = '<svg id="%s" class="c1"><a id="%s" class="c0"></a></svg>' % (sid, aid)
        sheet = self.sheet()
        if n_hosts and r.randrange(2):
            sheet += "\nx-host::part(%s) { %s }" % (r.choice(["a", "b", "a b"]), self.declarations())
        doctype = "" if self.quirks else "<!doctype html>"
        text = ("%s<html><head><title>t</title><style>\n%s\n</style></head><body><div id=\"w\">%s%s%s</div></body></html>" % (doctype, sheet, body, host_html, svg))
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
