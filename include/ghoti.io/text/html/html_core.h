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
 * @file html_core.h
 * @brief Status codes, options and the result of the HTML parser.
 *
 * The parser reads HTML as the WHATWG HTML Living Standard's parsing chapter
 * defines it (tokenization and tree construction, with the fragment parsing
 * algorithm) and builds a @ref GTEXT_Markup_Document in HTML mode; the tree
 * itself is described in markup.h. **HTML parsing does not fail on
 * content**: every input has a tree. A parse fails only for an argument, a
 * limit, memory, or an encoding this release cannot read; the mistakes in the
 * document are parse errors, which are reported (html_error.h) and are not
 * failures.
 *
 * It has the shape of xml_core.h: an options struct with a `_default()`, a
 * status enum, and `_free()` for what the result holds. The differences are
 * listed in the manual (documentation/formats/html.md): the result carries a
 * list of errors, the document mode and the encoding facts as well as the
 * failure, and a result is freed.
 */

#ifndef GHOTI_IO_GTEXT_HTML_HTML_CORE_H
#define GHOTI_IO_GTEXT_HTML_HTML_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_core.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_HTML_Status
 * @brief What parsing HTML reports.
 *
 * The first block is not about the document. The limits are each their own
 * status, because a document that is refused for being too deep is not a
 * document that is malformed (HTML has no malformed documents). There is no
 * status for a syntax error.
 */
typedef enum {
  GTEXT_HTML_OK = 0,    ///< Success.
  GTEXT_HTML_E_INVALID, ///< An argument was unusable (NULL output, NULL data
                        ///< with a length, an unusable fragment context), or
                        ///< a file could not be opened or read (the result's
                        ///< message says which).
  GTEXT_HTML_E_OOM,     ///< Out of memory. No tree, and nothing leaked.
  GTEXT_HTML_E_STATE,   ///< Incremental parser only: the call does not fit the
                        ///< parser's state - input after gtext_html_parser_finish(),
                        ///< or a second finish whose document was already
                        ///< handed out, or the wrong finish function for the
                        ///< kind of parser.
  GTEXT_HTML_E_UNSUPPORTED_ENCODING, ///< An encoding label (`transport_encoding`
                        ///< or `default_encoding`) that names anything but
                        ///< UTF-8. This release reads UTF-8 only; the
                        ///< other encodings, and the `<meta>` prescan, arrive
                        ///< with phases 3h and 3i. Not a verdict on the
                        ///< document, which is never examined.
  GTEXT_HTML_E_INTERNAL, ///< A defect in this library, never content: a rule
                        ///< of the tree builder did not terminate. Reported
                        ///< rather than looping; please report it.

  GTEXT_HTML_E_LIMIT_INPUT,      ///< The input is longer than `max_input_bytes`.
  GTEXT_HTML_E_LIMIT_DEPTH,      ///< More than `max_open_elements` elements
                                 ///< open at once (the stack of open
                                 ///< elements, `html` and `body` included).
  GTEXT_HTML_E_LIMIT_NODES,      ///< More than `max_nodes` nodes in the tree.
  GTEXT_HTML_E_LIMIT_NAME,       ///< A tag, attribute or DOCTYPE name, or a
                                 ///< processing instruction target, longer
                                 ///< than `max_name_bytes`.
  GTEXT_HTML_E_LIMIT_VALUE,      ///< An attribute value or a DOCTYPE
                                 ///< identifier longer than
                                 ///< `max_attribute_bytes`.
  GTEXT_HTML_E_LIMIT_COMMENT,    ///< A comment, or a processing instruction's
                                 ///< data, longer than `max_comment_bytes`.
  GTEXT_HTML_E_LIMIT_TEXT,       ///< A run of characters longer than
                                 ///< `max_text_bytes`.
  GTEXT_HTML_E_LIMIT_ATTRIBUTES  ///< More than `max_attributes` on one tag.
} GTEXT_HTML_Status;

/**
 * @brief A static, human-readable name for a status.
 *
 * Every status has a name of its own, and no two are alike.
 *
 * @param status Any value; an unknown one returns "unknown status".
 * @return A string that is never freed.
 */
GTEXT_API const char * gtext_html_status_string(GTEXT_HTML_Status status);

/**
 * @enum GTEXT_HTML_Encoding_Certainty
 * @brief How much a caller-supplied encoding label counts for.
 *
 * The standard's "certain" and "tentative". Both are accepted and, in this
 * release, both mean the same thing for the one encoding that is read; the
 * distinction is part of the options now so that the structure does not
 * change when other encodings arrive (a tentative label can be overridden by
 * a `<meta>` declaration, a certain one cannot).
 */
typedef enum {
  GTEXT_HTML_CERTAIN = 0, ///< As a transport header's: nothing in the
                          ///< document overrides it. The default.
  GTEXT_HTML_TENTATIVE    ///< A guess that the document may override.
} GTEXT_HTML_Encoding_Certainty;

/**
 * @struct GTEXT_HTML_Parse_Options
 * @brief What a parse may do.
 *
 * Every limit has a default from the first release: a field left at 0 selects
 * it, and 0 never means "unlimited" (SIZE_MAX is as close as the type allows).
 * Zeroed is therefore the same as gtext_html_parse_options_default(). New
 * fields are only ever appended, with a zero default, so that a program built
 * against an older header keeps its meaning.
 */
typedef struct {
  /**
   * The allocator every allocation of the parse, of the resulting document
   * and of the result goes through, or NULL for gtext_allocator_default().
   * The struct is copied; the `ctx` it holds must outlive the document and
   * the result.
   */
  const GTEXT_Allocator * allocator;

  /** Longest input, in bytes (a byte order mark included). Default 64 MiB. For
   *  an incremental parser it counts everything fed. */
  size_t max_input_bytes;

  /**
   * Most elements open at once (the standard's stack of open elements, `html`
   * and `body` included), which is the parser's depth limit. Default 512.
   * Nesting is where HTML parsing costs more than a constant per byte, so this
   * is also what bounds the work.
   */
  size_t max_open_elements;

  /** Most nodes in the document, the document node included. Default
   *  10,000,000. */
  size_t max_nodes;

  /** Longest tag, attribute or DOCTYPE name, or processing instruction
   *  target, in bytes. Default 8192. */
  size_t max_name_bytes;

  /** Longest attribute value or DOCTYPE identifier, in bytes, after
   *  character references are decoded. Default 16 MiB. */
  size_t max_attribute_bytes;

  /** Longest comment, or processing instruction's data, in bytes.
   *  Default 16 MiB. */
  size_t max_comment_bytes;

  /**
   * Longest run of characters between tags, in bytes. Default 64 MiB. A run is
   * held in memory until something that is not a character ends it, so this
   * is a limit on a failure and not on a split: a longer run fails the
   * parse with GTEXT_HTML_E_LIMIT_TEXT.
   */
  size_t max_text_bytes;

  /** Most attributes on one tag; a tag with more fails the parse. Default
   *  1024. */
  size_t max_attributes;

  /**
   * Most parse errors kept; the rest are counted (see
   * gtext_html_result_errors_dropped()). Default 1024. Not a failure, and not
   * a limit on how much of the document is read.
   */
  size_t max_errors;

  /** Keep no parse errors, only count them (`max_errors` cannot say 0: 0 there
   *  is the default). The count is in gtext_html_result_errors_dropped(). */
  bool count_errors_only;

  /**
   * The standard's scripting flag: whether the document is parsed for a user
   * agent that runs scripts. It changes how `noscript` parses: with the flag
   * its contents are raw text, without it they are elements. Nothing is ever
   * executed. Default false. For a fragment it also applies to a `noscript`
   * context element.
   */
  bool scripting;

  /**
   * The encoding the bytes came with from outside - the `charset` of a
   * `Content-Type` header - as a label (case-insensitive, surrounding white
   * space ignored), or NULL for none. **Only UTF-8 labels are accepted in this
   * release** (`utf-8`, `utf8`, `unicode-1-1-utf-8`, `unicode11utf8`,
   * `unicode20utf8`, `x-unicode20utf8`); any other string, the empty one
   * included, is GTEXT_HTML_E_UNSUPPORTED_ENCODING. Phase 3h reads the others.
   */
  const char * transport_encoding;

  /** How much `transport_encoding` counts for. Ignored when it is NULL. */
  GTEXT_HTML_Encoding_Certainty transport_encoding_certainty;

  /**
   * The encoding to assume when nothing else says (no byte order mark, no
   * transport label, no declaration), as a label, or NULL for UTF-8. The same
   * labels are accepted as for `transport_encoding`.
   */
  const char * default_encoding;

  /**
   * Skip the `<meta>` prescan that looks for a declared encoding. Accepted
   * and recorded; this release has no prescan to skip (phase 3h), so it
   * changes nothing yet.
   */
  bool ignore_meta;
} GTEXT_HTML_Parse_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return The defaults, by value: allocator NULL, 64 MiB of input, 512 open
 *   elements, ten million nodes, 8192 bytes of name, 16 MiB of attribute value
 *   and of comment, 64 MiB of text, 1024 attributes, 1024 kept errors, the
 *   scripting flag off, and no encoding labels.
 */
GTEXT_API GTEXT_HTML_Parse_Options gtext_html_parse_options_default(void);

/** @brief The namespace of a fragment's context element. */
typedef enum {
  GTEXT_HTML_NS_HTML = 0, ///< The HTML namespace.
  GTEXT_HTML_NS_SVG,      ///< The SVG namespace.
  GTEXT_HTML_NS_MATHML    ///< The MathML namespace.
} GTEXT_HTML_Namespace;

/**
 * @struct GTEXT_HTML_Context_Attribute
 * @brief One attribute of a fragment's context element.
 */
typedef struct {
  const char * name;  ///< NUL-terminated. Not NULL.
  const char * value; ///< NUL-terminated. Not NULL (use "" for none).
} GTEXT_HTML_Context_Attribute;

/**
 * @struct GTEXT_HTML_Fragment_Context
 * @brief The context element of the fragment parsing algorithm (`innerHTML`).
 *
 * Described by value: no tree node is needed and none is touched. Zero it,
 * then fill in what applies. Everything is borrowed for the duration of the
 * call and not kept.
 *
 * The algorithm reads the element's namespace and name (they choose the
 * tokenizer's starting state and the insertion mode, so that `td` parses
 * rows' contents, `select` options and `textarea` as text), its attributes
 * only for a MathML `annotation-xml` (whose `encoding` makes it an HTML
 * integration point), whether it is or has a `form` ancestor, and the mode of
 * its document.
 */
typedef struct {
  GTEXT_HTML_Namespace ns;  ///< The element's namespace.
  const char * local_name;  ///< Its local name, as the element has it (exact
                            ///< case: `foreignObject`, not `foreignobject`).
                            ///< NUL-terminated, not empty, not NULL.
  const GTEXT_HTML_Context_Attribute * attributes; ///< May be NULL when
                            ///< `attribute_count` is 0.
  size_t attribute_count;   ///< Their number.
  bool form_ancestor;       ///< The element is a `form` or has a `form`
                            ///< ancestor.
  GTEXT_Markup_Mode quirks; ///< The mode of the element's document.
} GTEXT_HTML_Fragment_Context;

/**
 * @struct GTEXT_HTML_Result
 * @brief What a parse reports besides the tree.
 *
 * Filled by every parse function, whether or not it succeeded, and **freed
 * with gtext_html_result_free()** (it holds the list of parse errors). The
 * document and the result are independent objects: freeing either does not
 * touch the other, in any order. The fields before `internal` may be read
 * directly; the errors are read through the accessors in html_error.h.
 *
 * **A parse function overwrites the struct without freeing what it held** (it cannot tell a live
 * result from uninitialized memory), as the XML error struct is overwritten. So: zero a result before
 * its first use, and call gtext_html_result_free() before passing it to another call, or the error
 * list of the first is leaked.
 * Passing NULL for the result is always allowed: the parse then keeps no
 * errors and only counts them.
 */
typedef struct {
  GTEXT_HTML_Status status; ///< What the call returned.
  const char * message;     ///< A static, human-readable description of the
                            ///< status, and for a file that could not be read
                            ///< which way ("No such file or directory", ...).
                            ///< Never freed. Never NULL after a call.
  GTEXT_Markup_Mode mode;   ///< The document mode (no-quirks, limited quirks,
                            ///< quirks), as it is also set on the document;
                            ///< for a fragment the mode of the context's
                            ///< document. NO_QUIRKS unless the parse succeeded.
  const char * encoding;    ///< The encoding the bytes were read as: "UTF-8". Set by every call that got as far
                            ///< as making a parser, failures after that included (the labels were accepted, and
                            ///< UTF-8 is what is read); NULL only for a failure before there was a parser
                            ///< (an unusable argument, an unsupported label, OOM making it, an unreadable
                            ///< file). Static; never freed.
  GTEXT_Markup_Encoding_Source encoding_source; ///< How it was decided:
                            ///< GTEXT_MARKUP_ENCODING_BOM (a UTF-8 byte order
                            ///< mark, which is skipped, found in the bytes fed so far), _CALLER (the
                            ///< `transport_encoding` label), _DECLARATION
                            ///< (a `<meta>`; never yet) or _DEFAULT. UNKNOWN
                            ///< with `encoding` NULL. For a fragment this is reported but not recorded on
                            ///< the fragment's scratch document (it has no encoding of its own).
  size_t input_bytes;       ///< Bytes of input accepted, a byte order mark
                            ///< included. For a failed parse, the bytes accepted before the one that failed it:
                            ///< a whole input refused for its length is 0, an input fed in pieces counts the
                            ///< pieces that were taken.
  size_t node_count;        ///< Nodes in the document (the document node
                            ///< included); 0 when there is none.

  struct GTEXT_HTML_Result_Internal * internal; ///< Private: the error list.
} GTEXT_HTML_Result;

/**
 * @brief Release what a result holds (the list of parse errors).
 *
 * Does not touch the document the parse returned. Resets the struct so that a
 * stale result cannot be read again, and so it is safe to call twice. Safe on
 * NULL and on a result that was never filled, if it was zeroed.
 *
 * @param result The result to free.
 */
GTEXT_API void gtext_html_result_free(GTEXT_HTML_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_HTML_HTML_CORE_H
