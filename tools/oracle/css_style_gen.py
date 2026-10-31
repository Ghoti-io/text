#!/usr/bin/env python3
"""The population of the computed-style differential (tools/oracle/css_style_diff.py): which documents are asked, and which properties of which elements.

    css_style_gen.py --seed N --count M [--out FILE]

A population is a pure function of the seed, the count, the SVG fixtures of tests/data/css/svg, the pinned web-platform-tests vectors and the pin; the same
arguments make the same file on every host. One line of the output is a document, as JSON:

    {"name", "mode": "h"|"x", "text", "queries": [[id, [property, ...]], ...], "items": [[id, property, description, origin], ...]}

`items` says what each answer is: `description` is what the element declares for the property (`style: fill: red`, `attr: stroke-width=2`, or empty
when the value is inherited or initial) and `origin` is one of

  fixture    an element of an SVG fixture of tests/data/css/svg (the properties of its .props file; this library's expected dumps are checked by the unit tests)
  value      one value of a property (`style="property: value"`), taken from the web-platform-tests' valid and computed vectors, in an HTML tree
  attribute  one value of a presentation attribute on an SVG element, in an inline SVG
  tree       an element of a generated tree of HTML and SVG elements with `style` and presentation attributes, `inherit`, `initial`, em, rem and percentages
  control    a case whose answer is known in advance
  probe      `initial` for the property: every engine that has the property computes something for it

**No style sheet and a display:none tree.** There is no `<style>` and no selector (those wait for C3). Every tree is inside an element with
`display: none`, which makes the CSSOM resolved value of a layout-dependent property (`width`, `margin-left`, `top`) the computed value, as it is for an
element with no box; `display` is a property of the elements inside it like another.

**The fixed context.** The default font size is the browser's, and the viewport is 1280 by 800 at a device pixel ratio of 1. Font-relative metrics (`ex`,
`ch`, `cap`, `ic`) are measured in the browser (style_driver.mjs metrics) and fed to this library's callback; a font that is not in the table makes
the element's computation unsupported, and the items of that element are not compared.
"""

import argparse
import collections
import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import css_prop_gen  # noqa: E402

SVG_NS = "http://www.w3.org/2000/svg"
FIXTURES = os.path.join(ROOT, "tests", "data", "css", "svg")

# the properties asked of every element of a tree: the inherited ones people write, and the presentation attributes
BASELINE = ["color", "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
            "stroke-dasharray", "stroke-dashoffset", "opacity", "font-size", "font-weight", "font-style", "font-family", "letter-spacing", "word-spacing",
            "text-anchor", "direction", "visibility", "display", "paint-order", "shape-rendering", "clip-rule", "pointer-events", "cursor", "line-height"]

# a presentation attribute on an SVG element and the values it is given: plain values, the attribute-only spellings, and mistakes
ATTRIBUTE_VALUES = {
    "default": ["inherit", "initial", "unset", "banana", ""],
    "length": ["5", "-3.5", "10px", "2em", "1.5rem", "50%", "calc(1px + 2px)", "calc(2 * (1px + 1em))", "3pt", "0.5in", "0", "1e2", " 7 ", "12px34", "+4", "5.", ".5"],
    "number": ["0.5", "2", "-1", "50%", "1e-1", "abc"],
    "color": ["red", "#abc", "#aabbccdd", "rgb(1 2 3)", "rgba(1, 2, 3, 0.5)", "hsl(120 100% 50%)", "currentColor", "transparent", "12px", "light-dark(red, blue)"],
    "paint": ["red", "none", "url(#a)", "url(#a) red", "currentColor", "context-fill", "12px", "#123", "rgb(10 20 30 / 50%)"],
    "keyword": ["bold", "italic", "none", "auto", "visible", "hidden", "banana"],
}
ATTRIBUTE_KIND = {
    "cx": "length", "cy": "length", "r": "length", "rx": "length", "ry": "length", "x": "length", "y": "length", "width": "length", "height": "length",
    "stroke-width": "length", "stroke-dashoffset": "length", "letter-spacing": "length", "word-spacing": "length", "font-size": "length",
    "stroke-dasharray": "length", "stroke-miterlimit": "number", "opacity": "number", "fill-opacity": "number", "stroke-opacity": "number",
    "flood-opacity": "number", "stop-opacity": "number", "color": "color", "flood-color": "color", "lighting-color": "color", "stop-color": "color",
    "fill": "paint", "stroke": "paint", "font-weight": "keyword", "font-style": "keyword", "display": "keyword", "visibility": "keyword",
    "text-anchor": "keyword", "overflow": "keyword",
}
SPECIAL_ATTRIBUTES = {
    "transform": ["translate(10,20)", "translate(10)", "rotate(45)", "rotate(45 10 20)", "scale(2)", "scale(2 3)", "skewX(30)", "matrix(1 0 0 1 5 6)",
                  "translate(10px, 20px) rotate(30deg)", "none", "", "rotate(1 2)", "bogus(1)", "translate(1,)", "scale(2) translate(3)"],
    "patternTransform": ["scale(2)", "rotate(30)"],
    "gradientTransform": ["scale(2)", "rotate(30)"],
    "d": ["M0 0 L10 10", "M 0,0 h 5 v 5 z", "", "none", "path('M0 0')", "bogus"],
}
# a presentation attribute that sets a shorthand is read through a longhand of it (getComputedStyle writes the shorthand, which this library does not)
LONGHAND = {"font-variant": "font-variant-caps", "text-decoration": "text-decoration-line", "white-space": "white-space-collapse", "overflow": "overflow-x",
            "mask": "mask-image", "marker": "marker-start"}
ATTRIBUTE_ELEMENT = {"cx": "circle", "cy": "circle", "r": "circle", "rx": "rect", "ry": "rect", "x": "rect", "y": "rect", "width": "rect", "height": "rect",
                     "d": "path", "patternTransform": "pattern", "gradientTransform": "linearGradient"}


def read_table():
    rows = []
    for line in open(os.path.join(ROOT, "tools", "css", "svg_presentation.tsv"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        a, p, ns, el, syn = line.rstrip("\n").split("\t")
        rows.append((a, p, ns, el, syn))
    return rows


def ours_properties(binary):
    """{name: (class, inherited, shorthand-or-alias)} from css-style-ours --properties."""
    import subprocess
    out = {}
    run = subprocess.run([binary, "--properties"], stdout=subprocess.PIPE, check=True)
    for line in run.stdout.decode("utf-8").split("\n"):
        f = line.split("\t")
        if len(f) == 4:
            out[f[0]] = (f[1], f[2] == "1", f[3] == "1")
    return out


def html_escape(s):
    return s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")


def doc(name, mode, text, queries, items):
    return {"name": name, "mode": mode, "text": text, "queries": queries, "items": items}


def wrap_html(body):
    """The tree goes inside an element with `display: none`: every resolved value is then the computed value."""
    return "<!doctype html><html><head><title>t</title></head><body><div id=\"wrapper\" style=\"display: none\">" + body + "</div></body></html>"


# ---- fixtures --------------------------------------------------------------------------------------------------------------

def fixtures():
    out = []
    if not os.path.isdir(FIXTURES):
        return out
    for name in sorted(os.listdir(FIXTURES)):
        if not name.endswith(".svg"):
            continue
        base = name[:-4]
        text = open(os.path.join(FIXTURES, name), encoding="utf-8").read()
        props = open(os.path.join(FIXTURES, base + ".props"), encoding="utf-8").read().split()
        ids = re.findall(r'\bid="([^"]+)"', text)
        queries = [[i, props] for i in ids]
        # the display-contents fixture names its elements in the key: which element a browser hoists is the divergence
        items = [[i, p, ("attr: display=" + i) if base == "display-contents" else "", "fixture"] for i in ids for p in props]
        out.append(doc("fixture-" + base, "x", text, queries, items))
    return out


# ---- the controls ---------------------------------------------------------------------------------------------------------

def controls():
    """Known answers: a plain declaration, an inherited one, an attribute and the style attribute over it."""
    body = ("<div id=\"c1\" style=\"font-size: 20px; color: red\"><div id=\"c2\" style=\"width: 2em; margin-left: 10px\"></div></div>"
            "<div id=\"c6\" style=\"margin: 1px 2px 3px; padding: 4px 5px 6px 7px\"></div>"
            "<div id=\"c7\" style=\"color-scheme: dark; color: light-dark(red, blue)\"></div><div id=\"c8\" style=\"color-scheme: light; color: light-dark(red, blue)\"></div>"
            "<div id=\"c9\" style=\"color: light-dark(red, blue)\"></div>"
            "<svg id=\"c3\" xmlns=\"%s\"><g id=\"c4\" fill=\"red\" stroke-width=\"2\"><rect id=\"c5\" style=\"stroke-width: 5\"/></g></svg>" % SVG_NS)
    queries = [["c1", ["font-size", "color"]], ["c2", ["width", "margin-left", "font-size", "color"]], ["c4", ["fill", "stroke-width"]], ["c5", ["fill", "stroke-width"]],
               ["c6", ["margin-left", "margin-right", "margin-bottom", "padding-left", "padding-bottom", "margin-top"]],
               ["c7", ["color"]], ["c8", ["color"]], ["c9", ["color"]]]
    items = [["c1", "font-size", "style: font-size: 20px", "control"], ["c1", "color", "style: color: red", "control"],
             ["c2", "width", "style: width: 2em", "control"], ["c2", "margin-left", "style: margin-left: 10px", "control"],
             ["c2", "font-size", "", "control"], ["c2", "color", "", "control"], ["c4", "fill", "attr: fill=red", "control"],
             ["c4", "stroke-width", "attr: stroke-width=2", "control"], ["c5", "fill", "", "control"], ["c5", "stroke-width", "style: stroke-width: 5", "control"],
             ["c6", "margin-left", "style: margin: 1px 2px 3px", "control"], ["c6", "margin-right", "style: margin: 1px 2px 3px", "control"],
             ["c6", "margin-bottom", "style: margin: 1px 2px 3px", "control"], ["c6", "padding-left", "style: padding: 4px 5px 6px 7px", "control"],
             ["c6", "padding-bottom", "style: padding: 4px 5px 6px 7px", "control"], ["c6", "margin-top", "style: margin: 1px 2px 3px", "control"],
             ["c7", "color", "style: color-scheme: dark", "control"], ["c8", "color", "style: color-scheme: light", "control"], ["c9", "color", "", "control"]]
    return [doc("control", "h", wrap_html(body), queries, items)]


CONTROL_ANSWERS = [
    # (document, id, property, what all three must say)
    ("control", "c1", "font-size", "20px"), ("control", "c1", "color", "rgb(255, 0, 0)"), ("control", "c2", "width", "40px"),
    ("control", "c2", "margin-left", "10px"), ("control", "c2", "font-size", "20px"), ("control", "c2", "color", "rgb(255, 0, 0)"),
    ("control", "c4", "stroke-width", "2px"), ("control", "c5", "stroke-width", "5px"), ("control", "c5", "fill", "rgb(255, 0, 0)"),
    ("control", "c6", "margin-left", "2px"), ("control", "c6", "margin-right", "2px"), ("control", "c6", "margin-bottom", "3px"), ("control", "c6", "margin-top", "1px"),
    ("control", "c6", "padding-left", "7px"), ("control", "c6", "padding-bottom", "6px"),
    # the used color scheme: an element that says dark is dark in either environment, one that says light is light in either, and one that says nothing is light
    ("control", "c7", "color", "rgb(0, 0, 255)"), ("control", "c8", "color", "rgb(255, 0, 0)"), ("control", "c9", "color", "rgb(255, 0, 0)"),
]


# ---- value probes: one value of one property -------------------------------------------------------------------------------

def wpt_values(wpt_dir):
    """property -> distinct values the extracted vectors give it (valid and computed vectors), in the order of the files."""
    by = collections.OrderedDict()
    for kind in ("valid", "computed"):
        path = os.path.join(wpt_dir, kind + ".json")
        if not os.path.exists(path):
            raise SystemExit("css_style_gen: %s is not there (run tools/css/extract-wpt-props.py)" % path)
        for v in json.load(open(path, encoding="utf-8")):
            prop = v.get("prop")
            if not prop or v.get("value") is None or v.get("pseudo") or v.get("custom"):
                continue
            prop = css_prop_gen.dash(prop)
            if "\n" in v["value"] or ";" in v["value"] or "{" in v["value"] or "}" in v["value"] or "!" in v["value"]:
                continue
            by.setdefault(prop, [])
            if v["value"] not in by[prop]:
                by[prop].append(v["value"])
    return by


def value_docs(rng, wpt_dir, props, per_property, chunk=60):
    """Each property with up to `per_property` values: an HTML element for each, children of a parent with a font-size of 20px (so that `em` and `inherit` mean
    something), in documents of `chunk` elements."""
    by = wpt_values(wpt_dir)
    items = []
    for prop in sorted(by):
        if prop not in props or props[prop][2]:
            continue
        vals = list(by[prop])
        rng.shuffle(vals)
        for v in vals[:per_property]:
            items.append((prop, v))
    out = []
    for k in range(0, len(items), chunk):
        part = items[k:k + chunk]
        body = "<div id=\"parent\" style=\"font-size: 20px; line-height: 30px\">"
        queries, its = [], []
        for n, (prop, v) in enumerate(part):
            i = "v%d" % n
            body += "<div id=\"%s\" style=\"%s\"></div>" % (i, html_escape("%s: %s" % (prop, v)))
            queries.append([i, [prop]])
            its.append([i, prop, "style: %s: %s" % (prop, v), "value"])
        body += "</div>"
        out.append(doc("value-%d" % (k // chunk), "h", wrap_html(body), queries, its))
    return out


# ---- the color family ------------------------------------------------------------------------------------------------------

COLOR_BASES = ["red", "rebeccapurple", "transparent", "currentcolor", "#abc", "#12345678", "rgb(10 20 30)", "rgb(10 20 30 / 40%)", "rgba(255, 128, 0, 0.5)", "hsl(120 50% 40%)",
               "hsl(200deg 100% 50% / 0.3)", "hwb(60 10% 20%)", "lab(50% 40 59.5)", "lch(52% 72 51)", "oklab(60% 0.1 -0.05)", "oklch(70% 0.15 200)",
               "color(srgb 0.2 0.4 0.6)", "color(display-p3 0.9 0.2 0.3)", "color(a98-rgb 0.5 0.5 0.5)", "color(prophoto-rgb 0.3 0.6 0.9)", "color(rec2020 0.1 0.8 0.4)",
               "color(xyz 0.2 0.3 0.4)", "color(xyz-d50 0.2 0.3 0.4)", "color(srgb-linear 0.5 0.25 0.125)", "oklch(none 0.1 none)", "rgb(none 20 30)", "hsl(none 50% 50%)"]
COLOR_RECT = ["srgb", "srgb-linear", "display-p3", "a98-rgb", "prophoto-rgb", "rec2020", "lab", "oklab", "xyz", "xyz-d50", "xyz-d65"]
COLOR_POLAR = ["hsl", "hwb", "lch", "oklch"]
COLOR_HUE = ["shorter", "longer", "increasing", "decreasing"]
COLOR_PERCENT = ["", "50%", "30%", "0%", "100%", "10%", "75%", "25%", "calc(10% * 2)"]
COLOR_RELATIVE = {
    "rgb": ["r g b", "r g b / alpha", "calc(r / 2) g b", "b g r / 0.5", "255 g b", "r 0 b / calc(alpha * 0.5)", "none g b"],
    "hsl": ["h s l", "h s l / alpha", "calc(h + 30) s l", "h calc(s / 2) l", "h s calc(l + 10)", "0 s l"],
    "hwb": ["h w b", "h w b / alpha", "calc(h + 180) w b", "h 0% 0%"],
    "lab": ["l a b", "l a b / alpha", "calc(l / 2) a b", "l calc(a * -1) b", "50 a b"],
    "lch": ["l c h", "l c h / alpha", "l c calc(h + 60)", "calc(l + 10) c h"],
    "oklab": ["l a b", "l a b / alpha", "calc(l * 0.5) a b", "l b a"],
    "oklch": ["l c h", "l c h / alpha", "l calc(c * 0.5) h", "l c calc(h + 90)", "0.5 0.1 h"],
}
COLOR_RELATIVE_FN = [("srgb", "r g b"), ("srgb", "calc(r / 2) g b / alpha"), ("display-p3", "r g b"), ("display-p3", "r calc(g * 0.5) b"), ("srgb-linear", "r g b"),
                     ("xyz", "x y z"), ("xyz-d50", "x y z"), ("xyz-d65", "x calc(y / 2) z / alpha"), ("a98-rgb", "r g b"), ("rec2020", "r g b / 0.5")]


def color_values(rng, count):
    """Values of the color family: color-mix() in every space with the hue methods and percentages, relative colors in every function, color() with every space,
    contrast-color(), light-dark() and nestings of them."""
    out = []
    def base():
        return rng.choice(COLOR_BASES)
    for space in COLOR_RECT + COLOR_POLAR:
        for _ in range(6):
            methods = [""]
            if space in COLOR_POLAR:
                methods = [""] + ["%s hue" % h for h in COLOR_HUE]
            m = rng.choice(methods)
            p1, p2 = rng.choice(COLOR_PERCENT), rng.choice(COLOR_PERCENT)
            c1 = (base() + " " + p1).strip() if rng.random() < 0.5 else (p1 + " " + base()).strip()
            c2 = (base() + " " + p2).strip()
            out.append("color-mix(in %s%s, %s, %s)" % (space, (" " + m) if m else "", c1, c2))
    for _ in range(max(0, count // 6)):
        space = rng.choice(COLOR_RECT + COLOR_POLAR)
        n = rng.choice([1, 3, 3, 4])
        parts = ", ".join((base() + " " + rng.choice(COLOR_PERCENT)).strip() for _ in range(n))
        out.append("color-mix(in %s, %s)" % (space, parts))
        out.append("color-mix(in %s, color-mix(in srgb, %s, %s), %s)" % (space, base(), base(), base()))
    for fn, chans in COLOR_RELATIVE.items():
        for ch in chans:
            for _ in range(2):
                out.append("%s(from %s %s)" % (fn, base(), ch))
    for space, ch in COLOR_RELATIVE_FN:
        for _ in range(2):
            out.append("color(from %s %s %s)" % (base(), space, ch))
    for space in COLOR_RECT:
        for vals in ("0 0 0", "1 1 1", "0.5 0.25 0.75", "none 0.5 1", "20% 40% 60%", "1.5 -0.2 0.3", "0.1 0.2 0.3 / 50%", "calc(1 / 2) 0 0"):
            if space in ("lab", "oklab"):
                continue
            out.append("color(%s %s)" % (space, vals))
    for fn, vals in (("lab", ["50% 40 -30", "100% 0 0", "0% 0 0", "120% 200 200", "none none none", "50 125 125 / 0.5", "calc(50 + 10)% 20 30"]),
                     ("lch", ["50% 40 300", "100% 150 0", "50% 0 none", "20% 230 360", "50% 40 -60deg", "50% 40 0.5turn", "50% 40 1rad"]),
                     ("oklab", ["0.5 0.1 -0.1", "1 0 0", "0 0 0", "1.2 0.5 0.5", "50% 25% -25%", "none 0.1 0.1"]),
                     ("oklch", ["0.5 0.1 300", "1 0.4 0", "0.5 0 none", "0.2 0.3 -60deg", "50% 50% 0.25turn", "0.5 0.1 3.14159rad"]),
                     ("hwb", ["120 10% 20%", "0 50% 50%", "0 80% 80%", "180 120% 0%", "none 10% 10%", "-120 0% 0%", "360 0% 0%"]),
                     ("hsl", ["120 100% 50%", "0 0% 100%", "400 50% 50%", "-30 50% 50%", "none 50% 50%", "120 150% 50%", "120 50% -10%", "120, 50%, 50%"])):
        for v in vals:
            out.append("%s(%s)" % (fn, v))
    for a in ("red", "white", "black", "#808080", "rgb(10 20 30 / 0.5)", "oklch(0.5 0.1 200)", "transparent"):
        out.append("contrast-color(%s)" % a)
    for _ in range(12):
        out.append("light-dark(%s, %s)" % (base(), base()))
    for _ in range(12):
        out.append("color-mix(in %s, light-dark(%s, %s), %s)" % (rng.choice(COLOR_RECT + COLOR_POLAR), base(), base(), base()))
    rng.shuffle(out)
    return out


def color_docs(rng, count=0, chunk=60):
    """The color values as `color: value` of the elements of a parent with `color: rgb(11 22 33)`, so that currentcolor and light-dark() mean something, in
    documents of `chunk` elements."""
    vals = color_values(rng, count)
    out = []
    for k in range(0, len(vals), chunk):
        part = vals[k:k + chunk]
        body = "<div id=\"parent\" style=\"color: rgb(11 22 33); font-size: 20px\">"
        queries, its = [], []
        for n, v in enumerate(part):
            i = "c%d" % n
            body += "<div id=\"%s\" style=\"%s\"></div>" % (i, html_escape("color: %s" % v))
            queries.append([i, ["color"]])
            its.append([i, "color", "style: color: %s" % v, "value"])
        body += "</div>"
        out.append(doc("color-%d" % (k // chunk), "h", wrap_html(body), queries, its))
    return out


# ---- attribute probes ----------------------------------------------------------------------------------------------------

def attribute_docs(rng, table, chunk=40):
    pairs = []
    for attr, prop, ns, el, syn in table:
        kind = ATTRIBUTE_KIND.get(attr, "default")
        values = list(SPECIAL_ATTRIBUTES.get(attr) or ATTRIBUTE_VALUES[kind] + ([] if kind == "default" else ATTRIBUTE_VALUES["default"][:3]))
        tag = ATTRIBUTE_ELEMENT.get(attr, "rect")
        if el.startswith("!") or el == "*":
            tag = ATTRIBUTE_ELEMENT.get(attr, "rect")
        for v in values:
            pairs.append((attr, prop, tag, v))
    out = []
    for k in range(0, len(pairs), chunk):
        part = pairs[k:k + chunk]
        body = "<svg xmlns=\"%s\" id=\"svg\" width=\"100\" height=\"100\"><g id=\"parent\" font-size=\"20\" color=\"purple\">" % SVG_NS
        queries, its = [], []
        for n, (attr, prop, tag, v) in enumerate(part):
            i = "a%d" % n
            body += "<%s id=\"%s\" %s=\"%s\"/>" % (tag, i, attr, html_escape(v))
            q = LONGHAND.get(prop, prop)
            queries.append([i, [q]])
            its.append([i, q, "attr: %s=%s" % (attr, v), "attribute"])
        body += "</g></svg>"
        out.append(doc("attribute-%d" % (k // chunk), "h", wrap_html(body), queries, its))
    return out


# ---- trees ---------------------------------------------------------------------------------------------------------------

PAINTS = ["red", "green", "blue", "none", "currentColor", "inherit", "#123", "rgb(10 20 30)", "initial", "url(#a)", "context-fill"]
LENGTHS = ["1", "2px", "1.5em", "50%", "3rem", "3pt", "1in", "inherit", "initial", "calc(1px + 1em)", "0", "unset", "10"]
FONT_SIZES = ["10px", "20", "1.5em", "50%", "larger", "smaller", "large", "inherit", "2rem", "12px", "calc(10px + 1em)"]
NUMBERS = ["0", "0.5", "1", "2", "-1", "50%", "inherit", "initial"]
KEYWORDS_FOR = {
    "fill-rule": ["evenodd", "nonzero", "inherit"], "stroke-linecap": ["round", "square", "butt", "inherit"], "stroke-linejoin": ["round", "bevel", "miter", "inherit"],
    "font-weight": ["bold", "lighter", "bolder", "700", "inherit", "initial", "100"], "font-style": ["italic", "oblique", "normal", "inherit", "oblique 10deg"],
    "text-anchor": ["middle", "end", "start", "inherit"], "visibility": ["hidden", "collapse", "visible", "inherit"], "direction": ["rtl", "ltr", "inherit"],
    "display": ["none", "block", "inline", "flex", "grid", "contents", "inherit"], "paint-order": ["stroke", "markers fill", "normal", "fill stroke markers"],
    "pointer-events": ["none", "fill", "all", "inherit"], "shape-rendering": ["crispEdges", "auto", "inherit"], "font-family": ["serif", "monospace", "sans-serif", "inherit"],
}
PROP_VALUES = {
    "fill": PAINTS, "stroke": PAINTS, "color": ["red", "purple", "#123456", "inherit", "currentColor", "rgb(1 2 3 / 50%)", "initial"],
    "stroke-width": LENGTHS, "stroke-dashoffset": LENGTHS, "letter-spacing": LENGTHS, "word-spacing": LENGTHS, "font-size": FONT_SIZES,
    "stroke-dasharray": ["1 2 3", "5", "none", "inherit", "1px, 2px", "1em 2em"], "opacity": NUMBERS, "fill-opacity": NUMBERS, "stroke-opacity": NUMBERS,
    "stroke-miterlimit": ["1", "4", "10", "inherit"], "line-height": ["1.5", "normal", "30px", "150%", "2em", "inherit"],
}
PROP_VALUES.update(KEYWORDS_FOR)
SVG_TAGS = ["g", "rect", "circle", "path", "text", "use", "ellipse", "line"]
SVG_CONTAINERS = ("g",)   # an element inside a shape or a `use` has no computed style in a browser: it is not in the flat tree
HTML_TAGS = ["div", "span", "p", "section"]
TREE_PROPS = sorted(PROP_VALUES)


def tree_doc(rng, n):
    """A document with an HTML wrapper, an inline SVG tree of depth 2 to 4 and an HTML tree, each element with declarations."""
    items, queries = [], []
    ids = []

    def declare(kind, tag):
        """(attribute text, style text, [(property, description)])"""
        attrs, style, desc = [], [], []
        used = set()
        for _ in range(rng.choice([0, 1, 1, 2, 3])):
            prop = rng.choice(TREE_PROPS)
            if prop in used:
                continue
            used.add(prop)
            value = rng.choice(PROP_VALUES[prop])
            if kind == "svg" and rng.random() < 0.6:
                attrs.append('%s="%s"' % (prop, html_escape(value)))
                desc.append((prop, "attr: %s=%s" % (prop, value)))
            else:
                style.append("%s: %s" % (prop, value))
                desc.append((prop, "style: %s: %s" % (prop, value)))
        return attrs, style, desc

    counter = [0]

    def element(kind, depth, max_depth, declared_above):
        counter[0] += 1
        i = "e%d" % counter[0]
        tag = rng.choice(SVG_TAGS if kind == "svg" else HTML_TAGS)
        if kind == "svg" and (depth == 0 or depth < max_depth and rng.random() < 0.5):
            tag = "g"
        attrs, style, desc = declare(kind, tag)
        s = "<%s id=\"%s\"%s%s>" % (tag, i, "".join(" " + a for a in attrs), (' style="%s"' % html_escape("; ".join(style))) if style else "")
        here = dict(declared_above)
        for prop, d in desc:
            here[prop] = d
        ids.append((i, here))
        if depth < max_depth and (kind != "svg" or tag in SVG_CONTAINERS):
            for _ in range(rng.choice([1, 1, 2, 3])):
                s += element(kind, depth + 1, max_depth, here)
        return s + "</%s>" % tag

    body = "<div id=\"root\" style=\"font-size: 16px\">"
    here = {"font-size": "style: font-size: 16px"}
    body += element("html", 0, rng.choice([1, 2, 3]), here)
    body += "<svg xmlns=\"%s\" id=\"svg%d\" width=\"100\" height=\"100\">" % (SVG_NS, n) + element("svg", 0, rng.choice([2, 3, 4]), {}) + "</svg>"
    body += "</div>"
    for i, declared in ids:
        props = sorted(set(BASELINE) | set(declared))
        queries.append([i, props])
        for p in props:
            items.append([i, p, declared.get(p, ""), "tree"])
    return doc("tree-%d" % n, "h", wrap_html(body), queries, items)


# ---- the population -------------------------------------------------------------------------------------------------------

def probes(props):
    """`initial` for every computed property, on one element: the engine that has a property computes a value for it."""
    names = sorted(p for p, (cls, inh, short) in props.items() if not short and cls not in ("unimplemented", "custom"))
    body = "<div id=\"probe\"></div>"
    queries = [["probe", names]]
    items = [["probe", p, "", "probe"] for p in names]
    return [doc("probe", "h", wrap_html(body), queries, items)]


# ---- the cascade: a style sheet, a presentation attribute and the style attribute ------------------------------------------------

CASCADE_CASES = [
    # (name, the sheet, the element's attributes, the property, the expected winner in words)
    ("hint-alone", "", "fill=\"red\"", "fill", "the attribute"),
    ("rule-over-hint", "#e{fill:blue}", "fill=\"red\"", "fill", "the rule"),
    ("universal-over-hint", "*{fill:blue}", "fill=\"red\"", "fill", "the rule of specificity zero over the attribute"),
    ("element-over-hint", "rect{fill:blue}", "fill=\"red\"", "fill", "the rule"),
    ("style-over-rule", "#e{fill:blue}", "fill=\"red\" style=\"fill: green\"", "fill", "the style attribute"),
    ("important-rule-over-style", "#e{fill:blue !important}", "style=\"fill: green\"", "fill", "the important rule"),
    ("important-style-over-important-rule", "#e{fill:blue !important}", "style=\"fill: green !important\"", "fill", "the important style attribute"),
    ("id-over-element", "rect{fill:blue} #e{fill:orange}", "", "fill", "the id rule"),
    ("later-rule-of-equal-specificity", "#e{fill:blue} #e{fill:orange}", "", "fill", "the later rule"),
    ("important-attribute-is-no-important", "#e{fill:blue}", "style=\"stroke: red\" fill=\"green\"", "fill", "the rule"),
    ("shorthand-in-rule-over-hint", "#e{margin:1px 2px}", "", "margin-left", "the rule"),
    ("hint-keeps-what-a-rule-leaves", "#e{stroke:blue}", "fill=\"red\" stroke=\"green\"", "fill", "the attribute"),
]


def cascade_docs():
    out = []
    for name, sheet, attrs, prop, _why in CASCADE_CASES:
        body = "<svg xmlns=\"%s\"><style>%s</style><rect id=\"e\" %s/></svg>" % (SVG_NS, sheet, attrs)
        out.append(doc("cascade-" + name, "h", wrap_html(body), [["e", [prop]]], [["e", prop, "cascade: " + name, "cascade"]]))
    return out


def population(seed, count, ours, wpt_dir, scope="core"):
    rng = random.Random(seed)
    props = ours_properties(ours)
    table = read_table()
    docs = []
    docs += controls()
    docs += probes(props)
    docs += fixtures()
    docs += cascade_docs()
    docs += attribute_docs(rng, table)
    if scope == "full":
        docs += value_docs(rng, wpt_dir, props, 12)
    if scope in ("full", "color"):
        docs += color_docs(rng, count)
    for n in range(count):
        docs.append(tree_doc(rng, n))
    return docs


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--scope", default="core", choices=("core", "full", "color"))
    ap.add_argument("--ours", default=os.path.join(ROOT, "build", "css-style-ours"))
    ap.add_argument("--wpt", default=os.path.join(ROOT, "build", "css-property-vectors"))
    ap.add_argument("--out", default="-")
    args = ap.parse_args(argv)
    docs = population(args.seed, args.count, args.ours, args.wpt, args.scope)
    out = sys.stdout if args.out == "-" else open(args.out, "w", encoding="utf-8")
    for d in docs:
        out.write(json.dumps(d, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
