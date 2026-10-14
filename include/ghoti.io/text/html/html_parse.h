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
 * @file html_parse.h
 * @brief Parse HTML into a markup tree: whole, as a fragment, in pieces, or
 *   from a file.
 *
 * The parser is the WHATWG HTML Living Standard's: its tokenizer and tree
 * construction, with the fragment parsing algorithm. It has the shape of
 * xml_parse.h and xml_stream.h: a function for a whole buffer, one for a file,
 * and an incremental parser that is fed in pieces; and one thing that XML does
 * not have, a function for a fragment in a context element.
 *
 * ## What is built
 *
 * - **Elements** in the HTML namespace, with their attributes in no namespace,
 *   in source order with the duplicates removed (the first of two equal names
 *   stays). Names are lower-cased. Inside `svg` and `math` the elements are in
 *   the SVG and MathML namespaces with the names and attributes the
 *   standard's tables give them (`foreignObject`, `viewBox`), and the
 *   attributes `xlink:*`, `xml:*` and `xmlns*` are in the XLink, XML and XMLNS
 *   namespaces.
 * - **Text** with character references decoded, CR LF and lone CR turned into
 *   LF, and adjacent runs one node; **comments**; **processing instructions**
 *   where the standard's tokenizer makes them; the **doctype** as a node.
 * - A **`template` element owns its content** as a document fragment
 *   (gtext_markup_element_template_content()); the element has no children of
 *   its own.
 * - The **document mode** is set (gtext_markup_document_mode()) from the
 *   doctype, and the encoding is recorded (gtext_markup_document_encoding()).
 * - **Source locations** on elements, text, comments, processing instructions
 *   and doctypes, and on attributes; offsets count bytes of the input **as the
 *   caller gave it**, whatever its encoding (see "Encodings").
 *
 * Every input has a tree: a document with no `html` element gets one, with
 * a `head` and a `body`, and the mistakes in the document are parse errors
 * (html_error.h), not failures. A parse fails only for an argument, a limit
 * or memory. Nothing is read from
 * a file or a network except by gtext_html_parse_file() for the file it is
 * given, no script is executed, and a reference to an external resource is
 * only an attribute value.
 *
 * ## Encodings
 *
 * The input is bytes, and the parser decides what they are as the standard's
 * **encoding sniffing algorithm** does (html.spec.whatwg.org, "Determining the
 * character encoding"), in this order, the first that answers:
 *
 * 1. a **byte order mark** (UTF-8, UTF-16LE, UTF-16BE): certain, and skipped;
 * 2. the caller's **transport label** (`transport_encoding`, the `charset` of a
 *    `Content-Type`), if `transport_encoding_certainty` is certain: certain;
 * 3. unless `ignore_meta`: the **`<meta>` prescan** of the first 1024 bytes -
 *    `<meta charset>`, `<meta http-equiv=content-type content=...>`, an XML
 *    declaration's `encoding` or a UTF-16 one - tentative. A UTF-16 label found
 *    this way means UTF-8, as the standard says (a document whose markup reads as
 *    ASCII is not in UTF-16), and `x-user-defined` means windows-1252;
 * 4. a tentative `transport_encoding`, then `default_encoding`, then UTF-8:
 *    tentative.
 *
 * **Nothing is guessed from the content** (no frequency analysis, no detector): the
 * standard allows it and leaves it to the user agent, and it is a source of
 * encodings that neither the document nor the caller named; the caller
 * supplies the default.
 *
 * The parser looks at the first 1024 bytes (or the whole input, if it is shorter)
 * before it builds anything, so an incremental parser holds back its first 1024
 * bytes. While the encoding is **tentative** it also keeps every byte it is
 * given until the tree builder leaves the head (its insertion mode is one of
 * `initial` to `after head`, `text` and `in template`), or the input ends, because a
 * `<meta>` that declares another encoding in the head makes the standard
 * "change the encoding": the parse starts again from the first byte in the
 * declared one (`reparsed`), and the tree, the errors and their offsets are exactly those
 * of a parse that had begun in it. A `<meta>` after the head is an ordinary
 * element, and so is every `<meta>` after a restart (the encoding is then certain). Once
 * the head is behind it the parser lets go of what it kept and the rest streams;
 * the memory this costs is the head and is bounded by `max_input_bytes`.
 * gtext_html_parse() and the file functions do not copy the input for it at all.
 *
 * The bytes are decoded with the Encoding Standard's decoders (encoding.h), in
 * their replacement mode: an ill-formed byte sequence is a U+FFFD and the parse
 * error `invalid-utf-8-in-input-stream` (named for UTF-8, raised for the same
 * thing in every encoding), never a failure. **Offsets** in the errors and the
 * locations of the nodes are offsets into the bytes **given**, so that they stay
 * right for a document in Shift_JIS or UTF-16 (the decoder reports where each code
 * point began); a byte order mark and the bytes of an ISO-2022-JP escape sequence
 * are counted. **Line and column count characters**, a column being a UTF-16 unit as
 * before. `max_input_bytes` counts the bytes given. Chunking, and a restart,
 * change none of this.
 *
 * Which encoding was used, how it was decided and how sure the parser is are in the
 * result and on the document (gtext_markup_document_encoding()).
 * gtext_html_sniff_encoding() runs the algorithm alone. A fragment is characters, not
 * bytes: it is not decoded or sniffed, and `transport_encoding` and `default_encoding`
 * are only checked and reported.
 *
 * ## Results
 *
 * Every function takes a GTEXT_HTML_Result (or NULL) that receives the
 * status, the document mode, the encoding and its source and the parse
 * errors, **on failure as well as on success**; the tree and the result are
 * independent objects and are freed separately. **Zero a result before its first use and free it
 * before reusing it**: a call overwrites the struct and does not free what it held. On a failure the document
 * output is not written and nothing is leaked, including when the allocator
 * fails at any point.
 *
 * ## Threads
 *
 * Independent parses share nothing, and any number may run at once. A parser
 * and a result are not synchronized: use each from one thread at a time.
 */

#ifndef GHOTI_IO_GTEXT_HTML_HTML_PARSE_H
#define GHOTI_IO_GTEXT_HTML_HTML_PARSE_H

#include <ghoti.io/text/html/html_core.h>
#include <ghoti.io/text/html/html_error.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a whole HTML document into a tree.
 *
 * Equivalent to creating a parser, feeding it `data` and finishing it, which
 * is also how it is made: the tree and the errors are exactly those of any
 * way of cutting the input.
 *
 * @param data   The document. Need not be NUL-terminated and is not kept; may
 *               be NULL only if `len` is 0 (an empty document, which has the
 *               tree `html`, `head`, `body`, quirks mode, and the parse error
 *               `missing-doctype`).
 * @param len    Bytes of `data`.
 * @param opts   Options, or NULL for gtext_html_parse_options_default().
 * @param out    Receives the document, which the caller frees with
 *               gtext_markup_document_free(). Written **only on success**.
 * @param result Receives the status, mode, encoding and parse errors, or a
 *               cleared result on a failure that happened before there was
 *               anything to say; may be NULL. Free it with
 *               gtext_html_result_free().
 * @return GTEXT_HTML_OK, or why the parse failed: GTEXT_HTML_E_INVALID (NULL
 *   `out`, NULL `data` with a length), GTEXT_HTML_E_OOM, or a limit's status. On any
 *   failure there is no tree and nothing is leaked.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parse(const void * data, size_t len,
    const GTEXT_HTML_Parse_Options * opts, GTEXT_Markup_Document ** out,
    GTEXT_HTML_Result * result);

/**
 * @brief Parse a fragment of HTML in the context of an element.
 *
 * The standard's HTML fragment parsing algorithm - what `innerHTML` does. The
 * bytes are parsed as the contents of `context`, an element that is described
 * by value and need not exist anywhere.
 *
 * **Ownership.** The fragment is a node of kind
 * GTEXT_MARKUP_KIND_FRAGMENT whose children are what was parsed, and it
 * belongs to `doc`, a scratch document that the call makes for it (with a
 * root element `html` that is empty). Free `doc` with
 * gtext_markup_document_free() and the fragment goes with it; there is nothing
 * to free for the fragment itself, and it must not be used afterwards. To keep
 * the fragment's contents in a document of your own, **clone** it with
 * gtext_markup_clone() (`deep` true) into that document and insert the copy:
 * inserting a fragment inserts its children. Then free `doc`.
 *
 * The input is characters already decoded, as `innerHTML`'s is: a leading
 * U+FEFF is text, not a byte order mark. The encoding options are checked and
 * reported (the labels they name or do not), with the source `DEFAULT` or `CALLER`
 * and the confidence irrelevant.
 * `max_open_elements` counts the context element's `html` root, so a limit of
 * 1 admits no element at all. The result's `mode` is the context's `quirks`. The
 * result's `encoding` and `encoding_source` are reported but **not recorded on the
 * scratch document** (gtext_markup_document_encoding() is NULL there): a fragment is
 * characters, not bytes of a document.
 *
 * @param context The context element. Not NULL; `local_name` not empty and not
 *                NULL; an attribute needs a name and a value.
 * @param data    The markup. May be NULL only if `len` is 0.
 * @param len     Bytes of `data`.
 * @param opts    Options, or NULL for the defaults. `scripting` applies to
 *                a `noscript` context.
 * @param doc     Receives the scratch document that owns the fragment. Written
 *                **only on success**, with `fragment`.
 * @param fragment Receives the fragment. Written only on success.
 * @param result  As for gtext_html_parse().
 * @return As gtext_html_parse(); GTEXT_HTML_E_INVALID also for a NULL or unusable
 *   `context`.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parse_fragment(
    const GTEXT_HTML_Fragment_Context * context, const void * data, size_t len,
    const GTEXT_HTML_Parse_Options * opts, GTEXT_Markup_Document ** doc,
    GTEXT_Markup_Node ** fragment, GTEXT_HTML_Result * result);

/**
 * @brief Parse an HTML file into a tree.
 *
 * Reads the file through the library's file layer (so a pipe works, and the
 * size limit is applied while reading, not after) and parses it as
 * gtext_html_parse() does; byte offsets in a parse error are offsets into the
 * file. Failures to read are reported as gtext_xml_parse_file() reports them,
 * which is the way the CSV and JSON file functions do: a path that cannot be
 * opened or read is GTEXT_HTML_E_INVALID with a result message that says which
 * ("No such file or directory", "Permission denied opening file", ...), a file
 * longer than `max_input_bytes` is GTEXT_HTML_E_LIMIT_INPUT, and an allocation
 * that failed is GTEXT_HTML_E_OOM. The result then has no parse errors.
 *
 * @param path   The file. Not NULL.
 * @param opts   Options, or NULL for the defaults.
 * @param out    Receives the document, which the caller frees with
 *               gtext_markup_document_free(). Written **only on success**.
 * @param result As for gtext_html_parse().
 * @return GTEXT_HTML_OK, or why not. On failure there is no tree and nothing
 *   is leaked.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parse_file(const char * path,
    const GTEXT_HTML_Parse_Options * opts, GTEXT_Markup_Document ** out,
    GTEXT_HTML_Result * result);

/**
 * @struct GTEXT_HTML_Sniff_Result
 * @brief What the encoding sniffing algorithm picked for some bytes.
 */
typedef struct {
  GTEXT_HTML_Status status;         ///< GTEXT_HTML_OK, or GTEXT_HTML_E_INVALID for an unusable argument or option (then nothing else is set).
  GTEXT_Encoding encoding;          ///< The encoding picked.
  const char * encoding_name;       ///< Its name by the Encoding Standard ("windows-1252", "Shift_JIS"...). Static; never freed.
  GTEXT_Markup_Encoding_Source source; ///< How it was decided, as in GTEXT_HTML_Result::encoding_source (a restart never happens here).
  GTEXT_HTML_Encoding_Certainty confidence; ///< _CERTAIN (a byte order mark, a certain transport label) or _TENTATIVE.
  size_t bom_length;                ///< Bytes of the byte order mark, or 0.
  bool transport_encoding_ignored;  ///< `transport_encoding` was given and names no encoding.
  bool default_encoding_ignored;    ///< `default_encoding` was given and names no encoding.
} GTEXT_HTML_Sniff_Result;

/**
 * @brief The encoding the parser would pick for these bytes, without parsing them.
 *
 * Runs the encoding sniffing algorithm of the standard - the byte order mark, the caller's
 * transport label, the `<meta>` prescan, the caller's default - over the first 1024 bytes of
 * `data` (or all of it if it is shorter, which the prescan reads as the end of the input), exactly as
 * gtext_html_parse() does before it begins. Only the options that decide it are read: `transport_encoding`,
 * `transport_encoding_certainty`, `default_encoding`, `ignore_meta`. **It is what the parser starts with, not
 * always what it ends with**: a tentative encoding can still be changed by a `<meta>` the parser meets in the
 * head after the first 1024 bytes, or one the prescan read differently from the tokenizer. No memory is allocated.
 *
 * @param data   The beginning of the document. May be NULL if `len` is 0.
 * @param len    Bytes of `data`.
 * @param opts   Options, or NULL for the defaults.
 * @param result Receives the answer. Not NULL.
 * @return The result's status: GTEXT_HTML_OK, or GTEXT_HTML_E_INVALID (NULL `result`, NULL `data` with a length, a
 *   certainty that is none of the enum).
 */
GTEXT_API GTEXT_HTML_Status gtext_html_sniff_encoding(const void * data, size_t len,
    const GTEXT_HTML_Parse_Options * opts, GTEXT_HTML_Sniff_Result * result);

/**
 * @brief Opaque incremental parser.
 *
 * Chunk boundaries do not matter: a document fed whole, a byte at a time, or
 * cut anywhere else gives the same tree with the same parse errors at the same
 * positions - a cut inside a multi-byte character, a tag, a character
 * reference or the byte order mark included. (The one exception is the
 * input limit, which counts what has been fed; a failure is a failure however
 * it is reached.)
 *
 * Memory is bounded by the document being built and by the largest single
 * token being read: unlike the XML stream there are no events, and the tree is
 * built as the bytes arrive. A run of characters is held until something that
 * is not a character ends it, so `max_text_bytes` bounds it; the other tokens
 * are bounded by their own limits.
 *
 * Once any call has failed - a limit was reached, memory ran out - the parser is
 * finished: every later call returns the same status.
 */
typedef struct GTEXT_HTML_Parser GTEXT_HTML_Parser;

/**
 * @brief Create an incremental parser for a document.
 *
 * @param opts Options, or NULL for the defaults. `max_input_bytes` counts all
 *             the bytes fed. The allocator is copied; the `ctx` it holds must
 *             outlive the parser, the document and the result.
 * @param out  Receives the parser, which the caller frees with
 *             gtext_html_parser_free(). Set to NULL on failure.
 * @return GTEXT_HTML_OK; GTEXT_HTML_E_INVALID for a NULL `out`;
 *   GTEXT_HTML_E_INVALID for a certainty that is none of the enum; GTEXT_HTML_E_OOM. (Unlike
 *   gtext_xml_stream_new() this returns a status, because there is more than
 *   one way for it to fail.)
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parser_new(
    const GTEXT_HTML_Parse_Options * opts, GTEXT_HTML_Parser ** out);

/**
 * @brief Create an incremental parser for a fragment in a context element.
 *
 * As gtext_html_parser_new(), for the algorithm of gtext_html_parse_fragment().
 * Finish it with gtext_html_parser_finish_fragment().
 *
 * @param context The context element; described by value and not kept.
 * @param opts    Options, or NULL for the defaults.
 * @param out     Receives the parser. Set to NULL on failure.
 * @return As gtext_html_parser_new(); GTEXT_HTML_E_INVALID also for an unusable
 *   `context`.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parser_new_fragment(
    const GTEXT_HTML_Fragment_Context * context,
    const GTEXT_HTML_Parse_Options * opts, GTEXT_HTML_Parser ** out);

/**
 * @brief Give the parser more of the document.
 *
 * Builds as much of the tree as the bytes so far decide. A `len` of 0 does
 * nothing, and `data` may then be NULL. The bytes are not kept past the call.
 *
 * @param parser The parser.
 * @param data   The next bytes. A multi-byte character, a tag or a character
 *               reference may be cut between calls.
 * @param len    Their number.
 * @return GTEXT_HTML_OK; GTEXT_HTML_E_INVALID for a NULL parser or `data` NULL
 *   with a length; the sticky failure if there was one (a limit,
 *   memory); GTEXT_HTML_E_STATE after the parser was finished.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parser_feed(GTEXT_HTML_Parser * parser,
    const void * data, size_t len);

/**
 * @brief End the input and take the document.
 *
 * Completes the tree (the end of the input is part of the algorithm) and hands
 * it over. May be called once with success: a second call is
 * GTEXT_HTML_E_STATE, and the document stays with its first caller. After a
 * failure it returns the failure again, and fills `result` again (with the
 * errors raised before the failure).
 *
 * @param parser The parser, made by gtext_html_parser_new().
 * @param out    Receives the document, which the caller frees with
 *               gtext_markup_document_free(). Written **only on success**.
 * @param result As for gtext_html_parse(); may be NULL.
 * @return GTEXT_HTML_OK, or why not. GTEXT_HTML_E_STATE also for a parser made
 *   by gtext_html_parser_new_fragment().
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parser_finish(GTEXT_HTML_Parser * parser,
    GTEXT_Markup_Document ** out, GTEXT_HTML_Result * result);

/**
 * @brief End the input and take the fragment.
 *
 * As gtext_html_parser_finish(), for a parser made by
 * gtext_html_parser_new_fragment(); the ownership of the fragment is that of
 * gtext_html_parse_fragment().
 *
 * @param parser   The parser.
 * @param doc      Receives the scratch document that owns the fragment.
 * @param fragment Receives the fragment. Both are written only on success.
 * @param result   As for gtext_html_parse(); may be NULL.
 * @return As gtext_html_parser_finish(); GTEXT_HTML_E_STATE also for a parser
 *   made by gtext_html_parser_new().
 */
GTEXT_API GTEXT_HTML_Status gtext_html_parser_finish_fragment(
    GTEXT_HTML_Parser * parser, GTEXT_Markup_Document ** doc,
    GTEXT_Markup_Node ** fragment, GTEXT_HTML_Result * result);

/**
 * @brief Free an incremental parser and everything it holds.
 *
 * Safe at any point, after a failure included, and for NULL. A document that
 * finish handed out is the caller's and is not touched.
 */
GTEXT_API void gtext_html_parser_free(GTEXT_HTML_Parser * parser);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_HTML_HTML_PARSE_H
