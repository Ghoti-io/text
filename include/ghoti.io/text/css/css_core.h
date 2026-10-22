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
 * @file css_core.h
 * @brief Statuses, options, the parse result and the tree of CSS Syntax.
 *
 * The CSS module reads CSS as CSS Syntax Module Level 3 defines it (the
 * Editor's Draft of 20 October 2026): tokenization with the input stream's
 * preprocessing, every parser entry point, CSS Nesting's interleaving of
 * declarations and rules inside blocks, and the An+B microsyntax. The input
 * is text that is already decoded, as UTF-8.
 *
 * **CSS parsing does not fail on content.** Every input has a tree; the
 * mistakes in it are parse errors, each reported as a diagnostic with its
 * position (css_parse.h), and the constructs the standard drops are dropped
 * and reported. A parse fails only for an argument, for memory, or for a
 * limit, and each limit is its own status.
 *
 * ## The tree
 *
 * A parse returns a @ref GTEXT_CSS_Tree, which owns every node in one block of
 * memory and is freed with gtext_css_tree_free(). The nodes are plain structs
 * read in place; they are never modified after the parse returns, so a tree
 * may be read from any number of threads at once.
 *
 * - A @ref GTEXT_CSS_Component_Value is a preserved token, a function or a
 *   simple block (CSS Syntax 5.2).
 * - A @ref GTEXT_CSS_Declaration has a name, a value (component values), the
 *   important flag and, for a custom property, its original text.
 * - A @ref GTEXT_CSS_Rule is a qualified rule, an at-rule or a nested
 *   declarations rule.
 *
 * Every rule, declaration and component value has an `id`, unique within its
 * tree and numbered from 1 in document order, and a span (GTEXT_CSS_Span): the
 * bytes of the input it was read from, and the line and column where they
 * begin.
 *
 * ## Positions
 *
 * `offset` is 0-based into the bytes the parser was given. `line` and
 * `column` are 1-based; a line ends at a newline as CSS preprocessing defines
 * it (LF, CR, CR LF or FF), and the column counts **UTF-16 code units** (a
 * character outside the Basic Multilingual Plane is 2), as the HTML module's
 * positions do.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CORE_H
#define GHOTI_IO_GTEXT_CSS_CSS_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/encoding/encoding_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_CSS_Status
 * @brief What parsing and serializing CSS report.
 *
 * There is no status for a syntax error: CSS has none. Each limit is its own
 * status, so that a refusal says which bound was reached.
 */
typedef enum {
  GTEXT_CSS_OK = 0,      ///< Success.
  GTEXT_CSS_E_INVALID,   ///< An argument was unusable: a NULL output, NULL
                         ///< data with a length, a sink with no `write`.
  GTEXT_CSS_E_OOM,       ///< Out of memory. No tree, and nothing leaked.
  GTEXT_CSS_E_INTERNAL,  ///< A defect in this library, never content.
  GTEXT_CSS_E_UNSUPPORTED, ///< Reserved for the layers above syntax (a
                         ///< computed value that is not implemented yet is
                         ///< refused with this and never guessed). The syntax
                         ///< layer never returns it.
  GTEXT_CSS_E_LIMIT_INPUT,        ///< The input is longer than `max_input_bytes`.
  GTEXT_CSS_E_LIMIT_TOKENS,       ///< More than `max_tokens` tokens.
  GTEXT_CSS_E_LIMIT_DEPTH,        ///< Blocks and functions nested deeper
                                  ///< than `max_depth`.
  GTEXT_CSS_E_LIMIT_RULES,        ///< More than `max_rules` rules.
  GTEXT_CSS_E_LIMIT_DECLARATIONS, ///< More than `max_declarations`
                                  ///< declarations.
  GTEXT_CSS_E_LIMIT_ERRORS,       ///< More than `max_total_errors` parse
                                  ///< errors.
  GTEXT_CSS_E_WRITE,              ///< Serializer: the sink refused output.
  GTEXT_CSS_E_LIMIT_OUTPUT,       ///< Serializer: more than
                                  ///< `max_output_bytes` of output.
  GTEXT_CSS_STATUS_COUNT          ///< The number of statuses; not a status.
} GTEXT_CSS_Status;

/**
 * @brief A static, human-readable name for a status.
 *
 * @param status Any value; one that is not a status returns "unknown status".
 * @return A string that is never freed, different for every status.
 */
GTEXT_API const char * gtext_css_status_string(GTEXT_CSS_Status status);

/**
 * @struct GTEXT_CSS_Span
 * @brief Where in the input a node, a token or a diagnostic was read.
 *
 * Bytes `offset` up to (not including) `end`, and the line and column of
 * `offset`. For a diagnostic `end` is `offset`: a diagnostic is a place, not
 * a range.
 */
typedef struct {
  size_t offset; ///< First byte, 0-based.
  size_t end;    ///< One past the last byte.
  size_t line;   ///< Line of `offset`, 1-based.
  size_t column; ///< Column of `offset` in UTF-16 code units, 1-based.
} GTEXT_CSS_Span;

/**
 * @struct GTEXT_CSS_String
 * @brief A string in the tree: UTF-8, owned by the tree.
 *
 * `data[length]` is a NUL, so `data` may be used as a C string. Preprocessing
 * replaces U+0000 and an escape cannot produce it, so a string never contains
 * a NUL of its own. An absent string has `data` NULL and `length` 0.
 */
typedef struct {
  const char * data; ///< The bytes, NUL-terminated; NULL if absent.
  size_t length;     ///< Their number, the NUL not counted.
} GTEXT_CSS_String;

/** @brief A node's identity: unique within its tree, from 1, in document order. */
typedef uint64_t GTEXT_CSS_Id;

/**
 * @enum GTEXT_CSS_Token_Type
 * @brief The tokens of CSS Syntax section 4, and an end-of-file marker.
 *
 * In a @ref GTEXT_CSS_Component_Value the type is that of the preserved token,
 * GTEXT_CSS_TOKEN_FUNCTION for a function, or the opening token of a simple
 * block. The closing tokens appear only as preserved tokens that matched
 * nothing.
 */
typedef enum {
  GTEXT_CSS_TOKEN_IDENT = 0,      ///< `<ident-token>`
  GTEXT_CSS_TOKEN_FUNCTION,       ///< `<function-token>`, or a function
  GTEXT_CSS_TOKEN_AT_KEYWORD,     ///< `<at-keyword-token>`
  GTEXT_CSS_TOKEN_HASH,           ///< `<hash-token>`
  GTEXT_CSS_TOKEN_STRING,         ///< `<string-token>`
  GTEXT_CSS_TOKEN_BAD_STRING,     ///< `<bad-string-token>`
  GTEXT_CSS_TOKEN_URL,            ///< `<url-token>`
  GTEXT_CSS_TOKEN_BAD_URL,        ///< `<bad-url-token>`
  GTEXT_CSS_TOKEN_DELIM,          ///< `<delim-token>`
  GTEXT_CSS_TOKEN_NUMBER,         ///< `<number-token>`
  GTEXT_CSS_TOKEN_PERCENTAGE,     ///< `<percentage-token>`
  GTEXT_CSS_TOKEN_DIMENSION,      ///< `<dimension-token>`
  GTEXT_CSS_TOKEN_UNICODE_RANGE,  ///< `<unicode-range-token>` (only in a
                                  ///< `unicode-range` declaration's value)
  GTEXT_CSS_TOKEN_WHITESPACE,     ///< `<whitespace-token>`
  GTEXT_CSS_TOKEN_CDO,            ///< `<CDO-token>` (`<!--`)
  GTEXT_CSS_TOKEN_CDC,            ///< `<CDC-token>` (`-->`)
  GTEXT_CSS_TOKEN_COLON,          ///< `<colon-token>`
  GTEXT_CSS_TOKEN_SEMICOLON,      ///< `<semicolon-token>`
  GTEXT_CSS_TOKEN_COMMA,          ///< `<comma-token>`
  GTEXT_CSS_TOKEN_OPEN_SQUARE,    ///< `<[-token>`, or a []-block
  GTEXT_CSS_TOKEN_CLOSE_SQUARE,   ///< `<]-token>`
  GTEXT_CSS_TOKEN_OPEN_PAREN,     ///< `<(-token>`, or a ()-block
  GTEXT_CSS_TOKEN_CLOSE_PAREN,    ///< `<)-token>`
  GTEXT_CSS_TOKEN_OPEN_CURLY,     ///< `<{-token>`, or a {}-block
  GTEXT_CSS_TOKEN_CLOSE_CURLY,    ///< `<}-token>`
  GTEXT_CSS_TOKEN_EOF,            ///< `<EOF-token>`; never in a tree
  GTEXT_CSS_TOKEN_TYPE_COUNT      ///< The number of types; not a type.
} GTEXT_CSS_Token_Type;

/**
 * @brief The name of a token type as the standard spells it ("ident",
 *   "function", "at-keyword", ... "{", "}", "EOF").
 *
 * @param type Any value; one that is not a type returns "unknown".
 * @return A static string, never freed.
 */
GTEXT_API const char * gtext_css_token_type_name(GTEXT_CSS_Token_Type type);

/** @brief What a component value is (CSS Syntax 5.2). */
typedef enum {
  GTEXT_CSS_VALUE_TOKEN = 0, ///< A preserved token.
  GTEXT_CSS_VALUE_FUNCTION,  ///< A function: a name and its arguments.
  GTEXT_CSS_VALUE_BLOCK      ///< A simple block: its associated token and contents.
} GTEXT_CSS_Value_Kind;

/** @name Component value flags
 * Bits of GTEXT_CSS_Component_Value::flags.
 * @{ */
/** A hash token whose type flag is "id" (otherwise "unrestricted"). */
#define GTEXT_CSS_FLAG_HASH_ID 0x01u
/** A number or dimension token whose type flag is "integer" (otherwise
 *  "number"). A percentage token has no type flag; this bit is clear on one. */
#define GTEXT_CSS_FLAG_INTEGER 0x02u
/** A numeric token whose sign character is "+". */
#define GTEXT_CSS_FLAG_SIGN_PLUS 0x04u
/** A numeric token whose sign character is "-". */
#define GTEXT_CSS_FLAG_SIGN_MINUS 0x08u
/** A string or url token, a function or a simple block that the end of the
 *  input closed (a parse error for the tokens; the standard's ordinary case
 *  for a block or a function). */
#define GTEXT_CSS_FLAG_UNCLOSED 0x10u
/** A number, dimension or percentage token whose value is not exactly the
 *  decimal it was written as: the nearest `double` was taken (ties to even). */
#define GTEXT_CSS_FLAG_NUMBER_INEXACT 0x20u
/** A numeric token whose magnitude is too large for a `double`: `number` is
 *  infinity (with the token's sign). CSS Syntax gives a number token an
 *  unbounded value; what a property does with one out of range is for the
 *  layers above, which this flag lets them see. */
#define GTEXT_CSS_FLAG_NUMBER_OVERFLOW 0x40u
/** A numeric token whose magnitude is nonzero and below the smallest normal
 *  `double` (2^-1022), and not exactly representable: `number` is subnormal or
 *  zero. */
#define GTEXT_CSS_FLAG_NUMBER_UNDERFLOW 0x80u
/** @} */

/**
 * @struct GTEXT_CSS_Component_Value
 * @brief A preserved token, a function or a simple block.
 *
 * Which fields mean something depends on `kind` and `type`:
 *
 * | type | `value` | other fields |
 * |---|---|---|
 * | ident, at-keyword, url | the token's value | |
 * | hash | the value | GTEXT_CSS_FLAG_HASH_ID |
 * | string | the value | GTEXT_CSS_FLAG_UNCLOSED if the input ended it |
 * | delim | the code point, as UTF-8 | `delim` |
 * | number, percentage | | `number`, `repr`, the flags |
 * | dimension | | `number`, `repr`, `unit`, the flags |
 * | unicode-range | | `range_start`, `range_end` |
 * | function | the name | `children` |
 * | `[`, `(`, `{` (a block) | | `children` |
 *
 * `repr` is the numeric part as it was written (`+.5`, `12e2`): the value is
 * `number`, the nearest `double` to that decimal as the math library rounds it
 * (correctly, ties to even, whatever the length of the spelling and whatever the
 * host's rounding mode or locale; GTEXT_CSS_FLAG_NUMBER_* say when it is not
 * exact), and the spelling is kept so that a number can be written back the
 * way it came and so that the type and the sign survive a round trip.
 */
/** @brief A component value (the struct below). */
typedef struct GTEXT_CSS_Component_Value GTEXT_CSS_Component_Value;
struct GTEXT_CSS_Component_Value {
  GTEXT_CSS_Id id;             ///< Unique in the tree, from 1, in document order.
  GTEXT_CSS_Value_Kind kind;   ///< Token, function or block.
  GTEXT_CSS_Token_Type type;   ///< The token's type; FUNCTION for a function;
                               ///< OPEN_SQUARE, OPEN_PAREN or OPEN_CURLY for a block.
  uint32_t flags;              ///< GTEXT_CSS_FLAG_* bits.
  GTEXT_CSS_Span span;         ///< The whole value: a block or a function
                               ///< from its opening token to its closing one.
  GTEXT_CSS_String value;      ///< See the table above.
  GTEXT_CSS_String unit;       ///< A dimension's unit.
  GTEXT_CSS_String repr;       ///< A numeric token's number as written.
  double number;               ///< A numeric token's value.
  uint32_t delim;              ///< A delim token's code point.
  uint32_t range_start;        ///< A unicode-range token's first code point.
  uint32_t range_end;          ///< Its last (inclusive; a range whose end is
                               ///< before its start is empty).
  const GTEXT_CSS_Component_Value * children; ///< A function's arguments or a
                               ///< block's contents; NULL when there are none.
  size_t child_count;          ///< Their number.
};

/**
 * @struct GTEXT_CSS_Value_List
 * @brief A list of component values: one group of a comma-separated list.
 */
typedef struct {
  const GTEXT_CSS_Component_Value * values; ///< NULL when empty.
  size_t count;                             ///< Their number.
  GTEXT_CSS_Span span;                      ///< From the first value to the last
                                            ///< (empty where there are none).
} GTEXT_CSS_Value_List;

/**
 * @struct GTEXT_CSS_Declaration
 * @brief A declaration (CSS Syntax 5.2).
 *
 * The value has had a trailing `!important` removed (and the important flag
 * set) and trailing white space removed, as "consume a declaration" says,
 * except for a `unicode-range` declaration, whose value is the standard's
 * "consume the value of a unicode-range descriptor" of the source the value
 * was read from (so the tokens are unicode-range tokens where the text spells
 * one).
 */
typedef struct {
  GTEXT_CSS_Id id;                 ///< Unique in the tree.
  GTEXT_CSS_Span span;             ///< From the name to the last token of the
                                   ///< value that is not white space.
  GTEXT_CSS_String name;           ///< The name, as written (case kept).
  GTEXT_CSS_Span name_span;        ///< The name token.
  const GTEXT_CSS_Component_Value * value; ///< NULL when empty.
  size_t value_count;              ///< Their number.
  GTEXT_CSS_Span value_span;       ///< From the first value token to the last.
  bool important;                  ///< The important flag.
  GTEXT_CSS_String original_text;  ///< A custom property's (`--*`) original
                                   ///< text: the source of its value tokens,
                                   ///< after preprocessing. Absent (`data`
                                   ///< NULL) for any other declaration.
} GTEXT_CSS_Declaration;

/** @brief What a rule is. */
typedef enum {
  GTEXT_CSS_RULE_QUALIFIED = 0,     ///< A qualified rule (a style rule, usually).
  GTEXT_CSS_RULE_AT,                ///< An at-rule.
  GTEXT_CSS_RULE_NESTED_DECLARATIONS ///< A nested declarations rule: the
                                    ///< declarations of a qualified rule's
                                    ///< block that follow a child rule.
} GTEXT_CSS_Rule_Kind;

/** @brief A rule (the struct GTEXT_CSS_Rule below). */
typedef struct GTEXT_CSS_Rule GTEXT_CSS_Rule;

/** @brief What an item of a block's contents is. */
typedef enum {
  GTEXT_CSS_ITEM_RULE = 0,     ///< A rule.
  GTEXT_CSS_ITEM_DECLARATIONS  ///< A list of declarations.
} GTEXT_CSS_Item_Kind;

/**
 * @struct GTEXT_CSS_Block_Item
 * @brief One item of a block's contents (CSS Syntax 5.5.5): a rule, or a run
 *   of declarations.
 */
typedef struct {
  GTEXT_CSS_Item_Kind kind;                    ///< Which.
  const GTEXT_CSS_Rule * rule;                 ///< The rule, for ITEM_RULE.
  const GTEXT_CSS_Declaration * declarations;  ///< The run, for
                                               ///< ITEM_DECLARATIONS.
  size_t declaration_count;                    ///< Its length.
} GTEXT_CSS_Block_Item;

/**
 * @struct GTEXT_CSS_Rule
 * @brief A qualified rule, an at-rule or a nested declarations rule.
 *
 * **A qualified rule's block** is materialized as the standard's "consume a
 * qualified rule" says: if its contents begin with declarations, those are
 * `declarations`; every rule in it is in `rules`, in order, and every later
 * run of declarations is a GTEXT_CSS_RULE_NESTED_DECLARATIONS rule in its
 * place among them.
 *
 * **An at-rule's block** is left as the standard's "consume a block" returns
 * it, because "how they're materialized in the CSSOM depends on the rule":
 * `contents` is the list of items, rules and runs of declarations, in order.
 *
 * **A nested declarations rule** has `declarations` only.
 */
struct GTEXT_CSS_Rule {
  GTEXT_CSS_Id id;                 ///< Unique in the tree.
  GTEXT_CSS_Rule_Kind kind;        ///< Which kind of rule.
  GTEXT_CSS_Span span;             ///< The whole rule.
  GTEXT_CSS_String name;           ///< An at-rule's name (without the `@`).
  const GTEXT_CSS_Component_Value * prelude; ///< The prelude; NULL when empty.
  size_t prelude_count;            ///< Its length.
  GTEXT_CSS_Span prelude_span;     ///< From the first prelude value to the last.
  bool has_block;                  ///< The rule ended in a {}-block (always for
                                   ///< a qualified rule; for an at-rule, false
                                   ///< for a statement at-rule).
  GTEXT_CSS_Span block_span;       ///< The block, from `{` to `}` (or to the
                                   ///< end of the input).
  const GTEXT_CSS_Declaration * declarations; ///< See above; NULL when none.
  size_t declaration_count;        ///< Their number.
  const GTEXT_CSS_Rule * rules;    ///< A qualified rule's child rules.
  size_t rule_count;               ///< Their number.
  const GTEXT_CSS_Block_Item * contents; ///< An at-rule's block contents.
  size_t content_count;            ///< Their number.
  bool unclosed;                   ///< The end of the input closed the block
                                   ///< (there was no `}`).
};

/** @brief Which parser entry point made a tree, and so which of its fields hold the result. */
typedef enum {
  GTEXT_CSS_TREE_STYLESHEET = 0,        ///< `rules` (and `location`).
  GTEXT_CSS_TREE_STYLESHEET_CONTENTS,   ///< `rules`.
  GTEXT_CSS_TREE_DECLARATION_LIST,      ///< `declarations`.
  GTEXT_CSS_TREE_BLOCK_CONTENTS,        ///< `items`.
  GTEXT_CSS_TREE_RULE,                  ///< `rules` (one, or none on a syntax error).
  GTEXT_CSS_TREE_DECLARATION,           ///< `declarations` (one, or none).
  GTEXT_CSS_TREE_COMPONENT_VALUE,       ///< `values` (one, or none).
  GTEXT_CSS_TREE_COMPONENT_VALUE_LIST,  ///< `values`.
  GTEXT_CSS_TREE_COMMA_LIST             ///< `lists`.
} GTEXT_CSS_Tree_Kind;

/**
 * @brief What an entry point that returns one thing found instead of it.
 *
 * "Parse a rule", "parse a declaration" and "parse a component value" return
 * either their construct or a syntax error. A syntax error is an answer, not a
 * failure: the call returns GTEXT_CSS_OK, the tree holds nothing, and this
 * says why.
 */
typedef enum {
  GTEXT_CSS_SYNTAX_NONE = 0,     ///< No syntax error: the tree holds the construct.
  GTEXT_CSS_SYNTAX_EMPTY,        ///< The input was empty, or only white space.
  GTEXT_CSS_SYNTAX_INVALID,      ///< No construct could be read (a qualified
                                 ///< rule with no block, a declaration with no
                                 ///< colon, ...).
  GTEXT_CSS_SYNTAX_EXTRA_INPUT   ///< The construct was followed by more than white space.
} GTEXT_CSS_Syntax_Error;

/**
 * @struct GTEXT_CSS_Tree
 * @brief What a parse built. Read only; freed with gtext_css_tree_free().
 *
 * `kind` says which entry point made it and so which fields hold the result;
 * the others are empty. Every pointer in the tree is into memory the tree
 * owns.
 */
typedef struct {
  GTEXT_CSS_Tree_Kind kind;               ///< Which entry point.
  GTEXT_CSS_Syntax_Error syntax_error;    ///< For RULE, DECLARATION and COMPONENT_VALUE.
  const char * location;                  ///< A stylesheet's location
                                          ///< (GTEXT_CSS_Parse_Options::location), or NULL.
  const GTEXT_CSS_Rule * rules;           ///< The rules.
  size_t rule_count;                      ///< Their number.
  const GTEXT_CSS_Declaration * declarations; ///< The declarations.
  size_t declaration_count;               ///< Their number.
  const GTEXT_CSS_Block_Item * items;     ///< A block's contents.
  size_t item_count;                      ///< Their number.
  const GTEXT_CSS_Component_Value * values; ///< The component values.
  size_t value_count;                     ///< Their number.
  const GTEXT_CSS_Value_List * lists;     ///< The groups of a comma-separated list.
  size_t list_count;                      ///< Their number.
  size_t node_count;                      ///< Rules, declarations and component
                                          ///< values in the tree (the highest id).
  struct GTEXT_CSS_Tree_Internal * internal; ///< Private.
} GTEXT_CSS_Tree;

/**
 * @brief Release a tree and everything in it.
 *
 * The tree struct itself lives in the memory this frees, so the pointer is
 * dead after the call, as is every pointer into the tree (values, strings,
 * rules); nothing in it, `kind` and the counts included, may be read.
 *
 * @param tree A tree a parse returned; NULL is ignored.
 */
GTEXT_API void gtext_css_tree_free(GTEXT_CSS_Tree * tree);

/**
 * @struct GTEXT_CSS_Parent
 * @brief The context a validity check is asked in.
 *
 * Every pointer is valid only for the duration of the callback.
 */
typedef struct {
  bool nested;                     ///< Inside a block (the standard's `nested`).
  bool top_level;                  ///< No rule encloses the construct (a
                                   ///< stylesheet's rules, or the input of an
                                   ///< entry point that parses a list).
  GTEXT_CSS_Rule_Kind kind;        ///< The enclosing rule's kind, when not top level.
  GTEXT_CSS_String name;           ///< The enclosing at-rule's name.
  const GTEXT_CSS_Component_Value * prelude; ///< The enclosing rule's prelude.
  size_t prelude_count;            ///< Its length.
  size_t depth;                    ///< How many rules enclose the construct.
} GTEXT_CSS_Parent;

/**
 * @struct GTEXT_CSS_Validity
 * @brief The standard's "valid in the current context", supplied by the
 *   layers that know grammars.
 *
 * CSS Syntax returns a rule or a declaration only "if it is valid in the
 * current context", and what is valid is defined by the specifications above
 * syntax (Selectors for a style rule's prelude, each property's grammar for a
 * declaration, each at-rule's definition). This layer knows none of them, so
 * by default everything is valid. A caller that knows can say otherwise, and
 * the parse then behaves as the standard does for an invalid construct: an
 * invalid declaration in a block is read again as a rule, an invalid nested
 * rule is an "invalid rule error" that ends the run of declarations before
 * it, and so on. A NULL callback accepts everything.
 *
 * The callbacks see a node that is complete (with its block, for a rule) but
 * whose `id` is not yet assigned (0). They must not keep the pointers.
 */
typedef struct {
  void * user; ///< Passed back unchanged.
  /** Whether a declaration is valid in its context. */
  bool (*declaration)(void * user, const GTEXT_CSS_Parent * parent,
      const GTEXT_CSS_Declaration * declaration);
  /** Whether a rule (qualified or at) is valid in its context. */
  bool (*rule)(void * user, const GTEXT_CSS_Parent * parent,
      const GTEXT_CSS_Rule * rule);
} GTEXT_CSS_Validity;

/**
 * @struct GTEXT_CSS_Parse_Options
 * @brief What a parse may do.
 *
 * Every limit has a default: a field left at 0 selects it, and 0 never means
 * "unlimited". A zeroed struct is therefore the same as
 * gtext_css_parse_options_default(). Fields are only ever appended, with a
 * zero default, so a program built against an older header keeps its
 * meaning.
 */
typedef struct {
  /** The allocator every allocation of the parse, of the tree and of the
   *  result goes through, or NULL for gtext_allocator_default(). The struct
   *  is copied; its `ctx` must outlive the tree and the result. */
  const GTEXT_Allocator * allocator;

  /** Longest input, in bytes. Default 64 MiB. */
  size_t max_input_bytes;

  /** Most tokens the tokenizer may produce (whitespace tokens included,
   *  comments not, the end-of-file marker not). Default 4,194,304. */
  size_t max_tokens;

  /** Deepest nesting of simple blocks and functions, a rule's block
   *  included: `a{b{c{}}}` is 3 deep, `f(g([x]))` is 3. Default 512. The
   *  parser never recurses, so this bounds memory, not the stack. */
  size_t max_depth;

  /** Most rules a parse may make: qualified rules, at-rules and nested
   *  declarations rules, counted as the standard's algorithms return them
   *  (so including those inside a block that is then dropped). Default
   *  1,048,576. */
  size_t max_rules;

  /** Most declarations a parse may make, counted the same way, except that a
   *  declaration the validity callback refuses is not one. Default
   *  4,194,304. */
  size_t max_declarations;

  /** Most parse errors a parse may raise; one more fails it with
   *  GTEXT_CSS_E_LIMIT_ERRORS. Default 1,048,576. */
  size_t max_total_errors;

  /** Most parse errors kept in the result; the rest are only counted
   *  (gtext_css_result_errors_dropped()). Default 1024. Not a failure.
   *
   *  This bounds the result, not the parse's memory: the errors the result
   *  keeps are the ones lowest in the input, so every error raised is held
   *  until the parse ends (up to `max_total_errors` of them, about 40 bytes
   *  each, and the same again transiently while they are sorted), unless
   *  `count_errors_only` is set or no result is asked for. To bound the
   *  memory, lower `max_total_errors`. */
  size_t max_errors;

  /** Keep no parse errors, only count them. */
  bool count_errors_only;

  /** Read the input as WTF-8: a surrogate code point encoded in three bytes
   *  (`ED A0 80` to `ED BF BF`) is one code point, which preprocessing then
   *  replaces with U+FFFD, as the standard's "filter code points" does with
   *  a surrogate. Without it those bytes are ill-formed UTF-8 and each
   *  maximal subpart becomes U+FFFD, as for any other ill-formed input. For a
   *  caller whose text came from a UTF-16 string that may hold a lone
   *  surrogate. Default false. */
  bool input_wtf8;

  /** A stylesheet's location (the standard's optional "url location"),
   *  NUL-terminated, copied into the tree; NULL for none. Read by
   *  gtext_css_parse_stylesheet() only. */
  const char * location;

  /** "Valid in the current context". All NULL (the default) accepts
   *  everything. */
  GTEXT_CSS_Validity validity;
} GTEXT_CSS_Parse_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return By value: allocator NULL, 64 MiB of input, 4,194,304 tokens, depth
 *   512, 1,048,576 rules, 4,194,304 declarations, 1,048,576 parse errors in
 *   all and 1024 of them kept, WTF-8 off, no location, no validity checks.
 */
GTEXT_API GTEXT_CSS_Parse_Options gtext_css_parse_options_default(void);

/**
 * @enum GTEXT_CSS_Encoding_Source
 * @brief Why a byte parse decoded the bytes as it did (CSS Syntax 3.2).
 */
typedef enum {
  GTEXT_CSS_ENCODING_SOURCE_NONE = 0,  ///< Nothing was decoded: the input was text.
  GTEXT_CSS_ENCODING_SOURCE_BOM,       ///< A byte order mark ("decode" gives it precedence over everything).
  GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,  ///< The encoding label the protocol gave (an HTTP `charset`).
  GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE, ///< A leading `@charset "...";` in the first 1024 bytes.
  GTEXT_CSS_ENCODING_SOURCE_ENVIRONMENT,  ///< The environment encoding the caller passed.
  GTEXT_CSS_ENCODING_SOURCE_DEFAULT,   ///< Nothing else applied: UTF-8.
  GTEXT_CSS_ENCODING_SOURCE_COUNT      ///< The number of sources; not a source.
} GTEXT_CSS_Encoding_Source;

/**
 * @brief The name of an encoding source ("bom", "protocol", "charset-rule",
 *   "environment", "default", "none").
 *
 * @param source Any value; one that is not a source returns "unknown".
 * @return A static string, different for every source.
 */
GTEXT_API const char * gtext_css_encoding_source_name(GTEXT_CSS_Encoding_Source source);

/**
 * @struct GTEXT_CSS_Result
 * @brief What a parse reports besides the tree.
 *
 * Filled by every parse function, whether or not it succeeded, and **freed
 * with gtext_css_result_free()** (it holds the parse errors). A parse
 * function overwrites the struct without freeing what it held, so zero a
 * result before its first use and free it before reusing it. NULL is always
 * allowed for the result: the parse then keeps no errors.
 */
typedef struct {
  GTEXT_CSS_Status status;   ///< What the call returned.
  const char * message;      ///< A static description of the status. Never NULL after a call.
  GTEXT_CSS_Span failure;    ///< For a limit: where it was reached (the byte,
                             ///< line and column of the construct that went
                             ///< past it). Zero otherwise.
  size_t input_bytes;        ///< The input's length in bytes, whatever the status (for
                             ///< GTEXT_CSS_E_LIMIT_INPUT too, where none of it was read).
  size_t token_count;        ///< Tokens the tokenizer produced (for a failed
                             ///< parse, those produced before it failed).
  size_t rule_count;         ///< Rules made (see `max_rules`).
  size_t declaration_count;  ///< Declarations made (see `max_declarations`).
  struct GTEXT_CSS_Result_Internal * internal; ///< Private: the error list.
  GTEXT_Encoding encoding;   ///< A byte parse: the encoding the bytes were decoded as
                             ///< (GTEXT_ENCODING_UTF_8 also for the UTF-8 that a byte
                             ///< order mark chose). GTEXT_ENCODING_NONE for text input.
  GTEXT_CSS_Encoding_Source encoding_source; ///< A byte parse: why that one.
  size_t bom_length;         ///< A byte parse: bytes of byte order mark at the start,
                             ///< which no token covers but every offset counts.
} GTEXT_CSS_Result;

/**
 * @brief Release what a result holds, and reset it.
 *
 * Safe on NULL, twice, and on a zeroed result. The struct is the caller's
 * and stays valid, zeroed; what it held (the error list) is freed, so the
 * count and the errors read from it before are the caller's copies only.
 *
 * @param result The result.
 */
GTEXT_API void gtext_css_result_free(GTEXT_CSS_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CORE_H
