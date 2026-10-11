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
 * @file markup_escape.h
 * @brief Escaping text for markup, and the lexical checks that decide whether a
 *   piece of text can be written at all.
 *
 * Two questions with different answers live here, and they are kept apart on
 * purpose. *Escaping* turns text the language can spell into the spelling:
 * `a<b` becomes `a&lt;b`. *Checking* says that the language cannot spell it
 * whatever is done: a comment containing `--`, a CDATA section containing
 * `]]>`, a control character XML 1.0 forbids even as a numeric reference. The
 * first is a transformation; the second is a refusal, because a writer that
 * repaired the text would be writing a different document and saying nothing.
 *
 * The XML flavours are here now. HTML's serialization has its own escaping
 * rules and arrives with the HTML serializer.
 */

#ifndef GHOTI_IO_GTEXT_MARKUP_MARKUP_ESCAPE_H
#define GHOTI_IO_GTEXT_MARKUP_MARKUP_ESCAPE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_core.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called with each chunk of output.
 *
 * @param user The pointer from the sink, unchanged.
 * @param bytes The chunk; not terminated.
 * @param len Its length, which may be 0.
 * @return GTEXT_MARKUP_OK to continue, anything else to stop. The status is
 *   returned to the caller of the function that was writing, unchanged.
 */
typedef GTEXT_Markup_Status (*GTEXT_Markup_Write_Function)(
    void * user, const char * bytes, size_t len);

/**
 * @struct GTEXT_Markup_Sink
 * @brief Where written bytes go.
 */
typedef struct {
  GTEXT_Markup_Write_Function write; ///< Called for each chunk. Not NULL.
  void * user;                       ///< Passed back unchanged.
} GTEXT_Markup_Sink;

/**
 * @enum GTEXT_Markup_Escape_Mode
 * @brief Which place the text is going.
 */
typedef enum {
  /**
   * XML character data. `&`, `<` and `>` become references, and so does a
   * carriage return: written raw it would be read back as a line feed,
   * because XML normalizes line endings before it parses anything.
   */
  GTEXT_MARKUP_ESCAPE_XML_TEXT = 0,
  /**
   * An XML attribute value in double quotes. As XML_TEXT, plus `"`, and plus
   * tab, line feed and carriage return as numeric references: a raw one in an
   * attribute value is read back as a space.
   */
  GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE
} GTEXT_Markup_Escape_Mode;

/**
 * @brief Write text to a sink, escaped.
 *
 * The whole input is checked before the first byte is written, so a refusal
 * has written nothing: the input must be well-formed UTF-8 and every
 * character must be one XML 1.0 allows (tab, line feed, carriage return, and
 * U+0020 and above except the surrogates and U+FFFE and U+FFFF).
 *
 * @param mode Where the text is going.
 * @param text The text; need not be terminated.
 * @param len Bytes of `text`.
 * @param sink Where the escaped text goes.
 * @return GTEXT_MARKUP_OK; GTEXT_MARKUP_E_INVALID for a NULL `sink` or one with
 *   no `write`; GTEXT_MARKUP_E_ENCODING; GTEXT_MARKUP_E_UNREPRESENTABLE; or
 *   whatever the sink returned.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_escape(GTEXT_Markup_Escape_Mode mode,
    const char * text, size_t len, const GTEXT_Markup_Sink * sink);

/**
 * @brief Escape text into a new string.
 *
 * Built by gtext_markup_escape() into a buffer, so it escapes exactly as that
 * does.
 *
 * @param mode Where the text is going.
 * @param text The text; need not be terminated.
 * @param len Bytes of `text`.
 * @param allocator The allocator, or NULL for the default.
 * @param out Receives a NUL-terminated string to release with
 *   gtext_markup_string_free(). Written only on success.
 * @param out_len Receives its length. May be NULL.
 * @return As gtext_markup_escape(), with GTEXT_MARKUP_E_OOM added.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_escape_alloc(
    GTEXT_Markup_Escape_Mode mode, const char * text, size_t len,
    const GTEXT_Allocator * allocator, char ** out, size_t * out_len);

/**
 * @brief Is this text well-formed UTF-8 made only of characters XML 1.0 allows?
 *
 * @param text The text.
 * @param len Bytes of `text`.
 * @return GTEXT_MARKUP_OK; GTEXT_MARKUP_E_ENCODING; or
 *   GTEXT_MARKUP_E_UNREPRESENTABLE for a valid character XML forbids.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_check_xml_chars(
    const char * text, size_t len);

/**
 * @brief Can this be the data of an XML comment?
 *
 * Valid characters, no `--` anywhere, and not ending in `-`.
 *
 * @param text The comment's data.
 * @param len Bytes of `text`.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_ENCODING or
 *   GTEXT_MARKUP_E_UNREPRESENTABLE.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_check_xml_comment(
    const char * text, size_t len);

/**
 * @brief Can this be the data of an XML processing instruction?
 *
 * Valid characters and no `?>`.
 *
 * @param text The instruction's data.
 * @param len Bytes of `text`.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_ENCODING or
 *   GTEXT_MARKUP_E_UNREPRESENTABLE.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_check_xml_pi_data(
    const char * text, size_t len);

/**
 * @brief Can this be the content of an XML CDATA section?
 *
 * Valid characters and no `]]>`. A writer that has text containing `]]>` splits
 * it across two sections; that is a choice for the writer, so this refuses.
 *
 * @param text The section's content.
 * @param len Bytes of `text`.
 * @return GTEXT_MARKUP_OK, GTEXT_MARKUP_E_ENCODING or
 *   GTEXT_MARKUP_E_UNREPRESENTABLE.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_check_xml_cdata(
    const char * text, size_t len);

/**
 * @brief Is this an XML Name?
 *
 * XML 1.0 fifth edition, production [5]: a name start character and then name
 * characters. Colons are allowed, as the production allows them.
 *
 * @param text The candidate.
 * @param len Bytes of `text`.
 * @return true if it is a Name. False for the empty string and for bytes that
 *   are not UTF-8.
 */
GTEXT_API bool gtext_markup_is_xml_name(const char * text, size_t len);

/**
 * @brief Is this an XML NCName: a Name with no colon?
 *
 * Namespaces in XML 1.0, production [4]. What a prefix and a local name must
 * be, and what a processing instruction's target must be.
 *
 * @param text The candidate.
 * @param len Bytes of `text`.
 * @return true if it is an NCName.
 */
GTEXT_API bool gtext_markup_is_xml_ncname(const char * text, size_t len);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_MARKUP_MARKUP_ESCAPE_H
