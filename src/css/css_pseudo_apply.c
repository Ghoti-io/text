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
 * Which properties apply to ::first-line, ::first-letter and ::marker. A declaration of a property that does not apply to the pseudo-element a rule styles takes no part in the
 * cascade of the pseudo-element, so its computed value is the inherited or the initial one, as if nothing had been declared. The rule is the specifications' and is the same for
 * every consumer of the rule index (the computed style and the cascaded view alike).
 *
 * ::first-line (CSS Pseudo-Elements 4, "Styling the ::first-line Pseudo-element"): "The following CSS properties apply to a ::first-line pseudo-element": all font properties,
 * color and opacity, all background properties, the typesetting properties that apply to inline elements (every property of CSS Text), all text decoration properties, ruby-position
 * and the inline layout properties that apply to inline elements; "User agents may apply other properties as well except for the following excluded properties: writing-mode,
 * direction, text-orientation". The text lets a user agent apply others, so a property it does not list is denied only where Chromium and Firefox both leave it unapplied
 * (tools/conformance/css_pseudo_answers.jsonl, recorded by tools/oracle/css_pseudo_record.py): display, the positioning and box properties, margin, padding and border, transforms,
 * transitions and animations, the layout of the flex, grid, multi-column and table models, lists, and the rest of the list below. The typesetting properties stay applied where the
 * browsers leave them (white-space, word-break, hyphens, tab-size, line-break, overflow-wrap, text-emphasis): the served text lists them.
 *
 * ::first-letter ("Styling the First-Letter Pseudo-elements"): the same families, "margin and padding properties" and "border properties and box-shadow"; float is applied by both
 * browsers. The positioning, size, transform, transition, animation and layout-model properties are denied.
 *
 * ::marker (CSS Lists 3, "Properties Applying to ::marker"): "only the following CSS properties actually apply to a marker box: the text-combine-upright, unicode-bidi, and
 * direction properties, the content property, all animation and transition properties", and "Inheritable properties that apply to text can be set on the ::marker pseudo-element".
 * "UAs may either treat such properties as not applying, or enforce their value or inheritance from the originating element", so a property that is not inherited and is not in the
 * list is denied, and so are the inherited properties that do not apply to text (the list, table and block-level text properties) that the browsers leave unapplied.
 *
 * tools/css/check-pseudo-apply.py holds this table to the record of the two browsers and to the sentences above.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "css_registry_internal.h"
#include "css_selector_internal.h"

#include "css_pseudo_apply.h"

enum {
  FL = 1u,  /* ::first-line */
  LT = 2u,  /* ::first-letter */
  MK = 4u   /* ::marker */
};

typedef struct {
  const char * name;
  bool prefix;       /* the name is the start of the property's */
  unsigned kinds;    /* the pseudo-elements it is denied for */
} deny_row;

/* The rows are the properties both browsers leave unapplied (css_pseudo_answers.jsonl) and that the served text does not list, grouped by what they are. */
static const deny_row DENY[] = {
  /* the box: display, positioning, size, margin, padding, border, outline, overflow */
  {"display", false, FL | LT | MK},
  {"position", false, FL | LT | MK},
  {"top", false, FL | LT | MK},
  {"right", false, FL | LT | MK},
  {"bottom", false, FL | LT | MK},
  {"left", false, FL | LT | MK},
  {"inset", true, FL | LT | MK},
  {"clear", false, FL | LT | MK},
  {"z-index", false, FL | LT | MK},
  {"width", false, FL | LT | MK},
  {"height", false, FL | LT | MK},
  {"min-", true, FL | LT | MK},
  {"max-", true, FL | LT | MK},
  {"box-sizing", false, FL | LT | MK},
  {"aspect-ratio", false, FL | LT | MK},
  {"float", false, FL | MK},
  {"margin", true, FL | MK},
  {"padding", true, FL | MK},
  {"border", true, FL | MK},
  {"border-collapse", false, LT},
  {"outline", true, FL | LT | MK},
  {"overflow", true, FL | LT | MK},
  {"text-overflow", false, FL | LT | MK},
  {"box-shadow", false, MK},
  /* transforms, transitions, animations (a marker takes the last two) */
  {"transform", true, FL | LT | MK},
  {"translate", false, FL | LT | MK},
  {"rotate", false, FL | LT | MK},
  {"scale", false, FL | LT | MK},
  {"perspective", true, FL | LT | MK},
  {"transition", true, FL | LT},
  {"animation", true, FL | LT},
  /* the layout models: flex, grid, alignment, multi-column, table, list */
  {"flex", true, FL | LT | MK},
  {"order", false, FL | LT | MK},
  {"align-", true, FL | LT | MK},
  {"justify-", true, FL | LT | MK},
  {"place-", true, FL | LT | MK},
  {"gap", false, FL | LT | MK},
  {"row-gap", false, FL | LT | MK},
  {"column", true, FL | LT | MK},
  {"grid", true, FL | LT | MK},
  {"table-layout", false, FL | LT | MK},
  {"caption-side", false, FL | LT | MK},
  {"empty-cells", false, FL | LT | MK},
  {"border-spacing", false, FL | MK},
  {"list-style", true, FL | LT | MK},
  {"counter-", true, FL | LT | MK},
  /* what a pseudo-element's text does not take */
  {"text-align", true, FL | LT | MK},
  {"text-indent", false, FL | LT | MK},
  {"cursor", false, FL | LT},
  {"quotes", false, FL | LT},
  {"content", false, FL | LT},
  {"pointer-events", false, FL | LT | MK},
  {"user-select", false, FL | LT | MK},
  {"filter", false, FL | LT | MK},
  {"clip-path", false, FL | LT | MK},
  {"mask", true, FL | LT | MK},
  {"mix-blend-mode", false, FL | LT | MK},
  {"isolation", false, FL | LT | MK},
  {"will-change", false, FL | LT | MK},
  {"contain", true, FL | LT | MK},
  {"resize", false, FL | LT | MK},
  {"object-fit", false, FL | LT | MK},
  {"scroll", true, FL | LT | MK},
  {"appearance", false, FL | LT | MK},
  {"accent-color", false, FL | LT | MK},
  /* the writing modes: "the following excluded properties: writing-mode, direction, text-orientation", and the two the browsers leave out */
  {"writing-mode", false, FL | LT | MK},
  {"direction", false, FL | LT},
  {"text-orientation", false, FL | LT},
  {"unicode-bidi", false, FL | LT},
  {"text-combine-upright", false, FL | LT},
  {"ruby-position", false, LT | MK},
  /* inherited, and not text: a marker leaves these to the list item */
  {"text-underline-position", false, MK},
  {"text-justify", false, MK},
  {"visibility", false, MK},
  {"opacity", false, MK},
  {"vertical-align", false, MK},
  {"text-decoration", true, MK},
  {"background", true, MK},
};

static bool starts(const char * name, size_t n, const char * p) {
  const size_t pl = strlen(p);
  return n >= pl && memcmp(name, p, pl) == 0;
}

/* a name a prefix row reaches that the served text puts among the applied properties */
static bool exempt(const char * name, size_t n) {
  return n == 13 && memcmp(name, "overflow-wrap", 13) == 0;   /* a typesetting property of CSS Text, which the "overflow" row reaches by its prefix */
}

/* what a marker box applies whether or not it is inherited: the list of CSS Lists 3 */
static bool marker_listed(const char * name, size_t n) {
  return starts(name, n, "animation") || starts(name, n, "transition") || (n == 7 && memcmp(name, "content", 7) == 0) || (n == 12 && memcmp(name, "unicode-bidi", 12) == 0) ||
         (n == 20 && memcmp(name, "text-combine-upright", 20) == 0) || (n == 9 && memcmp(name, "direction", 9) == 0);
}

static unsigned kind_of(unsigned pseudo) {
  switch (pseudo) {
  case GTEXT_CSS_SEL_PE_FIRST_LINE:
  case GTEXT_CSS_SEL_PE_FIRST_LINE_LEGACY:
    return FL;
  case GTEXT_CSS_SEL_PE_FIRST_LETTER:
  case GTEXT_CSS_SEL_PE_FIRST_LETTER_LEGACY:
    return LT;
  case GTEXT_CSS_SEL_PE_MARKER:
    return MK;
  default:
    return 0;
  }
}

bool css_pseudo_applies(unsigned pseudo, const GTEXT_CSS_Property_Info * info) {
  const unsigned kind = kind_of(pseudo);
  if (!kind || !info || !info->name || (info->flags & GTEXT_CSS_PROP_CUSTOM)) {
    return true;
  }
  const char * name = info->name;
  const size_t n = info->name_length;
  if (kind == MK) {
    if (marker_listed(name, n)) {
      return true;
    }
    /* what is not listed and not inherited does not apply to a marker box ("Inheritable properties that apply to text" are the others) */
    if (info->inherited == GTEXT_CSS_INHERIT_NO) {
      return false;
    }
  }
  for (size_t i = 0; i < sizeof DENY / sizeof DENY[0]; i++) {
    const deny_row * r = &DENY[i];
    if (!(r->kinds & kind)) {
      continue;
    }
    const size_t rl = strlen(r->name);
    if (r->prefix ? starts(name, n, r->name) : (n == rl && memcmp(name, r->name, rl) == 0)) {
      if (exempt(name, n)) {
        continue;
      }
      return false;
    }
  }
  return true;
}
