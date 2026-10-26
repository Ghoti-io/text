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
 * @file css_property.h
 * @brief Property-level parsing: a declaration's name and value to a typed value, by the registry.
 *
 * gtext_css_parse_property() is the whole-property rules in one place:
 *
 * - the name is looked up in the registry: ASCII case-insensitively, except that a custom property (`--*`) keeps its
 *   case; a legacy alias is parsed as the property it names; an unlisted name (any vendor prefix the pin does not list)
 *   is an **unknown** property, which the cascade drops;
 * - the CSS-wide keywords `initial`, `inherit`, `unset`, `revert`, `revert-layer` and `revert-rule` (the six the pin
 *   defines, in the `all` entry) are valid **alone** for every property, longhand, shorthand and custom property alike
 *   (and are not valid for a descriptor), and `all` accepts nothing else;
 * - a value that holds `var()`, `env()`, `attr()` (or another arbitrary substitution function, css_value.h) anywhere is
 *   **pending**: valid now, meaningful only after substitution, whatever the property's grammar (CSS Variables 1: "a
 *   declaration containing var() is assumed valid at parse time"). The one exception is `all`, which takes the CSS-wide
 *   keywords only;
 * - otherwise the value must match the property's grammar, and the result is the typed value of css_value.h in the
 *   grammar's slots. A shorthand's typed value follows the shorthand's grammar; expanding it into longhands is the
 *   cascade layer's, through the hook the shorthand carries (css_registry.h);
 * - in quirks mode (the option) a unitless number is a length in the properties of Values 4, Appendix C
 *   (GTEXT_CSS_PROP_QUIRK_UNITLESS), and nowhere else;
 * - in the context of an at-rule (the option) the name is a **descriptor** of it.
 *
 * ## What the status says
 *
 * A value that is invalid is not an error: the call returns GTEXT_CSS_OK and the outcome is
 * GTEXT_CSS_PROP_RESULT_INVALID. A property whose grammar the pin does not give, or names a type nothing resolves, cannot
 * be decided either way and returns GTEXT_CSS_E_UNSUPPORTED with the property in the result (never a guess); the depth
 * ledger (`tools/css/depth.tsv`) is the list of them, and the registry reports the cause
 * (gtext_css_registry_property_state()). A limit of the match is its own status: the answer is not known.
 *
 * ## Validity callbacks
 *
 * gtext_css_property_validity() fills the pair of callbacks of css_core.h for declarations: a declaration is valid
 * where the property (or the descriptor, inside an at-rule that has them) accepts its value, so the parser drops the
 * declarations CSS says it must, and reads them again as a rule where CSS Nesting says to.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_PROPERTY_H
#define GHOTI_IO_GTEXT_CSS_CSS_PROPERTY_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_grammar.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What parsing a declaration's value came to. */
typedef enum {
  GTEXT_CSS_PROP_RESULT_UNKNOWN = 0, ///< No such property (or, in an at-rule context, descriptor). Also what a zeroed or freed result is, and what every failed call leaves.
  GTEXT_CSS_PROP_RESULT_VALID,     ///< A typed value (`set`).
  GTEXT_CSS_PROP_RESULT_WIDE,      ///< A CSS-wide keyword alone (`wide`).
  GTEXT_CSS_PROP_RESULT_PENDING,   ///< Valid, with an arbitrary substitution in it (`set` holds the pending value).
  GTEXT_CSS_PROP_RESULT_INVALID    ///< The property is known and the value is not one of it.
} GTEXT_CSS_Property_Outcome;

/** @brief The CSS-wide keywords. */
typedef enum {
  GTEXT_CSS_WIDE_NONE = 0,
  GTEXT_CSS_WIDE_INITIAL,
  GTEXT_CSS_WIDE_INHERIT,
  GTEXT_CSS_WIDE_UNSET,
  GTEXT_CSS_WIDE_REVERT,
  GTEXT_CSS_WIDE_REVERT_LAYER,
  GTEXT_CSS_WIDE_REVERT_RULE
} GTEXT_CSS_Wide_Keyword;

/**
 * @brief The CSS-wide keyword a name spells.
 *
 * @param name The text; need not be terminated. ASCII case-insensitive.
 * @param length Its length.
 * @return The keyword, or GTEXT_CSS_WIDE_NONE.
 */
GTEXT_API GTEXT_CSS_Wide_Keyword gtext_css_wide_keyword_lookup(const char * name, size_t length);

/** @brief The spelling of a CSS-wide keyword (`revert-layer`); NULL for NONE or anything else. Static. */
GTEXT_API const char * gtext_css_wide_keyword_name(GTEXT_CSS_Wide_Keyword keyword);

/**
 * @struct GTEXT_CSS_Property_Options
 * @brief What a property parse may do. A zeroed struct is the default.
 */
typedef struct {
  /** The allocator of the match and of the value set, or NULL for the registry's. */
  const GTEXT_Allocator * allocator;
  /** The limits of the match (css_grammar.h), or NULL for the registry's. Its allocator and `unitless_length` are replaced. */
  const GTEXT_CSS_Match_Options * match;
  /** NULL: the name is a property. Otherwise the name of an at-rule (`@font-face`, ASCII case-insensitive) and the name
   *  is a descriptor of it. */
  const char * at_rule;
  /** Quirks mode: a unitless number is a length in the properties that allow it (GTEXT_CSS_PROP_QUIRK_UNITLESS). */
  bool quirks;
} GTEXT_CSS_Property_Options;

/**
 * @struct GTEXT_CSS_Property_Value
 * @brief The result of parsing a declaration's value. Free it with gtext_css_property_value_free().
 */
typedef struct {
  GTEXT_CSS_Property_Outcome outcome;  ///< What it came to.
  GTEXT_CSS_Wide_Keyword wide;         ///< WIDE: which keyword.
  GTEXT_CSS_Property_Id named;         ///< The property the name found (an alias itself), or 0.
  GTEXT_CSS_Property_Id property;      ///< The property the value was parsed for (the alias resolved), or 0.
  GTEXT_CSS_Descriptor_Id descriptor;  ///< In an at-rule context: the descriptor, or 0.
  GTEXT_CSS_Value_Set * set;           ///< VALID and PENDING: the typed value (css_value.h); else NULL. Owned.
  GTEXT_CSS_Match_Result match;        ///< What the match measured (zeroed when there was none).
} GTEXT_CSS_Property_Value;

/** @brief Free what a result holds and zero it; a zeroed or NULL result is ignored. */
GTEXT_API void gtext_css_property_value_free(GTEXT_CSS_Property_Value * value);

/**
 * @brief Parse the value of a declaration for the property (or descriptor) of this name.
 *
 * @param registry The registry. Not NULL. It may be shared with other threads.
 * @param name The property's name as written; need not be terminated.
 * @param name_length Its length.
 * @param values The declaration's value (css_core.h), `!important` already removed; NULL only with a count of 0.
 * @param count Their number.
 * @param options NULL for the defaults.
 * @param out Receives the result. Not NULL. Zeroed first, whatever happens.
 * @return GTEXT_CSS_OK (valid, invalid, wide, pending or unknown: see `out->outcome`); GTEXT_CSS_E_INVALID (a NULL argument);
 *   GTEXT_CSS_E_OOM; GTEXT_CSS_E_UNSUPPORTED (the property has no usable grammar: `out->property` is set);
 *   GTEXT_CSS_E_LIMIT_WORK and the other limits of the match (the answer is not known).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_property(const GTEXT_CSS_Registry * registry, const char * name, size_t name_length,
    const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Property_Options * options,
    GTEXT_CSS_Property_Value * out);

/**
 * @brief Parse a value given as text: the text is read as a list of component values first.
 *
 * @return As gtext_css_parse_property(), and the limits of the tokenizer.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_property_text(const GTEXT_CSS_Registry * registry, const char * name, size_t name_length,
    const char * text, size_t text_length, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Value * out);

/** @brief Parse a declaration of a tree (css_core.h): its name and value. As gtext_css_parse_property(). */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_property_declaration(const GTEXT_CSS_Registry * registry,
    const GTEXT_CSS_Declaration * declaration, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Value * out);

/**
 * @brief Write a result as CSSOM writes a specified value.
 *
 * A typed or pending value as gtext_css_serialize_value(), a CSS-wide keyword as its spelling.
 *
 * @return As gtext_css_serialize_value(); GTEXT_CSS_E_INVALID for an invalid or unknown outcome.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_property_value_serialize(const GTEXT_CSS_Property_Value * value,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/** @brief As gtext_css_property_value_serialize(), to a new buffer (release it with gtext_css_serialize_buffer_free()). */
GTEXT_API GTEXT_CSS_Status gtext_css_property_value_serialize_buffer(const GTEXT_CSS_Property_Value * value,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length);

/* ---- validity callbacks -------------------------------------------------------------- */

/**
 * @struct GTEXT_CSS_Property_Validity
 * @brief The state behind the validity callbacks of a parse. Caller-owned; initialize it with
 *   gtext_css_property_validity_init() and keep it alive for the parse.
 */
typedef struct {
  const GTEXT_CSS_Registry * registry;  ///< Borrowed.
  GTEXT_CSS_Property_Options options;   ///< Copied by init (`at_rule` is ignored: the parent rule decides).
  GTEXT_CSS_Status status;              ///< OK, or the first status that kept a declaration from being judged
                                        ///< (a limit, memory): such a declaration was reported invalid.
  size_t checked;                       ///< Declarations judged.
  size_t rejected;                      ///< Declarations reported invalid.
  size_t undecided;                     ///< Declarations of a property the library cannot judge (GTEXT_CSS_E_UNSUPPORTED), accepted.
} GTEXT_CSS_Property_Validity;

/** @brief Prepare the state. @return OK or INVALID (a NULL argument). */
GTEXT_API GTEXT_CSS_Status gtext_css_property_validity_init(GTEXT_CSS_Property_Validity * state,
    const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Property_Options * options);

/**
 * @brief The callbacks for GTEXT_CSS_Parse_Options::validity.
 *
 * A declaration is valid when its property accepts the value (valid, wide or pending), or is one the library cannot judge.
 * Inside an at-rule that has declaration descriptors (`@font-face`) the name is a descriptor of it. The `rule` callback is
 * not set: rules are judged by the layers that know their grammars (selectors, at-rules).
 */
GTEXT_API GTEXT_CSS_Validity gtext_css_property_validity(GTEXT_CSS_Property_Validity * state);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_PROPERTY_H
