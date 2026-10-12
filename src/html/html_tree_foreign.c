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
 * Foreign content in the HTML tree builder: WHATWG HTML, "Tree construction", as served at
 * html.spec.whatwg.org/multipage/parsing.html on 2026-10-12 (Living Standard, last updated 9 October
 * 2026, Last-Modified Fri, 09 Oct 2026 08:28:19 GMT: the revision stages 3b to 3d were written from).
 * Written from the text of: 13.2.6 (the tree construction dispatcher, "MathML text integration
 * point", "HTML integration point"), 13.2.6.1 ("adjust MathML attributes", "adjust SVG attributes",
 * "adjust foreign attributes", "insert a foreign element"), the "math" and "svg" start tags of "in
 * body" (13.2.6.4.7), and 13.2.6.5 "The rules for parsing tokens in foreign content".
 *
 * ## The tables
 *
 * The three tables of the standard (the 37 SVG element names, the 58 SVG attribute names, the one
 * MathML attribute name) and the eleven namespaced attributes are data below, each sorted by its lower
 * case name for a binary search. They were extracted from the served text by script, and the unit test
 * `ForeignNameTablesAreTheStandardsAndTheVocabularys` checks them against the vocabulary
 * markup_html.c holds for the writers (`gtext_markup_html_foreign_name_survives`).
 *
 * ## How an element is told apart
 *
 * A name slot is the pair (namespace, lower case name): the slot of `g` in SVG is not the one of `g`
 * in MathML, and neither is the slot of the HTML name `g`. The stack's per-name indexes (the topmost
 * open element of a name, the scope floors) therefore never see a foreign element as an HTML one of
 * the same name (`<svg><title>` is not an HTML title and `<svg><td>` not a table cell), and the nine
 * elements that the standard makes scope boundaries and special (MathML mi, mo, mn, ms, mtext,
 * annotation-xml; SVG foreignObject, desc, title) carry those flags. "Is this the HTML namespace"
 * costs a comparison, and the foreign end tag rule's walk down the stack is one comparison of two
 * indexes (the nearest HTML element below the current node is an entry's `fl_html`).
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "html_tree_private.h"

typedef struct {
  const char * from;
  const char * to;
} AdjPair;

/* "Any other start tag" of 13.2.6.5: the tag names of the SVG namespace whose case is restored. */
static const AdjPair k_svg_tags[] = {
    {"altglyph", "altGlyph"},
    {"altglyphdef", "altGlyphDef"},
    {"altglyphitem", "altGlyphItem"},
    {"animatecolor", "animateColor"},
    {"animatemotion", "animateMotion"},
    {"animatetransform", "animateTransform"},
    {"clippath", "clipPath"},
    {"feblend", "feBlend"},
    {"fecolormatrix", "feColorMatrix"},
    {"fecomponenttransfer", "feComponentTransfer"},
    {"fecomposite", "feComposite"},
    {"feconvolvematrix", "feConvolveMatrix"},
    {"fediffuselighting", "feDiffuseLighting"},
    {"fedisplacementmap", "feDisplacementMap"},
    {"fedistantlight", "feDistantLight"},
    {"fedropshadow", "feDropShadow"},
    {"feflood", "feFlood"},
    {"fefunca", "feFuncA"},
    {"fefuncb", "feFuncB"},
    {"fefuncg", "feFuncG"},
    {"fefuncr", "feFuncR"},
    {"fegaussianblur", "feGaussianBlur"},
    {"feimage", "feImage"},
    {"femerge", "feMerge"},
    {"femergenode", "feMergeNode"},
    {"femorphology", "feMorphology"},
    {"feoffset", "feOffset"},
    {"fepointlight", "fePointLight"},
    {"fespecularlighting", "feSpecularLighting"},
    {"fespotlight", "feSpotLight"},
    {"fetile", "feTile"},
    {"feturbulence", "feTurbulence"},
    {"foreignobject", "foreignObject"},
    {"glyphref", "glyphRef"},
    {"lineargradient", "linearGradient"},
    {"radialgradient", "radialGradient"},
    {"textpath", "textPath"},
};

/* "Adjust SVG attributes" (13.2.6.1). */
static const AdjPair k_svg_attrs[] = {
    {"attributename", "attributeName"},
    {"attributetype", "attributeType"},
    {"basefrequency", "baseFrequency"},
    {"baseprofile", "baseProfile"},
    {"calcmode", "calcMode"},
    {"clippathunits", "clipPathUnits"},
    {"diffuseconstant", "diffuseConstant"},
    {"edgemode", "edgeMode"},
    {"filterunits", "filterUnits"},
    {"glyphref", "glyphRef"},
    {"gradienttransform", "gradientTransform"},
    {"gradientunits", "gradientUnits"},
    {"kernelmatrix", "kernelMatrix"},
    {"kernelunitlength", "kernelUnitLength"},
    {"keypoints", "keyPoints"},
    {"keysplines", "keySplines"},
    {"keytimes", "keyTimes"},
    {"lengthadjust", "lengthAdjust"},
    {"limitingconeangle", "limitingConeAngle"},
    {"markerheight", "markerHeight"},
    {"markerunits", "markerUnits"},
    {"markerwidth", "markerWidth"},
    {"maskcontentunits", "maskContentUnits"},
    {"maskunits", "maskUnits"},
    {"numoctaves", "numOctaves"},
    {"pathlength", "pathLength"},
    {"patterncontentunits", "patternContentUnits"},
    {"patterntransform", "patternTransform"},
    {"patternunits", "patternUnits"},
    {"pointsatx", "pointsAtX"},
    {"pointsaty", "pointsAtY"},
    {"pointsatz", "pointsAtZ"},
    {"preservealpha", "preserveAlpha"},
    {"preserveaspectratio", "preserveAspectRatio"},
    {"primitiveunits", "primitiveUnits"},
    {"refx", "refX"},
    {"refy", "refY"},
    {"repeatcount", "repeatCount"},
    {"repeatdur", "repeatDur"},
    {"requiredextensions", "requiredExtensions"},
    {"requiredfeatures", "requiredFeatures"},
    {"specularconstant", "specularConstant"},
    {"specularexponent", "specularExponent"},
    {"spreadmethod", "spreadMethod"},
    {"startoffset", "startOffset"},
    {"stddeviation", "stdDeviation"},
    {"stitchtiles", "stitchTiles"},
    {"surfacescale", "surfaceScale"},
    {"systemlanguage", "systemLanguage"},
    {"tablevalues", "tableValues"},
    {"targetx", "targetX"},
    {"targety", "targetY"},
    {"textlength", "textLength"},
    {"viewbox", "viewBox"},
    {"viewtarget", "viewTarget"},
    {"xchannelselector", "xChannelSelector"},
    {"ychannelselector", "yChannelSelector"},
    {"zoomandpan", "zoomAndPan"},
};

/* "Adjust foreign attributes" (13.2.6.1): the attributes that become namespaced. `which` is the kind of
   namespace (1 XLink, 2 XML, 3 XMLNS); the prefix and the local name are what is before and after the colon
   (xmlns has neither). Sorted by name. */
typedef struct {
  const char * name;
  unsigned which;
} ForeignAttr;

static const ForeignAttr k_foreign_attrs[] = {
    {"xlink:actuate", 1},
    {"xlink:arcrole", 1},
    {"xlink:href", 1},
    {"xlink:role", 1},
    {"xlink:show", 1},
    {"xlink:title", 1},
    {"xlink:type", 1},
    {"xml:lang", 2},
    {"xml:space", 2},
    {"xmlns", 3},
    {"xmlns:xlink", 3},
};

static int cmp_name(const char * bytes, size_t len, const char * name) {
  const size_t nl = strlen(name);
  const int c = memcmp(bytes, name, len < nl ? len : nl);
  if (c != 0) {
    return c;
  }
  return len < nl ? -1 : len > nl ? 1 : 0;
}

static const AdjPair * pair_find(const AdjPair * table, size_t n, const char * lower, size_t len) {
  size_t lo = 0;
  size_t hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_name(lower, len, table[mid].from);
    if (c == 0) {
      return &table[mid];
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return NULL;
}

const char * ht_svg_tag_adjusted(const char * lower, size_t len) {
  const AdjPair * p = pair_find(k_svg_tags, sizeof(k_svg_tags) / sizeof(k_svg_tags[0]), lower, len);
  return p ? p->to : NULL;
}

const char * ht_svg_attribute_adjusted(const char * lower, size_t len) {
  const AdjPair * p = pair_find(k_svg_attrs, sizeof(k_svg_attrs) / sizeof(k_svg_attrs[0]), lower, len);
  return p ? p->to : NULL;
}

bool ht_adjust_foreign_attribute(const char * name, size_t len, unsigned * which) {
  size_t lo = 0;
  size_t hi = sizeof(k_foreign_attrs) / sizeof(k_foreign_attrs[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_name(name, len, k_foreign_attrs[mid].name);
    if (c == 0) {
      *which = k_foreign_attrs[mid].which;
      return true;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return false;
}

/* ---- tests ---------------------------------------------------------------- */

size_t gtext_html_tree_test_table_size(int which) {
  switch (which) {
    case 0:
      return sizeof(k_svg_tags) / sizeof(k_svg_tags[0]);
    case 1:
      return sizeof(k_svg_attrs) / sizeof(k_svg_attrs[0]);
    case 2:
      return sizeof(k_foreign_attrs) / sizeof(k_foreign_attrs[0]);
    default:
      return 0;
  }
}

bool gtext_html_tree_test_table_at(int which, size_t i, const char ** from, const char ** to) {
  if (i >= gtext_html_tree_test_table_size(which)) {
    return false;
  }
  if (which == 0) {
    *from = k_svg_tags[i].from;
    *to = k_svg_tags[i].to;
  }
  else if (which == 1) {
    *from = k_svg_attrs[i].from;
    *to = k_svg_attrs[i].to;
  }
  else {
    *from = k_foreign_attrs[i].name;
    *to = which == 2 && k_foreign_attrs[i].which == 1 ? "xlink"
        : k_foreign_attrs[i].which == 2 ? "xml" : "xmlns";
  }
  return true;
}

/* ---- the kind of an element ------------------------------------------------ */

uint32_t ht_foreign_flags(unsigned ns, const char * lower, size_t len) {
  if (ns == NS_MATH) {
    static const char * const text_ip[] = {"mi", "mn", "mo", "ms", "mtext"};
    for (size_t i = 0; i < sizeof(text_ip) / sizeof(text_ip[0]); i++) {
      if (cmp_name(lower, len, text_ip[i]) == 0) {
        return F_SPECIAL | F_SC_GEN | F_MTIP;
      }
    }
    if (cmp_name(lower, len, "annotation-xml") == 0) {
      return F_SPECIAL | F_SC_GEN | F_ANNOT;
    }
  }
  else if (ns == NS_SVG) {
    if (cmp_name(lower, len, "foreignobject") == 0 || cmp_name(lower, len, "desc") == 0
        || cmp_name(lower, len, "title") == 0) {
      return F_SPECIAL | F_SC_GEN | F_HIP;
    }
  }
  return 0;
}

/* ---- the adjusted current node and the dispatcher --------------------------- */

/* "The adjusted current node is the parser's fragment context element if that element is non-null and
   the stack of open elements has only one element in it (fragment case); otherwise, the adjusted current
   node is the current node." Its namespace and flags; false when there is none (the stack is empty). */
static bool adjusted_node(const GTEXT_HTML_Tree * tb, unsigned * ns, unsigned * flags) {
  if (tb->frag && tb->n == 1) {
    *ns = tb->ctx_ns;
    *flags = tb->ctx_flags;
    return true;
  }
  if (tb->n == 0) {
    return false;
  }
  *ns = HT_STACK_TOP(tb).ns;
  *flags = HT_STACK_TOP(tb).flags;
  return true;
}

unsigned ht_adjusted_ns(const GTEXT_HTML_Tree * tb) {
  unsigned ns = NS_HTML;
  unsigned flags = 0;
  return adjusted_node(tb, &ns, &flags) ? ns : NS_HTML;
}

bool ht_allow_cdata(void * user) {
  const GTEXT_HTML_Tree * tb = (const GTEXT_HTML_Tree *)user;
  /* "If there is an adjusted current node and it is not an element in the HTML namespace". */
  return tb && ht_adjusted_ns(tb) != NS_HTML;
}

/* The tree construction dispatcher (13.2.6): which rules a token is handled by. The list there, in
   order: the stack is empty; the adjusted current node is in the HTML namespace; a MathML text
   integration point and a start tag that is not mglyph or malignmark; a MathML text integration point
   and a character; an annotation-xml and a start tag svg; an HTML integration point and a start tag;
   an HTML integration point and a character; an end of file. All of those: the rules of the insertion
   mode. Otherwise the foreign content rules. */
bool ht_use_foreign_rules(const GTEXT_HTML_Tree * tb, const Tk * tk, bool character) {
  unsigned ns = NS_HTML;
  unsigned flags = 0;
  if (!adjusted_node(tb, &ns, &flags) || ns == NS_HTML) {
    return false;
  }
  const bool start = !character && tk->type == TK_START;
  if (!character && tk->type == TK_EOF) {
    return false;
  }
  if (flags & F_MTIP) {
    if (character) {
      return false;
    }
    if (start && !(tk->tok->name_length == 6 && memcmp(tk->tok->name, "mglyph", 6) == 0)
        && !(tk->tok->name_length == 10 && memcmp(tk->tok->name, "malignmark", 10) == 0)) {
      return false;
    }
  }
  if ((flags & F_ANNOT) && start && tk->id == T_SVG) {
    return false;
  }
  if ((flags & F_HIP) && (start || character)) {
    return false;
  }
  return true;
}

/* ---- the rules for parsing tokens in foreign content -------------------------- */

static bool is_ws_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* A character token. NUL is a parse error and a U+FFFD; white space is inserted; any other character is
   inserted and the frameset-ok flag becomes "not ok". */
bool ht_foreign_chars(GTEXT_HTML_Tree * tb, const char * p, size_t n) {
  const char * const base = tb->cur->data;
  while (n > 0) {
    const char * nul = memchr(p, '\0', n);
    const size_t len = nul ? (size_t)(nul - p) : n;
    if (len > 0) {
      if (!ht_insert_text(tb, p, len)) {
        return false;
      }
      for (size_t i = 0; i < len; i++) {
        if (!is_ws_char(p[i])) {
          tb->frameset_ok = false;
          break;
        }
      }
    }
    if (!nul) {
      break;
    }
    ht_seg_cursor_advance(&tb->tcur, (size_t)(nul - base));
    ht_tree_error_at(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_NULL_CHARACTER, tb->tcur.line,
        tb->tcur.column, tb->tcur.offset);
    if (!ht_insert_text(tb, "\xEF\xBF\xBD", 3)) {
      return false;
    }
    p += len + 1;
    n -= len + 1;
  }
  return tb->status == GTEXT_HTML_TREE_OK;
}

/* The start tags that end foreign content (the list of 13.2.6.5; `font` only with a color, face or size). */
static bool is_breakout_start(const Tk * tk) {
  switch (tk->id) {
    case T_B:
    case T_BIG:
    case T_BLOCKQUOTE:
    case T_BODY:
    case T_BR:
    case T_CENTER:
    case T_CODE:
    case T_DD:
    case T_DIV:
    case T_DL:
    case T_DT:
    case T_EM:
    case T_EMBED:
    case T_H1:
    case T_H2:
    case T_H3:
    case T_H4:
    case T_H5:
    case T_H6:
    case T_HEAD:
    case T_HR:
    case T_I:
    case T_IMG:
    case T_LI:
    case T_LISTING:
    case T_MENU:
    case T_META:
    case T_NOBR:
    case T_OL:
    case T_P:
    case T_PRE:
    case T_RUBY:
    case T_S:
    case T_SMALL:
    case T_SPAN:
    case T_STRONG:
    case T_STRIKE:
    case T_SUB:
    case T_SUP:
    case T_TABLE:
    case T_TT:
    case T_U:
    case T_UL:
    case T_VAR:
      return true;
    case T_FONT:
      for (size_t i = 0; i < tk->nattrs; i++) {
        const GTEXT_HTML_Attribute * a = &tk->attrs[i];
        if ((a->name_length == 5 && memcmp(a->name, "color", 5) == 0)
            || (a->name_length == 4 && memcmp(a->name, "face", 4) == 0)
            || (a->name_length == 4 && memcmp(a->name, "size", 4) == 0)) {
          return true;
        }
      }
      return false;
    default:
      return false;
  }
}

/* "Parse error. While the current node is not a MathML text integration point, an HTML integration point,
   or an element in the HTML namespace, pop elements from the stack of open elements. Reprocess the token
   according to the rules given in the section corresponding to the current insertion mode in HTML
   content." Each pop undoes a push, so the cost is amortised over the tokens that opened the elements. */
static int breakout(GTEXT_HTML_Tree * tb, const Tk * tk) {
  ht_tree_error(tb, GTEXT_HTML_TREE_ERR_HTML_IN_FOREIGN_CONTENT);
  while (tb->n > 0 && HT_STACK_TOP(tb).ns != NS_HTML
      && !(HT_STACK_TOP(tb).flags & (F_MTIP | F_HIP))) {
    ht_stack_pop(tb);
  }
  if (tb->status != GTEXT_HTML_TREE_OK) {
    return R_FAIL;
  }
  return ht_html_mode_token(tb, tk);
}

/* The end tag that is "any other end tag" of foreign content. */
static int foreign_end_tag(GTEXT_HTML_Tree * tb, const Tk * tk) {
  const char * name = tk->tok->name;
  const size_t len = tk->tok->name_length;
  /* 1. node is the current node. 2. If its tag name, converted to ASCII lowercase, is not the token's,
     a parse error. */
  const name_slot * cur = &tb->names[HT_STACK_TOP(tb).id];
  if (cur->len != len || memcmp(cur->text, name, len) != 0) {
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  }
  /* 3. If node is the topmost element in the stack of open elements, return (fragment case). */
  if (tb->n == 1) {
    return tb->status == GTEXT_HTML_TREE_OK ? R_DONE : R_FAIL;
  }
  /* 4 to 6. Walk down for a node of that name, stopping at the first element in the HTML namespace: the
     topmost open element of the name in SVG or in MathML, if it is above that one. */
  uint32_t j = NONE_IDX;
  for (unsigned ns = NS_SVG; ns <= NS_MATH; ns++) {
    uint32_t id = NONE_IDX;
    if (!ht_names_lookup_ns(tb, ns, name, len, false, &id)) {
      return R_FAIL;
    }
    if (id != NONE_IDX) {
      const uint32_t at = tb->names[id].topidx;
      if (at != NONE_IDX && (j == NONE_IDX || at > j)) {
        j = at;
      }
    }
  }
  if (j != NONE_IDX && j > HT_STACK_TOP(tb).fl_html) {
    while (tb->n > j) {
      ht_stack_pop(tb);
    }
    return tb->status == GTEXT_HTML_TREE_OK ? R_DONE : R_FAIL;
  }
  /* 7. Otherwise, process the token according to the rules of the current insertion mode. */
  return ht_html_mode_token(tb, tk);
}

/* A token other than a character, the end of the file or a DOCTYPE... handled by the foreign content rules. */
int ht_foreign_token(GTEXT_HTML_Tree * tb, const Tk * tk) {
  switch (tk->type) {
    case TK_COMMENT:
      HT_CK(ht_insert_comment(tb, tk->tok));
      return R_DONE;
    case TK_PI:
      HT_CK(ht_insert_pi(tb, tk->tok));
      return R_DONE;
    case TK_DOCTYPE:
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_DOCTYPE);
      return tb->status == GTEXT_HTML_TREE_OK ? R_DONE : R_FAIL;
    case TK_START: {
      if (is_breakout_start(tk)) {
        return breakout(tb, tk);
      }
      /* Any other start tag: an element in the namespace of the adjusted current node; its name's case is
         restored (SVG), its attributes adjusted (the element's namespace decides which table). */
      const unsigned ns = ht_adjusted_ns(tb);
      uint32_t id = 0;
      HT_CK(ht_names_lookup_ns(tb, ns, tk->tok->name, tk->tok->name_length, true, &id));
      HT_CK(ht_insert_foreign(tb, tk, id, NULL));
      if (tk->self_closing) {
        /* "If the token's tag name is "script", and the new current node is in the SVG namespace: Acknowledge
           the token's self-closing flag, and then act as described in the steps for a "script" end tag below.
           Otherwise: Pop the current node off the stack of open elements and acknowledge the token's
           self-closing flag." The steps for the end tag begin by popping the current node and go on to run the
           script, which this parser never does (it executes nothing), so for a script too the whole of the
           rule is: pop the current node, acknowledge the flag. */
        HT_CK(ht_pop_current_acknowledge(tb, tk));
      }
      return R_DONE;
    }
    case TK_END: {
      const GTEXT_HTML_Token * t = tk->tok;
      if (t->name_length == 6 && memcmp(t->name, "script", 6) == 0 && tb->n > 0
          && HT_STACK_TOP(tb).ns == NS_SVG) {
        const name_slot * cur = &tb->names[HT_STACK_TOP(tb).id];
        if (cur->len == 6 && memcmp(cur->text, "script", 6) == 0) {
          /* An end tag script, with an SVG script element current: pop it (the scripting steps are not
             taken: this parser runs nothing). */
          ht_stack_pop(tb);
          return R_DONE;
        }
      }
      if ((t->name_length == 2 && memcmp(t->name, "br", 2) == 0)
          || (t->name_length == 1 && t->name[0] == 'p')) {
        return breakout(tb, tk);
      }
      return foreign_end_tag(tb, tk);
    }
    default:
      return R_DONE;
  }
}

/* The `math` and `svg` start tags of "in body": reconstruct the active formatting elements, insert a foreign
   element in that namespace with the adjusted attributes, and if the token is self-closing pop it and
   acknowledge the flag. */
int ht_body_start_foreign(GTEXT_HTML_Tree * tb, const Tk * tk, unsigned ns) {
  HT_CK(ht_fmt_reconstruct(tb));
  uint32_t id = 0;
  HT_CK(ht_names_lookup_ns(tb, ns, tk->tok->name, tk->tok->name_length, true, &id));
  HT_CK(ht_insert_foreign(tb, tk, id, NULL));
  if (tk->self_closing) {
    HT_CK(ht_pop_current_acknowledge(tb, tk));
  }
  return R_DONE;
}
