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
 * @file css_grammar.h
 * @brief The CSS Value Definition Syntax: compiling a grammar as a
 *   specification writes it, and matching component values against it.
 *
 * ## Two steps
 *
 * gtext_css_definition_parse() reads the grammar text (`[ <length-percentage
 * [0,∞]> | auto ]{1,4}`) into an immutable @ref GTEXT_CSS_Definition. A
 * definition may name other types (`<foo>`, `<'property'>`, `<foo()>`); it is
 * gtext_css_definition_link() that resolves them, the native types first and
 * then a caller-supplied @ref GTEXT_CSS_Resolver, and returns an immutable
 * @ref GTEXT_CSS_Compiled that holds everything it needs and may be shared by
 * any number of threads. Composite types such as `<position>` and
 * `<length-percentage>` are not code: they come from the resolver, as the
 * grammars the specifications give them.
 *
 * ## What the text may say
 *
 * Juxtaposition, `&&`, `||` and `|` with the draft's precedence (juxtaposition
 * binds tightest); `[ ]`; the multipliers `?` `*` `+` `#` `{A}` `{A,B}` `{A,}`
 * and `#{A,B}` (and `+#`, `#?`); `!` after a group; keywords (matched ASCII
 * case-insensitively); `<name>`, `<'property'>` and `<name()>`; a range after
 * the name of a numeric type, `<number [0,∞]>`, `<length [0px,∞]>` (bounds
 * `∞`, `-∞`, or a number with a unit, inclusive); function notation `name(
 * ... )`; the literal characters `, / : ; =` and any quoted `'c'`; the block
 * notations `{ ... }` and `( ... )`; and the native types:
 *
 * `<integer> <number> <percentage> <dimension> <length> <angle> <time>
 * <frequency> <resolution> <flex> <ratio> <zero> <string> <url> <ident>
 * <custom-ident> <dashed-ident> <any-value> <declaration-value> <urange>
 * <an+b> <declaration-list> <rule-list>` and `<ident-token>`, `<function-token>`,
 * `<at-keyword-token>`, `<hash-token>`, `<string-token>`, `<bad-string-token>`,
 * `<url-token>`, `<bad-url-token>`, `<delim-token>`, `<number-token>`,
 * `<percentage-token>`, `<dimension-token>`, `<whitespace-token>`, `<CDO-token>`,
 * `<CDC-token>`, `<colon-token>`, `<semicolon-token>`, `<comma-token>`,
 * `<[-token>`, `<]-token>`, `<(-token>`, `<)-token>`, `<{-token>`, `<}-token>`.
 *
 * A range on a type that is not native applies to every numeric type in its
 * definition (`<length-percentage [0,∞]>` restricts both), together with any
 * range already there.
 *
 * ## Matching
 *
 * A match has the draft's meaning: it succeeds if a parse of the component
 * values exists that consumes all of them. White space and comments between
 * components are ignored. The search backtracks completely, memoized over the
 * grammar node, the position, and for `&&` and `||` the set of members used;
 * its work and its memory are counted and bounded, and a bound that is reached
 * is the status GTEXT_CSS_E_LIMIT_WORK and never a "no match".
 *
 * Alternatives are tried in written order and a juxtaposition prefers its
 * earlier items to be longer, so the typed result (css_value.h) is
 * deterministic.
 *
 * Linking refuses left recursion (a type that can match itself without
 * consuming a component value) with GTEXT_CSS_E_LEFT_RECURSION, and a reference
 * that nothing resolves with GTEXT_CSS_E_REFERENCE; both name the type in the
 * result. Nothing recurses in parsing, linking, matching or serializing.
 *
 * ## Details that are decided here
 *
 * - `0` matches `<length>` as 0px but not `<angle>`, `<time>`, `<percentage>` ...;
 *   a number that is a literal out of range does not match;
 *   a math function (`calc()`, `min()`, ...) matches any numeric type by name and
 *   is checked and clamped by the math layer.
 * - `<url>` is a url token or `url(` a string and any modifiers `)`.
 * - `<custom-ident>` is an identifier other than a CSS-wide keyword or `default`.
 * - `<any-value>` and `<declaration-value>` match one or more component values
 *   (white space alone is nothing); `<declaration-value>` refuses a top-level
 *   `;` or `!` and both refuse a bad string, a bad url and an unmatched closing token.
 * - `<urange>` is a unicode-range token, so it matches in a `unicode-range`
 *   declaration's value only (the tokenizer makes the token only there).
 * - `<whitespace-token>` matches no component value: it succeeds where white
 *   space precedes the next component value, consuming nothing.
 * - A value containing `var()`, `env()` or `attr()` anywhere is pending
 *   substitution and matches any grammar.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_GRAMMAR_H
#define GHOTI_IO_GTEXT_CSS_CSS_GRAMMAR_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- definitions ------------------------------------------------------ */

/** @brief A parsed grammar (the struct is private). Immutable. */
typedef struct GTEXT_CSS_Definition GTEXT_CSS_Definition;

/** @brief A linked grammar, ready to match (the struct is private). Immutable. */
typedef struct GTEXT_CSS_Compiled GTEXT_CSS_Compiled;

/** @brief What a reference names. */
typedef enum {
  GTEXT_CSS_REF_TYPE = 0, ///< `<name>` or `<name()>`
  GTEXT_CSS_REF_PROPERTY  ///< `<'name'>`: the grammar of a property's value
} GTEXT_CSS_Ref_Kind;

/**
 * @brief What links a reference: the definition of a type or property.
 *
 * Called once for each distinct reference (and range) that is not a native type.
 *
 * @param user The resolver's pointer, unchanged.
 * @param kind Type or property.
 * @param name The name without the angle brackets or quotes (`length-percentage`,
 *   `width`, `rgb()`); need not be terminated.
 * @param length Its length.
 * @return The definition, which the caller owns and keeps alive until the link
 *   call returns (the link copies what it needs), or NULL for a name it does not have.
 */
typedef const GTEXT_CSS_Definition * (*GTEXT_CSS_Resolver)(void * user,
    GTEXT_CSS_Ref_Kind kind, const char * name, size_t length);

/**
 * @struct GTEXT_CSS_Grammar_Options
 * @brief The allocator and limits of parsing and linking a grammar.
 *
 * A field left at 0 selects its default and 0 never means unlimited. A zeroed
 * struct is the default.
 */
typedef struct {
  /** The allocator of everything, or NULL for the default. The struct is
   *  copied; its `ctx` must outlive the definition or compiled grammar. */
  const GTEXT_Allocator * allocator;
  /** Longest definition text, in bytes. Default 65,536.
   *  One more is GTEXT_CSS_E_LIMIT_DEFINITION. */
  size_t max_definition_bytes;
  /** Deepest nesting of groups, functions and blocks: `[[a]]` is 2 deep. Default 256.
   *  One more is GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH. */
  size_t max_definition_depth;
  /** Most nodes in a definition, and in a linked grammar (every node of every
   *  definition it holds, counted once for each distinct range a type was
   *  linked with). Default 65,536. One more is GTEXT_CSS_E_LIMIT_DEFINITION_NODES. */
  size_t max_definition_nodes;
} GTEXT_CSS_Grammar_Options;

/** @brief The defaults, spelled out: no allocator, 65,536 bytes, depth 256, 65,536 nodes. */
GTEXT_API GTEXT_CSS_Grammar_Options gtext_css_grammar_options_default(void);

/**
 * @struct GTEXT_CSS_Grammar_Result
 * @brief What parsing or linking a grammar reports besides the grammar.
 *
 * Plain data: nothing to free. Filled whatever happens.
 */
typedef struct {
  GTEXT_CSS_Status status; ///< What the call returned.
  const char * message;    ///< A static description of the status; never NULL after a call.
  size_t offset;           ///< GTEXT_CSS_E_GRAMMAR and the limits: the byte of the definition text where it went wrong.
  char name[96];           ///< GTEXT_CSS_E_REFERENCE and GTEXT_CSS_E_LEFT_RECURSION: the reference as written
                           ///< (`<nosuch>`, `<'width'>`), cut to fit; empty otherwise.
  size_t nodes;            ///< Nodes the definition or the linked grammar has.
} GTEXT_CSS_Grammar_Result;

/**
 * @brief Parse the text of a grammar.
 *
 * @param text UTF-8; need not be terminated. NULL only with a length of 0.
 * @param length Its length.
 * @param options NULL for the defaults.
 * @param out Receives the definition; written only on success. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM;
 *   GTEXT_CSS_E_GRAMMAR (the text is not a definition: an empty one, an unbalanced
 *   bracket, a missing operand, a bad range or multiplier);
 *   GTEXT_CSS_E_LIMIT_DEFINITION, _DEFINITION_DEPTH, _DEFINITION_NODES;
 *   GTEXT_CSS_E_LIMIT_GROUP.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_definition_parse(const char * text, size_t length,
    const GTEXT_CSS_Grammar_Options * options, GTEXT_CSS_Definition ** out,
    GTEXT_CSS_Grammar_Result * result);

/** @brief Free a definition; NULL is ignored. A compiled grammar made from it
 *  does not depend on it. */
GTEXT_API void gtext_css_definition_free(GTEXT_CSS_Definition * definition);

/** @brief How many nodes a definition has. NULL is 0. */
GTEXT_API size_t gtext_css_definition_node_count(const GTEXT_CSS_Definition * definition);

/**
 * @brief Link a definition: resolve its references, check it, and make it ready to match.
 *
 * @param definition The definition. Not NULL.
 * @param resolver NULL when the definition names only native types; any other
 *   reference is then GTEXT_CSS_E_REFERENCE.
 * @param user The resolver's pointer.
 * @param options NULL for the defaults (as for the definition: `max_definition_nodes` bounds the linked whole).
 * @param out Receives the grammar; written only on success. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_REFERENCE;
 *   GTEXT_CSS_E_LEFT_RECURSION; GTEXT_CSS_E_LIMIT_DEFINITION_NODES.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_definition_link(const GTEXT_CSS_Definition * definition,
    GTEXT_CSS_Resolver resolver, void * user, const GTEXT_CSS_Grammar_Options * options,
    GTEXT_CSS_Compiled ** out, GTEXT_CSS_Grammar_Result * result);

/**
 * @brief Parse and link in one call (the definition is freed again).
 *
 * @return As gtext_css_definition_parse() and gtext_css_definition_link().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_compile(const char * text, size_t length,
    GTEXT_CSS_Resolver resolver, void * user, const GTEXT_CSS_Grammar_Options * options,
    GTEXT_CSS_Compiled ** out, GTEXT_CSS_Grammar_Result * result);

/** @brief Free a compiled grammar; NULL is ignored. Values it matched do not depend on it. */
GTEXT_API void gtext_css_compiled_free(GTEXT_CSS_Compiled * compiled);

/** @brief How many grammar nodes (slots) it has. NULL is 0. */
GTEXT_API size_t gtext_css_compiled_node_count(const GTEXT_CSS_Compiled * compiled);

/**
 * @brief The grammar text of a slot: what a value's `slot` stands for.
 *
 * @param compiled The grammar.
 * @param slot A value's `slot`.
 * @param length Receives the length of the text. Not NULL.
 * @return The slot's own source text in the definition it came from (for example
 *   `<length-percentage [0,∞]>` or `auto`), **not** NUL-terminated (it is a part
 *   of the definition's text) and owned by the grammar; NULL for a slot it does
 *   not have.
 */
GTEXT_API const char * gtext_css_compiled_slot_text(const GTEXT_CSS_Compiled * compiled,
    uint32_t slot, size_t * length);

/** @brief The atoms of the keywords the grammar names. Owned by it; NULL for a NULL grammar. */
GTEXT_API const GTEXT_CSS_Atom_Table * gtext_css_compiled_atoms(const GTEXT_CSS_Compiled * compiled);

/* ---- matching --------------------------------------------------------- */

/**
 * @struct GTEXT_CSS_Match_Options
 * @brief What a match may do. A zeroed struct is the default.
 */
typedef struct {
  /** The allocator of the match and of the value set, or NULL for the default. */
  const GTEXT_Allocator * allocator;
  /** Most units of work (a grammar node examined at a position, a position
   *  added to a set, a value made). Default 67,108,864. One more is GTEXT_CSS_E_LIMIT_WORK. */
  size_t max_match_work;
  /** Most memo entries: each remembered (node, position) answer, each interval
   *  of those answers, and each frame waiting on another. Default 4,194,304.
   *  One more is GTEXT_CSS_E_LIMIT_WORK. */
  size_t max_memo_entries;
  /** HTML quirks: a bare non-zero number matches `<length>` as that many pixels
   *  (GTEXT_CSS_VFLAG_UNITLESS_LENGTH). Which properties enable it is a decision of the
   *  property layer. Default false. */
  bool unitless_length;
} GTEXT_CSS_Match_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Match_Options gtext_css_match_options_default(void);

/**
 * @struct GTEXT_CSS_Match_Result
 * @brief What a match measured. Plain data. Filled whatever happens.
 */
typedef struct {
  GTEXT_CSS_Status status; ///< What the call returned.
  const char * message;    ///< A static description of the status.
  size_t work;             ///< Units of work done (see `max_match_work`).
  size_t memo_entries;     ///< The most memo entries held at once (see `max_memo_entries`).
  size_t positions;        ///< Items the match worked over: each component value (white space left out) and one end marker for each list of them.
} GTEXT_CSS_Match_Result;

/**
 * @brief Match component values against a compiled grammar.
 *
 * @param compiled The grammar. Not NULL.
 * @param values The component values (for example a declaration's value); NULL only with a count of 0.
 * @param count Their number.
 * @param options NULL for the defaults.
 * @param matched Receives whether they match; written only on success. Not NULL.
 * @param out Receives the value set when they matched (its root may be NULL
 *   when the grammar matched nothing to produce); NULL when they did not.
 *   Written only on success. NULL if the typed result is not wanted.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK (matched or not); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM;
 *   GTEXT_CSS_E_LIMIT_WORK (the answer is not known).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_match(const GTEXT_CSS_Compiled * compiled,
    const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Match_Options * options, bool * matched,
    GTEXT_CSS_Value_Set ** out, GTEXT_CSS_Match_Result * result);

/**
 * @struct GTEXT_CSS_Matcher
 * @brief Plugs a compiled grammar into the hook of css_parse.h.
 *
 * Caller-owned and zero-initialised by gtext_css_matcher_init(). The hook
 * answers a bool, so what it learned is kept here: the status of the last
 * match, and the value set of each group that matched.
 */
typedef struct {
  const GTEXT_CSS_Compiled * compiled; ///< The grammar; borrowed.
  GTEXT_CSS_Match_Options options;     ///< Copied by init.
  GTEXT_CSS_Status status;             ///< The status of the last match (OK, or why the hook said "no").
  GTEXT_CSS_Value_Set ** sets;         ///< Private: the value set of each group index that matched, or NULL.
  size_t set_count;                    ///< Private: entries of `sets`.
} GTEXT_CSS_Matcher;

/** @brief Prepare a matcher for a grammar. @return OK or INVALID (NULL arguments). */
GTEXT_API GTEXT_CSS_Status gtext_css_matcher_init(GTEXT_CSS_Matcher * matcher,
    const GTEXT_CSS_Compiled * compiled, const GTEXT_CSS_Match_Options * options);

/** @brief The hook for gtext_css_parse_grammar(), gtext_css_match_grammar() and
 *  the list forms: its `match` runs gtext_css_match() and keeps the value set under the group's index.
 *  A failed match (a limit, memory) answers false and leaves the status in `matcher->status`. */
GTEXT_API GTEXT_CSS_Grammar gtext_css_matcher_grammar(GTEXT_CSS_Matcher * matcher);

/** @brief The value set of group `index` of the last run that matched, or NULL. Owned by the matcher. */
GTEXT_API const GTEXT_CSS_Value_Set * gtext_css_matcher_value_set(const GTEXT_CSS_Matcher * matcher, size_t index);

/** @brief Free every value set the matcher holds and reset it. */
GTEXT_API void gtext_css_matcher_free(GTEXT_CSS_Matcher * matcher);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_GRAMMAR_H
