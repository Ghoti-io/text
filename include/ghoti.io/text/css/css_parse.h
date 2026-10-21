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
 * @file css_parse.h
 * @brief The parser entry points of CSS Syntax, its parse errors, the
 *   "parse according to a grammar" hook and the An+B microsyntax.
 *
 * Every entry point of CSS Syntax 5.4 is here, each reading text (UTF-8)
 * and returning a @ref GTEXT_CSS_Tree whose `kind` names the entry point:
 *
 * | Function | The standard's algorithm |
 * |---|---|
 * | gtext_css_parse_stylesheet() | parse a stylesheet (5.4.3) |
 * | gtext_css_parse_stylesheet_contents() | parse a stylesheet's contents (5.4.4) |
 * | gtext_css_parse_declaration_list() | parse a list of declarations (5.4.5), for a `style` attribute |
 * | gtext_css_parse_block_contents() | consume a block's contents (5.5.5) over the whole input |
 * | gtext_css_parse_rule() | parse a rule (5.4.6) |
 * | gtext_css_parse_declaration() | parse a declaration (5.4.7) |
 * | gtext_css_parse_component_value() | parse a component value (5.4.8) |
 * | gtext_css_parse_component_values() | parse a list of component values (5.4.9), for a presentation attribute |
 * | gtext_css_parse_comma_list() | parse a comma-separated list of component values (5.4.10) |
 * | gtext_css_parse_grammar() | parse something according to a CSS grammar (5.4.1) |
 * | gtext_css_parse_grammar_list() | parse a comma-separated list according to a CSS grammar (5.4.2) |
 *
 * The Editor's Draft of 20 October 2026 no longer has "parse a block's
 * contents" as an entry point (its changes section: it is "best suited as
 * just a 'consume' algo"); gtext_css_parse_block_contents() is that consume
 * algorithm run over a whole input, which is what the Candidate
 * Recommendation Draft's entry point was.
 *
 * ## Parse errors
 *
 * CSS Syntax says "this is a parse error" in a number of places without
 * naming them; the codes below are named for the place, and their comments
 * say which. A parse error is never a failure. The errors of a parse are in
 * its result, ordered by position, up to `max_errors`; the rest are counted.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_PARSE_H
#define GHOTI_IO_GTEXT_CSS_CSS_PARSE_H

#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_CSS_Error_Code
 * @brief Every parse error, by a number that does not change between releases.
 *
 * 0 to 31 are the tokenizer's, 32 and up the parser's.
 */
typedef enum {
  GTEXT_CSS_ERROR_INVALID_ESCAPE = 0,         ///< `invalid-escape`: a `\` that does not begin a valid escape (4.3.1, U+005C)
  GTEXT_CSS_ERROR_EOF_IN_COMMENT = 1,         ///< `eof-in-comment`: the input ended inside a comment (4.3.2)
  GTEXT_CSS_ERROR_EOF_IN_STRING = 2,          ///< `eof-in-string`: the input ended inside a string (4.3.5)
  GTEXT_CSS_ERROR_NEWLINE_IN_STRING = 3,      ///< `newline-in-string`: a newline ended a string: a bad-string token (4.3.5)
  GTEXT_CSS_ERROR_EOF_IN_URL = 4,             ///< `eof-in-url`: the input ended inside an unquoted url (4.3.6)
  GTEXT_CSS_ERROR_INVALID_CHARACTER_IN_URL = 5, ///< `invalid-character-in-url`: a quote, `(` or non-printable code point in an unquoted url: a bad-url token (4.3.6)
  GTEXT_CSS_ERROR_INVALID_ESCAPE_IN_URL = 6,  ///< `invalid-escape-in-url`: a `\` that does not begin a valid escape in an unquoted url: a bad-url token (4.3.6)
  GTEXT_CSS_ERROR_WHITESPACE_IN_URL = 7,      ///< `whitespace-in-url`: white space inside an unquoted url not followed by `)`: a bad-url token (4.3.6, and 5.2: a bad-url token is always a parse error)
  GTEXT_CSS_ERROR_EOF_IN_ESCAPE = 8,          ///< `eof-in-escape`: a `\` at the end of the input (4.3.7)
  GTEXT_CSS_ERROR_INVALID_UTF_8 = 9,          ///< `invalid-utf-8`: an ill-formed UTF-8 sequence, read as U+FFFD. Not the standard's: its input is code points.

  GTEXT_CSS_ERROR_EOF_IN_QUALIFIED_RULE = 32,   ///< `eof-in-qualified-rule`: the input ended before a qualified rule's block (5.5.3)
  GTEXT_CSS_ERROR_SEMICOLON_IN_QUALIFIED_RULE = 33, ///< `semicolon-in-qualified-rule`: a nested qualified rule's prelude met `;`, its stop token (5.5.3)
  GTEXT_CSS_ERROR_CLOSE_BRACE_IN_QUALIFIED_RULE = 34, ///< `close-brace-in-qualified-rule`: a `}` in a qualified rule's prelude (5.5.3)
  GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACE = 35,   ///< `unmatched-close-brace`: a `}` kept as a token (5.5.7; 5.2: always a parse error)
  GTEXT_CSS_ERROR_UNMATCHED_CLOSE_PAREN = 36,   ///< `unmatched-close-paren`: a `)` kept as a token (5.2: always a parse error)
  GTEXT_CSS_ERROR_UNMATCHED_CLOSE_BRACKET = 37, ///< `unmatched-close-bracket`: a `]` kept as a token (5.2: always a parse error)
  GTEXT_CSS_ERROR_INVALID_DECLARATION = 38,     ///< `invalid-declaration`: a declaration dropped (consume the remnants of a bad declaration, 5.5.6)
  GTEXT_CSS_ERROR_AT_RULE_IN_DECLARATION_LIST = 39, ///< `at-rule-in-declaration-list`: an at-rule read and discarded by "parse a list of declarations" (5.4.5)
  GTEXT_CSS_ERROR_RULE_LOOKS_LIKE_CUSTOM_PROPERTY = 40, ///< `rule-looks-like-custom-property`: a qualified rule whose prelude begins `--name:` dropped (5.5.3)
  GTEXT_CSS_ERROR_INVALID_RULE = 41,            ///< `invalid-rule`: a rule the validity callback refused (5.5.2, 5.5.3; for a nested qualified rule, the "invalid rule error")
  GTEXT_CSS_ERROR_INVALID_DECLARATION_IN_CONTEXT = 42 ///< `invalid-declaration-in-context`: a declaration the validity callback refused (5.5.6)
} GTEXT_CSS_Error_Code;

/**
 * @struct GTEXT_CSS_Error
 * @brief One parse error, with its position.
 */
typedef struct {
  GTEXT_CSS_Error_Code code; ///< Which.
  const char * name;         ///< Its name; static.
  size_t line;               ///< Line, 1-based.
  size_t column;             ///< Column in UTF-16 code units, 1-based.
  size_t byte_offset;        ///< Byte offset into the input, 0-based.
} GTEXT_CSS_Error;

/**
 * @brief The name of an error code.
 *
 * @param code Any value; one that is not a code returns "unknown".
 * @return A static string, different for every code.
 */
GTEXT_API const char * gtext_css_error_name(GTEXT_CSS_Error_Code code);

/** @brief How many parse errors the result holds (at most `max_errors`). NULL is 0. */
GTEXT_API size_t gtext_css_result_error_count(const GTEXT_CSS_Result * result);

/** @brief How many parse errors were raised and not kept. NULL is 0. */
GTEXT_API size_t gtext_css_result_errors_dropped(const GTEXT_CSS_Result * result);

/**
 * @brief One kept parse error.
 *
 * @param result A result a parse filled.
 * @param index 0 up to gtext_css_result_error_count().
 * @param out Receives it; written only on success.
 * @return true, or false for an index out of range or a NULL argument.
 */
GTEXT_API bool gtext_css_result_error_at(const GTEXT_CSS_Result * result,
    size_t index, GTEXT_CSS_Error * out);

/**
 * @brief Parse a stylesheet (CSS Syntax 5.4.3).
 *
 * The input is text that is already decoded. The tree's `rules` are the
 * stylesheet's, and its `location` is a copy of the options' `location`.
 * Every entry point below has the same parameters and the same contract.
 *
 * @param input  UTF-8 text; need not be terminated. NULL only with `length` 0.
 * @param length Its length in bytes.
 * @param options NULL for the defaults.
 * @param out    Receives the tree, written only on success. Not NULL.
 * @param result NULL, or filled whatever happens (free it with
 *               gtext_css_result_free()).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; or a
 *   GTEXT_CSS_E_LIMIT_* status (no tree; the result says where).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_stylesheet(const char * input,
    size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Parse a stylesheet's contents (5.4.4): the rules, with no stylesheet. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_stylesheet_contents(
    const char * input, size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Parse a list of declarations (5.4.5): the declarations; at-rules
 *  are read and discarded. What an HTML `style` attribute holds. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_declaration_list(
    const char * input, size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Consume a block's contents (5.5.5) over the whole input: rules and
 *  runs of declarations, interleaved in order, as inside a style rule. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_block_contents(const char * input,
    size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Parse a rule (5.4.6): one rule, or a syntax error in the tree's
 *  `syntax_error`. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_rule(const char * input,
    size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Parse a declaration (5.4.7): one declaration, or a syntax error. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_declaration(const char * input,
    size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Parse a component value (5.4.8): one value, or a syntax error. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_component_value(
    const char * input, size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Parse a list of component values (5.4.9). What an SVG
 *  presentation attribute holds. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_component_values(
    const char * input, size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/** @brief Parse a comma-separated list of component values (5.4.10): the
 *  groups, in the tree's `lists`. */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_comma_list(const char * input,
    size_t length, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/**
 * @brief A grammar, as the layer that knows it supplies it.
 *
 * @param user The grammar's pointer, unchanged.
 * @param index Which group: 0 for gtext_css_parse_grammar(), the group's
 *   position for the list forms.
 * @param values The component values to match; valid while the tree is.
 * @param count Their number.
 * @return Whether they match. What a match produced is the callback's to
 *   keep, in `user` (the standard's "unspecified structure").
 */
typedef bool (*GTEXT_CSS_Grammar_Match)(void * user, size_t index,
    const GTEXT_CSS_Component_Value * values, size_t count);

/** @struct GTEXT_CSS_Grammar
 *  @brief A grammar: a matcher and its pointer. */
typedef struct {
  GTEXT_CSS_Grammar_Match match; ///< Not NULL.
  void * user;                   ///< Passed back unchanged.
} GTEXT_CSS_Grammar;

/**
 * @brief Parse something according to a CSS grammar (5.4.1).
 *
 * Parses a list of component values from the input (the tree is a
 * COMPONENT_VALUE_LIST tree, which the caller frees) and asks the grammar
 * whether it matches.
 *
 * @param input  UTF-8 text, as for gtext_css_parse_stylesheet().
 * @param length Its length in bytes.
 * @param grammar The grammar. Not NULL, with a `match`.
 * @param options NULL for the defaults.
 * @param matched Receives whether it matched; written only on success. Not NULL.
 * @param out    Receives the tree, which holds the values the grammar was
 *               shown; written only on success. Not NULL.
 * @param result NULL, or filled whatever happens.
 * @return As the entry points above; GTEXT_CSS_E_INVALID for a grammar with
 *   no `match`.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_grammar(const char * input,
    size_t length, const GTEXT_CSS_Grammar * grammar,
    const GTEXT_CSS_Parse_Options * options, bool * matched,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/**
 * @brief Parse a comma-separated list according to a CSS grammar (5.4.2).
 *
 * An input of nothing but white space is an empty list (the grammar is not
 * called). Otherwise each group of the comma-separated list is matched, in
 * order, and the tree (a COMMA_LIST tree) holds the groups.
 *
 * @param input  UTF-8 text, as for gtext_css_parse_stylesheet().
 * @param length Its length in bytes.
 * @param grammar The grammar. Not NULL, with a `match`.
 * @param options NULL for the defaults.
 * @param matched_count Receives how many groups matched; written only on
 *   success. Not NULL. The number of groups is the tree's `list_count`.
 * @param out    Receives the tree (its `lists` are the groups); written only
 *               on success. Not NULL.
 * @param result NULL, or filled whatever happens.
 * @return As gtext_css_parse_grammar().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_grammar_list(const char * input,
    size_t length, const GTEXT_CSS_Grammar * grammar,
    const GTEXT_CSS_Parse_Options * options, size_t * matched_count,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/**
 * @brief Match component values already parsed against a grammar (5.4.1 for
 *   an input that is a list of component values: "normalize" leaves it as
 *   it is). The grammar is called once, with index 0.
 *
 * @return Whether it matched; false for a NULL grammar or matcher, or NULL
 *   values with a count.
 */
GTEXT_API bool gtext_css_match_grammar(const GTEXT_CSS_Component_Value * values,
    size_t count, const GTEXT_CSS_Grammar * grammar);

/**
 * @brief Match each comma-separated group of component values already
 *   parsed (5.4.2 for a list input). The groups are split at top-level comma
 *   tokens without allocating; a list of nothing but white space is empty.
 *
 * @param values The values; NULL only with a count of 0.
 * @param count Their number.
 * @param grammar The grammar.
 * @param group_count Receives the number of groups (NULL if not wanted).
 * @return How many groups matched.
 */
GTEXT_API size_t gtext_css_match_grammar_list(
    const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Grammar * grammar, size_t * group_count);

/**
 * @brief Read the An+B microsyntax (CSS Syntax 6.2) from component values.
 *
 * The values are what an `:nth-child()` holds: for example the children of
 * a function, or a list from gtext_css_parse_component_values(). White space
 * before and after is allowed; between a `+` and the `n` that follows it, it
 * is not (the standard's dagger). A and B are read exactly; their magnitudes
 * saturate at INT64_MAX (the range is symmetric, INT64_MIN is never produced),
 * so `n-9223372036854775808` and `n -9223372036854775808` both give
 * B = -INT64_MAX.
 *
 * @param values The values.
 * @param count Their number.
 * @param a Receives A on success (not NULL).
 * @param b Receives B on success (not NULL).
 * @return Whether the values are an `<an-plus-b>`.
 */
GTEXT_API bool gtext_css_anb_parse(const GTEXT_CSS_Component_Value * values,
    size_t count, int64_t * a, int64_t * b);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_PARSE_H
