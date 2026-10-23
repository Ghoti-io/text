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
 * Substitution and generated-content functions: var(), env(), attr(), counters, calc-size() and the Values 5 functions.
 *
 * Reference: CSS Variables 1, Environment 1, Values 5 (Editor's Draft, served 2026-10-22), Lists 3 and Counter Styles 3.
 *
 * The grammars are the drafts' own (Value Definition Syntax), retrieved 2026-10-22; where the
 * served draft differs from an older text the served one is followed and the difference is noted
 * beside the entry.
 * **Implicitly omitted commas are written out.** Values 4, 2.2 omits a comma from the match when the terms around it are omitted
 * (`var(--a)` for `var( <custom-property-name> , <declaration-value>? )`). The engine keeps a literal comma that separates a possibly
 * empty term as a token, so the grammars here say it with a group (`var( <custom-property-name> [ , <declaration-value>? ]? )`), which
 * matches the same inputs.
 *
 * They are matched by the Value Definition Syntax engine; nothing here is code.
 */

#include "css_function_internal.h"

static const css_fn_type TYPES[] = {
  /* Variables 1, 3 */
  {"var()", GTEXT_CSS_REF_TYPE,
      "var( <custom-property-name> [ , <declaration-value>? ]? )"},
  {"custom-property-name", GTEXT_CSS_REF_TYPE,
      "<dashed-ident>"},
  /* Environment 1 */
  {"env()", GTEXT_CSS_REF_TYPE,
      "env( <custom-ident> <integer [0,∞]>* [ , <declaration-value>? ]? )"},
  /* Values 5, 11 */
  {"attr()", GTEXT_CSS_REF_TYPE,
      "attr( <attr-name> <attr-type>? [ , <declaration-value>? ]? )"},
  {"attr-name", GTEXT_CSS_REF_TYPE,
      "[ <ident-token>? '|' ]? <ident-token>"},
  {"attr-type", GTEXT_CSS_REF_TYPE,
      "type( <syntax> ) | raw-string | number | <attr-unit>"},
  {"attr-unit", GTEXT_CSS_REF_TYPE,
      "<custom-ident>"},
  {"syntax", GTEXT_CSS_REF_TYPE,
      "'*' | <syntax-component> [ <syntax-combinator> <syntax-component> ]* | <string>"},
  {"syntax-component", GTEXT_CSS_REF_TYPE,
      "<syntax-single-component> <syntax-multiplier>? | '<' transform-list '>'"},
  {"syntax-single-component", GTEXT_CSS_REF_TYPE,
      "'<' <syntax-type-name> '>' | <ident>"},
  {"syntax-type-name", GTEXT_CSS_REF_TYPE,
      "angle | color | custom-ident | image | integer | length | length-percentage | number | percentage | resolution | string | time | url | transform-function"},
  {"syntax-combinator", GTEXT_CSS_REF_TYPE,
      "'|'"},
  {"syntax-multiplier", GTEXT_CSS_REF_TYPE,
      "'#' | '+'"},
  /* Lists 3, counter(); Content 3 uses it */
  {"counter()", GTEXT_CSS_REF_TYPE,
      "counter( <counter-name> [ , <counter-style> ]? )"},
  {"counters()", GTEXT_CSS_REF_TYPE,
      "counters( <counter-name> , <string> [ , <counter-style> ]? )"},
  {"counter-name", GTEXT_CSS_REF_TYPE,
      "<custom-ident>"},
  /* Counter Styles 3 */
  {"counter-style", GTEXT_CSS_REF_TYPE,
      "<counter-style-name> | <symbols()>"},
  {"counter-style-name", GTEXT_CSS_REF_TYPE,
      "<custom-ident>"},
  {"symbols()", GTEXT_CSS_REF_TYPE,
      "symbols( <symbols-type>? [ <string> | <image> ]+ )"},
  {"symbols-type", GTEXT_CSS_REF_TYPE,
      "cyclic | numeric | alphabetic | symbolic | fixed"},
  /* Values 5, 10.1 */
  {"calc-size()", GTEXT_CSS_REF_TYPE,
      "calc-size( <calc-size-basis> , <calc-sum> )"},
  {"calc-size-basis", GTEXT_CSS_REF_TYPE,
      "<size-keyword> | <calc-sum> | <calc-size()> | any"},
  {"size-keyword", GTEXT_CSS_REF_TYPE,
      "auto | min-content | max-content | fit-content | stretch | contain"},
  /* a calculation inside calc-size() is read by the math layer once it is used; as a type it is the text */
  {"calc-sum", GTEXT_CSS_REF_TYPE,
      "<declaration-value>"},
  /* Values 5, 8.1: an arbitrary substitution function, so a match of any grammar takes it as pending */
  {"if()", GTEXT_CSS_REF_TYPE,
      "if( [ <if-branch> ; ]* <if-branch> ;? )"},
  {"if-branch", GTEXT_CSS_REF_TYPE,
      "<if-condition> : <declaration-value>?"},
  {"if-condition", GTEXT_CSS_REF_TYPE,
      "<declaration-value> | else"},
  /* Values 5, 8.2 */
  {"first-valid()", GTEXT_CSS_REF_TYPE,
      "first-valid( <declaration-value># )"},
  /* Values 5, 10.3 */
  {"random()", GTEXT_CSS_REF_TYPE,
      "random( [ <random-key> , ]? <calc-sum> , <calc-sum> [ , <calc-sum> ]? )"},
  {"random-item()", GTEXT_CSS_REF_TYPE,
      "random-item( <random-key> , [ <declaration-value>? ]# )"},
  {"random-key", GTEXT_CSS_REF_TYPE,
      "auto | <random-cache-key> | fixed <number [0,1]>"},
  {"random-cache-key", GTEXT_CSS_REF_TYPE,
      "<dashed-ident> || element-scoped || [ property-scoped | property-index-scoped | <custom-ident> ]"},
  /* Values 5, 6.1 (tree counting functions) */
  {"sibling-index()", GTEXT_CSS_REF_TYPE,
      "sibling-index( )"},
  {"sibling-count()", GTEXT_CSS_REF_TYPE,
      "sibling-count( )"},
  /* Values 5, 10.2: a calc-sum is read by the math layer once it is used */
  {"progress()", GTEXT_CSS_REF_TYPE,
      "progress( no-clamp? <calc-sum> , <calc-sum> , <calc-sum> )"},
  /* Values 5, 8.5 */
  {"inherit()", GTEXT_CSS_REF_TYPE,
      "inherit( <custom-property-name> [ , <declaration-value>? ]? )"},
  /* Values 5, 8.6 */
  {"ident()", GTEXT_CSS_REF_TYPE,
      "ident( [ <string> | <integer> | <ident> ]+ )"},
  /* GCPM 3 (no served Values draft defines string()); named by the plan with the Values 5 functions */
  {"string()", GTEXT_CSS_REF_TYPE,
      "string( <custom-ident> [ , [ first | start | last | first-except ] ]? )"},
};

const css_fn_type * css_substitution_types(size_t * count) {
  *count = sizeof TYPES / sizeof TYPES[0];
  return TYPES;
}

/* ---- what each needs ------------------------------------------------------------------------------- */

#include <string.h>

GTEXT_CSS_Substitution_Kind gtext_css_substitution_kind(const char * name, size_t length) {
  static const struct { const char * n; GTEXT_CSS_Substitution_Kind k; } K[] = {
    {"var", GTEXT_CSS_SUBST_VAR}, {"env", GTEXT_CSS_SUBST_ENV}, {"attr", GTEXT_CSS_SUBST_ATTR}, {"if", GTEXT_CSS_SUBST_IF},
    {"first-valid", GTEXT_CSS_SUBST_FIRST_VALID}, {"random", GTEXT_CSS_SUBST_RANDOM}, {"random-item", GTEXT_CSS_SUBST_RANDOM_ITEM},
    {"sibling-index", GTEXT_CSS_SUBST_SIBLING_INDEX}, {"sibling-count", GTEXT_CSS_SUBST_SIBLING_COUNT},
    {"progress", GTEXT_CSS_SUBST_PROGRESS}, {"inherit", GTEXT_CSS_SUBST_INHERIT}, {"ident", GTEXT_CSS_SUBST_IDENT},
    {"string", GTEXT_CSS_SUBST_STRING}, {"counter", GTEXT_CSS_SUBST_COUNTER}, {"counters", GTEXT_CSS_SUBST_COUNTERS},
    {"calc-size", GTEXT_CSS_SUBST_CALC_SIZE},
  };
  if (!name) {
    return GTEXT_CSS_SUBST_NONE;
  }
  const GTEXT_CSS_String s = {name, length};
  for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
    if (css_fn_name_is(&s, K[i].n)) {
      return K[i].k;
    }
  }
  return GTEXT_CSS_SUBST_NONE;
}

GTEXT_CSS_Needs gtext_css_substitution_needs(GTEXT_CSS_Substitution_Kind kind) {
  switch (kind) {
  case GTEXT_CSS_SUBST_VAR: return GTEXT_CSS_NEEDS_VARIABLES;
  case GTEXT_CSS_SUBST_ENV: return GTEXT_CSS_NEEDS_ENVIRONMENT;
  case GTEXT_CSS_SUBST_ATTR: case GTEXT_CSS_SUBST_IF: case GTEXT_CSS_SUBST_RANDOM: case GTEXT_CSS_SUBST_RANDOM_ITEM:
  case GTEXT_CSS_SUBST_SIBLING_INDEX: case GTEXT_CSS_SUBST_SIBLING_COUNT: case GTEXT_CSS_SUBST_INHERIT:
  case GTEXT_CSS_SUBST_COUNTER: case GTEXT_CSS_SUBST_COUNTERS: case GTEXT_CSS_SUBST_STRING:
    return GTEXT_CSS_NEEDS_ELEMENT;
  case GTEXT_CSS_SUBST_CALC_SIZE: return GTEXT_CSS_NEEDS_LAYOUT;
  /* first-valid() picks the first argument that is a valid value of the property it is in (Values 5, 8.2); it needs that property's grammar */
  case GTEXT_CSS_SUBST_FIRST_VALID: return GTEXT_CSS_NEEDS_PROPERTY;
  /* progress() is a math function: with literal arguments it is a number, and ident() builds an identifier from its arguments, which are
     scanned for the functions that do need something (Values 5, 10.2 and 8.6) */
  case GTEXT_CSS_SUBST_PROGRESS: case GTEXT_CSS_SUBST_IDENT: return GTEXT_CSS_NEEDS_NOTHING;
  default: return GTEXT_CSS_NEEDS_NOTHING;
  }
}
