#!/usr/bin/env python3
"""The HTML tree builder against two HTML parsers.

    html_tree_diff.py <tree-runner> [--count N] [--seed S] [--plant KIND] [--propose]
                                    [--no-corpus] [--explain N]

The tree builder (src/html/html_tree*.c, stages 3c, 3d and 3e) is scored on the standard's own vectors by
`make conformance-html-tree`. This asks the same question of a population nobody wrote: documents
generated from the vocabulary of the constructs the stage implements, mutated documents, and the
inputs of the vectors, each put to **html5lib 1.1 and parse5 8.0.1** (the pinned image,
containers/html) and to this library, with the scripting flag off (and on where the document holds
a `noscript`). The three answers are trees in the tree-construction dump format; ours comes from
the runner of tools/conformance/html_tree_suite.c, theirs from the drivers, and nothing is
converted.

The population, each document tagged with where it came from:

  wpt    the input of every in-scope initial-document vector that the tree builder parses (one
         that stops at a construct of a later stage is not asked: it is counted)
  extra  the hand-chosen cases of tools/conformance/html_tree_extra.dat
  gen    generated documents: well-formed-ish nesting of blocks, lists, headings, formatting
         elements (with attributes, for the Noah's Ark clause), void elements, raw text
         elements, head elements, framesets, forms, buttons, markers, comments, text with
         references and white space, end tags of anything in any place; and, since 3d, tables
         (every combination of caption, colgroup, col, section, row and cell with tags
         missing, nested tables, text and elements in a table that foster parent, misnested
         forms, hidden inputs, `p` and formatting elements around and inside tables), templates
         (alone, around table parts, nested, in tables, tables in them), and select, option and
         optgroup
  mut    those, with tokens deleted, duplicated, swapped, inserted, or the document cut
  fgn    since 3e: documents with svg and math embedded in HTML at awkward places (in formatting
         elements, paragraphs, tables and cells, templates, lists, selects, forms), holding every
         SVG name whose case is restored, attributes that need adjusting (the SVG table, the MathML
         one, xlink, xml and xmlns), the integration points (foreignObject, desc, title; mi, mo, mn,
         ms, mtext; annotation-xml with each kind of encoding) with HTML content and foreign content
         again inside, the breakout tags and `font` with and without color, face and size, the end
         tags `</p>` and `</br>`, end tags that match, do not match, differ in case, CDATA sections
         and scripts
  fgm    those, mutated
  frg    fragments: random contexts (HTML ones, SVG and MathML ones) and token soup, table soup or
         foreign content for the input
  wptf   the fragment vectors, in their contexts

An SVG or MathML fragment context can only be given to parse5 (html5lib's fragment API takes an HTML
element name), so those documents have one reference: html5lib must say it cannot, and a divergence
from parse5 is counted as uncorroborated.

**The generator avoids processing instructions**, which neither reference has: a document that
reaches a limit is counted as "stopped", and the run fails if that is more than a small share of the
generated ones, so that the generator cannot drift out of the implemented constructs unnoticed. `select` is asked: the references parse it by the rules the
standard has replaced, and a divergence in a document with one is explained (verified by removal)
like any other element they read by older rules.

**What fails the run.** A document on which this library's tree differs from a reference's is a
divergence, and its *kind* is the pair (reference, signature), the signature being why: the
elements the reference reads by older rules (verified: removing them makes the two agree), the
transformations that stand for a reference limit in foreign content (`@endtags`, `@cdata`, `@context`,
`@noscript`, `@selectctx`: each keeps everything else of the document in the question; see the list's
header), or, for
html5lib alone with parse5 agreeing with this library, its older adoption agency rules. Every kind is a line of tools/oracle/html_tree_divergences.txt with a count and
a reason (a reference older than the standard, or the standard decides), or the run fails on

  * a divergence of a kind with no entry,
  * an entry that no longer occurs in the default population (stale) - except the ones counted
    `hunt`, which only larger populations reach,
  * an entry whose count has changed (the count is the signature of the population),
  * a reference that could not answer, or a population that has drifted (too few documents).

A divergence that BOTH references share and this library does not is listed under `both`: those
are the cases where the standard has moved past both references, and each has a reason.

`--plant KIND` corrupts the run on purpose and `run-html-oracle.sh --controls` requires it to
fail for each; a differential that cannot be seen to fail reports the same clean line as one
that passes. The tree builder itself is planted by compiling it with one rule undone (the
controls of run-html-oracle.sh).

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "conformance"))

import html_ref  # noqa: E402
import html_tree_suite as suite  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
LIST = os.path.join(HERE, "html_tree_divergences.txt")
EXTRA = os.path.join(ROOT, "tools", "conformance", "html_tree_extra.dat")
WPT = os.path.join(ROOT, "third_party", "html", "wpt", "html", "syntax", "parsing", "resources")
EXCEPTIONS = os.path.join(ROOT, "tools", "conformance", "HTML_TREE_EXCEPTIONS")
DEFAULT_COUNT, DEFAULT_SEED = 6000, 1
MIN_DOCUMENTS = 17000
PLANTS = {
    "ours-extra-line": "one of our trees gets a line it should not have",
    "ours-lost-line": "one of our trees loses a line",
    "stale-entry": "the list gets an entry for a divergence that does not occur",
    "change-count": "the count of one listed entry is changed",
    "no-list": "the list is empty",
    "witness": "a witness of a reference deviation does not hold",
    "uncorroborated": "the listed number of uncorroborated documents is changed",
}

# ---- the generator ----------------------------------------------------------------------------

BLOCKS = ["div", "p", "section", "article", "aside", "blockquote", "center", "details", "dialog", "dir",
          "fieldset", "figure", "figcaption", "footer", "header", "hgroup", "main", "menu", "nav", "search",
          "summary", "address"]
HEADINGS = ["h1", "h2", "h3", "h4", "h5", "h6"]
LISTS = ["ul", "ol", "dl", "li", "dd", "dt"]
FORMATTING = ["a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"]
INLINE = ["span", "sub", "sup", "label", "abbr", "cite", "q", "mark", "time", "var", "kbd", "samp", "bdi", "bdo"]
VOID = ["br", "img", "input", "area", "embed", "wbr", "param", "source", "track", "keygen", "hr"]
MARKERS = ["marquee", "object", "applet"]
RAW = ["textarea", "xmp", "iframe", "noembed", "pre", "listing"]
PLAIN = ["plaintext"]
HEADISH = ["title", "meta", "link", "base", "script", "style", "noscript", "noframes", "head", "body", "html",
           "basefont", "bgsound"]
RUBY = ["ruby", "rb", "rt", "rp", "rtc"]
FRAMES = ["frameset", "frame"]
FORMS = ["form", "button", "legend", "optgroup", "option", "caption", "col", "colgroup", "td", "tr", "th",
         "tbody", "thead", "tfoot"]  # the table ones are ignored in the body
TABLE_PARTS = ["table", "caption", "colgroup", "col", "tbody", "thead", "tfoot", "tr", "td", "th"]
SELECTS = ["select", "option", "optgroup"]
UNKNOWN = ["foo", "bar", "x-y", "a$b"]
ATTRS = ["", "", "", " class=a", " class=b", " id=x", " a", " a b", " b a", " href=u", " type=hidden",
         " data-x=\"1 2\"", " class=a id=x", " id=x class=a", " style='x:y'"]
TEXTS = ["x", "y z", " ", "\n", "a&amp;b", "&lt;", "&#65;", "t\tu", "  ", "\n\n", "&nbsp;", "&copy", "q"]
COMMENTS = ["<!-- c -->", "<!---->", "<!--a-->"]
DOCTYPES = ["<!DOCTYPE html>", "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\">",
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\" \"http://x\">", "<!doctype HTML>",
            "<!DOCTYPE>", "<!DOCTYPE foo>"]

# ---- foreign content (stage 3e) ----
# The SVG element names whose case the standard restores (the 37 of the "any other start tag" rule), as the
# tokenizer would lower them; the rest are ordinary SVG names, a few of them also HTML names.
SVG_ADJUSTED = ["altglyph", "altglyphdef", "altglyphitem", "animatecolor", "animatemotion", "animatetransform",
                "clippath", "feblend", "fecolormatrix", "fecomponenttransfer", "fecomposite", "feconvolvematrix",
                "fediffuselighting", "fedisplacementmap", "fedistantlight", "fedropshadow", "feflood", "fefunca",
                "fefuncb", "fefuncg", "fefuncr", "fegaussianblur", "feimage", "femerge", "femergenode",
                "femorphology", "feoffset", "fepointlight", "fespecularlighting", "fespotlight", "fetile",
                "feturbulence", "foreignobject", "glyphref", "lineargradient", "radialgradient", "textpath"]
SVG_PLAIN = ["g", "path", "rect", "circle", "text", "tspan", "use", "defs", "symbol", "a", "style", "script", "metadata",
             "stop", "mask", "filter", "line", "polygon", "image", "switch", "marker", "pattern", "view", "set",
             "animate", "desc", "title", "foo", "x-y", "td", "tr", "table", "button", "select", "option", "form",
             "input", "textarea", "caption", "template", "section", "main", "nav", "details", "dialog", "summary",
             "font", "head", "frame", "col", "tbody"]
SVG_HTML_IP = ["foreignobject", "desc", "title"]
SVG_ATTRS = ["viewbox", "viewBox", "preserveaspectratio", "attributename", "attributetype", "basefrequency",
             "clippathunits", "gradientunits", "gradienttransform", "markerheight", "markerwidth", "maskunits",
             "patterncontentunits", "pathlength", "refx", "refy", "repeatcount", "startoffset", "stddeviation",
             "targetx", "textlength", "zoomandpan", "xchannelselector", "d", "fill", "class", "id", "style",
             "x", "width", "xlink:href", "xlink:title", "xlink:type", "xlink:role", "xlink:show", "xlink:actuate",
             "xlink:arcrole", "xml:lang", "xml:space", "xmlns", "xmlns:xlink", "xlink:foo", "xml:base", "xmlns:x",
             "definitionurl", "href", "lang", "systemlanguage", "tablevalues"]
MATH_TAGS = ["mi", "mo", "mn", "ms", "mtext", "mrow", "mfrac", "msup", "msub", "mglyph", "malignmark",
             "annotation-xml", "semantics", "mstyle", "mfenced", "mspace", "mtable", "mtr", "mtd", "annotation",
             "g", "clippath", "foreignobject", "desc", "title", "svg", "td", "tr", "table", "p", "div", "b"]
MATH_TEXT_IP = ["mi", "mo", "mn", "ms", "mtext"]
MATH_ATTRS = ["definitionurl", "definitionURL", "xlink:href", "xml:lang", "xmlns", "xmlns:xlink", "display", "class",
              "id", "mathvariant", "viewbox", "href", "encoding"]
ENCODINGS = ["text/html", "TEXT/HTML", "application/xhtml+xml", "Application/XHTML+XML", "text/plain", "", "text/html ",
             "application/xml", "xhtml", "image/svg+xml"]
BREAKOUT = ["b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "h2",
            "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre",
            "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var"]
BREAKOUT_FONT = ["font color=r", "font face=x", "font size=3", "font COLOR", "font", "font id=a class=b"]
FOREIGN_TEXTS = ["x", "y z", " ", "\n", "a&amp;b", "&lt;", "t\tu", "&#65;", "q"]
CDATAS = ["<![CDATA[x]]>", "<![CDATA[a b]]>", "<![CDATA[]]>", "<![CDATA[x]]]]>"]
CONTEXTS_HTML = ["div", "td", "th", "tr", "tbody", "thead", "tfoot", "caption", "colgroup", "table", "template", "select",
                 "body", "html", "head", "frameset", "title", "textarea", "style", "script", "plaintext", "xmp",
                 "iframe", "noembed", "noframes", "noscript", "p", "li", "form", "span", "b", "a", "option", "optgroup",
                 "button", "dl", "h1", "ruby", "nobr", "foo"]
CONTEXTS_SVG = ["svg svg", "svg g", "svg path", "svg foreignObject", "svg desc", "svg title", "svg text", "svg td",
                "svg template", "svg select", "svg script"]
CONTEXTS_MATH = ["math math", "math mi", "math mo", "math mn", "math ms", "math mtext", "math annotation-xml",
                 "math mrow", "math mglyph", "math td", "math table", "math select"]

ALL_NAMES = BLOCKS + HEADINGS + LISTS + FORMATTING + INLINE + MARKERS + RAW + HEADISH + RUBY + FORMS + UNKNOWN
# Inputs the references cannot judge: processing instructions (neither has the states). The vectors
# that hold them are the conformance gate's. (`select` was one until 3d: the references parse it by the
# rules the standard has replaced; now a divergence in a document with a select is explained, and
# verified, like the other elements they read by older rules.)
UNASKABLE_RE = re.compile(r"<\?", re.I)


class Gen:
    def __init__(self, rng):
        self.r = rng

    def pick(self, seq):
        return seq[self.r.randrange(len(seq))]

    def start(self, name):
        return "<%s%s>" % (name, self.pick(ATTRS) if name not in ("input",) else self.pick(ATTRS + [" type=hidden"]))

    def end(self, name):
        return "</%s>" % name

    def token(self):
        r = self.r.random()
        if r < 0.20:
            return self.pick(TEXTS)
        if r < 0.40:
            return self.start(self.pick(FORMATTING))
        if r < 0.50:
            return self.end(self.pick(FORMATTING))
        if r < 0.62:
            return self.start(self.pick(BLOCKS + HEADINGS))
        if r < 0.68:
            return self.end(self.pick(BLOCKS + HEADINGS))
        if r < 0.76:
            return self.start(self.pick(LISTS))
        if r < 0.80:
            return self.end(self.pick(LISTS))
        if r < 0.84:
            return self.start(self.pick(VOID + ["image"]))
        if r < 0.87:
            return self.start(self.pick(INLINE + UNKNOWN))
        if r < 0.89:
            return self.end(self.pick(INLINE + UNKNOWN))
        if r < 0.91:
            return self.start(self.pick(MARKERS))
        if r < 0.92:
            return self.end(self.pick(MARKERS))
        if r < 0.93:
            return self.pick(COMMENTS)
        if r < 0.94:
            return self.start(self.pick(FORMS))
        if r < 0.95:
            return self.end(self.pick(FORMS))
        if r < 0.96:
            return self.start(self.pick(RAW)) + self.pick(TEXTS) + self.end(self.pick(RAW))
        if r < 0.97:
            return self.start(self.pick(HEADISH))
        if r < 0.98:
            return self.end(self.pick(HEADISH))
        if r < 0.99:
            return self.start(self.pick(RUBY + FRAMES))
        return self.end(self.pick(RUBY + FRAMES))

    def table_token(self):
        """One token of the table family: a part, a template, a form or input, a select, text."""
        r = self.r.random()
        if r < 0.30:
            return self.start(self.pick(TABLE_PARTS))
        if r < 0.50:
            return self.end(self.pick(TABLE_PARTS))
        if r < 0.58:
            return self.start("template")
        if r < 0.64:
            return self.end("template")
        if r < 0.69:
            return self.start("form")
        if r < 0.72:
            return self.end("form")
        if r < 0.76:
            return "<input type=hidden>" if self.r.random() < 0.6 else "<input%s>" % self.pick(ATTRS)
        if r < 0.81:
            return self.start(self.pick(SELECTS))
        if r < 0.85:
            return self.end(self.pick(SELECTS))
        if r < 0.92:
            return self.pick(TEXTS)
        if r < 0.96:
            return self.start(self.pick(FORMATTING)) if self.r.random() < 0.7 else self.end(self.pick(FORMATTING))
        if r < 0.98:
            return self.start(self.pick(BLOCKS + ["p"])) if self.r.random() < 0.7 else self.end(self.pick(BLOCKS + ["p"]))
        return self.pick(COMMENTS)

    def cell_content(self, depth):
        """What goes in a cell, a caption or a template: text, inline and block elements,
        formatting elements (some left open), forms, nested tables."""
        out = []
        for _ in range(self.r.randint(0, 4)):
            r = self.r.random()
            if r < 0.30:
                out.append(self.pick(TEXTS))
            elif r < 0.50:
                name = self.pick(FORMATTING + INLINE)
                out.append(self.start(name) + self.pick(TEXTS) + (self.end(name) if self.r.random() < 0.6 else ""))
            elif r < 0.62:
                name = self.pick(BLOCKS + HEADINGS + ["li", "dd"])
                out.append(self.start(name) + self.pick(TEXTS) + (self.end(name) if self.r.random() < 0.5 else ""))
            elif r < 0.70 and depth < 3:
                out.append(self.table(depth + 1))
            elif r < 0.78:
                out.append(self.start("form") + self.pick(TEXTS) + (self.end("form") if self.r.random() < 0.6 else ""))
            elif r < 0.84 and depth < 3:
                out.append(self.template(depth + 1))
            elif r < 0.90:
                out.append(self.start(self.pick(SELECTS)) + self.pick(TEXTS))
            else:
                out.append(self.table_token())
        return "".join(out)

    def maybe(self, tag_end, p=0.7):
        return tag_end if self.r.random() < p else ""

    def cell(self, depth):
        name = self.pick(["td", "td", "td", "th"])
        return self.start(name) + self.cell_content(depth) + self.maybe(self.end(name), 0.6)

    def stray(self):
        """Something between the parts of a table that is not a part of it: it foster parents or is ignored."""
        r = self.r.random()
        if r < 0.35:
            return self.pick(TEXTS)
        if r < 0.55:
            return self.start(self.pick(FORMATTING + INLINE + BLOCKS))
        if r < 0.65:
            return self.end(self.pick(FORMATTING + INLINE + BLOCKS + TABLE_PARTS))
        if r < 0.75:
            return self.pick(COMMENTS)
        if r < 0.85:
            return "<input type=hidden>"
        if r < 0.92:
            return self.start("form")
        return self.table_token()

    def row(self, depth):
        out = [self.start("tr")] if self.r.random() < 0.85 else []
        for _ in range(self.r.randint(0, 4)):
            out.append(self.cell(depth) if self.r.random() < 0.8 else self.stray())
        out.append(self.maybe(self.end("tr"), 0.7))
        return "".join(out)

    def section(self, depth):
        name = self.pick(["tbody", "tbody", "thead", "tfoot"])
        out = [self.start(name)] if self.r.random() < 0.7 else []
        for _ in range(self.r.randint(0, 3)):
            out.append(self.row(depth) if self.r.random() < 0.85 else self.stray())
        if out:
            out.append(self.maybe(self.end(name), 0.6))
        return "".join(out)

    def table(self, depth):
        """A table, with tags missing and strays in it."""
        out = [self.start("table")]
        if self.r.random() < 0.2:
            out.append(self.start("caption") + self.cell_content(depth) + self.maybe(self.end("caption"), 0.7))
        if self.r.random() < 0.25:
            out.append(self.start("colgroup") if self.r.random() < 0.7 else "")
            out.append("<col>" * self.r.randint(0, 3))
            out.append(self.maybe(self.end("colgroup"), 0.6))
        elif self.r.random() < 0.1:
            out.append(self.start("col"))
        for _ in range(self.r.randint(0, 3)):
            r = self.r.random()
            if r < 0.5:
                out.append(self.section(depth))
            elif r < 0.75:
                out.append(self.row(depth))
            elif r < 0.85:
                out.append(self.cell(depth))
            else:
                out.append(self.stray())
        out.append(self.maybe(self.end("table"), 0.75))
        return "".join(out)

    def template(self, depth):
        """A template with table parts or ordinary content, or other templates, inside."""
        out = [self.start("template")]
        for _ in range(self.r.randint(0, 3)):
            r = self.r.random()
            if r < 0.15:
                out.append(self.row(depth))
            elif r < 0.25:
                out.append(self.cell(depth))
            elif r < 0.35:
                out.append(self.section(depth))
            elif r < 0.43:
                out.append("<col>")
            elif r < 0.50:
                out.append(self.start("caption") + self.pick(TEXTS))
            elif r < 0.56:
                out.append(self.start("colgroup"))
            elif r < 0.68 and depth < 3:
                out.append(self.table(depth + 1))
            elif r < 0.76 and depth < 3:
                out.append(self.template(depth + 1))
            elif r < 0.88:
                out.append(self.cell_content(depth))
            else:
                out.append(self.table_token())
        out.append(self.maybe(self.end("template"), 0.7))
        return "".join(out)

    def tabular(self):
        """A document built around tables and templates."""
        out = []
        for _ in range(self.r.randint(1, 3)):
            r = self.r.random()
            if r < 0.55:
                out.append(self.table(0))
            elif r < 0.75:
                out.append(self.template(0))
            elif r < 0.85:
                out.append(self.start("p") + self.pick(TEXTS))
            elif r < 0.92:
                out.append(self.start(self.pick(FORMATTING)))
            else:
                out.append("".join(self.table_token() for _ in range(self.r.randint(1, 6))))
        if self.r.random() < 0.4:
            out.append("".join(self.table_token() for _ in range(self.r.randint(1, 5))))
        return "".join(out)

    def nested(self, depth):
        """Mostly well-formed nesting with some end tags missing or in the wrong order."""
        out = []
        for _ in range(self.r.randint(1, 4)):
            r = self.r.random()
            if r < 0.3 or depth > 4:
                out.append(self.pick(TEXTS) if self.r.random() < 0.7 else self.token())
            else:
                name = self.pick(FORMATTING * 3 + BLOCKS + HEADINGS + LISTS + INLINE + MARKERS)
                inner = self.nested(depth + 1)
                close = self.r.random()
                if close < 0.6:
                    out.append(self.start(name) + inner + self.end(name))
                elif close < 0.8:
                    out.append(self.start(name) + inner)
                else:
                    other = self.pick(FORMATTING + BLOCKS)
                    out.append(self.start(name) + inner + self.end(other))
        return "".join(out)

    def document(self):
        r = self.r.random()
        head = ""
        if self.r.random() < 0.4:
            head = self.pick(DOCTYPES)
        if r < 0.28:
            body = self.tabular()
        elif r < 0.36:
            body = "".join(self.table_token() for _ in range(self.r.randint(1, 20)))
        elif r < 0.55:
            body = "".join(self.token() if self.r.random() < 0.88 else self.table_token()
                           for _ in range(self.r.randint(1, 24)))
        elif r < 0.90:
            body = self.nested(0)
            if self.r.random() < 0.5:
                body += "".join(self.token() for _ in range(self.r.randint(1, 6)))
        else:
            pre = "".join(self.pick(["<html%s>" % self.pick(ATTRS), "<head>", "</head>", "<body%s>" % self.pick(ATTRS),
                                      self.pick(COMMENTS), " ", "<title>t</title>", "<frameset>", "<frame>",
                                      "<noscript>", "</noscript>", "<style>x</style>", "<script>y</script>",
                                      "<template>", "</template>"])
                          for _ in range(self.r.randint(1, 6)))
            body = pre + "".join(self.token() for _ in range(self.r.randint(0, 10)))
        return head + body


    # ---- foreign content ------------------------------------------------------------------------

    def fattrs(self, pool, k=2):
        """Zero to k attributes from the pool, with a value."""
        out = []
        for _ in range(self.r.randint(0, k)):
            name = self.pick(pool)
            if any(a.split("=")[0] == name for a in out):
                continue
            out.append("%s=%s" % (name, self.pick(["1", "a", "\"x y\"", "'q'", "u"])) if self.r.random() < 0.95 else name)
        return (" " + " ".join(out)) if out else ""

    def fname(self, ns):
        pool = SVG_ADJUSTED + SVG_PLAIN if ns == "svg" else MATH_TAGS
        name = self.pick(pool + (SVG_ADJUSTED[:8] if ns == "svg" else []))
        # The input's case does not matter to the tokenizer, but a case variant makes the point.
        r = self.r.random()
        return name.upper() if r < 0.05 else name

    def annotation(self):
        enc = self.pick(ENCODINGS)
        return "<annotation-xml%s>" % ("" if self.r.random() < 0.2 else " encoding=\"%s\"" % enc)

    def html_in_ip(self, depth):
        """What goes into an HTML integration point: ordinary content, and foreign content again."""
        out = []
        for _ in range(self.r.randint(0, 3)):
            r = self.r.random()
            if r < 0.25:
                out.append(self.pick(TEXTS))
            elif r < 0.45:
                name = self.pick(BLOCKS + HEADINGS + LISTS + FORMATTING + INLINE)
                out.append(self.start(name) + self.pick(TEXTS) + (self.end(name) if self.r.random() < 0.6 else ""))
            elif r < 0.60 and depth < 4:
                out.append(self.foreign(depth + 1))
            elif r < 0.68 and depth < 3:
                out.append(self.table(2))
            elif r < 0.74:
                out.append(self.pick(CDATAS))
            elif r < 0.82:
                out.append(self.start(self.pick(UNKNOWN + SVG_PLAIN[:12])))
            elif r < 0.90:
                out.append(self.end(self.pick(SVG_PLAIN + MATH_TAGS + BLOCKS)))
            else:
                out.append(self.pick(COMMENTS))
        return "".join(out)

    def foreign_children(self, ns, depth, ip):
        out = []
        for _ in range(self.r.randint(0, 4)):
            r = self.r.random()
            if r < 0.14:
                out.append(self.pick(FOREIGN_TEXTS))
            elif r < 0.20:
                out.append(self.pick(CDATAS))
            elif r < 0.24:
                out.append(self.pick(COMMENTS))
            elif r < 0.42 and depth < 5:
                out.append(self.foreign_element(ns, depth + 1))
            elif r < 0.48:
                out.append("<%s>" % self.pick(BREAKOUT + BREAKOUT_FONT))
            elif r < 0.52:
                out.append("</%s>" % self.pick(["p", "br", "p", "br", "div", "b", "span"]))
            elif r < 0.62:
                out.append(self.end(self.fname(ns) if self.r.random() < 0.6 else self.pick(SVG_PLAIN + MATH_TAGS)))
            elif r < 0.70 and depth < 5:
                out.append(self.foreign(depth + 1))
            elif r < 0.78:
                out.append(self.start(self.pick(FORMATTING)) + self.pick(FOREIGN_TEXTS))
            elif r < 0.84:
                out.append(self.end(self.pick(FORMATTING)))
            elif r < 0.88:
                out.append(self.start(self.pick(TABLE_PARTS)))
            elif r < 0.91:
                out.append("<script>%s</script>" % self.pick(FOREIGN_TEXTS))
            else:
                out.append(self.pick(TEXTS))
        return "".join(out)

    def foreign_element(self, ns, depth):
        name = self.fname(ns)
        low = name.lower()
        if ns == "math" and low == "annotation-xml":
            tag = self.annotation()
        else:
            tag = "<%s%s" % (name, self.fattrs(SVG_ATTRS if ns == "svg" else MATH_ATTRS))
            tag += ("/>" if self.r.random() < 0.15 else ">")
        if tag.endswith("/>"):
            return tag
        inner = ""
        if (ns == "svg" and low in SVG_HTML_IP) or (ns == "math" and low in MATH_TEXT_IP + ["annotation-xml"]):
            inner = self.html_in_ip(depth) if self.r.random() < 0.75 else self.foreign_children(ns, depth, True)
        else:
            inner = self.foreign_children(ns, depth, False)
        close = self.r.random()
        if close < 0.6:
            return tag + inner + "</%s>" % (name.lower() if self.r.random() < 0.8 else name)
        if close < 0.8:
            return tag + inner
        return tag + inner + "</%s>" % self.fname(ns)

    def foreign(self, depth=0):
        """An svg or math element with content."""
        ns = "svg" if self.r.random() < 0.6 else "math"
        tag = "<%s%s>" % (ns, self.fattrs(SVG_ATTRS if ns == "svg" else MATH_ATTRS, 3))
        if self.r.random() < 0.06:
            return tag[:-1] + "/>"
        out = tag + self.foreign_children(ns, depth, False)
        r = self.r.random()
        if r < 0.65:
            out += "</%s>" % ns
        elif r < 0.75:
            out += "</%s>" % ("math" if ns == "svg" else "svg")
        return out

    def foreign_document(self):
        """A document in which svg and math are embedded in HTML at awkward places."""
        r = self.r.random()
        head = self.pick(DOCTYPES) if self.r.random() < 0.3 else ""
        parts = []
        for _ in range(self.r.randint(1, 4)):
            r = self.r.random()
            f = self.foreign()
            if r < 0.25:
                parts.append(f)
            elif r < 0.37:
                name = self.pick(FORMATTING)
                parts.append(self.start(name) + f + (self.end(name) if self.r.random() < 0.6 else "") + self.pick(TEXTS))
            elif r < 0.47:
                parts.append(self.start(self.pick(BLOCKS + ["p"])) + self.pick(TEXTS) + f + self.pick(["", "</p>", "</div>"]))
            elif r < 0.60:
                parts.append(self.start("table") + (self.start("tr") + self.start("td") if self.r.random() < 0.6 else "")
                             + f + self.pick(["", "</td>", "</table>", "x"]))
            elif r < 0.68:
                parts.append(self.start("template") + f + self.maybe(self.end("template"), 0.6))
            elif r < 0.74:
                parts.append(self.start(self.pick(["ul", "ol", "dl"])) + self.start(self.pick(["li", "dd", "dt"])) + f
                             + self.start(self.pick(["li", "dd", "dt"])))
            elif r < 0.80:
                parts.append(self.start("select") + self.start("option") + f + self.maybe(self.end("select"), 0.5))
            elif r < 0.85:
                parts.append(self.start("form") + f + self.start("form") + self.maybe(self.end("form"), 0.5))
            elif r < 0.90:
                parts.append(self.pick(["<head>", "<frameset>", "<body>", "<title>t</title>", ""]) + f)
            elif r < 0.95:
                parts.append(self.pick(TEXTS) + f + self.token())
            else:
                parts.append(self.table() if False else self.table(1) + f)
        return head + "".join(parts) + ("".join(self.token() for _ in range(self.r.randint(0, 4))) if self.r.random() < 0.4 else "")

    def context(self):
        r = self.r.random()
        if r < 0.55:
            return self.pick(CONTEXTS_HTML)
        if r < 0.80:
            return self.pick(CONTEXTS_SVG)
        return self.pick(CONTEXTS_MATH)

    def fragment_input(self, ctx):
        """The input of a fragment: token soup, table stuff or foreign content, to suit the context."""
        r = self.r.random()
        if r < 0.30:
            return "".join(self.token() for _ in range(self.r.randint(1, 12)))
        if r < 0.50:
            return "".join(self.table_token() for _ in range(self.r.randint(1, 10)))
        if r < 0.58:
            return self.tabular()
        if r < 0.90:
            return "".join(self.foreign() if self.r.random() < 0.6 else self.token() for _ in range(self.r.randint(1, 4)))
        return self.nested(0)


TOKEN_RE = re.compile(r"<[^>]*>|[^<]+|<")


def mutate(rng, text, gen):
    toks = TOKEN_RE.findall(text) or [text]
    for _ in range(rng.randint(1, 3)):
        k = rng.randrange(6)
        i = rng.randrange(len(toks))
        if k == 0 and len(toks) > 1:
            del toks[i]
        elif k == 1:
            toks.insert(i, toks[i])
        elif k == 2 and len(toks) > 1:
            j = rng.randrange(len(toks))
            toks[i], toks[j] = toks[j], toks[i]
        elif k == 3:
            toks.insert(i, gen.token())
        elif k == 4:
            toks = toks[:i + 1]
        else:
            toks[i] = gen.token()
    return "".join(toks)


# ---- asking -------------------------------------------------------------------------------------

def hexs(s):
    return "x" + s.encode("utf-8", "surrogatepass").hex()


def ours_batch(runner, items):
    """[(status, dump, invariants)] for each (html, scripting) or (html, scripting, context): a
    context is the fragment parsing algorithm's (`td`, `svg path`, `math mi`)."""
    answers = []
    step = 400
    for lo in range(0, len(items), step):
        part = items[lo:lo + step]
        # A context that is a form element has a form ancestor (itself): the caller that has the element says so.
        reqs = ["whole %d %s%s%s" % (1 if it[1] else 0, hexs(it[0]),
                                    (" ctx:" + hexs(it[2])) if len(it) > 2 and it[2] else "",
                                    " form" if len(it) > 2 and it[2] == "form" else "") for it in part]
        got, _ = suite.run_batch(runner, reqs)
        if got is None:
            # isolate a crash
            got = []
            for r in reqs:
                one, _ = suite.run_batch(runner, [r])
                got.append(one[0] if one else ["STATUS CRASH", "END CRASH"])
        for lines in got:
            a = suite.parse_answer(lines)
            answers.append((a["status"], a["dump"], a["inv"]))
    return answers


def first_diff(a, b):
    la, lb = a.split("\n"), b.split("\n")
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else ""
        y = lb[i] if i < len(lb) else ""
        if x != y:
            return i, la, lb
    return None, la, lb


LINE_NAME = re.compile(r'^\|( *)(.*)$')


def line_info(lines, i):
    """(kind, names on the path from the root to the line, the line's own name)."""
    if i >= len(lines):
        return "end", [], ""
    m = LINE_NAME.match(lines[i])
    indent = len(m.group(1)) if m else 0
    body = m.group(2) if m else lines[i]

    def kind_name(b):
        if b.startswith("<!--"):
            return "comment", ""
        if b.startswith("<!DOCTYPE"):
            return "doctype", ""
        if b.startswith("<?"):
            return "pi", ""
        if b.startswith("<"):
            return "element", b[1:-1].split(" ")[-1] if b.endswith(">") else b
        if b.startswith('"'):
            return "text", ""
        return "attribute", b.split("=")[0]

    kind, own = kind_name(body)
    path = []
    want = indent
    for j in range(i - 1, -1, -1):
        mj = LINE_NAME.match(lines[j])
        if not mj:
            continue
        ind = len(mj.group(1))
        if ind < want:
            k, n = kind_name(mj.group(2))
            if k == "element":
                path.append(n)
            want = ind
            if ind == 0:
                break
    return kind, path, own


def coarse(ours, theirs):
    """The kinds of the two first-differing lines: used only when no element name explains a divergence."""
    i, la, lb = first_diff(ours, theirs)
    ka, _, _ = line_info(la, i)
    kb, _, _ = line_info(lb, i)
    return "%s/%s" % (ka, kb)


NAME_RE = re.compile(r"<([A-Za-z][A-Za-z0-9$-]*)")

# Elements a reference reads by rules older than the standard's (or does not know). A divergence
# is explained by them only if removing every one of them from the document makes the two agree;
# the list is the classification, not a licence: a divergence in a document without one, or one
# that survives their removal, is unexplained and fails the run. html5lib 1.1 is from 2020: it
# has no `search`, treats `summary`, `main`, `hgroup`, `figcaption` and `dialog` as ordinary
# elements in places the standard makes special or block, keeps `isindex`, `command` and
# `menuitem`, and has the older ruby and frameset rules. parse5 8.0.1 has everything but `search`.
#
# Stage 3d adds three. `select`: both parse it by the "in select" mode the standard no longer has
# (the standard as served has the relaxed rules in "in body"). `template`: html5lib 1.1 puts a
# template that follows nothing into the body and keeps its tables apart; parse5 ignores a form in
# a template, does not stop its table scope at a template (so an end tag `tr` or `table` closes
# rows and tables outside it), and keeps frameset-ok cleared after a template that "after head"
# makes the body from (the standard sets it again). A name that starts with `/` is the end tags of
# that element alone: parse5 ends a row at `</tbody>`, `</thead>` or `</tfoot>` when there is a tr
# in scope even if there is no such section (the standard needs both), and for `form` it asks whether
# any form is in scope at an end tag form, where the standard asks about the one the form element
# pointer names (it can have been popped already). Because "template" and
# "select" explain every divergence in a document that holds one, and a bug of ours would hide
# there, each reference deviation is also a WITNESS below: a small document, its tree worked out
# from the standard and written out by hand, which this library gives and the reference does not
# (so the claim is checked on every run, and goes stale if the reference is fixed).
KNOWN_OLD = {
    "html5lib": ["search", "summary", "dialog", "isindex", "ruby", "rb", "rt", "rp", "rtc", "main", "hgroup",
                 "figcaption", "command", "menuitem", "menu", "frameset", "select", "template",
                 # Stage 3e: the foreign elements the standard makes special and html5lib 1.1 does not
                 # (it has foreignObject only), `</p>` and `</br>` in foreign content (it keeps them inside), the
                 # element the SVG name table gained, and the two reference limits below.
                 "mi", "mo", "mn", "ms", "mtext", "annotation-xml", "desc", "title", "/p", "/br", "fedropshadow",
                 "@cdata", "@noscript", "@endtags", "@selectctx", "@resetnames"],
    "parse5": ["search", "select", "template", "/tbody", "/thead", "/tfoot", "form", "fedropshadow",
               # A CDATA section inside an integration point is a comment in parse5 (the standard has the
               # adjusted current node decide); a foreign fragment context named like an HTML element is read as
               # that element; the noscript context ignores the scripting flag.
               "@cdata", "@context", "@noscript", "@endtags", "@selectctx", "@rootend", "@resetnames"],
}
KNOWN_OLD["both"] = KNOWN_OLD["parse5"]
KNOWN_OLD["three-way"] = KNOWN_OLD["parse5"]


END_TAG_RE = re.compile(r"</([A-Za-z][A-Za-z0-9$-]*)")


# The HTML element names that parse5 reads a foreign fragment context as (it takes the context's local name
# without its namespace): not the names of the integration points, which it also gets right by name.
CONTEXT_NAMES = {"select", "table", "tr", "td", "th", "tbody", "thead", "tfoot", "caption", "colgroup", "template",
                 "html", "head", "body", "frameset", "textarea", "style", "script", "plaintext", "xmp", "iframe",
                 "noembed", "noframes", "noscript", "frame", "col", "option", "optgroup", "p", "li", "form"}
CDATA_RE = re.compile(r"<!\[CDATA\[(.*?)\]\]>", re.S)


def foreign_context(context):
    return bool(context) and (context.startswith("svg ") or context.startswith("math "))


FOREIGN_LINE = re.compile(r"^\|\s*<(?:svg|math) ([^ >]+)>$", re.M)


FOREIGN_SPECIAL = ("mi", "mo", "mn", "ms", "mtext", "annotation-xml", "foreignobject", "desc", "title")


def foreign_end_tags(html, ours):
    """The end tags of the document that name an element of the SVG or MathML namespace in this library's tree,
    and the end tags of the nine foreign special elements the document has start tags for (a tree that a frameset
    replaced no longer shows the elements that were there)."""
    names = {n.lower() for n in FOREIGN_LINE.findall(ours or "")}
    names |= {n for n in FOREIGN_SPECIAL if re.search(r"<%s[\s/>]" % re.escape(n), html, re.I)}
    return sorted(n for n in names if re.search(r"</%s[\s/>]" % re.escape(n), html, re.I))


ROOT_END_NAMES = ("a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u", "form")
ROOT_END_RE = re.compile(r"</(?:%s)(?=[\s/>])[^>]*>?" % "|".join(ROOT_END_NAMES), re.I)


RESET_NAMES = ("table", "tbody", "thead", "tfoot", "tr", "td", "th", "caption", "colgroup", "template", "head", "body",
               "frameset", "html")


def reset_names(html, ours):
    """The names of elements of foreign content in this library's tree that "reset the insertion mode
    appropriately" would read as the HTML element of the same name if it looked at names alone (both references
    do): a MathML or SVG `tr` on the stack puts them "in row"."""
    names = {n.lower() for n in FOREIGN_LINE.findall(ours or "")}
    return sorted(n for n in names if n in RESET_NAMES)


def applies(name, html, context, scripting, ours=None):
    """Does the explanation `name` (a pseudo-name starting with @) apply to this document?"""
    if name == "@resetnames":
        return bool(reset_names(html, ours))
    if name == "@rootend":
        # A fragment with foreign content (in the document or as the context) and an end tag whose HTML handling
        # changes the builder's state (the list of active formatting elements, the form element pointer).
        return bool(context) and ("<svg" in html.lower() or "<math" in html.lower() or foreign_context(context)) \
            and bool(ROOT_END_RE.search(html))
    if name == "@endtags":
        return bool(foreign_end_tags(html, ours))
    if name == "@selectctx":
        return context == "select"
    if name == "@cdata":
        return "<![CDATA[" in html
    if name == "@context":
        return foreign_context(context) and context.split(" ", 1)[1] in CONTEXT_NAMES
    if name == "@noscript":
        return context == "noscript" and not scripting
    return False


def candidates(who, html, context=None, scripting=False, ours=None):
    present = set(n.lower() for n in NAME_RE.findall(html))
    ends = set("/" + n.lower() for n in END_TAG_RE.findall(html))
    return [n for n in KNOWN_OLD[who]
            if (n.startswith("@") and applies(n, html, context, scripting, ours))
            or (not n.startswith("@") and (n in present or n in ends))]


# A reference's known deviations from the standard, each as a document whose tree was worked out by
# hand from the chapter (not taken from any parser): (who, document, the tree, what the reference does
# otherwise). `who` is a reference name, or "both". The run asks this library and the references
# and requires this library to give exactly the tree and each named reference not to.
WITNESSES = [
    ("parse5", "<table><tr><template><td></tr><dir>",
     "| <html>\n|   <head>\n|   <body>\n|     <table>\n|       <tbody>\n|         <tr>\n|           <template>\n"
     "|             content\n|               <td>\n|                 <dir>",
     "table scope does not stop at a template: the end tag tr in the cell inside the template closes the row outside it"),
    ("parse5", "<template><tr><form>",
     "| <html>\n|   <head>\n|     <template>\n|       content\n|         <tr>\n|           <form>\n|   <body>",
     "a form start tag in a table inside a template is ignored (the standard inserted it only from 2023-24)"),
    ("parse5", "<table><tr><th></th></thead> ",
     "| <html>\n|   <head>\n|   <body>\n|     <table>\n|       <tbody>\n|         <tr>\n|           <th>\n|           \" \"",
     "an end tag thead in a row ends the row when a tr is in scope, though no thead is: the standard asks for both"),
    ("html5lib", "<template>Hello</template>",
     "| <html>\n|   <head>\n|     <template>\n|       content\n|         \"Hello\"\n|   <body>",
     "a template in the head ends up in the body (html5lib 1.1's template support predates the standard's)"),
    ("parse5", "<form><p><table><th><table></form></table><form></table></form>x",
     "| <html>\n|   <head>\n|   <body>\n|     <form>\n|       <p>\n|         <table>\n|           <tbody>\n|             <tr>\n"
     "|               <th>\n|                 <table>\n|                 <form>\n|         \"x\"",
     "an end tag form closes the open p when some form is in scope, though the one the form element pointer names (a form "
     "that a cell took with it) is not"),
    ("both", "<form><template><table><form>",
     "| <html>\n|   <head>\n|   <body>\n|     <form>\n|       <template>\n|         content\n|           <table>\n"
     "|             <form>",
     "a form start tag in a table in a template is ignored when the form element pointer is set outside it (the standard "
     "ignores the pointer inside a template)"),
    ("both", "<head><template></template></head><p><frameset>",
     "| <html>\n|   <head>\n|     <template>\n|       content\n|   <frameset>",
     "\"after head\" sets frameset-ok back to ok when it makes the body, which a template start tag had cleared"),
    ("both", "<select><div>x</select>",
     "| <html>\n|   <head>\n|   <body>\n|     <select>\n|       <div>\n|         \"x\"",
     "an element in a select is kept (the standard has no \"in select\" mode; the references drop the tag)"),
    # Stage 3e. An entry may name a fragment context and the scripting flag it is asked with (6 fields).
    ("parse5", "<math><mn><u>x</mn>y",
     "| <html>\n|   <head>\n|   <body>\n|     <math math>\n|       <math mn>\n|         <u>\n|           \"xy\"",
     "an end tag mn in the HTML rules closes the MathML mn, though the standard asks for an HTML element of that name "
     "and mn is special (parse5 finds the element by its name alone)"),
    ("html5lib", "<span><math><mi>x</span>y",
     "| <html>\n|   <head>\n|   <body>\n|     <span>\n|       <math math>\n|         <math mi>\n|           \"xy\"",
     "the MathML text integration points and annotation-xml, and SVG desc and title, are special in the standard "
     "(\"any other end tag\" stops at them); html5lib 1.1 has foreignObject only"),
    ("parse5", "<math><mi><![CDATA[x]]>",
     "| <html>\n|   <head>\n|   <body>\n|     <math math>\n|       <math mi>\n|         \"x\"",
     "a CDATA section is text when the adjusted current node is not in the HTML namespace, an integration point included "
     "(parse5 makes a comment)"),
    ("both", "<svg><fedropshadow/>",
     "| <html>\n|   <head>\n|   <body>\n|     <svg svg>\n|       <svg feDropShadow>",
     "feDropShadow is in the standard's table of SVG names whose case is restored (both references lack it)"),
    ("html5lib", "<svg></p>x",
     "| <html>\n|   <head>\n|   <body>\n|     <svg svg>\n|     <p>\n|     \"x\"",
     "an end tag p in foreign content breaks out of it (html5lib 1.1 keeps it inside)"),
    ("html5lib", "<svg></br>x",
     "| <html>\n|   <head>\n|   <body>\n|     <svg svg>\n|     <br>\n|     \"x\"",
     "an end tag br in foreign content breaks out of it (html5lib 1.1 keeps it inside)"),
    ("both", "<b>x</b>",
     "| <b>\n|   \"x\"",
     "the noscript context leaves the tokenizer in the data state when scripting is disabled (both references make "
     "its content text whatever the flag)", "noscript", False),
    ("both", "<dt class=a>",
     "| <dt>\n|   class=\"a\"",
     "the select context is read by the \"in body\" rules (the standard has no \"in select\" mode); the references "
     "read the whole fragment by the mode the standard replaced and drop it", "select", False),
    ("parse5", "<table a></table>",
     "| <table>\n|   a=\"\"",
     "a context in the SVG namespace is not an HTML select: parse5 reads its local name without the namespace",
     "svg select", False),
    ("parse5", "<math><tr><annotation-xml encoding=text/html><table><table>",
     "| <html>\n|   <head>\n|   <body>\n|     <math math>\n|       <math tr>\n|         <math annotation-xml>\n"
     "|           encoding=\"text/html\"\n|           <table>\n|           <table>",
     "\"reset the insertion mode appropriately\" asks for an HTML tr element: a MathML tr on the stack is not one (parse5 reads "
     "the name alone, goes \"in row\" after the first table and drops the second)"),
    ("parse5", "<li><em></li><main></em><strong>",
     "| <li>\n|   <em>\n| <svg main>\n| <strong>",
     "in a fragment, an end tag that closes no foreign element is handled by the HTML rules (here the adoption agency drops the "
     "em that </li> left in the list); parse5's walk down the stack stops before the root element and never hands it over",
     "svg g", False),
]


def check_witnesses(runner):
    """(number held, [failures]) for WITNESSES.

    A witness holds when this library gives the hand-worked tree and each named reference **answered** (a
    dump, not an error, a crash or a time-out) with a different one. It is asked with the scripting flag off
    and on, and must hold both ways, unless the entry fixes the flag (and a context, for a fragment)."""
    entries = [w + (None, None) if len(w) == 4 else w for w in WITNESSES]
    bad = set()
    for scripting in (False, True):
        asked = [(i, w) for i, w in enumerate(entries) if w[5] is None or w[5] == scripting]
        mine = ours_batch(runner, [(w[1], scripting, w[4]) for _, w in asked])
        theirs = html_ref.ask_all([html_ref.request(w[1], scripting, w[4]) for _, w in asked])
        for name in html_ref.REFERENCES:
            for a in theirs[name]:
                if "dump" in a:
                    a["dump"] = html_ref.normalize_dump(a["dump"])
        for k, (i, (who, doc, tree, why, ctx, _sc)) in enumerate(asked):
            status, dump, inv = mine[k]
            got = dump[:-1] if dump.endswith("\n") else dump
            if status != "OK" or got != tree:
                bad.add((i, "%r (scripting %s): this library's tree is not the one worked out by hand" % (doc, scripting)))
            for ref in (html_ref.REFERENCES if who == "both" else (who,)):
                answer = theirs[ref][k]
                if "dump" not in answer or not isinstance(answer["dump"], str):
                    bad.add((i, "%r (scripting %s): %s did not answer (%s): a crash or an error is not a deviation" % (
                        doc, scripting, ref, answer.get("error", "no dump"))))
                elif answer["dump"] == tree:
                    bad.add((i, "%r (scripting %s): %s now gives the standard's tree (%s): the entry is stale" % (
                        doc, scripting, ref, why)))
    failures = sorted(bad)
    return len(entries) - len({i for i, _ in failures}), [m for _, m in failures]


def without(html, names, scripting=False, context=None, ours=None):
    """(html, scripting, context) with the named explanations applied: every tag of a named element removed (a
    name that starts with `/` is just that element's end tags), and for a name starting with `@` the one change
    it stands for: `@cdata` turns each CDATA section into its text (the generator's sections hold no markup),
    `@context` renames a foreign fragment context that is named like an HTML element, `@noscript` turns the
    scripting flag on for the noscript context, `@selectctx` makes a select context a div (the references parse
    the whole of a select context by the "in select" mode the standard replaced), and `@endtags` deletes the end
    tags that name an element of foreign content in this library's tree (the references find the element an end
    tag closes by its name alone, in the HTML rules, where the standard asks for an HTML element), and `@resetnames`
    renames the elements, named like an HTML table part, that foreign content holds in this library's tree (the
    references' "reset the insertion mode" reads names without namespaces: a MathML `tr` is "in row"), and `@rootend`
    deletes the end tags of formatting elements and `form` in a fragment with foreign content (parse5's walk down
    the stack in foreign content stops before the root element, so an end tag that matches no foreign element is
    never handed to the HTML rules in a fragment). Each leaves everything else of the document in the question."""
    whole = [n for n in names if not n.startswith("/") and not n.startswith("@")]
    ends = [n[1:] for n in names if n.startswith("/")]
    if whole:
        pattern = "|".join(re.escape(n) for n in sorted(whole))
        html = re.sub(r"</?(?:%s)(?=[\s/>])[^>]*>?" % pattern, "", html, flags=re.I)
    if ends:
        pattern = "|".join(re.escape(n) for n in sorted(ends))
        html = re.sub(r"</(?:%s)(?=[\s/>])[^>]*>?" % pattern, "", html, flags=re.I)
    if "@cdata" in names:
        html = CDATA_RE.sub(lambda m: m.group(1), html)
    if "@endtags" in names:
        gone = foreign_end_tags(html, ours)
        if gone:
            html = re.sub(r"</(?:%s)(?=[\s/>])[^>]*>?" % "|".join(re.escape(n) for n in gone), "", html, flags=re.I)
    if "@selectctx" in names and context == "select":
        context = "div"
    if "@rootend" in names:
        html = ROOT_END_RE.sub("", html)
    if "@resetnames" in names:
        gone = reset_names(html, ours)
        if gone:
            html = re.sub(r"(</?)(%s)(?=[\s/>])" % "|".join(re.escape(n) for n in gone), r"\1\2-x", html, flags=re.I)
    if "@context" in names and foreign_context(context):
        context = context.split(" ", 1)[0] + " zz"
    if "@noscript" in names and context == "noscript":
        scripting = True
    return html, scripting, context


def explain_all(runner, divergent):
    """Key each divergence (item, who, theirs-dump, parse5-dump), and verify the explanation by asking again.

    The explanation is a set of elements the reference reads by older rules (KNOWN_OLD): the document is asked
    again with one of the ones it holds removed, each in turn, and then with all of them removed; the divergence
    is explained by the first that makes this builder and that reference agree, and keyed by it (by `multiple`
    when only the removal of several does). `who` is html5lib (alone: parse5 agrees with this builder), parse5,
    both (both differ from this builder and agree with each other) or three-way (both differ from it and from each
    other; judged against parse5). A divergence the removal does not explain is `unverified` (html5lib alone: its
    older adoption agency and Noah's Ark rules, which no element name stands for) or `unexplained` and fails the
    run."""
    keys = [""] * len(divergent)
    queries = []  # (index of the divergence, key, the document with them removed, scripting)
    for idx, (it, who, theirs, p5) in enumerate(divergent):
        cand = candidates(who, it["html"], it.get("context"), it["scripting"], it["ours"])
        if not cand:
            continue
        for name in cand:
            h, sc, ctx = without(it["html"], [name], it["scripting"], it.get("context"), it["ours"])
            queries.append((idx, name, h, sc, ctx))
        if len(cand) > 1:
            h, sc, ctx = without(it["html"], cand, it["scripting"], it.get("context"), it["ours"])
            queries.append((idx, "multiple", h, sc, ctx))
    if queries:
        ours = ours_batch(runner_global, [(q[2], q[3], q[4]) for q in queries])
        answers = html_ref.ask_all([html_ref.request(q[2], q[3], q[4]) for q in queries])
        for name in html_ref.REFERENCES:
            for a in answers[name]:
                if "dump" in a:
                    a["dump"] = html_ref.normalize_dump(a["dump"])
        for (idx, key, v, sc, qctx), (status, dump, inv), a5, p5 in zip(
                queries, ours, answers["html5lib"], answers["parse5"]):
            if keys[idx]:
                continue
            who = divergent[idx][1]
            ref = a5 if who == "html5lib" else p5
            mine = dump[:-1] if dump.endswith("\n") else dump
            if status == "OK" and "dump" in ref and ref["dump"] == mine:
                keys[idx] = key
    for idx, (it, who, theirs, p5) in enumerate(divergent):
        if not keys[idx]:
            keys[idx] = "unverified" if who == "html5lib" and p5 == it["ours"] else \
                "unexplained/" + coarse(it["ours"], theirs)
    return keys


# The causes that explain a divergence in any document that holds the element: the blind spot of the method
# (removing the element can hide a defect of this library in it). A divergence in one of them where **neither
# reference agrees** with this library is *uncorroborated*: nothing but the hand-worked witnesses and the pinned
# vectors stands behind this library there. The number of such documents is gated like the kinds' counts (the
# `@uncorroborated` line of the list), so a change of behaviour in those constructs cannot pass unseen.
SUSPECT = ("template", "select", "form", "multiple", "@endtags", "@cdata", "@context", "@selectctx", "@noscript",
           "fedropshadow", "@rootend")


def uncorroborated(kinds):
    """Documents where nothing but the hand-worked witnesses and the pinned vectors stands behind this library: a
    divergence of both references in a document whose explanation is one of the SUSPECT ones, and, since stage
    3e, a divergence of parse5 in a fragment whose context is in the SVG or MathML namespace (html5lib cannot
    take one, so no second reference exists)."""
    n = 0
    for key, docs in kinds.items():
        who, _, cause = key.partition(":")
        if who in ("both", "three-way") and cause in SUSPECT:
            n += len(docs)
        elif who == "parse5":
            n += sum(1 for it, _ in docs if foreign_context(it.get("context")) or it.get("html5lib_crashed"))
    return n


def load_uncorroborated(path):
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            for raw in fh:
                if raw.startswith("@uncorroborated\t"):
                    return int(raw.split("\t")[1])
    return None


def load_list(path):
    table = {}
    if not os.path.exists(path):
        return table
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#") or raw.startswith("@"):
                continue
            parts = raw.split("\t")
            if len(parts) != 4 or not parts[3].strip():
                raise SystemExit("%s:%d: not <key> <count> <class> <reason>" % (path, n))
            key, count, cls, reason = parts
            if cls not in ("reference-old", "reference-wrong", "standard-newer"):
                raise SystemExit("%s:%d: %r is not a class" % (path, n, cls))
            if count != "hunt" and not count.isdigit():
                raise SystemExit("%s:%d: count must be a number or `hunt`" % (path, n))
            if key in table:
                raise SystemExit("%s:%d: %s is listed twice" % (path, n, key))
            table[key] = (count, cls, reason)
    return table


def population(runner, count, seed, corpus):
    rng = random.Random(seed)
    gen = Gen(rng)
    items = []  # dicts: id, html, scripting
    if corpus:
        vectors, _, _ = suite.load(WPT)
        listed = suite.load_exceptions(EXCEPTIONS)
        for v in vectors:
            if v["fragment"] or v["id"] in listed or UNASKABLE_RE.search(v["data"]):
                continue
            settings = [v["scripting"]] if v["scripting"] is not None else [False]
            if v["scripting"] is None and "noscript" in v["data"]:
                settings = [False, True]
            for sc in settings:
                items.append({"id": "wpt:%s" % v["id"], "html": v["data"], "scripting": bool(sc)})
        for v in suite.parse_dat(EXTRA, "extra"):
            items.append({"id": "extra:%s" % v["id"], "html": v["data"], "scripting": v["scripting"] is True,
                          "context": v["context"] if v["fragment"] else None})
    base = len(items)
    for i in range(count):
        h = gen.document()
        sc = bool(rng.getrandbits(1)) if "noscript" in h else False
        items.append({"id": "gen:%d" % i, "html": h, "scripting": sc})
    for i in range(count // 2):
        src = items[rng.randrange(len(items))]["html"] if corpus and rng.random() < 0.5 else items[base + rng.randrange(count)]["html"]
        h = mutate(rng, src, gen)
        sc = bool(rng.getrandbits(1)) if "noscript" in h else False
        items.append({"id": "mut:%d" % i, "html": h, "scripting": sc})
    # Stage 3e: documents with svg and math embedded in HTML, mutated ones, and fragments in random contexts.
    fbase = len(items)
    for i in range(count // 2):
        h = gen.foreign_document()
        sc = bool(rng.getrandbits(1)) if "noscript" in h else False
        items.append({"id": "fgn:%d" % i, "html": h, "scripting": sc})
    for i in range(count // 4):
        h = mutate(rng, items[fbase + rng.randrange(count // 2)]["html"], gen)
        items.append({"id": "fgm:%d" % i, "html": h, "scripting": False})
    for i in range(count // 2):
        ctx = gen.context()
        h = gen.fragment_input(ctx)
        sc = bool(rng.getrandbits(1)) if ctx == "noscript" or "noscript" in h else False
        items.append({"id": "frg:%d" % i, "html": h, "scripting": sc, "context": ctx})
    if corpus:
        # The fragment vectors, in their contexts.
        for v in vectors:
            if not v["fragment"] or v["id"] in listed or UNASKABLE_RE.search(v["data"]):
                continue
            settings = [v["scripting"]] if v["scripting"] is not None else [False]
            if v["scripting"] is None and ("noscript" in v["data"] or v["context"] == "noscript"):
                settings = [False, True]
            for sc in settings:
                items.append({"id": "wptf:%s" % v["id"], "html": v["data"], "scripting": bool(sc), "context": v["context"]})
    return items


runner_global = None


def main(argv):
    runner = None
    count, seed = DEFAULT_COUNT, DEFAULT_SEED
    plant = None
    propose = False
    corpus = True
    explain = 0
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--count":
            count = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        elif a == "--plant":
            plant = args.pop(0)
            if plant not in PLANTS:
                raise SystemExit("--plant: one of %s" % ", ".join(PLANTS))
        elif a == "--propose":
            propose = True
        elif a == "--no-corpus":
            corpus = False
        elif a == "--explain":
            explain = int(args.pop(0))
        elif runner is None:
            runner = a
        else:
            raise SystemExit("unexpected argument %r" % a)
    if runner is None:
        raise SystemExit(__doc__)
    count = int(os.environ.get("HTML_TREE_ORACLE_COUNT", count))
    seed = int(os.environ.get("HTML_TREE_ORACLE_SEED", seed))
    hunting = (count, seed) != (DEFAULT_COUNT, DEFAULT_SEED) or not corpus

    items = population(runner, count, seed, corpus)
    ours = ours_batch(runner, [(it["html"], it["scripting"], it.get("context")) for it in items])
    stopped = collections.Counter()
    asked = []
    for it, (status, dump, inv) in zip(items, ours):
        src = it["id"].split(":")[0]
        if status.startswith("E_LIMIT"):
            stopped[(src, status)] += 1
            continue
        if status != "OK":
            # A crash, an out-of-memory, an invalid-argument status: HTML parsing never fails for content.
            sys.stderr.write("\033[0;31m\n### The tree builder failed on a document (%s) ###\033[0m\n  %r\n" % (
                status, it["html"][:200]))
            return 3
        if inv != "OK":
            sys.stderr.write("\033[0;31m\n### The invariant checker failed ###\033[0m\n  %r: %s\n" % (it["html"][:120], inv))
            return 3
        it["ours"] = dump[:-1] if dump.endswith("\n") else dump
        asked.append(it)
    answers = html_ref.ask_all([html_ref.request(it["html"], it["scripting"], it.get("context")) for it in asked])
    for name in html_ref.REFERENCES:
        for a in answers[name]:
            if "dump" in a:
                a["dump"] = html_ref.normalize_dump(a["dump"])

    if plant == "ours-extra-line" and asked:
        asked[0]["ours"] += "\n| <planted>"
    if plant == "ours-lost-line":
        for it in asked:
            if it["ours"].count("\n") > 3:
                it["ours"] = it["ours"].rsplit("\n", 1)[0]
                break

    by_source = collections.Counter(it["id"].split(":")[0] for it in items)
    asked_by_source = collections.Counter(it["id"].split(":")[0] for it in asked)
    print("documents: " + ", ".join("%s %d (compared %d)" % (s, by_source[s], asked_by_source[s]) for s in sorted(by_source)))
    print("stopped before a comparison (a construct of a later stage, or a limit): " +
          ("; ".join("%d x %s %s" % (n, s, w) for (s, w), n in sorted(stopped.items())) or "none"))

    kinds = collections.defaultdict(list)
    agree_refs = 0
    equal_html5lib = equal_parse5 = equal_both = 0
    foreign_only = 0
    html5lib_crashed = 0
    refs_failed = []
    divergent = []  # (item, who, theirs, parse5's)
    for it, a5, p5 in zip(asked, answers["html5lib"], answers["parse5"]):
        ctx = it.get("context")
        foreign_ctx = bool(ctx) and (ctx.startswith("svg ") or ctx.startswith("math "))
        if foreign_ctx:
            # html5lib takes a fragment context by HTML name only: it must say so (that is the API's limit, and
            # the check that it still has it); parse5 alone judges these.
            if "error" not in a5:
                refs_failed.append((it, "answered a foreign context", p5.get("error")))
                continue
            if "dump" not in p5:
                refs_failed.append((it, a5.get("error"), p5.get("error")))
                continue
            foreign_only += 1
            if it["ours"] == p5["dump"]:
                equal_parse5 += 1
            else:
                divergent.append((it, "parse5", p5["dump"], p5["dump"]))
            continue
        if "dump" not in a5 and "dump" in p5 and a5.get("error", "").startswith("AssertionError"):
            # html5lib 1.1 stops on an assertion in its older template handling for a few documents (a hunt
            # finds them): that is its crash, not an answer. parse5 alone judges the document, as for a foreign
            # context, and the divergence is counted with the uncorroborated ones.
            html5lib_crashed += 1
            it["html5lib_crashed"] = True
            if it["ours"] == p5["dump"]:
                equal_parse5 += 1
            else:
                divergent.append((it, "parse5", p5["dump"], p5["dump"]))
            continue
        if "dump" not in a5 or "dump" not in p5:
            refs_failed.append((it, a5.get("error"), p5.get("error")))
            continue
        agree_refs += a5["dump"] == p5["dump"]
        d_a = it["ours"] != a5["dump"]
        d_p = it["ours"] != p5["dump"]
        equal_html5lib += not d_a
        equal_parse5 += not d_p
        equal_both += (not d_a) and (not d_p)
        if d_a and d_p and a5["dump"] == p5["dump"]:
            divergent.append((it, "both", a5["dump"], p5["dump"]))
        else:
            if d_a and not d_p:
                divergent.append((it, "html5lib", a5["dump"], p5["dump"]))
            if d_p:
                divergent.append((it, "three-way" if d_a else "parse5", p5["dump"], p5["dump"]))
    global runner_global
    runner_global = runner
    causes = explain_all(runner, divergent)
    for (it, who, theirs, _), cause in zip(divergent, causes):
        kinds["%s:%s" % (who if who != "html5lib" else "html5lib:alone", cause)].append((it, theirs))
    n = len(asked)
    print("compared %d documents: ours equals html5lib on %d, parse5 on %d, both on %d; the references equal each other on %d" % (
        n, equal_html5lib, equal_parse5, equal_both, agree_refs))
    print("fragments in an SVG or MathML context, judged by parse5 alone (html5lib's API cannot take one): %d" % foreign_only)
    if html5lib_crashed:
        print("documents on which html5lib 1.1 stopped on an assertion (judged by parse5 alone): %d" % html5lib_crashed)
    print("divergence kinds: %d, documents in them: %d" % (len(kinds), sum(len(v) for v in kinds.values())))

    listed = {} if plant == "no-list" else load_list(LIST)
    if plant == "stale-entry":
        listed["html5lib:element/element:planted-gone"] = ("1", "reference-old", "planted")
    if plant == "change-count":
        for k in sorted(listed):
            if listed[k][0] != "hunt":
                listed[k] = (str(int(listed[k][0]) + 1),) + listed[k][1:]
                break

    if propose:
        for k in sorted(kinds):
            print("%s\t%d\treference-old\tUNREAD" % (k, len(kinds[k])))
        return 0
    if explain:
        for k in sorted(kinds):
            for it, theirs in kinds[k][:explain]:
                print("== %s %s %s %r" % (k, it["id"], ("ctx=%r" % it["context"]) if it.get("context") else "", it["html"]))
                print("ours:\n" + it["ours"] + "\ntheirs:\n" + theirs)
        return 0

    failed = False
    held, witness_bad = check_witnesses(runner)
    print("witnesses: %d of %d reference deviations hold (this library gives the hand-worked tree, the reference does not)" % (
        held, len(WITNESSES)))
    if plant == "witness":
        witness_bad = ["planted: a witness that does not hold"]
    if witness_bad:
        failed = True
        sys.stderr.write("\033[0;31m\n### A witness of a reference deviation does not hold ###\033[0m\n")
        for line in witness_bad:
            sys.stderr.write("  %s\n" % line)
    if refs_failed:
        failed = True
        sys.stderr.write("\033[0;31m\n### A reference could not answer ###\033[0m\n")
        for it, e1, e2 in refs_failed[:10]:
            sys.stderr.write("  %r: html5lib %s, parse5 %s\n" % (it["html"][:80], e1, e2))
    unlisted = sorted(k for k in kinds if k not in listed)
    if unlisted:
        failed = True
        sys.stderr.write("\033[0;31m\n### Divergences with no entry (tools/oracle/html_tree_divergences.txt) ###\033[0m\n")
        for k in unlisted[:30]:
            it, theirs = kinds[k][0]
            sys.stderr.write("  %s  x%d\n    e.g. %s %r\n" % (k, len(kinds[k]), it["id"], it["html"][:160]))
    gone = sorted(k for k in listed if k not in kinds and listed[k][0] != "hunt")
    if hunting:
        gone = []  # the list describes the default population: a hunt need not reach every one of its kinds
    if gone:
        failed = True
        sys.stderr.write("\033[0;31m\n### Listed divergences that are gone ###\033[0m\n")
        for k in gone:
            sys.stderr.write("  %s  %s\n" % (k, listed[k][2][:100]))
    unc = uncorroborated(kinds)
    print("uncorroborated (neither reference agrees with this library and the explanation could hide a defect of it, or only parse5 could be asked): %d documents" % unc)
    listed_unc = load_uncorroborated(LIST)
    if plant == "uncorroborated" and listed_unc is not None:
        listed_unc += 1
    if not hunting and listed_unc != unc:
        failed = True
        sys.stderr.write("\033[0;31m\n### Uncorroborated documents whose count has changed ###\033[0m\n  listed %s, now %d\n" % (
            listed_unc, unc))
    if not hunting:
        changed = sorted(k for k in listed if k in kinds and listed[k][0] != "hunt" and int(listed[k][0]) != len(kinds[k]))
        if changed:
            failed = True
            sys.stderr.write("\033[0;31m\n### Listed divergences whose count has changed ###\033[0m\n")
            for k in changed:
                sys.stderr.write("  %s  listed %s, now %d\n" % (k, listed[k][0], len(kinds[k])))
    if not hunting:
        if len(items) < MIN_DOCUMENTS:
            failed = True
            sys.stderr.write("\033[0;31m\n### The population has shrunk: %d documents, floor %d ###\033[0m\n" % (len(items), MIN_DOCUMENTS))
        gen_stopped = sum(v for (s, w), v in stopped.items() if s in ("gen", "mut"))
        gen_total = by_source["gen"] + by_source["mut"]
        if gen_stopped * 50 > gen_total:
            failed = True
            sys.stderr.write("\033[0;31m\n### The generator left the implemented constructs: %d of %d generated documents stopped ###\033[0m\n" % (
                gen_stopped, gen_total))
    if failed:
        return 3
    print("html tree differential: every divergence is listed, none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
