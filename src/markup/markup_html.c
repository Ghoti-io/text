/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The HTML vocabulary tables and their queries.
 *
 * Source of the tables: the HTML Standard (WHATWG, Living Standard), retrieved
 * 2026-10-11 from
 *   https://html.spec.whatwg.org/multipage/syntax.html, section 13.1.2
 *     "Elements": "Void elements", "Raw text elements", "Escapable raw text
 *     elements" (the leading line feed of "pre", "textarea" and "listing" is
 *     in the tree construction chapter, parsing.html, "The in body insertion mode", the start tag rules), and
 *   https://html.spec.whatwg.org/multipage/indices.html, section "Index",
 *     "Attributes": every row whose Value column is "Boolean attribute", with
 *     the elements of its Elements column; "HTML elements" there means every
 *     HTML element (the empty element string in k_booleans). As retrieved,
 *     that includes `controls` on `img` (the row lists audio, video and img)
 *     and does not include `disablepictureinpicture` or
 *     `disableremoteplayback`, which are not standardized HTML attributes.
 * Nothing here was derived from this code's tests or from another parser; the
 * tests check it against the web-platform-tests tree-construction vectors and
 * the html5lib serializer vectors (tests/test-markup-html.cpp), which can show
 * an entry wrong but not a missing one.
 * The five legacy void elements (basefont, bgsound, frame, keygen, param) are
 * not in the standard's list of void elements but are in its serialization
 * and parsing algorithms' list, so a serializer must treat them so.
 *
 * The names an HTML parser restores in foreign content (k_svg_tags,
 * k_svg_attributes, k_math_attributes) are the standard's tables in
 * parsing.html, "Tree construction": "adjust MathML attributes" (definitionURL),
 * "adjust SVG attributes" (58 names) and the SVG tag name table of the "any
 * other start tag" rule for foreign content (37 names). They were transcribed
 * from parse5 8.0.1's copy of those tables, read out of the pinned image
 * (tools/oracle/containers/html) on 2026-10-11, not from the standard's text,
 * so they were as good as that parser is: **stage 3e checked them against the
 * standard as served on 2026-10-12** (the tree builder's own tables,
 * src/html/html_tree_foreign.c, were extracted from it by script) and parse5's
 * copy was missing `feDropShadow` (its lower case form `fedropshadow` is in the
 * standard's table); it is added here. The attribute tables agree. tools/oracle/html_diff.py's fixed case writes
 * every entry of every table in this file (the elements, the boolean attributes, the SVG and
 * MathML names, and the folded form of each SVG name) and puts it through both references, so a
 * transcription error shows there; that is a check on what the references do, not on the standard. They do not carry contentScriptType, which
 * the standard dropped: that attribute is read lower case, and is why the
 * table is here.
 *
 * All the tables are sorted by plain byte comparison and searched with a binary
 * search. Nothing is allocated. A table that is out of order would make a
 * lookup miss without any error, so tests/test-markup-html.cpp looks up every
 * entry and checks the order.
 */

#include <ghoti.io/text/markup/markup_html.h>

#include <stddef.h>
#include <string.h>
#include <strings.h>

typedef struct {
  const char * name;
  unsigned flags;
} html_element;

/* Sorted by name. */
static const html_element k_elements[] = {
    {"area", GTEXT_MARKUP_HTML_VOID},
    {"base", GTEXT_MARKUP_HTML_VOID},
    {"basefont", GTEXT_MARKUP_HTML_VOID},
    {"bgsound", GTEXT_MARKUP_HTML_VOID},
    {"br", GTEXT_MARKUP_HTML_VOID},
    {"col", GTEXT_MARKUP_HTML_VOID},
    {"embed", GTEXT_MARKUP_HTML_VOID},
    {"frame", GTEXT_MARKUP_HTML_VOID},
    {"hr", GTEXT_MARKUP_HTML_VOID},
    {"img", GTEXT_MARKUP_HTML_VOID},
    {"input", GTEXT_MARKUP_HTML_VOID},
    {"keygen", GTEXT_MARKUP_HTML_VOID},
    {"link", GTEXT_MARKUP_HTML_VOID},
    {"listing", GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE},
    {"meta", GTEXT_MARKUP_HTML_VOID},
    {"param", GTEXT_MARKUP_HTML_VOID},
    {"pre", GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE},
    {"script", GTEXT_MARKUP_HTML_RAW_TEXT},
    {"source", GTEXT_MARKUP_HTML_VOID},
    {"style", GTEXT_MARKUP_HTML_RAW_TEXT},
    {"textarea",
        GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT
            | GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE},
    {"title", GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT},
    {"track", GTEXT_MARKUP_HTML_VOID},
    {"wbr", GTEXT_MARKUP_HTML_VOID},
};

typedef struct {
  const char * attribute;
  const char * element; /* "" : every HTML element */
} html_boolean;

/* Sorted by attribute, then element. */
static const html_boolean k_booleans[] = {
    {"allowfullscreen", "iframe"},
    {"alpha", "input"},
    {"async", "script"},
    {"autofocus", ""},
    {"autoplay", "audio"},
    {"autoplay", "video"},
    {"checked", "input"},
    {"controls", "audio"},
    {"controls", "img"},
    {"controls", "video"},
    {"default", "track"},
    {"defer", "script"},
    {"disabled", "button"},
    {"disabled", "fieldset"},
    {"disabled", "input"},
    {"disabled", "link"},
    {"disabled", "optgroup"},
    {"disabled", "option"},
    {"disabled", "select"},
    {"disabled", "textarea"},
    {"formnovalidate", "button"},
    {"formnovalidate", "input"},
    {"headingreset", ""},
    {"inert", ""},
    {"ismap", "img"},
    {"itemscope", ""},
    {"loop", "audio"},
    {"loop", "video"},
    {"multiple", "input"},
    {"multiple", "select"},
    {"muted", "audio"},
    {"muted", "video"},
    {"nomodule", "script"},
    {"novalidate", "form"},
    {"open", "details"},
    {"open", "dialog"},
    {"playsinline", "video"},
    {"readonly", "input"},
    {"readonly", "textarea"},
    {"required", "input"},
    {"required", "select"},
    {"required", "textarea"},
    {"reversed", "ol"},
    {"selected", "option"},
    {"shadowrootclonable", "template"},
    {"shadowrootcustomelementregistry", "template"},
    {"shadowrootdelegatesfocus", "template"},
    {"shadowrootserializable", "template"},
};

/* The SVG tag names the HTML parser restores after lower-casing; sorted. */
static const char * const k_svg_tags[] = {
    "altGlyph", "altGlyphDef", "altGlyphItem", "animateColor",
    "animateMotion", "animateTransform", "clipPath", "feBlend",
    "feColorMatrix", "feComponentTransfer", "feComposite",
    "feConvolveMatrix", "feDiffuseLighting", "feDisplacementMap",
    "feDistantLight", "feDropShadow", "feFlood", "feFuncA", "feFuncB", "feFuncG",
    "feFuncR", "feGaussianBlur", "feImage", "feMerge", "feMergeNode", "feMorphology",
    "feOffset", "fePointLight", "feSpecularLighting", "feSpotLight",
    "feTile", "feTurbulence", "foreignObject", "glyphRef", "linearGradient",
    "radialGradient", "textPath",
};

/* The SVG attribute names the HTML parser restores after lower-casing; sorted. */
static const char * const k_svg_attributes[] = {
    "attributeName", "attributeType", "baseFrequency", "baseProfile",
    "calcMode", "clipPathUnits", "diffuseConstant", "edgeMode",
    "filterUnits", "glyphRef", "gradientTransform", "gradientUnits",
    "kernelMatrix", "kernelUnitLength", "keyPoints", "keySplines",
    "keyTimes", "lengthAdjust", "limitingConeAngle", "markerHeight",
    "markerUnits", "markerWidth", "maskContentUnits", "maskUnits",
    "numOctaves", "pathLength", "patternContentUnits", "patternTransform",
    "patternUnits", "pointsAtX", "pointsAtY", "pointsAtZ", "preserveAlpha",
    "preserveAspectRatio", "primitiveUnits", "refX", "refY", "repeatCount",
    "repeatDur", "requiredExtensions", "requiredFeatures",
    "specularConstant", "specularExponent", "spreadMethod", "startOffset",
    "stdDeviation", "stitchTiles", "surfaceScale", "systemLanguage",
    "tableValues", "targetX", "targetY", "textLength", "viewBox",
    "viewTarget", "xChannelSelector", "yChannelSelector", "zoomAndPan",
};

/* The MathML attribute names it restores; sorted. */
static const char * const k_math_attributes[] = {"definitionURL"};

static bool listed(const char * const * table, size_t n, const char * name) {
  size_t lo = 0;
  size_t hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = strcmp(name, table[mid]);
    if (c == 0) {
      return true;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

/* Whether `name` is the lower-cased form of a table entry (and not the entry): HTML restores such a
   name to the entry, which is another name. */
static bool is_folded_entry(const char * const * table, size_t n, const char * name) {
  for (size_t i = 0; i < n; i++) {
    if (strcasecmp(table[i], name) == 0 && strcmp(table[i], name) != 0) {
      return true;
    }
  }
  return false;
}

bool gtext_markup_html_foreign_name_survives(
    const char * ns, bool attribute, const char * local) {
  if (!ns || !local) {
    return false;
  }
  const bool svg = strcmp(ns, GTEXT_MARKUP_NS_SVG) == 0;
  const bool math = strcmp(ns, GTEXT_MARKUP_NS_MATHML) == 0;
  if (!svg && !math) {
    return false;
  }
  bool upper = false;
  for (const char * p = local; *p; p++) {
    upper |= *p >= 'A' && *p <= 'Z';
  }
  const char * const * table = NULL;
  size_t count = 0;
  if (svg) {
    table = attribute ? k_svg_attributes : k_svg_tags;
    count = attribute ? sizeof(k_svg_attributes) / sizeof(k_svg_attributes[0])
                      : sizeof(k_svg_tags) / sizeof(k_svg_tags[0]);
  }
  else if (attribute) {
    table = k_math_attributes;
    count = sizeof(k_math_attributes) / sizeof(k_math_attributes[0]);
  }
  if (!upper) {
    /* Read back as written, unless it is the folded form of a name in the table: `viewbox` comes back
       as `viewBox`. */
    return table == NULL || !is_folded_entry(table, count, local);
  }
  return table != NULL && listed(table, count, local);
}

static bool is_html_ns(const char * ns) {
  return ns && strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0;
}

unsigned gtext_markup_html_element_flags(const char * ns, const char * local) {
  if (!local || !is_html_ns(ns)) {
    return 0;
  }
  size_t lo = 0;
  size_t hi = sizeof(k_elements) / sizeof(k_elements[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = strcmp(local, k_elements[mid].name);
    if (c == 0) {
      return k_elements[mid].flags;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

/* ---- the parser's categories ------------------------------------------- */

/* The standard's "special" category, HTML members, from parsing.html section
   13.2.4.2 "The stack of open elements" as retrieved 2026-10-11. Sorted. (`image`
   is not in it: it is a token the tree builder turns into `img`. `dialog` is not
   in it either, though `dialog` has start and end tag rules.) */
static const char * const k_special_html[] = {"address", "applet", "area",
    "article", "aside", "base", "basefont", "bgsound", "blockquote", "body",
    "br", "button", "caption", "center", "col", "colgroup", "dd", "details",
    "dir", "div", "dl", "dt", "embed", "fieldset", "figcaption", "figure",
    "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6",
    "head", "header", "hgroup", "hr", "html", "iframe", "img", "input", "keygen",
    "li", "link", "listing", "main", "marquee", "menu", "meta", "nav", "noembed",
    "noframes", "noscript", "object", "ol", "p", "param", "plaintext", "pre",
    "script", "search", "section", "select", "source", "style", "summary",
    "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead",
    "title", "tr", "track", "ul", "wbr", "xmp"};
/* MathML members, and SVG members. Sorted. */
static const char * const k_special_math[] = {
    "annotation-xml", "mi", "mn", "mo", "ms", "mtext"};
static const char * const k_special_svg[] = {"desc", "foreignObject", "title"};
/* The "formatting" category (HTML only). Sorted. */
static const char * const k_formatting_html[] = {"a", "b", "big", "code", "em",
    "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"};

unsigned gtext_markup_html_category(const char * ns, const char * local) {
  if (!ns || !local) {
    return 0;
  }
  if (strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0) {
    unsigned bits = 0;
    if (listed(k_special_html, sizeof(k_special_html) / sizeof(k_special_html[0]), local)) {
      bits |= GTEXT_MARKUP_HTML_SPECIAL;
    }
    if (listed(k_formatting_html,
            sizeof(k_formatting_html) / sizeof(k_formatting_html[0]), local)) {
      bits |= GTEXT_MARKUP_HTML_FORMATTING;
    }
    return bits;
  }
  if (strcmp(ns, "http://www.w3.org/1998/Math/MathML") == 0) {
    return listed(k_special_math, sizeof(k_special_math) / sizeof(k_special_math[0]), local)
        ? GTEXT_MARKUP_HTML_SPECIAL : 0;
  }
  if (strcmp(ns, "http://www.w3.org/2000/svg") == 0) {
    return listed(k_special_svg, sizeof(k_special_svg) / sizeof(k_special_svg[0]), local)
        ? GTEXT_MARKUP_HTML_SPECIAL : 0;
  }
  return 0;
}

bool gtext_markup_html_is_special(const char * ns, const char * local) {
  return (gtext_markup_html_category(ns, local) & GTEXT_MARKUP_HTML_SPECIAL) != 0;
}

bool gtext_markup_html_is_formatting(const char * ns, const char * local) {
  return (gtext_markup_html_category(ns, local) & GTEXT_MARKUP_HTML_FORMATTING) != 0;
}

bool gtext_markup_html_is_void(const char * ns, const char * local) {
  return (gtext_markup_html_element_flags(ns, local) & GTEXT_MARKUP_HTML_VOID)
      != 0;
}

bool gtext_markup_html_is_raw_text(const char * ns, const char * local) {
  return (gtext_markup_html_element_flags(ns, local)
             & GTEXT_MARKUP_HTML_RAW_TEXT)
      != 0;
}

bool gtext_markup_html_is_escapable_raw_text(
    const char * ns, const char * local) {
  return (gtext_markup_html_element_flags(ns, local)
             & GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT)
      != 0;
}

bool gtext_markup_html_drops_first_newline(const char * ns, const char * local) {
  return (gtext_markup_html_element_flags(ns, local)
             & GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE)
      != 0;
}

static bool boolean_listed(const char * attribute, const char * element) {
  size_t lo = 0;
  size_t hi = sizeof(k_booleans) / sizeof(k_booleans[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    int c = strcmp(attribute, k_booleans[mid].attribute);
    if (c == 0) {
      c = strcmp(element, k_booleans[mid].element);
    }
    if (c == 0) {
      return true;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

bool gtext_markup_html_is_boolean_attribute(
    const char * ns, const char * element, const char * attribute) {
  if (!element || !attribute || !is_html_ns(ns)) {
    return false;
  }
  return boolean_listed(attribute, element) || boolean_listed(attribute, "");
}

unsigned gtext_markup_html_node_flags(const GTEXT_Markup_Node * element) {
  if (!element || gtext_markup_node_kind(element) != GTEXT_MARKUP_KIND_ELEMENT) {
    return 0;
  }
  return gtext_markup_html_element_flags(
      gtext_markup_element_namespace(element),
      gtext_markup_element_local_name(element));
}

bool gtext_markup_html_node_is_void(const GTEXT_Markup_Node * element) {
  return (gtext_markup_html_node_flags(element) & GTEXT_MARKUP_HTML_VOID) != 0;
}

bool gtext_markup_html_node_is_raw_text(const GTEXT_Markup_Node * element) {
  return (gtext_markup_html_node_flags(element) & GTEXT_MARKUP_HTML_RAW_TEXT)
      != 0;
}

bool gtext_markup_html_node_is_escapable_raw_text(
    const GTEXT_Markup_Node * element) {
  return (gtext_markup_html_node_flags(element)
             & GTEXT_MARKUP_HTML_ESCAPABLE_RAW_TEXT)
      != 0;
}

bool gtext_markup_html_node_drops_first_newline(
    const GTEXT_Markup_Node * element) {
  return (gtext_markup_html_node_flags(element)
             & GTEXT_MARKUP_HTML_DROPS_FIRST_NEWLINE)
      != 0;
}

bool gtext_markup_html_node_is_boolean_attribute(
    const GTEXT_Markup_Node * element, const char * attribute) {
  if (!element || gtext_markup_node_kind(element) != GTEXT_MARKUP_KIND_ELEMENT) {
    return false;
  }
  return gtext_markup_html_is_boolean_attribute(
      gtext_markup_element_namespace(element),
      gtext_markup_element_local_name(element), attribute);
}
