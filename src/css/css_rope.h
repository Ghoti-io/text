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
 * Token ropes: the values that substitution makes. Private.
 *
 * Substituting `var()` into a value copies a token stream. Done with bytes, a few lines of
 * `--b: var(--a) var(--a); --c: var(--b) var(--b)` make a value that doubles with every line (CSS Values 5, "Safely Handling Overly-Long
 * Substitution"). A rope is the immutable tree of the concatenations, with the two halves of a concatenation allowed to be the same
 * node: memory stays linear in the text that was written, and the length the value would have is a number, saturating, kept beside it
 * (`tokens`, `bytes`). A rope is only written out when something needs its bytes, and the caller has refused a rope whose `tokens`
 * pass its budget by then.
 *
 * A rope knows the first and the last token it holds, as a class, so that joining two ropes can tell when the bytes of the join would
 * read as another token (`20` then `px`, an identifier then a number) and puts a comment between them, as CSS Syntax 9
 * ("Serialization") does for a list of tokens. It knows whether any of it came from an `attr()`, which Values 5 8.7.2 keeps out of
 * URLs ("attr()-tainted").
 *
 * Nothing here recurses. Releasing a rope is a walk with a worklist threaded through the nodes being freed; writing one is a walk with a
 * stack that grows as the rope is deep, not as it is long.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_ROPE_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_ROPE_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// What kind of token begins or ends a rope, as far as joining is concerned (CSS Syntax 9, "Serialization").
typedef enum {
  CSS_TC_NONE = 0, ///< Nothing (an empty rope).
  CSS_TC_SAFE,     ///< White space, a comment, a string, punctuation, a bracket: nothing glues to it or to what it ends.
  CSS_TC_IDENT,
  CSS_TC_FUNC,     ///< A function token (as a first token only: what a function ends with is a parenthesis).
  CSS_TC_ATKW,
  CSS_TC_HASH,
  CSS_TC_DIM,
  CSS_TC_NUM,
  CSS_TC_PCT,
  CSS_TC_URL,
  CSS_TC_BADURL,
  CSS_TC_CDC,
  CSS_TC_LPAREN,   ///< `(` (a parenthesis block, as a first token only).
  CSS_TC_D_HASH,   ///< The delimiters that the serialization rules name.
  CSS_TC_D_MINUS,
  CSS_TC_D_AT,
  CSS_TC_D_DOT,
  CSS_TC_D_PLUS,
  CSS_TC_D_SLASH,
  CSS_TC_D_PCT,
  CSS_TC_D_STAR
} css_tokclass;

/// Whether bytes that end with a token of class `last` and are followed by bytes that begin with one of class `first` would read back as
/// other tokens, so that a comment has to go between them.
bool css_tokclass_needs_comment(css_tokclass last, css_tokclass first);

/// The class of the first token of a component value (a function's is CSS_TC_FUNC, a `(` block's CSS_TC_LPAREN) and of its last (a function and a block end with a
/// bracket: CSS_TC_SAFE).
css_tokclass css_cv_first_class(const GTEXT_CSS_Component_Value * cv);
css_tokclass css_cv_last_class(const GTEXT_CSS_Component_Value * cv);

/// The number of tokens of component values: one for each token, and for a function or a block its opening and closing token and what is inside.
/// Saturating. GTEXT_CSS_OK or GTEXT_CSS_E_OOM (the walk's stack).
GTEXT_CSS_Status css_cv_count_tokens(const GTEXT_Allocator * alloc, const GTEXT_CSS_Component_Value * values, size_t count, uint64_t * tokens);

/// A rope (the struct is private). NULL is not a rope: the empty value is a rope of no bytes.
typedef struct css_rope css_rope;

/// A saturated count: this many, or more.
#define CSS_ROPE_MAX UINT64_MAX

/// The most a count of tokens or bytes is held at: sums that pass it stay there.
uint64_t css_sat_add(uint64_t a, uint64_t b);

/// A rope of these bytes (copied). `first` and `last` are the classes of its first and last token, `tokens` its number of tokens, `tainted` whether it came from
/// an `attr()`. NULL when memory ran out. A rope of no bytes has class NONE at both ends.
css_rope * css_rope_leaf(const GTEXT_Allocator * alloc, const char * bytes, size_t length, css_tokclass first, css_tokclass last, uint64_t tokens,
    bool tainted);

/// The two ropes one after the other, with `comment` (a leaf holding a comment; may be NULL, then one is made) between them where their bytes would join into
/// another token. Does not take the references of `a` and `b`; the result holds its own. An empty rope on either side is the other. NULL when memory ran out.
css_rope * css_rope_cat(const GTEXT_Allocator * alloc, css_rope * a, css_rope * b, css_rope * comment);

/// The same rope with everything in it marked tainted. Returns `r` retained if it is empty or already wholly tainted. NULL when memory ran out.
css_rope * css_rope_taint(const GTEXT_Allocator * alloc, css_rope * r);

css_rope * css_rope_retain(css_rope * r);

/// One owner fewer; the last frees the nodes nobody else holds. NULL is ignored.
void css_rope_release(css_rope * r);

/// The number of tokens the rope holds (saturating), counting a shared part once for each time it is in the rope.
uint64_t css_rope_tokens(const css_rope * r);

/// The number of bytes it would be written as (saturating).
uint64_t css_rope_bytes(const css_rope * r);

/// Whether any part of it is attr()-tainted.
bool css_rope_has_taint(const css_rope * r);

/// The classes of its first and last token.
css_tokclass css_rope_first(const css_rope * r);
css_tokclass css_rope_last(const css_rope * r);

/// A range of bytes of a written rope.
typedef struct {
  size_t begin;
  size_t end;
} css_range;

/// Write the rope as a new NUL-terminated buffer (release it with the allocator), refusing one longer than `max_bytes` (GTEXT_CSS_E_LIMIT_OUTPUT). The tainted
/// ranges, merged and in order, go in `*taint` (a new array, `*ntaint` of them; NULL when there is none or `taint` is NULL). `work` (may be NULL) is increased by the
/// nodes visited. GTEXT_CSS_OK, GTEXT_CSS_E_OOM or GTEXT_CSS_E_LIMIT_OUTPUT.
GTEXT_CSS_Status css_rope_flatten(const GTEXT_Allocator * alloc, const css_rope * r, size_t max_bytes, char ** out, size_t * length, css_range ** taint,
    size_t * ntaint, size_t * work);

/// Hand the bytes of the rope to a callback in pieces, in order. The callback returns false to stop (the status is then GTEXT_CSS_E_WRITE).
typedef bool (*css_rope_sink)(void * user, const char * bytes, size_t length);
GTEXT_CSS_Status css_rope_write(const GTEXT_Allocator * alloc, const css_rope * r, css_rope_sink sink, void * user, size_t * work);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_ROPE_H
