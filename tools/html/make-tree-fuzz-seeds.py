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
"""Write the seed corpus for fuzz_html_tree.

    tools/html/make-tree-fuzz-seeds.py

The seeds are tracked (tests/fuzz/corpus/.gitignore keeps `*.seed` and nothing else) and they
are *derived*: this script reads the web-platform-tests tree-construction vectors under
third_party/html/wpt (fetch them with tools/html/fetch.sh), the hand-chosen cases of
tools/conformance/html_tree_extra.dat and the lists below, and writes them again, the same
bytes each time. Run it after a change to the selection and commit the result. Files named
`reproducer-*.seed` are not touched: they are the recorded finds, replayed by `make test`
(tests/test-html-tree-fuzz-seeds.cpp) and by every fuzz run.

Each seed is four option bytes and then the bytes to parse (the layout is in
tests/fuzz/fuzz_html_tree.cpp): o0 bit 0 is the scripting flag, bit 1 says the bytes are a fragment
and bits 2 to 7 are the index of its context in the harness's table (read from the harness source
here, so that the two cannot drift), o1 the size of the pieces
(low bits) and the open-element limit (high bits), o2 the seed of the random cut, o3 the cut
and the allocator-failure modes. The vectors that stop at a construct of a later stage are
seeds too (the harness checks that they stop cleanly); the option bytes are one of a few sets
chosen by a hash of the input, so that adding or removing a vector changes no other file.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "conformance"))
import html_tree_suite as suite  # noqa: E402

WPT = os.path.join(ROOT, "third_party", "html", "wpt", "html", "syntax", "parsing", "resources")
EXTRA = os.path.join(ROOT, "tools", "conformance", "html_tree_extra.dat")
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus", "html-tree")
MAX_SEED = 4096
HARNESS = os.path.join(ROOT, "tests", "fuzz", "fuzz_html_tree.cpp")
def _min_seeds():
    """The floor the replay test asserts (tests/test-html-tree-fuzz-seeds.cpp, kMinSeeds): one number, read from there."""
    with open(os.path.join(ROOT, "tests", "test-html-tree-fuzz-seeds.cpp"), encoding="utf-8") as fh:
        m = re.search(r"constexpr size_t kMinSeeds = (\d+);", fh.read())
    if not m:
        sys.exit("kMinSeeds is not in tests/test-html-tree-fuzz-seeds.cpp")
    return int(m.group(1))


EXPECTED = (_min_seeds(), 2700)  # the size of the corpus; a selection that drifts out of it is a mistake

OPTIONS = (
    (0x00, 0x00, 0x00, 0x00),  # scripting off, 64 open, whole, no failure modes
    (0x01, 0x21, 0x05, 0x01),  # scripting on, pieces of 2, tiny depth
    (0x00, 0x53, 0x11, 0x02),  # a byte at a time in the failure sweep
    (0x01, 0x07, 0x22, 0x03),
    (0x00, 0xA5, 0x33, 0x00),
)

def harness_contexts():
    """The harness's table of fragment contexts: {(namespace word, name, encoding, form, quirks): index}."""
    with open(HARNESS, encoding="utf-8") as fh:
        text = fh.read()
    body = text[text.index("const FuzzCtx kContexts[64] = {"):]
    body = body[:body.index("};")]
    table = {}
    rows = re.findall(r'\{GTEXT_HTML_NS_(HTML|SVG|MATHML), "([^"]*)", (nullptr|"[^"]*"), (true|false), GTEXT_MARKUP_MODE_(\w+)\}', body)
    if len(rows) != 64:
        sys.exit("the harness has %d contexts in its table, not 64" % len(rows))
    for i, (ns, name, enc, form, quirks) in enumerate(rows):
        key = (ns, name, None if enc == "nullptr" else enc.strip('"'), form == "true", quirks)
        table.setdefault(key, i)
    return table


def context_index(table, context):
    """The index for a vector's `#document-fragment` line (`td`, `svg path`, `math mi`), or None."""
    words = context.split(" ") if context else []
    if not 1 <= len(words) <= 2 or any(not w for w in words) or (len(words) == 2 and words[0] not in ("svg", "math")):
        return None
    ns, name = ("HTML", words[0]) if len(words) == 1 else (words[0].upper() if words[0] == "svg" else "MATHML", words[1])
    if ns == "SVG" or (len(words) == 2 and words[0] == "svg"):
        ns = "SVG"
    return table.get((ns, name, None, False, "NO_QUIRKS"))


# Foreign content and fragments at the sizes that make a naive implementation quadratic or overflow a
# fixed table: (name, text, context or None).
FOREIGN_EXTRAS = [
    ("deep-svg", "<svg>" * 120 + "x", None),
    ("deep-svg-g", "<svg>" + "<g>" * 120 + "x</g></svg>y", None),
    ("deep-math", "<math>" + "<mrow>" * 100 + "x", None),
    ("svg-stray-end-tags", "<svg>" + "<g>" * 60 + "</nothing></path>" * 100, None),
    ("breakout-run", "<svg><g><g><p>" * 80, None),
    ("breakout-everything", "".join("<svg><g><%s>" % n for n in ("b", "big", "blockquote", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var")), None),
    ("font-breakout", "<svg><font color=r><svg><font face=x><svg><font size=1><svg><font>" * 20, None),
    ("foreignobject-switches", "<svg><foreignObject><div><svg><foreignObject><p>x" * 40, None),
    ("integration-points", "<math><mi><b>x</b><mglyph/><malignmark><annotation-xml encoding=text/html><div><svg><desc><title><td>" * 10, None),
    ("annotation-xml-encodings", "".join("<math><annotation-xml encoding='%s'><b>x" % e for e in ("text/html", "TEXT/HTML", "application/xhtml+xml", "text/plain", "")), None),
    ("svg-name-table", "<svg>" + "".join("<%s/>" % n for n in ("altglyph", "altglyphdef", "altglyphitem", "animatecolor", "animatemotion", "animatetransform", "clippath", "feblend", "fecolormatrix", "fecomponenttransfer", "fecomposite", "feconvolvematrix", "fediffuselighting", "fedisplacementmap", "fedistantlight", "fedropshadow", "feflood", "fefunca", "fefuncb", "fefuncg", "fefuncr", "fegaussianblur", "feimage", "femerge", "femergenode", "femorphology", "feoffset", "fepointlight", "fespecularlighting", "fespotlight", "fetile", "feturbulence", "foreignobject", "glyphref", "lineargradient", "radialgradient", "textpath")), None),
    ("svg-attribute-table", "<svg " + " ".join("%s=1" % n for n in ("attributename", "attributetype", "basefrequency", "baseprofile", "calcmode", "clippathunits", "diffuseconstant", "edgemode", "filterunits", "glyphref", "gradienttransform", "gradientunits", "kernelmatrix", "kernelunitlength", "keypoints", "keysplines", "keytimes", "lengthadjust", "limitingconeangle", "markerheight", "markerunits", "markerwidth", "maskcontentunits", "maskunits", "numoctaves", "pathlength", "patterncontentunits", "patterntransform", "patternunits", "pointsatx", "pointsaty", "pointsatz", "preservealpha", "preserveaspectratio", "primitiveunits", "refx", "refy", "repeatcount", "repeatdur", "requiredextensions", "requiredfeatures", "specularconstant", "specularexponent", "spreadmethod", "startoffset", "stddeviation", "stitchtiles", "surfacescale", "systemlanguage", "tablevalues", "targetx", "targety", "textlength", "viewbox", "viewtarget", "xchannelselector", "ychannelselector", "zoomandpan")) + ">", None),
    ("foreign-attributes", "<svg xlink:href=a xlink:title=b xlink:type=c xlink:role=d xlink:arcrole=e xlink:show=f xlink:actuate=g xml:lang=h xml:space=i xmlns=j xmlns:xlink=k xmlns:x=l><math definitionurl=m xmlns=n xmlns:xlink=o>", None),
    ("xmlns-mismatch", "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'><math xmlns='http://www.w3.org/1998/Math/MathML'><svg xmlns='x' xmlns:xlink='y'>", None),
    ("cdata-sections", "<svg><![CDATA[a<b]]c]]]><![CDATA[]]><title><![CDATA[x]]></title></svg><![CDATA[y]]><p><![CDATA[z" , None),
    ("svg-scripts", "<svg><script>a<b>c</script><script/>d<g/><script>" * 30, None),
    ("foreign-in-tables", "<table><svg><tr><math><td>x</table><table><tr><td><svg><td><p>y" * 6, None),
    ("foreign-in-formatting", "<b><i><svg><g></b><p>x</svg></i><math><mi></b>y" * 12, None),
    ("foreign-end-walk", "<svg><foreignObject><math><mi><svg><g></foreignObject></svg></mi></math>x" * 30, None),
    ("nul-in-foreign", "<svg>\x00a\x00<g>\x00</g></svg>\x00<math>\x00", None),
    ("distinct-foreign-names", "<svg>" + "".join("<e%d/>" % i for i in range(500)), None),
    # Fragments, in the contexts the table holds.
    ("frag-svg-breakouts", "<g><p>x</g><b><i>y" * 30, ("SVG", "g")),
    ("frag-foreignobject", "<div>x<svg><g>y</div>z<p>" * 30, ("SVG", "foreignObject")),
    ("frag-mi", "<b>x</b><mglyph/><malignmark>y<svg>z</svg>" * 30, ("MATHML", "mi")),
    ("frag-annotation-xml", "<div>x<svg><g></annotation-xml>" * 30, ("MATHML", "annotation-xml")),
    ("frag-tr", "<td>x<th>y<tr><td>z" * 60, ("HTML", "tr")),
    ("frag-table", "x<tr><td>y</table>z<table><b>" * 30, ("HTML", "table")),
    ("frag-template", "<tr><td>x<template><tr><td>y</template>z" * 30, ("HTML", "template")),
    ("frag-select", "<option>a<select>b<input>c<optgroup>" * 30, ("HTML", "select")),
    ("frag-frameset", "<frameset><frame></frameset>x</frameset><frame>" * 30, ("HTML", "frameset")),
    ("frag-html", "<head><title>t</title></head><body>x<frameset>" * 30, ("HTML", "html")),
    ("frag-title", "a</title><b>&amp;" * 60, ("HTML", "title")),
    ("frag-script", "a</script><!--<script>b</script>--></script>" * 40, ("HTML", "script")),
    ("frag-form", "<form><form>x</form></form><input>" * 30, ("HTML", "form", True)),
    ("frag-quirks-table", "<p>a<table><tr><td>b" * 30, ("HTML", "div", False, "QUIRKS")),
    ("frag-deep", "<div>" * 70 + "<svg>" * 20 + "x", ("HTML", "div")),
    ("frag-foster", "<b>x<table>y<b>z</table><i>w" * 30, ("HTML", "tbody")),
]

EXTRAS = [
    # A deep run of divs and one of formatting elements: the limits and the work bound.
    ("deep-div", "<div>" * 120 + "x"),
    ("deep-formatting", "<b><i>" * 80 + "x</b>y"),
    ("many-attributes", "<div " + " ".join("a%d=v" % i for i in range(300)) + ">x"),
    ("html-attributes", "<html" + "".join(" h%d=v" % i for i in range(100)) + ">" + "<html a=b>" * 50),
    ("many-paragraphs", "<span>" * 60 + "<p>x" * 200),
    ("formatting-noahs-ark", "<p><i a=1><i a=1><i a=1><i a=1><i a=2></p>x" * 20),
    ("adoption-nine-blocks", "<a>" + "<div>" * 9 + "x</a>y"),
    ("misnested-everything", "<b><i><u><s><em><strong><small><big><font><tt><code>x</b></i></u></s></em>y</p>z"),
    ("text-in-pieces", "<p>" + "a</x>" * 300),
    ("frameset-then-more", "<frameset><frame>x<frameset><noframes>n</noframes></frameset> </frameset><!--c--></html>q"),
    ("odd-names", "<a$b><x:y c\"d=1 e<f=2 \u00e9=3><\u00e9>\x01\ufffe<!--\x01-->\ufdd0<!DOCTYPE a$b \u00e9>x"),
    ("custom-elements", "".join("<c-%d>%s" % (i % 230, "</c-%d>" % (i % 7) if i % 5 == 0 else "x") for i in range(400))),
    ("stray-end-tags", "</p></div></b></html></body></head>x</br>" * 5),
    # Stage 3d: the table modes, foster parenting and the template modes, at the sizes that make a
    # naive implementation quadratic or overflow a fixed table.
    ("deep-tables", "<table><tr><td>" * 40 + "x" + "</td></tr></table>" * 5 + "y"),
    ("wide-table", "<table>" + "<tr><td>x<td>y<th>z" * 100 + "</table>"),
    ("foster-text-runs", "<table>" + "x</z> </z>" * 200 + "</table>"),
    ("table-text-pending", "<table> \x00 \n <!--c--> \t</table> x <table>\x00 y\x00</table>"),
    ("nested-templates", "<template>" * 30 + "<tr><td>x" + "</template>" * 12 + "y"),
    ("template-end-chain", "<template>" * 60 + "x" + "</template>" * 30),
    ("templates-in-tables", "<table><template><tr><td><template><col></template>x</template><tr><td>y" * 3),
    ("table-adoption", "<b><table><tr><b><div>x</b>y</table><i>z" * 4 + "<a><table><a>"),
    ("table-modes-everywhere", "<table><caption>c<col><colgroup> <col> x <tbody><td>a<th>b<tr><tfoot><tr><td>c</table>d"),
    ("forms-in-tables", "<form><table><form><input type=hidden><tr><td><form>x</form></table></form><input type=hidden>"),
    ("selects-in-tables", "<table><tr><td><select><option>a<optgroup><td>b<select><tr><select></table>"),
    ("long-table-white-space", "<table>" + " \n\t" * 600 + "<tr><td>x</td></tr>" + " \n" * 300 + "</table> " + "\r\n " * 100 + "<table> " * 5 + "y"),
    ("table-text-positions", "<table>" + "a&amp;b\r\n \u00e9\U0001F600\x00 y&#65;" * 30 + "</table>"),
    ("template-forms-and-bodies", "<template><html a=1><body b=2><form><form></form></form><p></template><form><form>"),
]


def main():
    if not os.path.isdir(WPT):
        sys.exit("%s is not here; run tools/html/fetch.sh" % WPT)
    table = harness_contexts()
    os.makedirs(CORPUS, exist_ok=True)
    for old in os.listdir(CORPUS):
        if old.endswith(".seed") and not old.startswith("reproducer-"):
            os.unlink(os.path.join(CORPUS, old))
    vectors, _, _ = suite.load(WPT)
    vectors += suite.parse_dat(EXTRA, "html_tree_extra.dat")
    kept = 0
    seen = set()
    skipped = 0
    for v in vectors:
        raw = v["data"].encode("utf-8", "surrogatepass")
        ctx = None
        if v["fragment"]:
            ctx = context_index(table, v["context"])
            if ctx is None:
                skipped += 1
                continue
        key = (raw, ctx)
        if len(raw) > MAX_SEED or key in seen:
            continue
        seen.add(key)
        o0, o1, o2, o3 = OPTIONS[int.from_bytes(hashlib.sha256(raw if ctx is None else raw + bytes([ctx, 1])).digest()[:4], "big") % len(OPTIONS)]
        if ctx is not None:
            o0 = (o0 & 1) | 2 | (ctx << 2)
        if v["scripting"]:
            o0 |= 1
        stem, n = v["id"].split("#")
        path = os.path.join(CORPUS, "%s-%04d.seed" % (os.path.splitext(stem)[0], int(n)))
        with open(path, "wb") as fh:
            fh.write(bytes((o0, o1, o2, o3)) + raw)
        kept += 1
    for name, text, where in FOREIGN_EXTRAS:
        raw = text.replace("\\x00", "\x00").encode()
        if len(raw) > MAX_SEED:
            sys.exit("the extra seed %s is %d bytes: the harness skips an input over %d" % (name, len(raw), MAX_SEED))
        o0 = 0
        if where:
            ns, nm = where[0], where[1]
            form = len(where) > 2 and where[2] is True
            quirks = where[3] if len(where) > 3 else "NO_QUIRKS"
            idx = table.get((ns, nm, None, form, quirks))
            if idx is None:
                sys.exit("the extra seed %s names a context %r that the harness's table does not hold" % (name, where))
            o0 = 2 | (idx << 2)
        with open(os.path.join(CORPUS, "%s.seed" % name), "wb") as fh:
            fh.write(bytes((o0, 0, 0, 0)) + raw)
        kept += 1
    for name, text in EXTRAS:
        if len(text.encode()) > MAX_SEED:
            sys.exit("the extra seed %s is %d bytes: the harness skips an input over %d" % (name, len(text.encode()), MAX_SEED))
        with open(os.path.join(CORPUS, "%s.seed" % name), "wb") as fh:
            fh.write(bytes((0, 0, 0, 0)) + text.encode())
        kept += 1
    if not EXPECTED[0] <= kept <= EXPECTED[1]:
        sys.exit("%d seeds written; the documentation says %d to %d" % ((kept,) + EXPECTED))
    print("fuzz_html_tree: %d seeds from the vectors (%d fragment vectors had a context the table lacks)" % (kept, skipped))


if __name__ == "__main__":
    main()
