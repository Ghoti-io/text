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
 * What the files of the Value Definition Syntax engine share: the node of a
 * grammar, the parsed definition and the linked grammar, the tables of native
 * types, and the matcher's entry points.
 *
 * css_grammar_parse.c reads the text of a definition (no recursion: an explicit
 * stack of open groups); css_grammar_link.c resolves references, checks for left
 * recursion and numbers the nodes; css_grammar_match.c holds the recognizer (the
 * memoized sets of end positions of a node at a position), the extraction of the
 * typed value from it, and the match entry points.
 *
 * Reference: CSS Values and Units Module Level 4, Editor's Draft, 2 (Value
 * Definition Syntax), https://drafts.csswg.org/css-values-4/ (the draft's own
 * text, and Level 5 for the additions, is quoted where it decides something).
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_GRAMMAR_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_GRAMMAR_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// What a node of a grammar is.
typedef enum {
  GN_SEQ = 0,  ///< Juxtaposition: `kids`, in order.
  GN_ALT,      ///< `|`: `kids`, in written order.
  GN_AND,      ///< `&&`: `kids` (at most 64).
  GN_OR,       ///< `||`: `kids` (at most 64).
  GN_MULT,     ///< A multiplier: one kid, `min`, `max` (GN_INF for none), GNF_COMMA.
  GN_REQ,      ///< `!`: one kid, which must match at least one component value.
  GN_KEYWORD,  ///< A keyword: `str_off`/`str_len`; `atom` once linked.
  GN_LITERAL,  ///< A literal character: `lit`.
  GN_NATIVE,   ///< A native type: `native` (NT_*), with a range if GNF_RANGE.
  GN_REF,      ///< A reference to a type or a property, until linked: the name in `str_*`, `native` its GTEXT_CSS_Ref_Kind, and a range if GNF_RANGE.
  GN_FUNC,     ///< `name( kid )`: `str_*` the name, no kid for an empty argument list.
  GN_BLOCK     ///< `{ kid }`, `( kid )` or `'[' kid ']'`: `native` the opening token type.
} gn_kind;

/// The value a node-making function returns when it failed (node 0 is a node).
#define GN_NONE UINT32_MAX

/// The most a limit on bytes or nodes can be: an offset or an index into a definition is 32 bits.
#define GN_MAX_BYTES ((size_t)1 << 30)
#define GN_MAX_NODES ((size_t)1 << 28)
/// The most memo entries a limit can be (an entry's place in the pool is 32 bits).
#define GN_MAX_MEMO ((size_t)1 << 30)

/// The largest count of a multiplier that has none.
#define GN_INF UINT32_MAX

/// A multiplier's counts are at most this when they are written.
#define GN_MAX_COUNT 65535u

/// Most members of a `&&` or `||` group.
#define GN_MAX_GROUP 64u

/// Node flags.
#define GNF_COMMA 0x01u   ///< A MULT: the repetitions are separated by commas (`#`).
#define GNF_RANGE 0x02u   ///< A NATIVE or REF: `lo` and `hi` are a range.
#define GNF_INHERITED 0x04u ///< The range was not written on this node but on a type that contains it (it may be dropped where it does not apply).
#define GNF_GROUP 0x08u   ///< Written as a `[ ]` group.

/// What the bounds of a range were written in (gnode::rclass): nothing but plain numbers, percent, or a unit type + 2.
#define RC_NONE 0
#define RC_PERCENT 1
#define RC_UNIT_BASE 2

/// The native types.
typedef enum {
  NT_INTEGER = 0, NT_NUMBER, NT_PERCENTAGE, NT_DIMENSION, NT_LENGTH, NT_ANGLE, NT_TIME,
  NT_FREQUENCY, NT_RESOLUTION, NT_FLEX, NT_RATIO, NT_ZERO, NT_STRING, NT_URL, NT_IDENT,
  NT_CUSTOM_IDENT, NT_DASHED_IDENT, NT_ANY_VALUE, NT_DECL_VALUE, NT_URANGE, NT_ANB,
  NT_DECL_LIST, NT_RULE_LIST,
  NT_TOK_IDENT, NT_TOK_FUNCTION, NT_TOK_AT_KEYWORD, NT_TOK_HASH, NT_TOK_STRING,
  NT_TOK_BAD_STRING, NT_TOK_URL, NT_TOK_BAD_URL, NT_TOK_DELIM, NT_TOK_NUMBER,
  NT_TOK_PERCENTAGE, NT_TOK_DIMENSION, NT_TOK_WS, NT_TOK_CDO, NT_TOK_CDC, NT_TOK_COLON,
  NT_TOK_SEMICOLON, NT_TOK_COMMA, NT_TOK_OPEN_SQUARE, NT_TOK_CLOSE_SQUARE, NT_TOK_OPEN_PAREN,
  NT_TOK_CLOSE_PAREN, NT_TOK_OPEN_CURLY, NT_TOK_CLOSE_CURLY,
  NT_COUNT
} gn_native;

/// A node of a grammar.
typedef struct {
  uint8_t kind;      ///< gn_kind.
  uint8_t native;    ///< See gn_kind.
  uint8_t flags;     ///< GNF_*.
  uint8_t sep;       ///< SEQ: 0 plain, 1 the kids are comma-separated segments, 2 slash-separated segments.
  uint8_t rclass;    ///< A range's bounds: RC_*.
  uint32_t kids;     ///< Index of the first kid in the kid array.
  uint32_t nkids;    ///< How many.
  uint32_t min;      ///< MULT: fewest repetitions.
  uint32_t max;      ///< MULT: most (GN_INF).
  uint32_t target;   ///< REF, once linked: the node it stands for.
  uint32_t str_off;  ///< Keyword, function and reference names: offset in the text pool.
  uint32_t str_len;  ///< Their length.
  uint32_t src_off;  ///< The node's own source text: offset in the text pool.
  uint32_t src_len;  ///< Its length.
  uint32_t atom;     ///< KEYWORD, once linked.
  uint32_t lit;      ///< LITERAL: the code point.
  double lo;         ///< The range's lower bound, in the canonical unit.
  double hi;         ///< Its upper bound.
} gnode;

/// A parsed definition.
struct GTEXT_CSS_Definition {
  GTEXT_Allocator alloc;   ///< Everything is allocated through this.
  char * text;             ///< A copy of the definition's text (NUL-terminated).
  size_t text_len;         ///< Its length.
  gnode * nodes;           ///< The nodes.
  size_t nnodes;           ///< How many.
  size_t ncap;             ///< Allocated.
  uint32_t * kids;         ///< The kid array.
  size_t nkids;            ///< In use.
  size_t kcap;             ///< Allocated.
  uint32_t root;           ///< The root node.
  size_t max_nodes;        ///< The node limit it was made under.
};

/// A linked grammar.
struct GTEXT_CSS_Compiled {
  GTEXT_Allocator alloc;   ///< Everything is allocated through this.
  char * text;             ///< The texts of every definition it holds, one after another (NUL after each).
  size_t text_len;         ///< Its length.
  gnode * nodes;           ///< The nodes; no REF is reachable from `root`.
  size_t nnodes;           ///< How many.
  uint32_t * kids;         ///< The kid array.
  size_t nkids;            ///< In use.
  uint32_t root;           ///< Where matching starts.
  GTEXT_CSS_Atom_Table * atoms; ///< The keywords.
};

/// Whether the bounds of a range written in `rclass` fit native type `type`.
bool css_grammar_range_fits(gn_native type, uint8_t rclass);

/// Where the native type of this name is, or NT_COUNT.
gn_native css_grammar_native_lookup(const char * name, size_t length);

/// Whether a native type is numeric (a range makes sense on it).
bool css_grammar_native_numeric(gn_native type);

/// Whether a native type may match without consuming a component value.
bool css_grammar_native_nullable(gn_native type);

/// Set a grammar result.
void css_grammar_result(GTEXT_CSS_Grammar_Result * result, GTEXT_CSS_Status status, size_t offset,
    const char * name, size_t name_length, size_t nodes, size_t work);

/// The static message of a grammar status.
const char * css_grammar_message(GTEXT_CSS_Status status);

/// The allocator of options (the default if none).
GTEXT_Allocator css_grammar_allocator(const GTEXT_CSS_Grammar_Options * options);

/// The limits of options with the defaults applied.
void css_grammar_limits(const GTEXT_CSS_Grammar_Options * options, size_t * bytes, size_t * depth, size_t * nodes);

/// Grow `*p` (of `*cap` elements of `elem` bytes) to hold at least `need`; false when the allocator refused.
bool css_grammar_grow(const GTEXT_Allocator * alloc, void ** p, size_t * cap, size_t need, size_t elem);


#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_GRAMMAR_INTERNAL_H
