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
 * @file html_serialize.h
 * @brief Write a markup tree as HTML: the standard's fragment serialization
 *   algorithm, to a sink, a buffer or a file, or token by token.
 *
 * The default output is the WHATWG HTML Living Standard's "serializing HTML
 * fragments" algorithm, as served on 2026-10-12 (the manual quotes it): the
 * start tag with the element's name and its attributes in tree order, each
 * value in double quotes with `&`, U+00A0, `<`, `>` and the double quote escaped; no end tag
 * for a void element, and nothing written for its children; text escaped
 * (`&`, U+00A0, `<`, `>`) except when the parent is `style`, `script`, `xmp`,
 * `iframe`, `noembed`, `noframes` or `plaintext` (or `noscript` with the
 * scripting flag), where it is written as it is; a template's contents in
 * place of its children; `<!--data-->`, `<?target data?>` and
 * `<!DOCTYPE name>`. **Nothing is added and nothing is removed**: the output
 * is a function of the tree.
 *
 * ## Serialization does not fail on content
 *
 * HTML has a spelling for every tree. There is no refusal for a comment that
 * contains `-->`, a `script` that holds `</script>`, or a void element with
 * children: the standard writes them as they are and says that the output
 * "will not return the original tree structure". The statuses here are for
 * an argument, an option combination, a limit, memory, a sink that failed, and
 * an encoding this release cannot write. Characters are never validated:
 * control characters and noncharacters are written as they are.
 *
 * Because nothing is refused there is **no first pass**: the sink receives
 * bytes as the walk produces them, so a sink that is given a failure status
 * (`E_LIMIT_OUTPUT`, `E_LIMIT_DEPTH`, `E_WRITE`, `E_OOM`) has received a
 * prefix of the document. The XML writer checks the whole document first and
 * writes nothing on a refusal; this cannot, and does not promise it. (The one
 * tree refused is a document type identifier with both kinds of quote in it,
 * and only with `doctype_identifiers`.)
 *
 * ## Round trips
 *
 * `parse(serialize(tree))` is **not** guaranteed to give the tree back, for
 * any tree, and the standard says so itself: a `p` that contains a `ul`, an
 * `a` in an `a`, a `form` in a `form`, a `textarea` with a comment in it, a
 * `script` containing `</script>`, a comment containing `-->`, and the line
 * feed that `pre`, `textarea` and `listing` lose at the start of their text
 * (this serializer does not add one, because the standard does not) are not
 * read back as written. For a tree the HTML parser itself made the round trip
 * is stable for conforming input: `serialize(parse(serialize(t)))` is
 * `serialize(t)`; the manual says what was measured and which known cases are
 * not stable. The HTML syntax's optional tags (omit_optional_tags) assume a
 * conforming document and can change the tree of one that is not.
 *
 * ## Options
 *
 * The options are a superset of the standard, so that html5lib's serializer
 * vectors can be scored: zeroed is the standard's output exactly, and every
 * other option is off. They are html5lib's names (the manual's table says which
 * vector defines each). An unknown combination is GTEXT_HTML_E_OPTIONS.
 *
 * ## Three forms, one engine
 *
 * - gtext_html_serialize() and its buffer and file forms walk a tree
 *   (iteratively: any depth, within max_depth) and are the way to write a
 *   document, a fragment or a subtree.
 * - GTEXT_HTML_Serializer is the token-stream core the walker drives:
 *   start_tag, end_tag, empty_tag, characters, comment, doctype and
 *   processing_instruction, then finish. It keeps a one-token lookahead, which
 *   is what lets `</li>` be left out when the next token is `<li>`. Use it to
 *   write HTML from something that is not a markup tree.
 *
 * ## Threads
 *
 * Independent calls share nothing. The tree must not be modified, from
 * another thread or from the sink, while it is being written. A serializer
 * is not synchronized: use one from one thread at a time.
 */

#ifndef GHOTI_IO_GTEXT_HTML_HTML_SERIALIZE_H
#define GHOTI_IO_GTEXT_HTML_HTML_SERIALIZE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/html/html_core.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_HTML_Serialize_Scope
 * @brief What of the node is written.
 */
typedef enum {
  /** The node's children, as `innerHTML` is: the standard's algorithm. For a
   *  document or a fragment, its children; for a template, its contents; for
   *  a void element, nothing. The default. */
  GTEXT_HTML_SERIALIZE_CHILDREN = 0,
  /** The node itself and then its children, as `outerHTML` is. For a document
   *  or a fragment it is the same as CHILDREN; for any other node it is the
   *  node (the text of a text node being escaped for its parent). */
  GTEXT_HTML_SERIALIZE_OUTER
} GTEXT_HTML_Serialize_Scope;

/**
 * @enum GTEXT_HTML_Quote_Mode
 * @brief When an attribute value is put in quotes.
 */
typedef enum {
  GTEXT_HTML_QUOTE_ALWAYS = 0, ///< Always: the standard's output. The default.
  /**
   * Only when the HTML syntax needs them: the value is empty, or has ASCII
   * white space, a double quote, an apostrophe, an equals sign, a greater-than
   * sign or a backquote in it. (A less-than sign does not need them, because it
   * is escaped; a greater-than sign is escaped too and still counts, which is
   * what html5lib's vectors define.) Defined by core.test "proper attribute
   * value non-quoting" and its neighbours.
   */
  GTEXT_HTML_QUOTE_WHEN_NEEDED
} GTEXT_HTML_Quote_Mode;

/**
 * @struct GTEXT_HTML_Serialize_Options
 * @brief What a serialization may do. Zeroed is the same as the defaults.
 *
 * New fields are only ever appended, with a zero default.
 */
typedef struct {
  /** The allocator for everything the serializer allocates, and for the
   *  buffer of gtext_html_serialize_buffer(); NULL for the tree's own (or the
   *  default one, for a token serializer). The struct is copied; the `ctx` it
   *  holds must outlive the call. */
  const GTEXT_Allocator * allocator;

  /** Deepest element nesting, the first element written being depth 1, or 0
   *  for the default of 1024 (twice the parser's default of 512 open
   *  elements: a tree the parser built with its defaults fits). Exceeding it
   *  is GTEXT_HTML_E_LIMIT_DEPTH, and the sink has then received a prefix. */
  size_t max_depth;

  /** Most bytes of output, or 0 for the default of 256 MiB. Exceeding it is
   *  GTEXT_HTML_E_LIMIT_OUTPUT; the sink has then received a prefix (never
   *  more than the limit). */
  size_t max_output_bytes;

  /** The standard's scripting flag: with it a `noscript` element's text is
   *  written as it is (the parser reads it as raw text), without it the text
   *  is escaped like any other. Nothing is executed. Default false. */
  bool scripting;

  /**
   * The output encoding as a label, or NULL for UTF-8. **Only UTF-8 labels are
   * accepted** (`utf-8`, `utf8`, `unicode-1-1-utf-8`, `unicode11utf8`,
   * `unicode20utf8`, `x-unicode20utf8`; case-insensitive, surrounding white
   * space ignored): any other string, the empty one included, is
   * GTEXT_HTML_E_UNSUPPORTED_ENCODING, as it is for the parser; phases 3h and
   * 3i write the others. It is the label `inject_meta_charset` writes.
   */
  const char * encoding;

  /** When values are quoted. Defined by core.test. */
  GTEXT_HTML_Quote_Mode quote_attr_values;

  /** The quote character: 0 for the double quote (the standard's), or the
   *  double quote or the apostrophe. With the apostrophe, one in a value is
   *  written as the reference for U+0027, `&#39;` (options.test "quote_char").
   *  Not with use_best_quote_char. */
  char quote_char;

  /** Choose the quote that needs no escape: the double quote for a value that
   *  has an apostrophe and no double quote, the apostrophe for one that has a
   *  double quote and no apostrophe, `quote_char` otherwise. Defined by
   *  core.test "proper attribute value quoting (with ...)". */
  bool use_best_quote_char;

  /**
   * Leave out the start and end tags the HTML syntax makes optional (the
   * standard's "optional tags", section 13.1.2.4; the manual lists the rules
   * and where html5lib's vectors disagree with them). Needs the one-token
   * lookahead, so a tag may be written one call late; the counts are in the
   * result. Assumes a conforming tree. Defined by optionaltags.test.
   */
  bool omit_optional_tags;

  /** Write a boolean attribute (`disabled`, for the elements the standard
   *  lists) whose value is empty or equals its name, as the name alone. The
   *  parser reads the empty value back, so a value equal to the name is not
   *  kept. Defined by options.test "quote_attr_values=true". */
  bool minimize_boolean_attributes;

  /** Write `<br/>` for a void element. Defined by options.test
   *  "use_trailing_solidus=true". */
  bool use_trailing_solidus;
  /** With use_trailing_solidus, a space before the solidus: `<br />`. Without
   *  it, GTEXT_HTML_E_OPTIONS. */
  bool space_before_trailing_solidus;

  /** Escape the text of `style`, `script`, `xmp`, `iframe`, `noembed`,
   *  `noframes`, `plaintext` and `noscript` like other text, which the parser
   *  would not read back. Defined by options.test "rcdata". */
  bool escape_rcdata;

  /** Collapse every run of ASCII white space in text to one space, except in
   *  `pre`, `textarea`, `plaintext` and the raw text elements (and below
   *  them). Adjacent text collapses as one. Defined by whitespace.test. */
  bool strip_whitespace;

  /**
   * With `encoding`, make the `head` say so: a `meta` in it with a `charset`
   * attribute, or with `http-equiv="content-type"` and a `content`, has that
   * value replaced (`text/html; charset=LABEL` for the second); if there is
   * none a `<meta charset=LABEL>` becomes the first thing in the `head`. Needs
   * to see all of the `head` before it writes any of it, so the token form
   * keeps those tokens until the end tag, as bytes of up to about four times
   * the size of the output they make; the queue is bounded by eight times
   * max_output_bytes and 4 KiB, which is the most memory a `head` can cost.
   * Without `encoding` it does nothing. Defined by injectmeta.test.
   */
  bool inject_meta_charset;

  /** Write the attributes of an element sorted by namespace and then name
   *  instead of in tree order. (html5lib's vectors list some in the order
   *  that its filter produces.) */
  bool alphabetical_attributes;

  /** Write the identifiers of a document type: `<!DOCTYPE html PUBLIC "p"
   *  "s">`, `<!DOCTYPE html SYSTEM "s">`. The standard writes the name only.
   *  Defined by core.test "HTML 4.01 DOCTYPE". An identifier that holds both
   *  kinds of quote has no spelling: with this option it is refused with
   *  GTEXT_HTML_E_INVALID (the one tree this serializer refuses). */
  bool doctype_identifiers;

  /**
   * The element whose contents are being written, when the node written has no
   * such parent in the tree: the local name of the context element of a
   * fragment parsed by gtext_html_parse_fragment() (`style`, `textarea`,
   * `td`, ...), or NULL for none. It decides what the nodes at the top are the
   * children of - raw text in a `script` or `style`, the white space of a
   * `pre`, whether a final `</p>` may be left out - exactly as the same
   * element does when it is in the tree and its children are written, and it
   * needs no copy of the fragment. The node written should be a fragment or a
   * document; for an element the element itself is the context. Not
   * empty. Not kept past the call.
   */
  const char * context_name;

  /** The namespace URI of `context_name`, or NULL for HTML. Without a
   *  `context_name` it must be NULL. */
  const char * context_namespace;
} GTEXT_HTML_Serialize_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return Allocator NULL, depth 1024, 256 MiB, scripting off, UTF-8, every
 *   attribute quoted with the double quote, nothing omitted, minimized, sorted or injected.
 */
GTEXT_API GTEXT_HTML_Serialize_Options gtext_html_serialize_options_default(void);

/**
 * @struct GTEXT_HTML_Serialize_Result
 * @brief What a serialization did. Cleared at the start of every call that
 *   takes one; on a failure the counts are what was seen before it.
 *
 * Unlike GTEXT_HTML_Result it holds nothing that is freed. Unlike the XML
 * writer's it has no path: the only failures that belong to a place in the
 * tree are limits, and the message says which.
 */
typedef struct {
  GTEXT_HTML_Status status; ///< The status; GTEXT_HTML_OK if nothing failed.
  const char * message;     ///< A static description. Never freed, never NULL after a call.
  size_t bytes_written;     ///< Bytes the sink accepted.
  size_t nodes_visited;     ///< Tree walk: nodes written (the node itself for OUTER);
                            ///< 0 for a serializer driven token by token.
  size_t omitted_start_tags; ///< By omit_optional_tags.
  size_t omitted_end_tags;   ///< By omit_optional_tags.
  size_t minimized_attributes; ///< By minimize_boolean_attributes.
  size_t meta_charset_replaced; ///< `meta` elements inject_meta_charset rewrote.
  size_t meta_charset_injected; ///< 0 or 1: a `meta` it added.
} GTEXT_HTML_Serialize_Result;

/**
 * @brief Write a node as HTML to a sink.
 *
 * The sink receives the output in chunks of up to 4 KiB, cut anywhere; a
 * non-zero return from it stops the write with GTEXT_HTML_E_WRITE, or
 * GTEXT_HTML_E_OOM when it returned GTEXT_MARKUP_E_OOM. **On any failure the
 * sink has received a prefix of the output**: every byte produced before the
 * failure except a tag the optional-tag lookahead was still holding and the
 * tokens `inject_meta_charset` keeps for a `head`; the walk and the token
 * stream hand over the same prefix, and a sink that has refused is not called
 * again. The scope GTEXT_HTML_SERIALIZE_CHILDREN needs a node that has
 * children: a document, a fragment or an element; for a text, comment,
 * processing instruction or document type it is GTEXT_HTML_E_INVALID.
 *
 * @param node   The document, fragment, element or other node. Not NULL.
 * @param scope  What of it is written. A document or fragment is written by its
 *               children in either scope.
 * @param opts   Options, or NULL for gtext_html_serialize_options_default().
 * @param sink   Where the bytes go. Not NULL, and `write` not NULL.
 * @param result Receives the outcome, or NULL.
 * @return GTEXT_HTML_OK, or why not: GTEXT_HTML_E_INVALID (a NULL or unusable
 *   argument, a scope out of range), GTEXT_HTML_E_OPTIONS,
 *   GTEXT_HTML_E_UNSUPPORTED_ENCODING (all three before any byte is written),
 *   GTEXT_HTML_E_OOM, GTEXT_HTML_E_WRITE, GTEXT_HTML_E_LIMIT_DEPTH,
 *   GTEXT_HTML_E_LIMIT_OUTPUT. Nothing is leaked on any of them.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serialize(const GTEXT_Markup_Node * node,
    GTEXT_HTML_Serialize_Scope scope, const GTEXT_HTML_Serialize_Options * opts,
    const GTEXT_Markup_Sink * sink, GTEXT_HTML_Serialize_Result * result);

/**
 * @brief Write a node as HTML to a new buffer.
 *
 * @param node    The node.
 * @param scope   What of it is written.
 * @param opts    Options, or NULL for the defaults.
 * @param out     Receives the bytes, followed by one NUL byte not counted in
 *                `out_len` (the output may hold NULs of its own: use
 *                `out_len`). Release with gtext_html_serialize_buffer_free().
 *                Written only on success.
 * @param out_len Receives the length. May be NULL. Untouched on failure.
 * @param result  Receives the outcome, or NULL.
 * @return As gtext_html_serialize(); a failure leaves no buffer.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serialize_buffer(const GTEXT_Markup_Node * node,
    GTEXT_HTML_Serialize_Scope scope, const GTEXT_HTML_Serialize_Options * opts,
    char ** out, size_t * out_len, GTEXT_HTML_Serialize_Result * result);

/**
 * @brief Release a buffer from gtext_html_serialize_buffer().
 *
 * The buffer is released with the allocator it was written with, which it
 * carries in a header of its own: `doc` and `opts` are accepted so that the
 * call reads like the XML writer's, are not needed, and may be NULL. (Do not
 * pass the buffer to free(), and do not use the memory before it.)
 *
 * @param doc    Ignored; may be NULL.
 * @param opts   Ignored; may be NULL.
 * @param buffer The buffer. NULL is ignored.
 */
GTEXT_API void gtext_html_serialize_buffer_free(const GTEXT_Markup_Document * doc,
    const GTEXT_HTML_Serialize_Options * opts, char * buffer);

/**
 * @brief Write a node as HTML to a file, replacing it atomically.
 *
 * The content goes to a temporary file beside the destination and replaces it
 * only when complete. A write that fails for any reason - the serializer's, the
 * allocator's, the sink of the file layer's - leaves the destination as it was:
 * the previous file, or no file if there was none. A failed call's result has
 * the status and message of the failure and **no counts** (nothing was
 * replaced). This is the file layer the XML writer uses and it reports the same way:
 * a path that cannot be opened is GTEXT_HTML_E_INVALID, a failed write
 * GTEXT_HTML_E_WRITE, and a failure of the serializer is its own status.
 *
 * @param node   The node.
 * @param scope  What of it is written.
 * @param path   The file to write. Not NULL.
 * @param opts   Options, or NULL for the defaults.
 * @param result Receives the outcome, or NULL.
 * @return GTEXT_HTML_OK or why not, as gtext_html_serialize().
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serialize_file(const GTEXT_Markup_Node * node,
    GTEXT_HTML_Serialize_Scope scope, const char * path,
    const GTEXT_HTML_Serialize_Options * opts, GTEXT_HTML_Serialize_Result * result);

/* ---- the token stream --------------------------------------------------- */

/**
 * @brief Opaque token-stream serializer.
 *
 * The streams it takes need not be balanced or well formed (a vector is a few
 * tokens cut out of a document). It keeps a stack of the elements it has seen
 * open. **An end tag is always written** (or left out by the optional-tag
 * rules, which judge the element it names), whether or not anything is open;
 * it closes the innermost open element when it names that element (the same
 * kind of namespace and, among the elements the rules name, the same name; any
 * element they do not name matches), and otherwise closes nothing: the stack,
 * and the white space count of `strip_whitespace`, are as they were. A void
 * HTML element's end tag is dropped.
 * Once any call has failed the serializer is finished: every later call
 * returns the same status.
 */
typedef struct GTEXT_HTML_Serializer GTEXT_HTML_Serializer;

/**
 * @brief Create a token-stream serializer.
 *
 * @param opts Options, or NULL for the defaults. The allocator is copied.
 * @param sink Where the bytes go. Copied; its `user` must outlive the serializer.
 * @param out  Receives the serializer, to be freed with gtext_html_serializer_free().
 *             Set to NULL on failure.
 * @return GTEXT_HTML_OK; GTEXT_HTML_E_INVALID (NULL `out`, `sink` or its
 *   `write`); GTEXT_HTML_E_OPTIONS; GTEXT_HTML_E_UNSUPPORTED_ENCODING;
 *   GTEXT_HTML_E_OOM.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_new(
    const GTEXT_HTML_Serialize_Options * opts, const GTEXT_Markup_Sink * sink,
    GTEXT_HTML_Serializer ** out);

/**
 * @brief Say what element the stream is the contents of.
 *
 * Whether text at the top of the stream is raw text (`script`) and whether a
 * `p` may lose its end tag at the end of the stream (not in an `a`) depend
 * on the parent, which the stream does not contain. Without a context the
 * top level has no parent: its text is escaped and a final `</p>` is omitted.
 * Call it before the first token. The tree walk calls it for the node it
 * writes the children of; GTEXT_HTML_Serialize_Options::context_name does the
 * same at creation.
 *
 * @param ser  The serializer.
 * @param ns   The element's namespace URI, or NULL for the HTML namespace.
 * @param name Its name as the elements of the stream are named (a local name).
 * @return GTEXT_HTML_OK, GTEXT_HTML_E_INVALID (NULL), or the sticky failure;
 *   GTEXT_HTML_E_STATE after the first token or after finish.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_context(
    GTEXT_HTML_Serializer * ser, const char * ns, const char * name);

/**
 * @brief A start tag.
 *
 * @param ser    The serializer.
 * @param ns     The element's namespace URI, or NULL for the HTML namespace.
 *               Only an HTML element is void, raw text, or has optional tags.
 * @param name   The name to write, NUL-terminated: the local name for an HTML,
 *               SVG or MathML element, the qualified name otherwise. A void
 *               HTML element is written as an empty tag: its end tag, if the
 *               stream has one, is dropped.
 * @param attrs  The attributes, or NULL when `count` is 0. Each is written
 *               under the standard's serialized name for its namespace.
 * @param count  Their number.
 * @return GTEXT_HTML_OK, GTEXT_HTML_E_INVALID (NULL name, NULL attribute
 *   parts), GTEXT_HTML_E_STATE after finish, GTEXT_HTML_E_LIMIT_DEPTH, a
 *   limit or sink failure, or the sticky failure.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_start_tag(GTEXT_HTML_Serializer * ser,
    const char * ns, const char * name, const GTEXT_Markup_Attribute * attrs, size_t count);

/** @brief An end tag, as start_tag's name. Pops the stack of open elements. */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_end_tag(
    GTEXT_HTML_Serializer * ser, const char * ns, const char * name);

/** @brief A start tag for an element that has no end tag and no content
 *  (html5lib's EmptyTag): written as a start tag whatever the name. */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_empty_tag(GTEXT_HTML_Serializer * ser,
    const char * ns, const char * name, const GTEXT_Markup_Attribute * attrs, size_t count);

/**
 * @brief Character data, escaped for the parent (or written as it is for a raw
 *   text parent). `data` need not be terminated. A length of 0 is not a token.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_characters(
    GTEXT_HTML_Serializer * ser, const char * data, size_t length);

/** @brief A comment: `<!--data-->`. */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_comment(
    GTEXT_HTML_Serializer * ser, const char * data, size_t length);

/** @brief A processing instruction: `<?target data?>`. */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_processing_instruction(
    GTEXT_HTML_Serializer * ser, const char * target, const char * data, size_t length);

/**
 * @brief A document type. `public_id` and `system_id` may be NULL (absent); they
 *   are written only with GTEXT_HTML_Serialize_Options::doctype_identifiers.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_doctype(GTEXT_HTML_Serializer * ser,
    const char * name, const char * public_id, const char * system_id);

/**
 * @brief End the stream: write what the lookahead was holding and flush.
 *
 * May be called once with success; later token calls return GTEXT_HTML_E_STATE.
 *
 * @param ser    The serializer.
 * @param result Receives the outcome, or NULL.
 * @return GTEXT_HTML_OK, or why not (the sticky failure, a sink failure).
 */
GTEXT_API GTEXT_HTML_Status gtext_html_serializer_finish(
    GTEXT_HTML_Serializer * ser, GTEXT_HTML_Serialize_Result * result);

/** @brief Free a serializer, at any point, after a failure included. NULL is ignored. */
GTEXT_API void gtext_html_serializer_free(GTEXT_HTML_Serializer * ser);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_HTML_HTML_SERIALIZE_H
