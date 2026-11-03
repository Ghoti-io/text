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
"""The population of the CSSOM differential (tools/oracle/css_cssom_diff.py): mutation sequences on style sheets, their rules, declarations and media lists, constructed and adopted
sheets and `matchMedia()`.

    Gen(seed, index).document() -> {"ops": [[op, field...], ...]}      the ops are listed in tools/oracle/css_cssom_ours.cpp

The first documents are fixed: the pairs of adjacent conditions of every list of 'The order of checks' (the rule-operations population of css_ruleops_gen.py), the page properties of
Paged Media 3, Appendix A and the properties each category of its prefix list admits that the appendix does not name, the nested-rule runs and the preserved kinds,
the `cssText` setters of a declaration and of a rule, the `CSSFontFeatureValuesMap`, media lists, `adoptedStyleSheets` and `matchMedia`. The rest are drawn from pools by the seed and the
document's index alone, so a population is a function of those two numbers.
"""

import random

import css_ruleops_gen

SHEETS = css_ruleops_gen.SHEETS + [
    "a{color:red;margin:1px} b{}",
    "@media screen{ a{color:red} }",
    "@import url(a.css) screen;",
    "@counter-style c{system:cyclic;symbols:a b}",
    "@font-feature-values F{@swash{a:1}@styleset{b:1 2}}",
    "@keyframes k{from{opacity:0}50%{opacity:.5}to{opacity:1}} a{}",
    "a{ b{ color:red } color:blue }",
    "@page :first{ margin:1px @top-left{ content:\"x\" } }",
    "@font-face{font-family:x;src:url(a)}",
]

SELECTORS = ["a", "b, c", "a >", "1a", "", ":is(x)", "& d", "> e", "x|f", "a::before", ":hover", "a:bogus", "a,,b", "a b", "*", "#i.j[k]", "a{}", ":first", ":left", "foo"]
KEYS = ["from", "50%", "to, 10%", "bogus", "", "150%", "-1%", "from to", "FROM", "0%,100%", " 25% "]
NAMES = ["k2", "none", "a b", "", "initial", "\"q\"", "default", "Z"]
ATTRS = [("system", "fixed 3"), ("system", "bogus"), ("symbols", "x y z"), ("symbols", ""), ("prefix", "\"[\""), ("suffix", "bogus bogus"), ("range", "1 5"), ("fallback", "decimal"),
         ("negative", "\"-\" \"+\""), ("speakAs", "bullets"), ("fontFamily", "A, B"), ("fontFamily", ""), ("name", "n2"), ("pad", "3 \"0\"")]
PROPS = [("color", "red", ""), ("color", "bogus", ""), ("margin", "1px 2px", ""), ("margin-top", "5px", "important"), ("width", "calc(1px + 2px)", ""), ("--x", "y z", ""),
         ("background", "red url(a.png)", ""), ("font", "12px/1 serif", ""), ("color", "blue", "IMPORTANT"), ("color", "blue", "bogus"), ("margin", "", ""), ("bogus", "1", ""),
         ("margin-left", "1px", ""), ("display", "block", ""), ("opacity", "0.5", "")]
REMOVES = ["color", "margin", "margin-top", "--x", "bogus", "background", "font"]
CSSTEXTS = ["color:red;margin:1px", "color:red; bogus:1; width:2px", "", ";;", "color:red !important; color: blue", "margin: 1px; margin-top: 2px", "{}", "a{b:c}", "/* c */ color:red",
            "color: red; color: blue", "margin-top:1px;margin:2px", "color:red;color:blue;margin:1px;color:green", "width:1px;;height:2px", "--a:b;--a:c", "color:red}", "background:red;background-color:blue",
            "margin:1px !important;margin-top:2px"]
RULE_TEXTS = ["a{}", "@media print{}", "@keyframes k{}", "b{color:red}", "", "bogus"]
MEDIA = ["screen", "print, tv", "not all", "(min-width: 100px)", "bogus", "", "screen and (", "only screen", "ALL", "screen, , print", "(min-width:1px) and (max-width:5px)", "tv,", " print "]
MEDIA_ITEMS = ["print", "screen, print", "Screen", "bogus!!", "", "(min-width: 1px)", "screen and (color)", "not print", "all"]
FFV = [("swash", "a", "1"), ("swash", "b", "1,2"), ("swash", "c", ""), ("characterVariant", "d", "1,2"), ("characterVariant", "e", "1,2,3"), ("styleset", "f", "1,2,3,4,5"),
       ("annotation", "g", "7"), ("stylistic", "h", "2,3"), ("ornaments", "i", "1"), ("historicalForms", "j", "1"), ("bogus", "k", "1")]
ADOPTED = ["0", "0,0", "1", "0,1", "", "1,0", "0,1,0", "2"]
MATCH = ["(min-width: 100px)", "(max-width: 100px)", "print", "screen", "all", "not all", "bogus", "", "(orientation: landscape)", "(orientation: portrait)", "(min-resolution: 1dppx)",
         "(min-width: 1280px)", "(min-width: 1281px)", "screen and (min-width: 100px), print", "(width: 1280px)", "(min-aspect-ratio: 16/10)"]
READS = ["cssText", "selectorText", "keyText", "name", "conditionText", "fontFamily", "type", "prefix", "href"]
PATHS = ["", "", "0", "0", "0", "1", "1", "2", "0.0", "0.1", "1.0"]

# Paged Media 3, Appendix A: "CSS 2.1 Properties that apply within the page context" and "within the margin contexts"
PAGE_PROPS = [
    ("direction", "rtl"), ("background-color", "red"), ("background-image", "none"), ("background-repeat", "repeat-x"), ("background-attachment", "fixed"), ("background-position", "1px 2px"),
    ("background", "red"), ("border-top-width", "1px"), ("border-right-width", "1px"), ("border-bottom-width", "1px"), ("border-left-width", "1px"), ("border-width", "1px"),
    ("border-top-color", "red"), ("border-right-color", "red"), ("border-bottom-color", "red"), ("border-left-color", "red"), ("border-color", "red"), ("border-top-style", "solid"),
    ("border-right-style", "solid"), ("border-bottom-style", "solid"), ("border-left-style", "solid"), ("border-style", "solid"), ("border-top", "1px solid red"),
    ("border-right", "1px solid red"), ("border-bottom", "1px solid red"), ("border-left", "1px solid red"), ("border", "1px solid red"), ("counter-reset", "a 1"),
    ("counter-increment", "a 2"), ("color", "red"), ("font-family", "serif"), ("font-size", "12px"), ("font-style", "italic"), ("font-variant", "small-caps"), ("font-weight", "bold"),
    ("font", "12px serif"), ("height", "10px"), ("min-height", "10px"), ("max-height", "10px"), ("line-height", "1.5"), ("margin-top", "1px"), ("margin-right", "1px"),
    ("margin-bottom", "1px"), ("margin-left", "1px"), ("margin", "1px"), ("outline-width", "1px"), ("outline-style", "solid"), ("outline-color", "red"), ("outline", "1px solid red"),
    ("padding-top", "1px"), ("padding-right", "1px"), ("padding-bottom", "1px"), ("padding-left", "1px"), ("padding", "1px"), ("quotes", "none"), ("letter-spacing", "1px"),
    ("text-align", "center"), ("text-decoration", "underline"), ("text-indent", "1px"), ("text-transform", "uppercase"), ("white-space", "pre"), ("word-spacing", "1px"),
    ("visibility", "hidden"), ("width", "10px"), ("min-width", "10px"), ("max-width", "10px"),
]
MARGIN_ONLY = [("unicode-bidi", "embed"), ("content", "\"x\""), ("overflow", "hidden"), ("vertical-align", "top"), ("z-index", "1")]
# properties of the categories the prefix list of this library admits that the appendix does not name, and ones in no category
PREFIX_EXTRA = [
    ("background-clip", "content-box"), ("background-size", "cover"), ("border-radius", "1px"), ("border-image-source", "none"), ("border-collapse", "collapse"), ("border-spacing", "1px"),
    ("counter-set", "a 1"), ("font-kerning", "none"), ("font-stretch", "condensed"), ("font-size-adjust", "0.5"), ("font-variant-caps", "small-caps"), ("margin-block-start", "1px"),
    ("margin-inline", "1px"), ("outline-offset", "1px"), ("padding-inline-start", "1px"), ("padding-block", "1px"), ("text-shadow", "1px 1px red"), ("text-overflow", "ellipsis"),
    ("text-wrap", "balance"), ("text-justify", "inter-word"), ("text-underline-offset", "1px"),
]
OUTSIDERS = [("display", "block"), ("position", "absolute"), ("float", "left"), ("opacity", "0.5"), ("transform", "none"), ("top", "1px"), ("box-sizing", "border-box"), ("--custom", "1px")]


def page_documents():
    """One document for each property and each of the two contexts. `p` says what Appendix A says: the context, the property and whether the appendix lists it there."""
    docs = []
    for ctx in ("page", "margin"):
        listed = set(n for n, _ in PAGE_PROPS) | (set(n for n, _ in MARGIN_ONLY) if ctx == "margin" else set())
        props = PAGE_PROPS + (MARGIN_ONLY if ctx == "margin" else []) + PREFIX_EXTRA + OUTSIDERS + ([] if ctx == "margin" else MARGIN_ONLY)
        for p, v in props:
            if ctx == "page":
                ops = [["S", "@page { %s: %s }" % (p, v)], ["V", "0", "cssText"]]
            else:
                ops = [["S", "@page { @top-left { %s: %s } }" % (p, v)], ["V", "0.0", "cssText"]]
            docs.append({"g": "page-prop", "p": [ctx, p, "listed" if p in listed else "unlisted"], "ops": ops})
    return docs


def open_items_documents():
    docs = []
    # a run of declarations ended by an invalid nested rule, and nested rules between declarations
    for css in ["a{color:red; bogus!selector{} margin:1px}", "a{color:red; 1b{} margin:1px; c{} width:2px}", "a{ @media print{ color:red } margin:1px }", "a{ b{} color:red; c{} d:e }",
                "a{color:red; @bogus x; margin:1px}", "a{ & b{} color:red; & c{} }", "a{color:red;; }b{margin:1px}", "a{ @media print{ b{} } width:1px; @media screen{ c{} } height:2px }",
                "a{color:red; ::bogus{} margin:1px; &:hover{} padding:1px}", "a{ b{color:red} c:d{} e{} f:g }"]:
        docs.append({"g": "nested-run", "ops": [["S", css], ["V", "0", "cssText"]]})
    # a preserved kind (@mixin and kin) and the unknown at-rules nested in it
    for css in ["@mixin foo { @media print { a{} } b{} }", "@mixin foo { @bogus { a{} } b{} } a{}", "@function --f() { result: 1; } a{}", "@custom-media --m (min-width: 1px); a{}",
                "@when media(print) { a{} } @else { b{} }", "a{ @mixin x; color:red }", "@media print { @mixin foo { a{} } b{} }", "@starting-style { a{} }"]:
        docs.append({"g": "preserved-kind", "ops": [["S", css], ["V", "0", "cssText"]]})
    return docs


def setter_documents():
    docs = []
    # the cssText-set half: the declaration's cssText and the rule's, on the rules that have a block
    for base in ["a{color:red;margin:1px}", "@page{margin:1px}", "@font-face{font-family:x;src:url(a)}", "@keyframes k{from{opacity:0}}", "@counter-style c{system:cyclic;symbols:a}"]:
        path = "0.0" if base.startswith("@keyframes") else "0"
        for text in (CSSTEXTS if base.startswith("a{") else CSSTEXTS[:9] + CSSTEXTS[9:12]):
            docs.append({"g": "decl-cssText-set", "ops": [["S", base], ["Y", path, text], ["V", path, "cssText"], ["VL", path]]})
    for text in RULE_TEXTS:
        docs.append({"g": "rule-cssText-set", "ops": [["S", "a{color:red}"], ["Z", "0", text], ["V", "0", "cssText"]]})
    # the declaration algorithms in sequence
    docs.append({"g": "decl-sequence", "ops": [["S", "a{margin:1px}"], ["P", "0", "margin-top", "2px", ""], ["V", "0", "cssText"], ["Q", "0", "margin"], ["V", "0", "cssText"]]})
    docs.append({"g": "decl-sequence", "ops": [["S", "a{color:red}"], ["P", "0", "color", "blue", "important"], ["P", "0", "color", "green", ""], ["VP", "0", "color"], ["VS", "0", "color"]]})
    # the feature value maps
    for t, n, v in FFV:
        docs.append({"g": "feature-map", "ops": [["S", "@font-feature-values F{@swash{a:1}@styleset{b:1 2}}"], ["W", "0", t, n, v], ["V", "0", "cssText"]]})
    # media lists
    for m in MEDIA:
        docs.append({"g": "media-text", "ops": [["S", "@media screen{a{}} @import url(a.css) print;"], ["M", "0", m], ["V", "0", "conditionText"], ["M", "", m], ["M", "1", m], ["V", "1", "cssText"]]})
    for m in MEDIA_ITEMS:
        docs.append({"g": "media-item", "ops": [["S", "@media screen, print{a{}}"], ["MA", "0", m], ["V", "0", "conditionText"], ["MD", "0", m], ["V", "0", "conditionText"]]})
    # adopted sheets and constructed sheets
    for a in ADOPTED:
        docs.append({"g": "adopted", "ops": [["C"], ["S", "a{}"], ["AD", a]]})
    docs.append({"g": "constructed", "ops": [["C"], ["R", "a{color:red} @import url(a.css); b{}"], ["V", "0", "cssText"], ["I", "", 0, "@import url(b.css);"], ["R", "c{}"], ["AD", "0"], ["AD", "0,0"]]})
    docs.append({"g": "constructed", "ops": [["C"], ["R", "a{}"], ["DIS", "1"], ["AD", "0"]]})
    for m in MATCH:
        docs.append({"g": "match-media", "ops": [["MM", m]]})
    return docs


# A `<general-enclosed>` of a media query list is written as it was read (white space, comments and escapes kept, and an unclosed parenthesis not closed): the shapes Chromium 156 and
# Firefox 158 agree on. The ones with a known feature (`(color`) are closed, as a feature is parsed and written; `(foo(a` has its condition closed and its function not.
MEDIA_GENERAL = ["screen and (", "(a (b ( c", "(a (b) ", "screen and (a:b", "screen and (a/**/b)", "screen and ( a )", "screen and (  a   b  )", "screen and foo( a )", "(a:\"x", "(a: url(x",
                 "(--x", "(foo(a", "not (foo(", "(color", "(min-width: 1px", "(not (color)", "screen and (color", "screen and (\\41 b)", "screen and ('x' )", "screen and (a  ,  b)",
                 "screen and (a: url( x ))", "screen and ( { a } )", "screen and (a /* x", "screen and foo(a, b", "screen and (--x: y; z", "(a\n\tb)", "not ( a )", "screen and ( a ) and ( b )",
                 "only screen and (x: y "]


def media_general_documents():
    docs = []
    for m in MEDIA_GENERAL:
        docs.append({"g": "media-general", "ops": [["S", "@media screen{a{}} @import url(a.css) print;"], ["M", "0", m], ["V", "0", "conditionText"], ["V", "0", "cssText"], ["M", "", m],
                                                    ["MA", "0", "tv"], ["V", "0", "conditionText"], ["MD", "0", "tv"], ["V", "0", "conditionText"], ["MD", "0", m], ["MM", m]]})
        docs.append({"g": "media-general-import", "ops": [["S", "@import url(a.css) print; @media screen{a{}}"], ["M", "0", m], ["V", "0", "cssText"], ["MA", "0", "tv"], ["V", "0", "cssText"],
                                                           ["MD", "0", "tv"], ["V", "0", "cssText"], ["Z", "1", "@media print{b{}}"], ["V", "1", "cssText"]]})
    return docs


# A `<general-enclosed>` of a prelude read from a style sheet's text (a parse, `insertRule`, `replaceSync`) is written as it was read, as the media list the setters write is: `( a )` stays
# `( a )`. The `@supports` row is the one the browsers differ on (Chromium writes a declaration it knows normalized, Firefox as written).
SHEET_GENERAL = ["@media screen and ( a ) {a{}}", "@media (a/**/b) {a{}}", "@media screen and (a: b ) , print {a{}}", "@media  not  ( a )  {a{}}", "@container ( a ) {a{}}",
                 "@container  foo ( a:  b ) {a{}}", "@media screen and (  b  ), print and ( c ) {a{}}", "@media (--x) {a{}}", "@media screen and ( a ) {@media ( b ) {a{}} a{}}",
                 "@media screen and (", "@media (a (b ( c) {a{}}", "@media screen and (min-width:  1px) and ( a ) {a{}}", "@container  foo  ( min-width:  1px ) {a{}}",
                 "@container foo ( a ) and ( b ) {a{}}", "@import url(a.css) screen and ( a );", "@import url(a.css) layer(x) supports(display: grid) screen and ( a ) , print;",
                 "@import \"a.css\" ( a );", "@supports (  display :  grid ) {a{}}"]


def sheet_general_documents():
    docs = []
    for css in SHEET_GENERAL:
        imp = css.startswith("@import")
        docs.append({"g": "media-general-sheet", "ops": [["S", css if imp else css + " b{}"], ["V", "0", "cssText"], ["V", "0", "conditionText"], ["Z", "0", css], ["V", "0", "cssText"]]})
        docs.append({"g": "media-general-sheet-insert", "ops": [["S", "a{}"], ["I", "", 0, css], ["V", "0", "cssText"], ["V", "0", "conditionText"]]})
        docs.append({"g": "media-general-sheet-replace", "ops": [["C"], ["R", css], ["V", "0", "cssText"], ["V", "0", "conditionText"]]})
    return docs


# CSSOM, "serialize a CSS declaration block": a shorthand is not written when a property of its logical group with another mapping logic stands between its longhands; `font` takes
# a `font-stretch` that is a calculation of a percentage a keyword names; `display: layout()` (CSS Layout API) is something neither browser takes.
DECL_INTERMIX = ["margin: 10px; margin-inline: 10px; margin-block: 10px; margin-inline-end: 10px; margin-bottom: 10px;", "margin: 10px; margin-inline: 10px; margin-block: 10px; margin-bottom: 10px;",
                 "margin: 10px; margin-inline-end: 10px;", "margin: 10px; margin-inline: 10px; margin-inline-end: 10px;", "margin: 10px; margin-bottom: 10px;",
                 "margin-top: 10px; margin-left: 10px; margin-right: 10px; margin-bottom: 10px; margin-inline-start: 10px; margin-inline-end: 10px; margin-block-start: 10px; margin-block-end: 10px;",
                 "margin-left: 1px; margin: 10px; margin-inline-end: 10px; margin-top: 3px;",
                 "margin-top: 10px; margin-left: 10px; margin-right: 10px; margin-bottom: 10px; margin-inline: 10px; margin-block-start: 10px; margin-inline-end: 10px;",
                 "padding-top: 1px; padding-inline-start: 2px; padding-left: 1px; padding-right: 1px; padding-bottom: 1px;",
                 "border-top-left-radius: 1px; border-start-start-radius: 2px; border-top-right-radius: 1px; border-bottom-left-radius: 1px; border-end-start-radius: 3px; border-bottom-right-radius: 1px;",
                 "inset: 1px; inset-inline-start: 2px;", "top: 1px; inset-block-start: 2px; right: 1px; bottom: 1px; left: 1px;"]
FONT_STRETCH_CALC = ["calc(50%)", "calc(100%)", "calc(50% + 0%)", "calc(25% * 2)", "calc(50% + 25% * sign(100em - 1px))", "calc(62.5%)", "calc(10% * 5)", "calc(60%)", "calc(200%)", "calc(50% * 4)"]


def declaration_documents():
    docs = []
    for t in DECL_INTERMIX:
        docs.append({"g": "decl-intermix", "ops": [["S", "a{}"], ["Y", "0", t], ["V", "0", "cssText"], ["VS", "0", "margin"], ["VS", "0", "margin-inline"], ["VS", "0", "inset"]]})
    for v in FONT_STRETCH_CALC:
        docs.append({"g": "decl-font-calc", "ops": [["S", "a{font: medium serif}"], ["P", "0", "font-stretch", v, ""], ["VS", "0", "font"], ["VS", "0", "font-stretch"], ["Y", "0", ""]]})
    for v in ["layout(test1)", "layout()", "layout(test3, invalid)", "inline-layout(a)", "block layout(a)", "layout(a) flow"]:
        docs.append({"g": "decl-display-layout", "ops": [["S", "a{}"], ["P", "0", "display", v, ""], ["VS", "0", "display"], ["V", "0", "cssText"]]})
    return docs


# Two documents on one registry: the second is an iframe of the page, 640 by 400 where the page is 1280 by 800 (the op D2), and an op whose first argument is "@2" runs in it. The
# sheets of both are numbered together; the current sheet is a document's own. What is asked: whether the media environment is a document's own (matchMedia), whether a sheet or a
# rule of one shows in the other, and what adopting a sheet into the document that did not construct it does ("NotAllowedError", CSSOM: "The sheet's constructor document is not
# the same as the document").
TWO_DOC_MEDIA = ["(min-width: 641px)", "(min-width: 640px)", "(max-width: 640px)", "(width: 640px)", "(width: 1280px)", "(min-width: 1280px)", "(min-width: 100px)", "(orientation: landscape)",
                 "(min-aspect-ratio: 16/10)", "(max-aspect-ratio: 16/10)", "print", "screen", "(min-height: 401px)", "(height: 400px)", "(height: 800px)", "(min-width: 700px) and (max-width: 1300px)",
                 "not all", "bogus"]


def two_document_documents():
    docs = []
    for m in TWO_DOC_MEDIA:
        docs.append({"g": "two-docs", "ops": [["D2"], ["MM", m], ["MM", "@2", m]]})
    # adoption: a sheet a document constructed, and one it did not
    for adopt in (["AD", "0"], ["AD", "@2", "1"]):
        docs.append({"g": "two-docs", "ops": [["D2"], ["C"], ["C", "@2"], adopt, ["AD", "@2", "0"], ["AD", "1"], ["AD", "0,1"], ["AD", "@2", "0,1"], ["AD", ""], ["AD", "@2", ""]]})
    docs.append({"g": "two-docs", "ops": [["D2"], ["S", "a{}"], ["S", "@2", "a{}"], ["AD", "0"], ["AD", "@2", "1"], ["AD", "@2", "0"]]})
    docs.append({"g": "two-docs", "ops": [["D2"], ["C"], ["R", "a{color:red}"], ["C", "@2"], ["R", "@2", "b{}"], ["AD", "0"], ["AD", "@2", "1"], ["AD", "@2", "0"], ["AD", "1"], ["AD", "0"], ["AD", "@2", ""]]})
    docs.append({"g": "two-docs", "ops": [["D2"], ["C"], ["R", "a{color:red}"], ["AD", "0"], ["R", "a{color:blue} c{}"], ["C", "@2"], ["AD", "@2", "1"], ["AD", "@2", "0,1"]]})
    # isolation: the sheets of the two documents are two sheets
    docs.append({"g": "two-docs", "ops": [["D2"], ["S", "a{color:red}"], ["S", "@2", "a{color:red}"], ["I", "", 0, "b{}"], ["V", "", "cssText"], ["I", "@2", "", 0, "c{}"], ["D", "", 0],
                                          ["U", "0"], ["D", "", 0]]})
    docs.append({"g": "two-docs", "ops": [["D2"], ["S", "@media (min-width: 700px){a{}} b{}"], ["S", "@2", "@media (min-width: 700px){a{}} b{}"], ["MM", "(min-width: 700px)"], ["MM", "@2", "(min-width: 700px)"],
                                          ["M", "0", "(max-width: 100px)"], ["V", "@2", "0", "cssText"], ["V", "0", "cssText"]]})
    # a sheet reached from the other document's side is the same sheet
    docs.append({"g": "two-docs", "ops": [["D2"], ["C"], ["U", "@2", "0"], ["R", "@2", "a{color:red}"], ["I", "@2", "", 0, "b{}"], ["V", "0", "cssText"], ["DIS", "1"], ["AD", "@2", "0"], ["AD", "0"]]})
    docs.append({"g": "two-docs", "ops": [["D2"], ["S", "@2", "a{}"], ["S", "@2", "b{}"], ["U", "0"], ["V", "", "cssText"], ["U", "@2", "1"], ["DIS", "@2", "1"], ["DIS", "1"]]})
    # an op in a second document that was never made, and a second one made twice
    docs.append({"g": "two-docs", "ops": [["S", "a{}"], ["S", "@2", "a{}"], ["MM", "@2", "print"], ["D2"], ["S", "@2", "b{}"], ["D2"], ["S", "@2", "c{}"], ["I", "@2", "", 0, "d{}"], ["MM", "@2", "(width: 640px)"]]})
    return docs


# A pseudo-element named -webkit-<ident> that no browser knows is valid (Selectors 4, "Compatibility"): the selectors both pinned browsers accept and refuse, and the ones on which
# they differ (a vendor pseudo-class, a pseudo-class after the pseudo-element that only one takes).
WEBKIT_SELECTORS = ["span::-webkit-something-invalid",
                    "::-WeBkIt-sOmEtHiNg-NoNeXiSt123",
                    "::-webkit-\\ escaped",
                    "::-webkitfoo",
                    "::-webkit-",
                    "::-webkit-a:hover",
                    "::-webkit-a:active",
                    "::-webkit-a:first-child",
                    "::-webkit-a::before",
                    "::before::-webkit-a",
                    "::-webkit-a::-webkit-b",
                    "::-webkit-input-placeholder",
                    "::-webkit-file-upload-button",
                    "::-webkit-scrollbar",
                    "::-webkit-scrollbar-thumb:horizontal",
                    "::-webkit-slider-thumb",
                    "::-webkit-search-cancel-button",
                    "::-webkit-a(1)",
                    "::-webkit-a(b)",
                    "a:-webkit-any-link",
                    "a:-webkit-foo",
                    "a:-webkit-autofill",
                    ":-webkit-full-screen",
                    "::-webkit-details-marker",
                    "::-webkit-validation-bubble",
                    "a::-webkit-a, b",
                    "*::-webkit-a",
                    "a > ::-webkit-a",
                    "a::-webkit-a:is(.x)",
                    "a:not(::-webkit-a)",
                    "a:is(::-webkit-a)",
                    "a:where(::-webkit-a)",
                    "::after::-webkit-a",
                    ":host::-webkit-a",
                    "::part(x)::-webkit-a",
                    "::-webkit-a::part(x)",
                    "::-moz-foo",
                    "::-ms-foo",
                    "::-o-foo",
                    "::-webkit-A",
                    "::-WEBKIT-a",
                    "::-webkit-a-",
                    "::-webkit-1",
                    "::-webkit--x",
                    "::-webkit-\\31 x",
                    "::-webkit-a\\(",
                    "::-webkit-a:hover:active",
                    "::-webkit-a::selection",
                    "::-webkit-a:focus-visible"]


def selector_webkit_documents():
    return [{"g": "selector-webkit", "ops": [["S", s + " {color:red}"], ["V", "0", "selectorText"], ["V", "0", "cssText"]]} for s in WEBKIT_SELECTORS]


# `all` is written as its longhands (CSS Cascade 5; CSSOM): what a declaration block says of it after a cssText, a setProperty and a removeProperty. Both browsers agree that a block
# in which something is left of `all` besides a CSS-wide keyword is the list of every longhand, in each engine's own order and with each engine's own set of properties, so those
# are not asked: the texts here leave `all` whole or leave nothing of it.
ALL_TEXTS = ["all: revert", "width: 50px; all: revert", "all: revert; width: 50px; all: unset", "margin: initial; all: revert", "margin-top: 10px; all: revert", "all: inherit",
             "width: 100px; all: inherit; height: inherit", "direction: ltr; all: inherit; unicode-bidi: plaintext", "width: 100px; --a: a; all: inherit; --b: b; height: inherit",
             "--foo: bar; all: initial", "all: inherit !important", "all: var(--x)", "all: initial; width: 5px; all: unset", "all: bogus", "all: 10px", "ALL: INHERIT"]


def all_documents():
    docs = []
    for t in ALL_TEXTS:
        docs.append({"g": "decl-all", "ops": [["S", "a{}"], ["Y", "0", t], ["V", "0", "cssText"], ["VS", "0", "all"], ["VS", "0", "width"], ["VS", "0", "margin"], ["VS", "0", "font"],
                                               ["VS", "0", "direction"], ["VP", "0", "all"], ["VP", "0", "width"]]})
    for t in ["width: 50px; color: green; direction: rtl", "all: revert", "width: 40px; all: revert", "--a: 1; all: revert; direction: rtl", "all: inherit !important"]:
        docs.append({"g": "decl-all", "ops": [["S", "a{}"], ["Y", "0", t], ["Q", "0", "all"], ["V", "0", "cssText"], ["VS", "0", "all"], ["VS", "0", "direction"], ["VL", "0"]]})
    for v in ["revert", "unset", "inherit", "initial", "revert-layer", "bogus", "10px", "INHERIT", " inherit ", "var(--x)"]:
        docs.append({"g": "decl-all", "ops": [["S", "a{width:5px}"], ["P", "0", "all", v, ""], ["V", "0", "cssText"], ["VS", "0", "all"], ["VS", "0", "width"]]})
        docs.append({"g": "decl-all", "ops": [["S", "a{all:revert}"], ["P", "0", "all", v, "important"], ["V", "0", "cssText"], ["VP", "0", "width"], ["VP", "0", "all"]]})
    return docs


_FIXED = None


def labelled(group, docs):
    return [dict(d, g=d.get("g", group)) for d in docs]


def computed_documents():
    """getComputedStyle() after a mutation: the page is inside an element with display: none, where the resolved value is the computed one. Each document reads, changes the sheet, and reads again."""
    docs = []
    base = ("p{color:red} div p{margin-top:1px} #b{color:green} .x{margin-top:7px} @media (min-width:100px){ #a{color:navy} } @media print{ #a{color:black} }", "<div><p id=a class=x>t</p><p id=b>u</p></div><span id=s>v</span>")
    steps = [
        [["I", "", 0, "#a{color:blue}"]],
        [["I", "", 0, "p{color:blue !important}"]],
        [["I", "", 99, "#a{color:blue}"], ["I", "", 4, "#a{color:blue}"]],
        [["D", "", 0]],
        [["D", "", 2]],
        [["T", "0", "#b"]],
        [["T", "0", "#a"]],
        [["T", "0", "p, span"]],
        [["P", "0", "color", "blue", ""]],
        [["P", "0", "color", "blue", "important"]],
        [["P", "1", "margin-top", "9px", ""]],
        [["P", "0", "--x", "5", ""], ["I", "", 0, "#a{width:var(--x)}"]],
        [["Q", "0", "color"]],
        [["Y", "0", "color:teal;margin-top:2px"]],
        [["M", "4", "print"]],
        [["M", "5", "all"]],
        [["I", "", 0, "@media all{ #a{color:olive} }"]],
        [["I", "", 0, "@supports (color:red){ #a{color:maroon} }"]],
        [["I", "", 0, "@layer z{ #a{color:purple} }"]],
        [["R", "p{color:aqua}"]],
        [["DIS", "1"]],
        [["I", "", 0, "#s{color:fuchsia}"]],
    ]
    props = ["color", "margin-top", "width", "font-size"]
    for st in steps:
        ops = [["H", base[0], base[1]]]
        for sel in ("#a", "#b", "#s"):
            ops.append(["GC", sel, "color"])
        ops.append(["GC", "#a", "margin-top"])
        ops += st
        for sel in ("#a", "#b", "#s"):
            ops.append(["GC", sel, "color"])
        for p in props[1:]:
            ops.append(["GC", "#a", p])
        docs.append({"g": "computed", "ops": ops})
    return docs


def fixed_documents():
    global _FIXED
    if _FIXED is None:
        _FIXED = labelled("order", css_ruleops_gen.order_documents()) + open_items_documents() + setter_documents() + page_documents() + computed_documents()
    return _FIXED


class Gen:
    def __init__(self, seed, index):
        self.index = index
        self.rng = random.Random(seed * 1000003 + index)

    def document(self):
        fixed = fixed_documents()
        if self.index < len(fixed):
            return fixed[self.index]
        r = self.rng
        ops = [["C"] if r.random() < 0.15 else ["S", r.choice(SHEETS)]]
        if r.random() < 0.15:
            ops.append(["C"])
        for _ in range(r.randint(1, 6)):
            ops.append(self.op())
        return {"ops": ops}

    def op(self):
        r = self.rng
        path = r.choice(PATHS)
        x = r.random()
        if x < 0.14:
            return ["I", path, r.choice(css_ruleops_gen.INDEXES), r.choice(css_ruleops_gen.RULES if path == "" or r.random() < 0.6 else css_ruleops_gen.RULES + css_ruleops_gen.NESTED_ONLY)]
        if x < 0.20:
            return ["D", path, r.choice([0, 0, 1, 2, 9])]
        if x < 0.23:
            return ["A", path, r.choice(css_ruleops_gen.KEYFRAMES)]
        if x < 0.25:
            return ["X", path, r.choice(css_ruleops_gen.SELECTS)]
        if x < 0.27:
            return ["F", path, r.choice(css_ruleops_gen.SELECTS)]
        if x < 0.34:
            return ["T", path, r.choice(SELECTORS)]
        if x < 0.38:
            return ["K", path, r.choice(KEYS)]
        if x < 0.41:
            return ["N", path, r.choice(NAMES)]
        if x < 0.45:
            a = r.choice(ATTRS)
            return ["E", path, a[0], a[1]]
        if x < 0.55:
            p = r.choice(PROPS)
            return ["P", path, p[0], p[1], p[2]]
        if x < 0.59:
            return ["Q", path, r.choice(REMOVES)]
        if x < 0.66:
            return ["Y", path, r.choice(CSSTEXTS)]
        if x < 0.68:
            return ["Z", path, r.choice(RULE_TEXTS)]
        if x < 0.73:
            return ["M", path, r.choice(MEDIA)]
        if x < 0.76:
            return ["MA", path, r.choice(MEDIA_ITEMS)]
        if x < 0.79:
            return ["MD", path, r.choice(MEDIA_ITEMS)]
        if x < 0.82:
            f = r.choice(FFV)
            return ["W", path, f[0], f[1], f[2]]
        if x < 0.84:
            return ["R", r.choice(SHEETS)]
        if x < 0.86:
            return ["DIS", r.choice(["0", "1"])]
        if x < 0.89:
            return ["AD", r.choice(ADOPTED)]
        if x < 0.91:
            return ["MM", r.choice(MATCH)]
        if x < 0.93:
            return ["U", r.choice([0, 1])]
        if x < 0.95:
            return ["C"]
        if x < 0.97:
            return ["V", path, r.choice(READS)]
        if x < 0.98:
            return ["VS", path, r.choice(REMOVES)]
        if x < 0.99:
            return ["VP", path, r.choice(REMOVES)]
        return ["VL", path]
