/**
 * @file
 *
 * This library's half of the XHTML differential (html_diff.py): everything that
 * has to be done with the markup tree and the XML writer, so that the Python
 * side only moves bytes between this program and the pinned HTML references.
 *
 *     html_ours gen <count> <seed> [plant]   generated trees, written with xhtml
 *     html_ours fromdump                     trees given in the tree-construction
 *                                            dump format, written with xhtml
 *     html_ours fromxml                      XML documents, parsed, then written
 *     html_ours compare                      score an HTML parser's tree against
 *                                            the XML parser's, with html_equivalent
 *
 * The first three emit one record per tree and share the writer call:
 *
 *     ITEM <id> <tags> <status> <hazards> <hazard kind> <path hex> <message hex> <length>
 *     <length bytes>
 *
 * `status` is OK, XHTML (refused by the mode), or another status name (a refusal
 * for an XML reason: a name, a namespace, an encoding); only OK records carry bytes.
 * The declaration is NEVER, the encoding UTF-8, and nothing else is changed from
 * the defaults, which is what a caller who wants one document for both parsers
 * writes.
 *
 * `compare` reads frames `<id> <length>\n<bytes>\n`, four to a document: the
 * source (`gen:<seed>:<i>`, which regenerates the tree a record was written from,
 * or `xml:` or `dump:` and the document it came from), the bytes the writer
 * produced, then html5lib's dump of them and parse5's. The bytes are read with the
 * XML parser, each dump is rebuilt as a tree, and html_equivalent
 * (html_equivalent.h, the same file the unit tests use) is applied four ways: the
 * source against each of the XML parser, html5lib and parse5, and html5lib against
 * parse5. It is the *source* the HTML trees are held to, not what the XML parser
 * reads from the bytes: a writer defect both parsers see alike would otherwise
 * pass.
 *
 * Generated trees come from a splitmix-seeded xorshift generator of our own, so a
 * population is a function of (count, seed) and nothing else. Half of them are
 * **tame**: built from a content model that keeps clear of the hazards the writer
 * lists (so that the claim "a tree with no hazards is read the same" is tested on
 * a large number of non-trivial trees), the rest are **wild**: any element in any
 * vocabulary in any position. Both are needed. A generator that only builds what
 * the hazard list already knows about can never find what it is missing.
 *
 * The plants (the optional fourth argument of gen) corrupt the *output* of the
 * writer on purpose, to show that the differential reports a writer defect; see
 * apply_plant().
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/xml.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_equivalent.h"

/* The library's own HTML vocabulary tables, for `fixed` below: this program is compiled with the
   source of the tables so that it can walk every entry, which the public functions do not allow.
   (Its copies of the public functions replace the archive's, so the program uses these tables.) */
#include "../../src/markup/markup_html.c"

#define NS_XHTML "http://www.w3.org/1999/xhtml"
#define NS_SVG "http://www.w3.org/2000/svg"
#define NS_MATH "http://www.w3.org/1998/Math/MathML"
#define NS_XLINK "http://www.w3.org/1999/xlink"
#define NS_XML "http://www.w3.org/XML/1998/namespace"
#define NS_XMLNS "http://www.w3.org/2000/xmlns/"

typedef GTEXT_Markup_Node Node;
typedef GTEXT_Markup_Document Doc;

/* ---- output helpers ---------------------------------------------------------- */

static void put_hex(const char * bytes, size_t len) {
  if (!bytes || !len) {
    fputs(".", stdout);
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char) bytes[i]);
  }
}

static void put_str(const char * s) {
  put_hex(s, s ? strlen(s) : 0);
}

_Noreturn static void die(const char * what) {
  fprintf(stderr, "html_ours: %s\n", what);
  exit(3);
}

/* ---- random numbers -------------------------------------------------------------- */

typedef struct {
  uint64_t s;
} Rng;

static uint64_t splitmix(uint64_t * x) {
  uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static void rng_seed(Rng * r, uint64_t seed) {
  uint64_t x = seed;
  r->s = splitmix(&x) | 1;
}

static uint64_t rng_next(Rng * r) {
  uint64_t x = r->s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->s = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static size_t pick(Rng * r, size_t n) {
  return (size_t) (rng_next(r) % n);
}

static int chance(Rng * r, int percent) {
  return (int) (rng_next(r) % 100) < percent;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define ONE_OF(r, a) ((a)[pick((r), COUNT(a))])

/* ---- building trees ---------------------------------------------------------------- */

typedef struct {
  Rng r;
  Doc * doc;
  int tame;
  int budget; /* nodes left to make; keeps a tree from growing without end */
  const char * const * focus; /* a group of names the wild generator favours; "s:" svg, "m:" math */
  size_t nfocus;
} Gen;

static Doc * new_doc(void) {
  GTEXT_Markup_Options o = gtext_markup_options_default();
  o.max_depth = 0;
  o.max_attributes = 0;
  Doc * doc = NULL;
  if (gtext_markup_document_new(&o, &doc) != GTEXT_MARKUP_OK) {
    die("cannot make a document");
  }
  return doc;
}

static Node * el(Gen * g, Node * parent, const char * local, const char * ns) {
  Node * n = NULL;
  if (gtext_markup_create_element(g->doc, ns, NULL, local, &n) != GTEXT_MARKUP_OK) {
    die("cannot create an element");
  }
  if (gtext_markup_append_child(parent ? parent : gtext_markup_document_root(g->doc), n) != GTEXT_MARKUP_OK) {
    die("cannot append an element");
  }
  g->budget--;
  return n;
}

static void attr(Node * e, const char * ns, const char * prefix, const char * local, const char * value) {
  if (gtext_markup_element_set_attribute(e, ns, prefix, local, value, strlen(value)) != GTEXT_MARKUP_OK) {
    die("cannot set an attribute");
  }
}

static void xmlns(Node * e, const char * prefix, const char * uri) {
  if (prefix) {
    attr(e, NS_XMLNS, "xmlns", prefix, uri);
  }
  else {
    attr(e, NS_XMLNS, NULL, "xmlns", uri);
  }
}

static void text(Gen * g, Node * parent, const char * s) {
  Node * n = NULL;
  if (gtext_markup_create_text(g->doc, s, strlen(s), &n) != GTEXT_MARKUP_OK
      || gtext_markup_append_child(parent, n) != GTEXT_MARKUP_OK) {
    die("cannot add text");
  }
  g->budget--;
}

static void comment(Gen * g, Node * parent, const char * s) {
  Node * n = NULL;
  if (gtext_markup_create_comment(g->doc, s, strlen(s), &n) != GTEXT_MARKUP_OK
      || gtext_markup_append_child(parent ? parent : gtext_markup_document_root(g->doc), n) != GTEXT_MARKUP_OK) {
    die("cannot add a comment");
  }
  g->budget--;
}

static void cdata(Gen * g, Node * parent, const char * s) {
  Node * n = NULL;
  if (gtext_markup_create_cdata(g->doc, s, strlen(s), &n) != GTEXT_MARKUP_OK
      || gtext_markup_append_child(parent, n) != GTEXT_MARKUP_OK) {
    die("cannot add cdata");
  }
  g->budget--;
}

/* ---- vocabularies ----------------------------------------------------------------- */

static const char * const TEXTS[] = {"x", "hello", "hello world", "a < b", "a & b", "a > b", "]]>", "-->", "<!--",
    "&amp;", "&lt;p&gt;", "&#65;", "&notit;", "&copy", "'q\"", " ", "  ", "\t", "\n", "x\n", "\nx", " x ", "x\ny",
    "\xC2\xA0", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "\r", "a\rb", "\xEF\xBF\xBD", "\xC2\x80", "\xC2\x85", "\xC2\x9F", "\x7f",
    "\xEF\xB7\x90", "\xEF\xBB\xBF", "</p>", "<b>", "&", "<", ">", "--", "\xE2\x80\xA8"};
static const char * const ATTR_VALUES[] = {"", "x", "a b", " a ", "a\nb", "a\tb", "a\rb", "\"", "'", "<", "&", "&amp;",
    ">", "a&b<c>\"d'", "\xC3\xA9", "\xE2\x82\xAC", "1", "0", "true", "#top", "/a/b?c=d&e=f", "text/html",
    "application/xhtml+xml", "UTF-8", "\xC2\xA0x", "x\n  y"};
static const char * const COMMENTS[] = {"c", " c ", "", " ", "a-b", "a--b", "x-", ">", "->", "a>b", "<!--", "a<b",
    "--!>", "\n", "?", "&amp;", "\xC3\xA9"};

static const char * const BLOCKS[] = {"div", "section", "article", "aside", "nav", "header", "footer", "main",
    "address", "blockquote", "figure", "details", "fieldset", "center", "dialog", "search", "hgroup", "menu",
    "dir", "p", "pre", "listing", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "form", "ul", "ol", "dl", "table",
    "li", "dt", "dd", "legend", "figcaption", "summary", "caption", "optgroup", "option", "select", "textarea"};
static const char * const PHRASING[] = {"span", "a", "b", "i", "em", "strong", "code", "small", "sub", "sup",
    "mark", "abbr", "cite", "q", "kbd", "var", "samp", "u", "s", "del", "ins", "label", "bdi", "bdo", "data",
    "time", "button", "font", "big", "tt", "strike", "nobr", "dfn", "ruby", "rt", "rp", "rb", "rtc", "output",
    "meter", "progress", "canvas", "object", "audio", "video", "picture", "slot", "custom-el", "x-foo",
    "a.b", "a_b", "a1", "a\xC3\xA9", "a\xC2\xB7z"};
static const char * const VOIDS[] = {"br", "img", "input", "hr", "wbr", "area", "base", "col", "embed", "link",
    "meta", "param", "source", "track", "basefont", "bgsound", "frame", "keygen"};
static const char * const RAWS[] = {"script", "style", "title", "textarea", "xmp", "iframe", "noembed", "noframes",
    "noscript", "plaintext", "template"};
static const char * const TABLES[] = {"table", "caption", "colgroup", "col", "thead", "tbody", "tfoot", "tr", "td",
    "th"};
static const char * const SPECIALS[] = {"html", "head", "body", "frameset", "frame", "image", "isindex", "marquee",
    "applet", "rp", "menuitem", "select", "option", "optgroup", "datalist", "selectedcontent", "svg", "math",
    "foreignObject", "annotation-xml", "mi", "mo", "mn", "ms", "mtext", "desc", "path", "g"};

static const char * const SVG_NAMES[] = {"svg", "g", "path", "circle", "rect", "line", "polyline", "polygon", "ellipse",
    "text", "tspan", "title", "desc", "defs", "use", "symbol", "image", "a", "switch", "style", "script",
    "linearGradient", "radialGradient", "stop", "clipPath", "mask", "pattern", "marker", "filter", "feBlend",
    "feGaussianBlur", "foreignObject", "textPath", "altGlyph", "animateTransform", "animate", "set", "metadata",
    "view", "font-face", "glyphRef", "feDistantLight", "feFuncA", "feMergeNode", "customElement", "Foo", "foo",
    "div", "p", "br", "span", "b", "table", "img", "font", "pre", "h1", "ul", "li", "hr", "body", "head"};
static const char * const MATH_NAMES[] = {"math", "mi", "mo", "mn", "ms", "mtext", "mrow", "mfrac", "msqrt", "mroot",
    "msub", "msup", "msubsup", "munder", "mover", "munderover", "mtable", "mtr", "mtd", "mstyle", "mspace",
    "mphantom", "menclose", "semantics", "annotation", "annotation-xml", "maction", "malignmark", "mglyph", "merror",
    "mpadded", "mfenced", "none", "mprescripts", "Foo", "div", "p", "br", "span", "b", "table", "img", "font"};

static const char * const ATTR_NAMES[] = {"class", "id", "style", "title", "lang", "dir", "hidden", "data-x",
    "aria-label", "role", "tabindex", "contenteditable", "href", "src", "alt", "type", "name", "value", "for",
    "rel", "width", "height", "colspan", "rowspan", "disabled", "checked", "selected", "readonly", "multiple",
    "required", "autofocus", "onclick", "slot", "is", "rev", "color", "face", "size", "nowrap", "align",
    "bgcolor", "border", "xmlns-foo", "a.b", "a_b", "a-b", "x:y", "\xC3\xA9"};
static const char * const SVG_ATTRS[] = {"viewBox", "preserveAspectRatio", "d", "fill", "stroke", "x", "y", "cx", "cy",
    "r", "width", "height", "transform", "points", "gradientUnits", "markerWidth", "clipPathUnits",
    "baseFrequency", "attributeName", "class", "id", "style", "lang", "requiredFeatures", "xlinkhref", "fooBar",
    "contentScriptType", "textLength", "startOffset", "repeatCount", "onclick", "data-x", "tabindex"};
static const char * const MATH_ATTRS[] = {"mathvariant", "displaystyle", "encoding", "definitionURL", "display",
    "xmlns-x", "class", "id", "href", "mathsize", "fooBar", "columnalign", "scriptlevel", "lspace", "data-x"};

static const char * attr_value(Gen * g) {
  return ONE_OF(&g->r, ATTR_VALUES);
}

static const char * a_text(Gen * g) {
  return ONE_OF(&g->r, TEXTS);
}

/* Attributes on an element in the vocabulary `vocab` (0 html, 1 svg, 2 math). */
static void some_attrs(Gen * g, Node * e, int vocab) {
  const int n = chance(&g->r, 55) ? (int) pick(&g->r, 4) : 0;
  for (int i = 0; i < n; i++) {
    const char * name;
    if (vocab == 1 && chance(&g->r, 55)) {
      name = ONE_OF(&g->r, SVG_ATTRS);
    }
    else if (vocab == 2 && chance(&g->r, 55)) {
      name = ONE_OF(&g->r, MATH_ATTRS);
    }
    else {
      name = ONE_OF(&g->r, ATTR_NAMES);
    }
    /* "Tame" means free of the listed hazards, so it keeps to names HTML reads back as written. */
    if (g->tame && (strchr(name, ':') || strchr(name, '\xC3') || strcmp(name, "xlinkhref") == 0
                    || (vocab != 0
                        && !gtext_markup_html_foreign_name_survives(vocab == 1 ? NS_SVG : NS_MATH, true, name)))) {
      continue;
    }
    if (g->tame && vocab == 0 && (strcmp(name, "xmlns-foo") == 0)) {
      continue;
    }
    if (strchr(name, ':')) {
      /* x:y is a name with a colon that is not a prefix of any namespace: skip it as an
         attribute here; the namespaced forms are made below. */
      continue;
    }
    const char * value = attr_value(g);
    /* A duplicate overwrites in place, which is fine. */
    attr(e, NULL, NULL, name, value);
    if (!g->tame && vocab != 0 && chance(&g->r, 6) && strlen(name) < 60) {
      /* The same name with the case of its first letter flipped: HTML reads the two as one attribute. */
      char other[64];
      snprintf(other, sizeof(other), "%s", name);
      other[0] = (char) (other[0] >= 'a' && other[0] <= 'z' ? other[0] - 32 : other[0] >= 'A' && other[0] <= 'Z' ? other[0] + 32 : other[0]);
      attr(e, NULL, NULL, other, attr_value(g));
    }
  }
  if (vocab == 1 && chance(&g->r, 12)) {
    xmlns(e, "xlink", NS_XLINK);
    attr(e, NS_XLINK, "xlink", "href", attr_value(g));
  }
  if (!g->tame && chance(&g->r, 5)) {
    attr(e, NS_XML, "xml", chance(&g->r, 50) ? "lang" : "space", chance(&g->r, 50) ? "en" : "preserve");
  }
  if (g->tame && vocab != 0 && chance(&g->r, 8)) {
    attr(e, NS_XML, "xml", "lang", "en");
  }
}

/* Groups of names that interact in HTML's tree construction. A wild tree favours one group,
 * because a uniform choice from every name rarely puts a ruby part inside a ruby part or a
 * table part inside a cell, and those are where the standard's rules are. */
#define GROUP(name, ...) static const char * const name[] = {__VA_ARGS__}
GROUP(G_RUBY, "ruby", "rb", "rt", "rp", "rtc", "span", "b", "p");
GROUP(G_TABLE, "table", "thead", "tbody", "tfoot", "tr", "td", "th", "caption", "colgroup", "col", "div", "p",
    "form", "input", "script", "template", "style", "a", "b", "select", "option");
GROUP(G_LISTS, "ul", "ol", "li", "dl", "dt", "dd", "div", "p", "menu", "dir", "search", "details", "summary",
    "address", "center");
GROUP(G_FORMS, "form", "input", "select", "option", "optgroup", "textarea", "button", "label", "fieldset", "legend",
    "datalist", "hr", "div", "a", "p", "table", "td");
GROUP(G_HEADINGS, "h1", "h2", "h3", "h4", "h5", "h6", "p", "div", "span", "header", "hgroup", "section", "address",
    "pre", "listing");
GROUP(G_FORMATTING, "a", "b", "i", "em", "strong", "code", "nobr", "font", "u", "s", "big", "small", "tt", "strike",
    "p", "div", "span", "table", "td", "button", "applet", "object", "marquee", "caption");
GROUP(G_DOCUMENT, "html", "head", "body", "title", "meta", "link", "base", "script", "style", "noscript", "template",
    "div", "p", "frameset", "frame");
GROUP(G_FOREIGN, "s:svg", "m:math", "s:g", "s:foreignObject", "s:title", "s:desc", "s:path", "s:text", "m:mi",
    "m:mo", "m:mtext", "m:annotation-xml", "m:ms", "div", "p", "b", "span", "br", "table", "td", "font", "img", "a",
    "s:a", "s:style", "s:script", "s:tspan", "s:image", "m:mrow");
GROUP(G_RAWISH, "script", "style", "textarea", "title", "xmp", "iframe", "noembed", "noframes", "noscript",
    "plaintext", "listing", "pre", "template", "select", "option", "div", "span");
GROUP(G_OBJECTS, "button", "object", "applet", "marquee", "td", "th", "caption", "p", "div", "a", "form", "li", "ul",
    "table", "select", "br", "hr", "img", "embed", "param", "source", "track", "audio", "video", "picture");

typedef struct {
  const char * const * names;
  size_t count;
} FocusGroup;

static const FocusGroup FOCUS_GROUPS[] = {{G_RUBY, COUNT(G_RUBY)}, {G_TABLE, COUNT(G_TABLE)},
    {G_LISTS, COUNT(G_LISTS)}, {G_FORMS, COUNT(G_FORMS)}, {G_HEADINGS, COUNT(G_HEADINGS)},
    {G_FORMATTING, COUNT(G_FORMATTING)}, {G_DOCUMENT, COUNT(G_DOCUMENT)}, {G_FOREIGN, COUNT(G_FOREIGN)},
    {G_RAWISH, COUNT(G_RAWISH)}, {G_OBJECTS, COUNT(G_OBJECTS)}};

/* ---- tame content ------------------------------------------------------------------- */

static void gen_flow(Gen * g, Node * parent, int depth, int in_a, int in_form, int in_button);
static void gen_phrasing(Gen * g, Node * parent, int depth, int in_a, int in_button);
static void gen_svg(Gen * g, Node * parent, int depth);
static void gen_math(Gen * g, Node * parent, int depth);

static void tame_text(Gen * g, Node * parent) {
  const char * s = a_text(g);
  /* `]]>` and the like are fine in text; the writer escapes them. */
  text(g, parent, s);
}

static void gen_phrasing(Gen * g, Node * parent, int depth, int in_a, int in_button) {
  const int kids = depth > 3 || g->budget < 0 ? 0 : (int) pick(&g->r, 4);
  int last_text = 0;
  for (int i = 0; i < kids; i++) {
    const size_t what = pick(&g->r, 14);
    if (what < 4) {
      if (!last_text) {
        tame_text(g, parent);
        last_text = 1;
      }
    }
    else if (what < 9) {
      const char * name = ONE_OF(&g->r, PHRASING);
      if ((in_a && strcmp(name, "a") == 0) || (in_button && strcmp(name, "button") == 0)
          || strcmp(name, "rt") == 0 || strcmp(name, "rp") == 0 || strcmp(name, "rb") == 0
          || strcmp(name, "rtc") == 0 || strcmp(name, "nobr") == 0 || strcmp(name, "canvas") == 0
          || strcmp(name, "object") == 0 || strcmp(name, "audio") == 0 || strcmp(name, "video") == 0
          || strcmp(name, "picture") == 0) {
        name = "span";
      }
      Node * e = el(g, parent, name, NS_XHTML);
      some_attrs(g, e, 0);
      if (strcmp(name, "ruby") == 0) {
        /* The parts in the order the content model has them, as siblings: no ruby part ends another. */
        for (int k = 1 + (int) pick(&g->r, 2); k > 0; k--) {
          if (chance(&g->r, 40)) {
            text(g, el(g, e, "rb", NS_XHTML), "b");
          }
          else {
            tame_text(g, e);
          }
          if (chance(&g->r, 50)) {
            text(g, el(g, e, "rp", NS_XHTML), "(");
          }
          text(g, el(g, e, "rt", NS_XHTML), "r");
          if (chance(&g->r, 50)) {
            text(g, el(g, e, "rp", NS_XHTML), ")");
          }
        }
      }
      else {
        gen_phrasing(g, e, depth + 1, in_a || strcmp(name, "a") == 0, in_button || strcmp(name, "button") == 0);
      }
      last_text = 0;
    }
    else if (what == 9) {
      static const char * const PHRASING_VOIDS[] = {"br", "img", "input", "wbr", "embed"};
      Node * e = el(g, parent, ONE_OF(&g->r, PHRASING_VOIDS), NS_XHTML);
      some_attrs(g, e, 0);
      last_text = 0;
    }
    else if (what == 10) {
      comment(g, parent, ONE_OF(&g->r, COMMENTS));
      last_text = 0;
    }
    else if (what == 11 && depth < 3) {
      Node * s = el(g, parent, "svg", NS_SVG);
      xmlns(s, NULL, NS_SVG);
      gen_svg(g, s, depth + 1);
      last_text = 0;
    }
    else if (what == 12 && depth < 3) {
      Node * m = el(g, parent, "math", NS_MATH);
      xmlns(m, NULL, NS_MATH);
      gen_math(g, m, depth + 1);
      last_text = 0;
    }
    else if (!last_text) {
      tame_text(g, parent);
      last_text = 1;
    }
  }
}

/* The SVG/MathML names HTML does not break out of, and that need no special case. */
static void gen_svg(Gen * g, Node * parent, int depth) {
  some_attrs(g, parent, 1);
  const int kids = depth > 4 || g->budget < 0 ? 0 : (int) pick(&g->r, 4);
  for (int i = 0; i < kids; i++) {
    const size_t what = pick(&g->r, 10);
    if (what < 6) {
      const char * name = ONE_OF(&g->r, SVG_NAMES);
      static const char * const OK[] = {"g", "path", "circle", "rect", "line", "ellipse", "text", "tspan", "title", "desc",
          "defs", "use", "symbol", "image", "a", "switch", "linearGradient", "stop", "clipPath", "mask", "pattern",
          "marker", "filter", "feBlend", "feGaussianBlur", "foreignObject", "textPath", "animate", "set", "metadata",
          "foo", "svg", "style", "script", "polyline", "polygon", "view", "radialGradient", "animateTransform"};
      if (g->tame) {
        int ok = 0;
        for (size_t k = 0; k < COUNT(OK); k++) {
          ok |= strcmp(name, OK[k]) == 0;
        }
        if (!ok) {
          name = ONE_OF(&g->r, OK);
        }
      }
      Node * e = el(g, parent, name, NS_SVG);
      if (strcmp(name, "svg") == 0 && g->tame) {
        gen_svg(g, e, depth + 1);
      }
      else if (strcmp(name, "foreignObject") == 0 || strcmp(name, "title") == 0 || strcmp(name, "desc") == 0) {
        some_attrs(g, e, 1);
        if (chance(&g->r, 60)) {
          Node * d = el(g, e, chance(&g->r, 50) ? "div" : "p", NS_XHTML);
          xmlns(d, NULL, NS_XHTML);
          gen_phrasing(g, d, depth + 2, 0, 0);
        }
        else {
          tame_text(g, e);
        }
      }
      else if (strcmp(name, "style") == 0 || strcmp(name, "script") == 0) {
        text(g, e, g->tame ? "a { b: c }" : a_text(g));
      }
      else {
        some_attrs(g, e, 1);
        if (strcmp(name, "text") == 0 || strcmp(name, "tspan") == 0 || strcmp(name, "textPath") == 0) {
          tame_text(g, e);
        }
        else if (depth < 4 && chance(&g->r, 40)) {
          gen_svg(g, e, depth + 1);
        }
      }
    }
    else if (what < 8) {
      tame_text(g, parent);
    }
    else if (what == 8) {
      comment(g, parent, ONE_OF(&g->r, COMMENTS));
    }
    else if (!g->tame && chance(&g->r, 50)) {
      cdata(g, parent, "d");
    }
    else {
      tame_text(g, parent);
    }
  }
}

static void gen_math(Gen * g, Node * parent, int depth) {
  some_attrs(g, parent, 2);
  const int kids = depth > 4 || g->budget < 0 ? 0 : (int) pick(&g->r, 4);
  for (int i = 0; i < kids; i++) {
    const size_t what = pick(&g->r, 10);
    if (what < 6) {
      static const char * const OK[] = {"mi", "mo", "mn", "ms", "mtext", "mrow", "mfrac", "msqrt", "mroot", "msub",
          "msup", "munder", "mover", "mtable", "mtr", "mtd", "mstyle", "mspace", "semantics", "annotation",
          "annotation-xml", "maction", "malignmark", "mglyph", "merror", "mpadded", "mfenced", "none", "math"};
      const char * name = g->tame ? ONE_OF(&g->r, OK) : ONE_OF(&g->r, MATH_NAMES);
      Node * e = el(g, parent, name, NS_MATH);
      some_attrs(g, e, 2);
      if (strcmp(name, "mi") == 0 || strcmp(name, "mo") == 0 || strcmp(name, "mn") == 0
          || strcmp(name, "ms") == 0 || strcmp(name, "mtext") == 0) {
        if (chance(&g->r, 85)) {
          tame_text(g, e);
        }
        else if (chance(&g->r, 50)) {
          Node * d = el(g, e, "span", NS_XHTML);
          xmlns(d, NULL, NS_XHTML);
          gen_phrasing(g, d, depth + 2, 0, 0);
        }
      }
      else if (strcmp(name, "annotation-xml") == 0) {
        if (chance(&g->r, 60)) {
          attr(e, NULL, NULL, "encoding", chance(&g->r, 50) ? "text/html" : "application/xhtml+xml");
          Node * d = el(g, e, "div", NS_XHTML);
          xmlns(d, NULL, NS_XHTML);
          gen_phrasing(g, d, depth + 2, 0, 0);
        }
        else {
          tame_text(g, e);
        }
      }
      else if (strcmp(name, "math") == 0 || depth < 3) {
        gen_math(g, e, depth + 1);
      }
    }
    else if (what < 8) {
      tame_text(g, parent);
    }
    else if (what == 8) {
      comment(g, parent, ONE_OF(&g->r, COMMENTS));
    }
    else {
      tame_text(g, parent);
    }
  }
}

static void gen_table(Gen * g, Node * parent, int depth, int in_form) {
  Node * t = el(g, parent, "table", NS_XHTML);
  some_attrs(g, t, 0);
  if (chance(&g->r, 30)) {
    Node * c = el(g, t, "caption", NS_XHTML);
    gen_phrasing(g, c, depth + 1, 0, 0);
  }
  if (chance(&g->r, 20)) {
    Node * cg = el(g, t, "colgroup", NS_XHTML);
    for (int i = (int) pick(&g->r, 3); i > 0; i--) {
      el(g, cg, "col", NS_XHTML);
    }
  }
  const int n = 1 + (int) pick(&g->r, 2);
  for (int i = 0; i < n; i++) {
    /* thead first, tfoot last, as the content model has them, or tbody. */
    const char * kind = i == 0 && chance(&g->r, 30) ? "thead" : i == n - 1 && n > 1 && chance(&g->r, 30) ? "tfoot" : "tbody";
    Node * sec = el(g, t, kind, NS_XHTML);
    for (int r = 1 + (int) pick(&g->r, 2); r > 0; r--) {
      Node * tr = el(g, sec, "tr", NS_XHTML);
      for (int c = 1 + (int) pick(&g->r, 3); c > 0; c--) {
        Node * td = el(g, tr, chance(&g->r, 25) ? "th" : "td", NS_XHTML);
        some_attrs(g, td, 0);
        if (depth < 3 && chance(&g->r, 25)) {
          gen_flow(g, td, depth + 2, 0, in_form, 0);
        }
        else {
          gen_phrasing(g, td, depth + 2, 0, 0);
        }
      }
    }
  }
}

static void gen_flow(Gen * g, Node * parent, int depth, int in_a, int in_form, int in_button) {
  const int kids = depth > 4 || g->budget < 0 ? 0 : 1 + (int) pick(&g->r, 3);
  int last_text = 0;
  for (int i = 0; i < kids; i++) {
    const size_t what = pick(&g->r, 24);
    if (what < 3) {
      if (!last_text) {
        tame_text(g, parent);
        last_text = 1;
      }
      continue;
    }
    last_text = 0;
    if (what < 8) {
      static const char * const DIVS[] = {"div", "section", "article", "aside", "nav", "header", "footer", "main",
          "blockquote", "figure", "center", "search", "address", "hgroup", "details"};
      const char * name = ONE_OF(&g->r, DIVS);
      Node * e = el(g, parent, name, NS_XHTML);
      some_attrs(g, e, 0);
      if (strcmp(name, "details") == 0 && chance(&g->r, 70)) {
        gen_phrasing(g, el(g, e, "summary", NS_XHTML), depth + 2, 0, 0);
      }
      if (strcmp(name, "figure") == 0 && chance(&g->r, 50)) {
        gen_phrasing(g, el(g, e, "figcaption", NS_XHTML), depth + 2, 0, 0);
      }
      if (strcmp(name, "address") == 0 || strcmp(name, "hgroup") == 0) {
        gen_phrasing(g, e, depth + 1, in_a, in_button);
      }
      else {
        gen_flow(g, e, depth + 1, in_a, in_form, in_button);
      }
    }
    else if (what < 11) {
      static const char * const P[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "listing"};
      const char * name = ONE_OF(&g->r, P);
      Node * e = el(g, parent, name, NS_XHTML);
      some_attrs(g, e, 0);
      gen_phrasing(g, e, depth + 1, in_a, in_button);  /* a leading line feed in pre is the writer's to refuse */
    }
    else if (what < 13) {
      Node * list = el(g, parent, chance(&g->r, 50) ? "ul" : "ol", NS_XHTML);
      some_attrs(g, list, 0);
      for (int k = 1 + (int) pick(&g->r, 3); k > 0; k--) {
        Node * li = el(g, list, "li", NS_XHTML);
        gen_phrasing(g, li, depth + 2, in_a, in_button);
      }
    }
    else if (what == 13) {
      Node * dl = el(g, parent, "dl", NS_XHTML);
      for (int k = 1 + (int) pick(&g->r, 2); k > 0; k--) {
        gen_phrasing(g, el(g, dl, "dt", NS_XHTML), depth + 2, in_a, in_button);
        gen_phrasing(g, el(g, dl, "dd", NS_XHTML), depth + 2, in_a, in_button);
      }
    }
    else if (what == 14 && depth < 3) {
      gen_table(g, parent, depth, in_form);
    }
    else if (what == 15) {
      el(g, parent, "hr", NS_XHTML);
    }
    else if (what == 16 && !in_form) {
      Node * f = el(g, parent, "form", NS_XHTML);
      some_attrs(g, f, 0);
      gen_flow(g, f, depth + 1, in_a, 1, in_button);
    }
    else if (what == 17) {
      Node * s = el(g, parent, "select", NS_XHTML);
      for (int k = (int) pick(&g->r, 3); k > 0; k--) {
        if (chance(&g->r, 25)) {
          Node * og = el(g, s, "optgroup", NS_XHTML);
          for (int j = 1 + (int) pick(&g->r, 2); j > 0; j--) {
            text(g, el(g, og, "option", NS_XHTML), "o");
          }
        }
        else {
          Node * o = el(g, s, "option", NS_XHTML);
          some_attrs(g, o, 0);
          text(g, o, "o");
        }
      }
    }
    else if (what == 18) {
      Node * ta = el(g, parent, "textarea", NS_XHTML);
      if (chance(&g->r, 70)) {
        const char * s = a_text(g);
        if (s[0] != '\n') {
          text(g, ta, s);
        }
      }
    }
    else if (what == 19) {
      Node * e = el(g, parent, chance(&g->r, 50) ? "script" : "style", NS_XHTML);
      static const char * const CODE[] = {"var a = 1;", "if (a > b) { c(); }", "a > b { color: red }", "/* c */",
          "x = '\"';", "a\nb", "<!-", "x ]] y", "", "if (1 > 0) {}\n"};
      const char * s = ONE_OF(&g->r, CODE);
      if (*s) {
        text(g, e, s);
      }
    }
    else if (what == 20) {
      Node * e = el(g, parent, "p", NS_XHTML);
      gen_phrasing(g, e, depth + 1, in_a, in_button);
    }
    else if (what == 21 && depth < 3) {
      Node * s = el(g, parent, "svg", NS_SVG);
      xmlns(s, NULL, NS_SVG);
      gen_svg(g, s, depth + 1);
    }
    else if (what == 22 && depth < 3) {
      Node * m = el(g, parent, "math", NS_MATH);
      xmlns(m, NULL, NS_MATH);
      gen_math(g, m, depth + 1);
    }
    else if (what == 23) {
      comment(g, parent, ONE_OF(&g->r, COMMENTS));
    }
    else {
      Node * e = el(g, parent, "div", NS_XHTML);
      gen_phrasing(g, e, depth + 1, in_a, in_button);
    }
  }
}

/* ---- wild content ------------------------------------------------------------------- */

static const char * wild_name(Gen * g, const char ** ns_out) {
  *ns_out = NS_XHTML;
  if (g->focus && chance(&g->r, 75)) {
    const char * name = g->focus[pick(&g->r, g->nfocus)];
    if (name[0] == 's' && name[1] == ':') {
      *ns_out = NS_SVG;
      return name + 2;
    }
    if (name[0] == 'm' && name[1] == ':') {
      *ns_out = NS_MATH;
      return name + 2;
    }
    return name;
  }
  const size_t k = pick(&g->r, 100);
  if (k < 30) {
    return ONE_OF(&g->r, BLOCKS);
  }
  if (k < 55) {
    return ONE_OF(&g->r, PHRASING);
  }
  if (k < 65) {
    return ONE_OF(&g->r, VOIDS);
  }
  if (k < 72) {
    return ONE_OF(&g->r, RAWS);
  }
  if (k < 80) {
    return ONE_OF(&g->r, TABLES);
  }
  if (k < 86) {
    return ONE_OF(&g->r, SPECIALS);
  }
  if (k < 93) {
    *ns_out = NS_SVG;
    return ONE_OF(&g->r, SVG_NAMES);
  }
  *ns_out = NS_MATH;
  return ONE_OF(&g->r, MATH_NAMES);
}

static void gen_wild(Gen * g, Node * parent, const char * parent_ns, int depth) {
  const int kids = depth > 5 || g->budget < 0 ? 0 : (int) pick(&g->r, 5);
  int last_text = 0;
  for (int i = 0; i < kids; i++) {
    const size_t what = pick(&g->r, 12);
    if (what < 7) {
      const char * ns;
      const char * name = wild_name(g, &ns);
      if (chance(&g->r, 6)) {
        static const char * const NSS[] = {NS_XHTML, NS_SVG, NS_MATH};
        ns = ONE_OF(&g->r, NSS);
      }
      Node * e = el(g, parent, name, ns);
      if (strcmp(ns, parent_ns) != 0 || chance(&g->r, 5)) {
        xmlns(e, NULL, ns);
      }
      if (chance(&g->r, 3)) {
        xmlns(e, "xlink", NS_XLINK);
        attr(e, NS_XLINK, "xlink", "href", attr_value(g));
      }
      some_attrs(g, e, strcmp(ns, NS_SVG) == 0 ? 1 : strcmp(ns, NS_MATH) == 0 ? 2 : 0);
      if (strcmp(name, "annotation-xml") == 0 && chance(&g->r, 60)) {
        attr(e, NULL, NULL, "encoding", chance(&g->r, 50) ? "text/html" : "application/xhtml+xml");
      }
      gen_wild(g, e, ns, depth + 1);
      last_text = 0;
    }
    else if (what < 9) {
      if (!last_text) {
        text(g, parent, a_text(g));
        last_text = 1;
      }
    }
    else if (what == 9) {
      comment(g, parent, ONE_OF(&g->r, COMMENTS));
      last_text = 0;
    }
    else if (what == 10) {
      if (chance(&g->r, 50)) {
        cdata(g, parent, "d");
      }
      else {
        Node * n = NULL;
        if (gtext_markup_create_processing_instruction(g->doc, "t", "d", 1, &n) != GTEXT_MARKUP_OK
            || gtext_markup_append_child(parent, n) != GTEXT_MARKUP_OK) {
          die("cannot add a processing instruction");
        }
      }
      last_text = 0;
    }
    else if (!last_text) {
      text(g, parent, chance(&g->r, 50) ? " " : "\n  ");
      last_text = 1;
    }
  }
}

/* ---- documents ------------------------------------------------------------------------ */

static void maybe_doctype(Gen * g, int percent_html) {
  Node * n = NULL;
  GTEXT_Markup_Status st = GTEXT_MARKUP_OK;
  if (chance(&g->r, percent_html)) {
    st = gtext_markup_create_doctype(g->doc, "html", NULL, NULL, &n);
  }
  else if (chance(&g->r, 10)) {
    if (chance(&g->r, 50)) {
      st = gtext_markup_create_doctype(g->doc, "html", "-//W3C//DTD XHTML 1.0 Strict//EN",
          "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd", &n);
    }
    else {
      st = gtext_markup_create_doctype(g->doc, "svg", NULL, NULL, &n);
    }
  }
  if (st != GTEXT_MARKUP_OK) {
    die("cannot create a doctype");
  }
  if (n && gtext_markup_append_child(gtext_markup_document_root(g->doc), n) != GTEXT_MARKUP_OK) {
    die("cannot append a doctype");
  }
}

static void head_content(Gen * g, Node * head) {
  if (chance(&g->r, 60)) {
    text(g, el(g, head, "title", NS_XHTML), chance(&g->r, 70) ? "t" : a_text(g));
  }
  for (int i = (int) pick(&g->r, 4); i > 0; i--) {
    const size_t k = pick(&g->r, 6);
    if (k == 0) {
      Node * m = el(g, head, "meta", NS_XHTML);
      attr(m, NULL, NULL, chance(&g->r, 50) ? "charset" : "name", attr_value(g));
    }
    else if (k == 1) {
      Node * l = el(g, head, "link", NS_XHTML);
      attr(l, NULL, NULL, "rel", "stylesheet");
      attr(l, NULL, NULL, "href", attr_value(g));
    }
    else if (k == 2) {
      text(g, el(g, head, "style", NS_XHTML), "a > b { c: d }");
    }
    else if (k == 3) {
      text(g, el(g, head, "script", NS_XHTML), "if (a > b) { c(); }");
    }
    else if (k == 4) {
      comment(g, head, ONE_OF(&g->r, COMMENTS));
    }
    else {
      Node * b = el(g, head, "base", NS_XHTML);
      attr(b, NULL, NULL, "href", "/x");
    }
  }
}

/* One random change to an otherwise well-behaved tree: an element from the wild vocabulary put
 * somewhere in it. A tree that is safe except for one thing is the tree that finds a hazard the
 * list does not know, so these are the most useful ones the generator makes. */
static int in_group(const FocusGroup * fg, const char * name) {
  for (size_t i = 0; i < fg->count; i++) {
    const char * n = fg->names[i];
    if ((n[0] == 's' || n[0] == 'm') && n[1] == ':') {
      n += 2;
    }
    if (strcmp(n, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void perturb(Gen * g) {
  Node * elements[400];
  size_t n = 0;
  Node * root = gtext_markup_document_root(g->doc);
  for (Node * c = gtext_markup_node_next_in_order(root, root, false); c && n < 400;
       c = gtext_markup_node_next_in_order(root, c, false)) {
    if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_ELEMENT) {
      elements[n++] = c;
    }
  }
  if (!n) {
    return;
  }
  const FocusGroup * fg = &FOCUS_GROUPS[pick(&g->r, COUNT(FOCUS_GROUPS))];
  g->focus = fg->names;
  g->nfocus = fg->count;
  /* Prefer a place the group is about (a change among ruby parts goes in a ruby, among table parts in a
     table), when the tree has one. */
  Node * target = elements[pick(&g->r, n)];
  if (chance(&g->r, 75)) {
    size_t start = pick(&g->r, n);
    for (size_t k = 0; k < n; k++) {
      Node * cand = elements[(start + k) % n];
      if (in_group(fg, gtext_markup_element_local_name(cand))) {
        target = cand;
        break;
      }
    }
  }
  Node * e = NULL;
  const size_t mode = pick(&g->r, 100);
  const char * tns = gtext_markup_element_namespace(target);
  const int foreign = tns && (strcmp(tns, NS_SVG) == 0 || strcmp(tns, NS_MATH) == 0);
  if (mode < 22 && foreign) {
    /* A name HTML lower-cases without restoring, on an element or as an attribute, or a case twin. */
    static const char * const ODD_ELEMENTS[] = {"customElement", "Foo", "linearGradiant", "ClipPath", "myShape"};
    static const char * const ODD_ATTRIBUTES[] = {"contentScriptType", "fooBar", "dataFoo", "viewbox", "ViewBox", "CLASS"};
    const size_t kind = pick(&g->r, 3);
    if (kind == 0) {
      e = el(g, target, ONE_OF(&g->r, ODD_ELEMENTS), tns);
      (void) e;
    }
    else if (kind == 1) {
      attr(target, NULL, NULL, ONE_OF(&g->r, ODD_ATTRIBUTES), attr_value(g));
    }
    else {
      attr(target, NULL, NULL, "viewBox", "0 0 1 1");
      attr(target, NULL, NULL, "viewbox", "1 1 2 2");
    }
    return;
  }
  if (mode < 45) {
    /* Text put straight into the element: white space that is spelled with a reference, a line feed, an
       odd character - where it stands in a table, a colgroup or a head it is a different thing. */
    static const char * const BITS[] = {"\r", " ", "\n", " \r\n", "x", "\xC2\xA0", "\t", "&", "<"};
    Node * t = NULL;
    const char * bit = chance(&g->r, 70) ? ONE_OF(&g->r, BITS) : a_text(g);
    if (gtext_markup_create_text(g->doc, bit, strlen(bit), &t) != GTEXT_MARKUP_OK
        || gtext_markup_append_child(target, t) != GTEXT_MARKUP_OK) {
      die("cannot place the perturbing text");
    }
    return;
  }
  const char * ns;
  const char * name = wild_name(g, &ns);
  if (gtext_markup_create_element(g->doc, ns, NULL, name, &e) != GTEXT_MARKUP_OK) {
    die("cannot create the perturbing element");
  }
  if (!tns || strcmp(tns, ns) != 0) {
    xmlns(e, NULL, ns);
  }
  if (chance(&g->r, 40)) {
    text(g, e, a_text(g));
  }
  else if (chance(&g->r, 40)) {
    gen_wild(g, e, ns, 4);
  }
  Node * before = NULL;
  const size_t kids = gtext_markup_node_child_count(target);
  if (kids && chance(&g->r, 70)) {
    before = gtext_markup_node_first_child(target);
    for (size_t i = pick(&g->r, kids); i > 0 && before; i--) {
      before = gtext_markup_node_next_sibling(before);
    }
  }
  if ((before ? gtext_markup_insert_before(target, e, before) : gtext_markup_append_child(target, e))
      != GTEXT_MARKUP_OK) {
    die("cannot place the perturbing element");
  }
}

static const char * gen_document(Gen * g, uint64_t id) {
  (void) id;
  g->budget = 160;
  if (!g->tame && chance(&g->r, 60)) {
    const FocusGroup * fg = &FOCUS_GROUPS[pick(&g->r, COUNT(FOCUS_GROUPS))];
    g->focus = fg->names;
    g->nfocus = fg->count;
  }
  const size_t shape = g->tame ? (pick(&g->r, 100) < 92 ? 0 : 1) : pick(&g->r, 12);
  if (g->tame || shape < 6) {
    if (chance(&g->r, 25)) {
      comment(g, NULL, ONE_OF(&g->r, COMMENTS));
    }
    maybe_doctype(g, g->tame ? 85 : 70);
    Node * html = el(g, NULL, "html", NS_XHTML);
    if (!(g->tame ? chance(&g->r, 10) : chance(&g->r, 30))) {
      xmlns(html, NULL, NS_XHTML);
    }
    if (chance(&g->r, 30)) {
      attr(html, NULL, NULL, "lang", "en");
    }
    Node * head = el(g, html, "head", NS_XHTML);
    head_content(g, head);
    if (!g->tame && chance(&g->r, 10)) {
      text(g, html, "\n");
    }
    Node * body = el(g, html, "body", NS_XHTML);
    some_attrs(g, body, 0);
    if (g->tame) {
      gen_flow(g, body, 0, 0, 0, 0);
    }
    else {
      gen_wild(g, body, NS_XHTML, 0);
    }
    if (g->tame && chance(&g->r, 60)) {
      perturb(g);
      return "tame+change";
    }
    return g->tame ? "tame" : g->focus ? "wild:focus" : "wild:standard";
  }
  if (shape < 8) {
    maybe_doctype(g, 50);
    Node * html = el(g, NULL, "html", NS_XHTML);
    xmlns(html, NULL, NS_XHTML);
    gen_wild(g, html, NS_XHTML, 0);
    return "wild:html-free";
  }
  if (shape < 10) {
    maybe_doctype(g, 30);
    const char * ns;
    const char * name = wild_name(g, &ns);
    Node * e = el(g, NULL, name, ns);
    xmlns(e, NULL, ns);
    some_attrs(g, e, strcmp(ns, NS_SVG) == 0 ? 1 : strcmp(ns, NS_MATH) == 0 ? 2 : 0);
    gen_wild(g, e, ns, 0);
    return "wild:other-root";
  }
  if (shape == 10) {
    Node * s = el(g, NULL, "svg", NS_SVG);
    xmlns(s, NULL, NS_SVG);
    gen_svg(g, s, 0);
    return "wild:svg-root";
  }
  Node * m = el(g, NULL, "math", NS_MATH);
  xmlns(m, NULL, NS_MATH);
  gen_math(g, m, 0);
  return "wild:math-root";
}

/* ---- writing ---------------------------------------------------------------------------- */

static const char * hazard_name(GTEXT_XML_Xhtml_Hazard k) {
  switch (k) {
    case GTEXT_XML_XHTML_HAZARD_NONE: return "none";
    case GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART: return "table-implied-part";
    case GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT: return "table-foster-parent";
    case GTEXT_XML_XHTML_HAZARD_P_BLOCK: return "p-block";
    case GTEXT_XML_XHTML_HAZARD_NESTED_A: return "nested-a";
    case GTEXT_XML_XHTML_HAZARD_NESTED_FORM: return "nested-form";
    case GTEXT_XML_XHTML_HAZARD_IMPLIED_END: return "implied-end";
    case GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT: return "raw-text-content";
    case GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE: return "document-structure";
    case GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT: return "special-element";
    case GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT: return "foreign-content";
    case GTEXT_XML_XHTML_HAZARD_NAMESPACED_ATTRIBUTE: return "namespaced-attribute";
    case GTEXT_XML_XHTML_HAZARD_DOCTYPE: return "doctype";
    default: return "other";
  }
}

/* The corruptions of the writer's output that html_diff.py --plant asks for. Each is a
 * defect a real change to the writer could have: the pre-XHTML rule that wrote an empty
 * element as <x/>, a script written as CDATA, a void element given an end tag, a lost
 * attribute, a lost doctype, a swapped case. Returns the new length (the buffer is large
 * enough), and sets *fired when it changed the document. */
static size_t apply_plant(const char * plant, uint64_t n, char * buf, size_t len, size_t cap, int * fired) {
  (void) cap;
  if (!plant || !*plant) {
    return len;
  }
  *fired = 0;
  if (strcmp(plant, "selfclose-empty") == 0 && n % 7 == 0) {
    /* <p></p> -> <p/> for the first empty non-void element. */
    for (size_t i = 0; i + 4 < len; i++) {
      if (buf[i] == '<' && buf[i + 1] >= 'a' && buf[i + 1] <= 'z') {
        size_t j = i + 1;
        while (j < len && buf[j] != '>' && buf[j] != ' ') {
          j++;
        }
        if (j < len && buf[j] == '>' && j + 1 + 2 + (j - i - 1) + 1 <= len && buf[j + 1] == '<'
            && buf[j + 2] == '/' && memcmp(buf + j + 3, buf + i + 1, j - i - 1) == 0
            && buf[j + 3 + (j - i - 1)] == '>') {
          const size_t name = j - i - 1;
          /* <name> </name>  ->  <name/> followed by the rest. */
          const size_t tail = len - (j + 3 + name + 1);
          buf[j] = '/';
          buf[j + 1] = '>';
          memmove(buf + j + 2, buf + j + 3 + name + 1, tail);
          *fired = 1;
          return j + 2 + tail;
        }
      }
    }
    return len;
  }
  if (strcmp(plant, "cdata-script") == 0 && n % 5 == 0) {
    const char * open = "<script>";
    const char * hit = NULL;
    for (size_t i = 0; i + 8 < len; i++) {
      if (memcmp(buf + i, open, 8) == 0) {
        hit = buf + i + 8;
        break;
      }
    }
    if (hit && (size_t) (hit - buf) < len && *hit != '<') {
      const size_t at = (size_t) (hit - buf);
      memmove(buf + at + 9, buf + at, len - at);
      memcpy(buf + at, "<![CDATA[", 9);
      /* The closing: before the next </script>. */
      for (size_t i = at + 9; i + 9 <= len + 9; i++) {
        if (memcmp(buf + i, "</script>", 9) == 0) {
          memmove(buf + i + 3, buf + i, len + 9 - i);
          memcpy(buf + i, "]]>", 3);
          *fired = 1;
          return len + 12;
        }
      }
      return len;
    }
    return len;
  }
  if (strcmp(plant, "void-end-tag") == 0 && n % 5 == 0) {
    /* <br /> -> <br></br>: the same tree to an XML parser, and to HTML a br and a second one
       (an end tag </br> is read as a start tag). */
    for (size_t i = 0; i + 6 <= len; i++) {
      if (memcmp(buf + i, "<br />", 6) == 0) {
        memmove(buf + i + 9, buf + i + 6, len - i - 6);
        memcpy(buf + i, "<br></br>", 9);
        *fired = 1;
        return len + 3;
      }
    }
    return len;
  }
  if (strcmp(plant, "drop-doctype") == 0 && n % 3 == 0) {
    if (len > 15 && memcmp(buf, "<!DOCTYPE html>", 15) == 0) {
      memmove(buf, buf + 15, len - 15);
      *fired = 1;
      return len - 15;
    }
    return len;
  }
  if (strcmp(plant, "wrong-escape") == 0 && n % 4 == 0) {
    /* A text ampersand left as a character reference to a different character. */
    for (size_t i = 0; i + 5 <= len; i++) {
      if (memcmp(buf + i, "&amp;", 5) == 0) {
        memcpy(buf + i, "&lt;\0\0", 4);
        memmove(buf + i + 4, buf + i + 5, len - i - 5);
        *fired = 1;
        return len - 1;
      }
    }
    return len;
  }
  return len;
}

/* Write `doc` with the xhtml mode and print one ITEM record. Returns 1 when the tree was
 * accepted, 0 when refused. `n` numbers the accepted-or-not tree for the plants. */
static int emit_item(const char * id, const char * tags, Doc * doc, const char * plant, uint64_t n,
    uint64_t * fired_total) {
  GTEXT_XML_Write_Options o = gtext_xml_write_options_default();
  o.xhtml = true;
  o.xml_declaration = GTEXT_XML_DECLARATION_NEVER;
  /* A fifth of the generated trees are written in US-ASCII, where every character beyond it is a
     numeric reference, and a few in ISO-8859-1: a reference is read by HTML on its own terms. */
  char tagbuf[96];
  if (n != 0 && n % 20 == 0) {
    /* HTML reads this label as windows-1252. The declaration is what lets the bytes be read back
       at all (the writer refuses non-ASCII ISO-8859-1 without one); html_diff.py takes it off before
       the references see the document, as an HTML parser's decoder would read it as a comment. */
    o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
    o.xml_declaration = GTEXT_XML_DECLARATION_ALWAYS;
    snprintf(tagbuf, sizeof(tagbuf), "%s/latin1", tags);
    tags = tagbuf;
  }
  else if (n != 0 && n % 5 == 0) {
    o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
    snprintf(tagbuf, sizeof(tagbuf), "%s/ascii", tags);
    tags = tagbuf;
  }
  /* And one in nine is pretty-printed. The white space that adds is not in the tree that was
     written, so such a record is held to what the XML parser reads from its own bytes (html_diff.py
     sees the tag): the question is whether the two parsers agree about the indented document. */
  char tagbuf2[128];
  if (n != 0 && n % 9 == 0 && o.encoding == GTEXT_XML_OUTPUT_UTF8) {
    o.indent = "  ";
    snprintf(tagbuf2, sizeof(tagbuf2), "%s/pretty", tags);
    tags = tagbuf2;
  }
  char * buf = NULL;
  size_t len = 0;
  GTEXT_XML_Write_Result r;
  const GTEXT_XML_Status st = gtext_xml_write_buffer(doc, &o, &buf, &len, &r);
  printf("ITEM %s %s ", id, tags);
  if (st != GTEXT_XML_OK) {
    printf("%s 0 none ", st == GTEXT_XML_E_XHTML ? "XHTML" : "OTHER");
    put_str(r.path);
    printf(" ");
    put_str(r.message);
    printf(" %d\n\n", 0);
    return 0;
  }
  size_t out_len = len;
  char * work = buf;
  if (plant && *plant && r.xhtml_hazards == 0) {
    work = (char *) malloc(len + 64);
    if (!work) {
      die("out of memory");
    }
    memcpy(work, buf, len);
    int fired = 0;
    out_len = apply_plant(plant, n, work, len, len + 64, &fired);
    if (fired) {
      (*fired_total)++;
    }
  }
  printf("OK %zu %s ", r.xhtml_hazards, hazard_name(r.xhtml_hazard_kind));
  put_str(r.xhtml_hazard_path);
  printf(" ");
  put_str(r.message);
  printf(" %zu\n", out_len);
  fwrite(work, 1, out_len, stdout);
  fputc('\n', stdout);
  if (work != buf) {
    free(work);
  }
  gtext_xml_write_buffer_free(doc, &o, buf);
  return 1;
}

/* ---- the dump format, read back as a tree ------------------------------------------------------ */

#define MAX_DUMP_DEPTH 2048

typedef struct {
  char message[256];
} DumpError;

static const char * ns_for_word(const char * word, size_t n) {
  if (n == 3 && memcmp(word, "svg", 3) == 0) {
    return NS_SVG;
  }
  if (n == 4 && memcmp(word, "math", 4) == 0) {
    return NS_MATH;
  }
  return NULL;
}

static int starts_with(const char * s, size_t n, const char * prefix) {
  const size_t p = strlen(prefix);
  return n >= p && memcmp(s, prefix, p) == 0;
}

/* One logical entry of the dump, already stripped of "| " and its indent. */
static int add_entry(Doc * doc, Node ** stack, int depth, char * item, size_t n, DumpError * err) {
  Node * parent = stack[depth];
  if (!parent) {
    snprintf(err->message, sizeof(err->message), "no parent at depth %d", depth);
    return -1;
  }
  Node * made = NULL;
  GTEXT_Markup_Status st;
  if (n >= 2 && item[0] == '"' && item[n - 1] == '"') {
    st = gtext_markup_create_text(doc, item + 1, n - 2, &made);
    if (st == GTEXT_MARKUP_OK) {
      st = gtext_markup_append_child(parent, made);
    }
    if (st != GTEXT_MARKUP_OK) {
      snprintf(err->message, sizeof(err->message), "text: status %d", (int) st);
      return -1;
    }
    return 0;
  }
  if (starts_with(item, n, "<!-- ") && n >= 9 && memcmp(item + n - 4, " -->", 4) == 0) {
    st = gtext_markup_create_comment(doc, item + 5, n - 9, &made);
    if (st == GTEXT_MARKUP_OK) {
      st = gtext_markup_append_child(parent, made);
    }
    if (st != GTEXT_MARKUP_OK) {
      snprintf(err->message, sizeof(err->message), "comment: status %d", (int) st);
      return -1;
    }
    return 0;
  }
  if (starts_with(item, n, "<!DOCTYPE ")) {
    char * name = item + 10;
    char * end = item + n - 1; /* the '>' */
    if (*end != '>') {
      snprintf(err->message, sizeof(err->message), "doctype without >");
      return -1;
    }
    *end = '\0';
    char * pub = NULL;
    char * sys = NULL;
    char * q = strstr(name, " \"");
    if (q) {
      *q = '\0';
      char * p1 = q + 2;
      char * mid = strstr(p1, "\" \"");
      if (!mid || end[-1] != '"') {
        snprintf(err->message, sizeof(err->message), "doctype identifiers");
        return -1;
      }
      *mid = '\0';
      pub = p1;
      sys = mid + 3;
      end[-1] = '\0';
    }
    st = gtext_markup_create_doctype(doc, name, pub, sys, &made);
    if (st == GTEXT_MARKUP_OK) {
      st = gtext_markup_append_child(parent, made);
    }
    if (st != GTEXT_MARKUP_OK) {
      snprintf(err->message, sizeof(err->message), "doctype: status %d", (int) st);
      return -1;
    }
    return 0;
  }
  if (item[0] == '<') {
    char * inner = item + 1;
    size_t inner_len = n - 2;
    if (item[n - 1] != '>') {
      snprintf(err->message, sizeof(err->message), "element without >");
      return -1;
    }
    inner[inner_len] = '\0';
    const char * ns = NS_XHTML;
    char * space = strchr(inner, ' ');
    if (space) {
      const char * foreign = ns_for_word(inner, (size_t) (space - inner));
      if (foreign) {
        ns = foreign;
        inner = space + 1;
      }
    }
    st = gtext_markup_create_element(doc, ns, NULL, inner, &made);
    if (st == GTEXT_MARKUP_OK) {
      st = gtext_markup_append_child(parent, made);
    }
    if (st != GTEXT_MARKUP_OK) {
      snprintf(err->message, sizeof(err->message), "element name %s: status %d", inner, (int) st);
      return -1;
    }
    if (depth + 1 >= MAX_DUMP_DEPTH) {
      snprintf(err->message, sizeof(err->message), "too deep");
      return -1;
    }
    stack[depth + 1] = made;
    return 0;
  }
  if (n == 7 && memcmp(item, "content", 7) == 0) {
    if (depth + 1 >= MAX_DUMP_DEPTH) {
      return -1;
    }
    stack[depth + 1] = parent; /* the template's content reads as its children */
    return 0;
  }
  /* An attribute of the element at `parent`: name="value", the name possibly "prefix local". */
  char * eq = strstr(item, "=\"");
  if (!eq || item[n - 1] != '"') {
    snprintf(err->message, sizeof(err->message), "unreadable entry %.40s", item);
    return -1;
  }
  *eq = '\0';
  item[n - 1] = '\0';
  const char * name = item;
  const char * value = eq + 2;
  if (strncmp(name, "xmlns", 5) == 0 && (name[5] == '\0' || name[5] == ' ' || name[5] == ':')) {
    return 0; /* rule 3 drops these from both sides */
  }
  const char * ans = NULL;
  const char * aprefix = NULL;
  const char * local = name;
  char * sp = strchr(item, ' ');
  if (sp) {
    *sp = '\0';
    local = sp + 1;
    if (strcmp(name, "xlink") == 0) {
      ans = NS_XLINK;
    }
    else if (strcmp(name, "xml") == 0) {
      ans = NS_XML;
    }
    else {
      snprintf(err->message, sizeof(err->message), "unknown attribute prefix %s", name);
      return -1;
    }
    aprefix = name;
  }
  st = gtext_markup_element_set_attribute(parent, ans, aprefix, local, value, strlen(value));
  if (st != GTEXT_MARKUP_OK) {
    snprintf(err->message, sizeof(err->message), "attribute %s: status %d", local, (int) st);
    return -1;
  }
  return 0;
}

static Doc * tree_from_dump(const char * dump, size_t len, DumpError * err) {
  Doc * doc = new_doc();
  Node ** stack = (Node **) calloc(MAX_DUMP_DEPTH + 2, sizeof(Node *));
  char * copy = (char *) malloc(len + 2);
  if (!stack || !copy) {
    die("out of memory");
  }
  memcpy(copy, dump, len);
  copy[len] = '\0';
  stack[0] = gtext_markup_document_root(doc);
  /* Split into entries: a line starting "| " begins one; other lines continue the last. */
  char * entry = NULL;
  size_t entry_len = 0;
  int bad = 0;
  size_t at = 0;
  for (;;) {
    if (bad) {
      break;
    }
    const int done = at >= len;
    size_t eol = at;
    while (eol < len && copy[eol] != '\n') {
      eol++;
    }
    const int is_start = eol - at >= 2 && copy[at] == '|' && copy[at + 1] == ' ';
    if (is_start || done) {
      if (entry) {
        /* Finish the previous entry. */
        entry[entry_len] = '\0';
        size_t spaces = 0;
        while (entry[spaces] == ' ') {
          spaces++;
        }
        if (spaces % 2 != 0 || (int) (spaces / 2) >= MAX_DUMP_DEPTH) {
          snprintf(err->message, sizeof(err->message), "odd indentation");
          bad = 1;
        }
        else if (add_entry(doc, stack, (int) (spaces / 2), entry + spaces, entry_len - spaces, err) != 0) {
          bad = 1;
        }
      }
      if (done) {
        break;
      }
      entry = copy + at + 2;
      entry_len = eol - at - 2;
    }
    else if (entry) {
      /* A continuation line: the entry runs to the end of this one, newline included. */
      entry_len = (size_t) (copy + eol - entry);
    }
    at = eol + 1;
  }
  free(stack);
  free(copy);
  if (bad) {
    gtext_markup_document_free(doc);
    return NULL;
  }
  return doc;
}

/* ---- input ------------------------------------------------------------------------------------- */

typedef struct {
  char id[512];
  char * data;
  size_t length;
} Frame;

#define MAX_FRAME (1L << 28)

static int read_frame(Frame * f) {
  char header[512];
  if (!fgets(header, sizeof(header), stdin)) {
    return 0;
  }
  const size_t hl = strlen(header);
  if (hl == 0 || header[hl - 1] != '\n') {
    die("a frame header is a line of at most 511 bytes");
  }
  header[hl - 1] = '\0';
  char * sp = strrchr(header, ' ');
  if (!sp || sp == header) {
    die("a frame header is `<id> <length>`");
  }
  *sp = '\0';
  snprintf(f->id, sizeof(f->id), "%s", header);
  char * end = NULL;
  const long n = strtol(sp + 1, &end, 10);
  if (end == sp + 1 || *end != '\0' || n < 0 || n > MAX_FRAME) {
    die("a frame length is a number from 0 to 2^28");
  }
  f->data = (char *) malloc((size_t) n + 1);
  if (!f->data) {
    die("out of memory");
  }
  if (n && fread(f->data, 1, (size_t) n, stdin) != (size_t) n) {
    die("a frame ended early");
  }
  f->data[n] = '\0';
  f->length = (size_t) n;
  if (fgetc(stdin) != '\n') {
    die("a frame's bytes are followed by a line feed");
  }
  return 1;
}

/* ---- the modes --------------------------------------------------------------------------------- */

/* The tree for generated item `i` of population `seed`; a function of the two and nothing else,
 * which is how `compare` gets back the tree a record was written from. */
static Doc * make_tree(uint64_t seed, uint64_t i, const char ** shape) {
  Gen g;
  memset(&g, 0, sizeof(g));
  /* (seed, i) -> state is one-to-one in i for a seed, and the seeds are spread by splitmix, so no two
     trees of a population share a generator state (seed * K + i, as it was, shared them). */
  uint64_t x = seed;
  rng_seed(&g.r, splitmix(&x) + i * 0x9E3779B97F4A7C15ULL);
  g.tame = pick(&g.r, 100) < 55;
  g.doc = new_doc();
  const char * s = gen_document(&g, i);
  if (shape) {
    *shape = s;
  }
  return g.doc;
}

static int mode_gen(uint64_t count, uint64_t seed, const char * plant) {
  uint64_t fired = 0;
  for (uint64_t i = 1; i <= count; i++) {
    const char * shape = NULL;
    Doc * doc = make_tree(seed, i, &shape);
    char id[64];
    snprintf(id, sizeof(id), "gen:%" PRIu64 ":%" PRIu64, seed, i);
    emit_item(id, shape, doc, plant, i, &fired);
    gtext_markup_document_free(doc);
  }
  if (plant && *plant) {
    printf("PLANT %" PRIu64 "\n", fired);
  }
  return 0;
}

/* The fixed case: one tree for every entry of every vocabulary table, so that a transcription error in
 * a table is seen by both references rather than assumed away. */
static Doc * fixed_doc(Gen * g, Node ** body_out) {
  g->doc = new_doc();
  g->budget = 1000;
  Node * html = el(g, NULL, "html", NS_XHTML);
  xmlns(html, NULL, NS_XHTML);
  el(g, html, "head", NS_XHTML);
  *body_out = el(g, html, "body", NS_XHTML);
  return g->doc;
}

static void fixed_emit_n(Gen * g, const char * id, uint64_t n) {
  uint64_t fired = 0;
  emit_item(id, "fixed", g->doc, NULL, n, &fired);
  gtext_markup_document_free(g->doc);
}

static void fixed_emit(Gen * g, const char * id) {
  fixed_emit_n(g, id, 0);
}

static int mode_fixed(void) {
  Gen g;
  memset(&g, 0, sizeof(g));
  rng_seed(&g.r, 1);
  char id[160];
  Node * body;
  for (size_t i = 0; i < COUNT(k_svg_tags); i++) {
    fixed_doc(&g, &body);
    Node * s = el(&g, body, "svg", NS_SVG);
    xmlns(s, NULL, NS_SVG);
    Node * e = el(&g, s, k_svg_tags[i], NS_SVG);
    (void) e;
    snprintf(id, sizeof(id), "fixed:svg-element:%s", k_svg_tags[i]);
    fixed_emit(&g, id);
  }
  /* The folded form of each entry is a different name to HTML (it restores the entry): the writer must call
     it a hazard, which html_diff.py asserts of every id ending in `-folded`. */
  for (size_t i = 0; i < COUNT(k_svg_tags); i++) {
    char low[64];
    snprintf(low, sizeof(low), "%s", k_svg_tags[i]);
    for (char * p = low; *p; p++) {
      *p = (char) (*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    }
    fixed_doc(&g, &body);
    Node * s = el(&g, body, "svg", NS_SVG);
    xmlns(s, NULL, NS_SVG);
    el(&g, s, low, NS_SVG);
    snprintf(id, sizeof(id), "fixed:svg-element-folded:%s", low);
    fixed_emit(&g, id);
  }
  for (size_t i = 0; i < COUNT(k_svg_attributes); i++) {
    char low[64];
    snprintf(low, sizeof(low), "%s", k_svg_attributes[i]);
    for (char * p = low; *p; p++) {
      *p = (char) (*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    }
    fixed_doc(&g, &body);
    Node * s = el(&g, body, "svg", NS_SVG);
    xmlns(s, NULL, NS_SVG);
    attr(s, NULL, NULL, low, "v");
    snprintf(id, sizeof(id), "fixed:svg-attribute-folded:%s", low);
    fixed_emit(&g, id);
  }
  for (size_t i = 0; i < COUNT(k_svg_attributes); i++) {
    fixed_doc(&g, &body);
    Node * s = el(&g, body, "svg", NS_SVG);
    xmlns(s, NULL, NS_SVG);
    attr(s, NULL, NULL, k_svg_attributes[i], "v");
    snprintf(id, sizeof(id), "fixed:svg-attribute:%s", k_svg_attributes[i]);
    fixed_emit(&g, id);
  }
  for (size_t i = 0; i < COUNT(k_math_attributes); i++) {
    fixed_doc(&g, &body);
    Node * m = el(&g, body, "math", NS_MATH);
    xmlns(m, NULL, NS_MATH);
    attr(m, NULL, NULL, k_math_attributes[i], "v");
    snprintf(id, sizeof(id), "fixed:math-attribute:%s", k_math_attributes[i]);
    fixed_emit(&g, id);
  }
  /* Ruby: every part inside every part (and inside a span and a p) of a ruby. The writer calls the ones HTML
     ends hazards; the rest must be read back as written. */
  {
    static const char * const PARENTS[] = {"rb", "rt", "rp", "rtc", "span", "p", "li"};
    static const char * const CHILDREN[] = {"rb", "rt", "rp", "rtc"};
    for (size_t i = 0; i < COUNT(PARENTS); i++) {
      for (size_t j = 0; j < COUNT(CHILDREN); j++) {
        fixed_doc(&g, &body);
        Node * ruby = el(&g, body, "ruby", NS_XHTML);
        Node * parent = el(&g, ruby, PARENTS[i], NS_XHTML);
        el(&g, parent, CHILDREN[j], NS_XHTML);
        snprintf(id, sizeof(id), "fixed:ruby:%s:%s", PARENTS[i], CHILDREN[j]);
        fixed_emit(&g, id);
      }
    }
  }
  /* The places the writer's rules are about, one tree each, so that each rule is exercised on every run
     whatever the random population holds: text where white space and other text differ, markup where
     HTML reads text, depth in a select, the end of the document, an indented document, a reference in
     an element read as text, the li chain. */
  {
    static const char * const BITS[] = {"x", "\r", " \n", "\xC2\xA0"};
    for (size_t i = 0; i < COUNT(BITS); i++) {
      const char * name[] = {"bit-x", "bit-cr", "bit-space", "bit-nbsp"};
      fixed_doc(&g, &body);
      text(&g, el(&g, el(&g, body, "table", NS_XHTML), "colgroup", NS_XHTML), BITS[i]);
      snprintf(id, sizeof(id), "fixed:colgroup-text:%s", name[i]);
      fixed_emit(&g, id);
      Node * html = gtext_markup_node_parent(body);
      fixed_doc(&g, &body);
      html = gtext_markup_node_parent(body);
      text(&g, gtext_markup_node_first_child(html), BITS[i]);
      snprintf(id, sizeof(id), "fixed:head-text:%s", name[i]);
      fixed_emit(&g, id);
      fixed_doc(&g, &body);
      html = gtext_markup_node_parent(body);
      text(&g, el(&g, gtext_markup_node_first_child(html), "noscript", NS_XHTML), BITS[i]);
      snprintf(id, sizeof(id), "fixed:head-noscript-text:%s", name[i]);
      fixed_emit(&g, id);
      fixed_doc(&g, &body);
      text(&g, el(&g, body, "table", NS_XHTML), BITS[i]);
      snprintf(id, sizeof(id), "fixed:table-text:%s", name[i]);
      fixed_emit(&g, id);
    }
  }
  {
    fixed_doc(&g, &body);
    el(&g, el(&g, el(&g, body, "select", NS_XHTML), "option", NS_XHTML), "b", NS_XHTML);
    fixed_emit(&g, "fixed:select-option-b");
    fixed_doc(&g, &body);
    el(&g, el(&g, el(&g, body, "select", NS_XHTML), "optgroup", NS_XHTML), "hr", NS_XHTML);
    fixed_emit(&g, "fixed:select-optgroup-hr");
    fixed_doc(&g, &body);
    el(&g, el(&g, el(&g, body, "select", NS_XHTML), "option", NS_XHTML), "template", NS_XHTML);
    fixed_emit(&g, "fixed:select-option-template");
    fixed_doc(&g, &body);
    el(&g, el(&g, el(&g, body, "li", NS_XHTML), "search", NS_XHTML), "li", NS_XHTML);
    fixed_emit(&g, "fixed:li-search-li");
    fixed_doc(&g, &body);
    el(&g, el(&g, body, "nobr", NS_XHTML), "nobr", NS_XHTML);
    fixed_emit(&g, "fixed:nobr-nobr");
    static const char * const RAWISH[] = {"iframe", "noembed", "noframes", "xmp", "noscript"};
    for (size_t i = 0; i < COUNT(RAWISH); i++) {
      fixed_doc(&g, &body);
      comment(&g, el(&g, body, RAWISH[i], NS_XHTML), "c");
      snprintf(id, sizeof(id), "fixed:comment-in:%s", RAWISH[i]);
      fixed_emit(&g, id);
      /* US-ASCII (n = 5): the euro sign is a reference, shown literally where HTML reads text. */
      fixed_doc(&g, &body);
      text(&g, el(&g, body, RAWISH[i], NS_XHTML), "\xE2\x82\xAC");
      snprintf(id, sizeof(id), "fixed:reference-in:%s", RAWISH[i]);
      fixed_emit_n(&g, id, 5);
    }
    /* After the root, and indented (n = 9). */
    fixed_doc(&g, &body);
    comment(&g, NULL, "after");
    fixed_emit_n(&g, "fixed:comment-after-root", 9);
    fixed_doc(&g, &body);
    el(&g, el(&g, body, "div", NS_XHTML), "p", NS_XHTML);
    fixed_emit_n(&g, "fixed:indented-html", 9);
  }
  for (size_t i = 0; i < COUNT(k_elements); i++) {
    fixed_doc(&g, &body);
    Node * e = el(&g, body, k_elements[i].name, NS_XHTML);
    if (!(k_elements[i].flags & GTEXT_MARKUP_HTML_VOID)) {
      text(&g, e, "x");
    }
    snprintf(id, sizeof(id), "fixed:html-element:%s", k_elements[i].name);
    fixed_emit(&g, id);
  }
  for (size_t i = 0; i < COUNT(k_booleans); i++) {
    fixed_doc(&g, &body);
    const char * element = k_booleans[i].element[0] ? k_booleans[i].element : "div";
    Node * e = el(&g, body, element, NS_XHTML);
    attr(e, NULL, NULL, k_booleans[i].attribute, k_booleans[i].attribute);
    snprintf(id, sizeof(id), "fixed:boolean:%s:%s", k_booleans[i].attribute, element);
    fixed_emit(&g, id);
  }
  return 0;
}

static int mode_fromdump(void) {
  Frame f;
  uint64_t fired = 0;
  while (read_frame(&f)) {
    DumpError err;
    memset(&err, 0, sizeof(err));
    Doc * doc = tree_from_dump(f.data, f.length, &err);
    if (!doc) {
      printf("ITEM %s corpus:dump UNBUILDABLE 0 none . ", f.id);
      put_str(err.message);
      printf(" 0\n\n");
    }
    else {
      emit_item(f.id, "corpus:wpt-tree", doc, NULL, 0, &fired);
      gtext_markup_document_free(doc);
    }
    free(f.data);
  }
  return 0;
}

static int mode_fromxml(void) {
  Frame f;
  uint64_t fired = 0;
  while (read_frame(&f)) {
    Doc * doc = NULL;
    GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
    GTEXT_XML_Error perr;
    memset(&perr, 0, sizeof(perr));
    const GTEXT_XML_Status st = gtext_xml_parse(f.data, f.length, &po, &doc, &perr);
    if (st != GTEXT_XML_OK) {
      printf("ITEM %s corpus:xml NOTPARSED 0 none . ", f.id);
      put_str(gtext_xml_status_string(st));
      printf(" 0\n\n");
    }
    else {
      emit_item(f.id, "corpus:xml", doc, NULL, 0, &fired);
      gtext_markup_document_free(doc);
    }
    gtext_xml_error_free(&perr);
    free(f.data);
  }
  return 0;
}

static void put_diff(int rc, const HtmlEq_Diff * d) {
  printf(" %d ", rc);
  if (rc == 0) {
    put_str(d->path);
    printf(" ");
    put_str(d->why);
    printf(" ");
    put_str(d->ctx);
  }
  else if (rc == 3) {
    printf(". ");
    put_str(d->why);
    printf(" .");
  }
  else {
    printf(". . .");
  }
}

/* The source tree of a compare record: "gen:<seed>:<i>", "xml:<bytes>" or "dump:<bytes>". */
static Doc * source_tree(const Frame * f, DumpError * err) {
  if (starts_with(f->data, f->length, "gen:")) {
    uint64_t seed = 0, i = 0;
    if (sscanf(f->data + 4, "%" SCNu64 ":%" SCNu64, &seed, &i) != 2) {
      die("a gen source is gen:<seed>:<i>");
    }
    return make_tree(seed, i, NULL);
  }
  if (starts_with(f->data, f->length, "xml:")) {
    Doc * doc = NULL;
    GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
    GTEXT_XML_Error perr;
    memset(&perr, 0, sizeof(perr));
    const GTEXT_XML_Status st = gtext_xml_parse(f->data + 4, f->length - 4, &po, &doc, &perr);
    gtext_xml_error_free(&perr);
    if (st != GTEXT_XML_OK) {
      snprintf(err->message, sizeof(err->message), "the source document does not parse");
      return NULL;
    }
    return doc;
  }
  if (starts_with(f->data, f->length, "dump:")) {
    return tree_from_dump(f->data + 5, f->length - 5, err);
  }
  die("a source is gen:, xml: or dump:");
}

/* Compare one tree against another, or say why that cannot be done. */
static void compare_into(const Node * a, const Doc * b, const char * what, const DumpError * e) {
  HtmlEq_Diff d;
  memset(&d, 0, sizeof(d));
  if (!b) {
    snprintf(d.why, sizeof(d.why), "%s: %s", what, e->message);
    put_diff(3, &d);
    return;
  }
  const int rc = htmleq_compare(a, gtext_markup_document_root((Doc *) b), "", &d);
  if (rc == HTMLEQ_OOM) {
    die("out of memory comparing trees");
  }
  if (rc == HTMLEQ_TOO_DEEP) {
    snprintf(d.why, sizeof(d.why), "%s: tree deeper than %d levels", what, HTMLEQ_MAX_DEPTH);
    put_diff(3, &d);
    return;
  }
  put_diff(rc, &d);
}

/* Four frames to a document: the source, the bytes the writer produced, html5lib's dump
 * of them, parse5's. Four verdicts back: source against what the XML parser reads from the
 * bytes (the writer's own guarantee), against html5lib, against parse5, and html5lib
 * against parse5. */
static int mode_compare(void) {
  Frame src, xml, h5, p5;
  while (read_frame(&src)) {
    if (!read_frame(&xml) || !read_frame(&h5) || !read_frame(&p5)) {
      die("compare takes four frames to a document");
    }
    DumpError es, ex, e1, e2;
    memset(&es, 0, sizeof(es));
    memset(&ex, 0, sizeof(ex));
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));
    Doc * sd = source_tree(&src, &es);
    Doc * xd = NULL;
    GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
    GTEXT_XML_Error perr;
    memset(&perr, 0, sizeof(perr));
    if (gtext_xml_parse(xml.data, xml.length, &po, &xd, &perr) != GTEXT_XML_OK) {
      snprintf(ex.message, sizeof(ex.message), "the written bytes do not parse as XML");
      xd = NULL;
    }
    gtext_xml_error_free(&perr);
    Doc * d1 = tree_from_dump(h5.data, h5.length, &e1);
    Doc * d2 = tree_from_dump(p5.data, p5.length, &e2);
    printf("CMP %s", xml.id);
    if (!sd) {
      printf(" SOURCEFAIL ");
      put_str(es.message);
      printf("\n");
    }
    else {
      const Node * sr = gtext_markup_document_root(sd);
      compare_into(sr, xd, "xml", &ex);
      compare_into(sr, d1, "html5lib", &e1);
      compare_into(sr, d2, "parse5", &e2);
      if (d1 && d2) {
        HtmlEq_Diff d;
        memset(&d, 0, sizeof(d));
        const int rc = htmleq_compare(gtext_markup_document_root(d1), gtext_markup_document_root(d2), "", &d);
        if (rc == HTMLEQ_OOM) {
          die("out of memory comparing trees");
        }
        if (rc == HTMLEQ_TOO_DEEP) {
          snprintf(d.why, sizeof(d.why), "references: tree deeper than %d levels", HTMLEQ_MAX_DEPTH);
        }
        put_diff(rc == HTMLEQ_TOO_DEEP ? 3 : rc, &d);
      }
      else {
        HtmlEq_Diff d;
        memset(&d, 0, sizeof(d));
        snprintf(d.why, sizeof(d.why), "a dump was not buildable");
        put_diff(3, &d);
      }
      printf("\n");
    }
    gtext_markup_document_free(sd);
    gtext_markup_document_free(xd);
    gtext_markup_document_free(d1);
    gtext_markup_document_free(d2);
    free(src.data);
    free(xml.data);
    free(h5.data);
    free(p5.data);
  }
  return 0;
}

int main(int argc, char ** argv) {
  if (argc >= 4 && strcmp(argv[1], "gen") == 0) {
    return mode_gen(strtoull(argv[2], NULL, 10), strtoull(argv[3], NULL, 10), argc > 4 ? argv[4] : NULL);
  }
  if (argc == 2 && strcmp(argv[1], "fixed") == 0) {
    return mode_fixed();
  }
  if (argc == 2 && strcmp(argv[1], "fromdump") == 0) {
    return mode_fromdump();
  }
  if (argc == 2 && strcmp(argv[1], "fromxml") == 0) {
    return mode_fromxml();
  }
  if (argc == 2 && strcmp(argv[1], "compare") == 0) {
    return mode_compare();
  }
  fprintf(stderr, "usage: html_ours gen <count> <seed> [plant] | fromdump | fromxml | compare\n");
  return 2;
}
