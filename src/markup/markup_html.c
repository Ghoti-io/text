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
 * Both tables are sorted by plain byte comparison and searched with a binary
 * search. Nothing is allocated. A table that is out of order would make a
 * lookup miss without any error, so tests/test-markup-html.cpp looks up every
 * entry and checks the order.
 */

#include <ghoti.io/text/markup/markup_html.h>

#include <stddef.h>
#include <string.h>

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
