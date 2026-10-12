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
 * The spelling of the HTML parse error codes.
 *
 * WHATWG HTML, "Parse errors" (13.2.2). The names are the standard's, which is
 * what the html5lib-tests vectors compare. tests/test-html-tokenizer.cpp checks
 * that the table has one entry per enumerator and that no two are alike.
 */

#include <ghoti.io/text/macros.h>

#include "html_tokenizer_internal.h"

static const char * const error_names[GTEXT_HTML_ERR_COUNT] = {
  [GTEXT_HTML_ERR_ABRUPT_CLOSING_OF_EMPTY_COMMENT] =
      "abrupt-closing-of-empty-comment",
  [GTEXT_HTML_ERR_ABRUPT_DOCTYPE_PUBLIC_IDENTIFIER] =
      "abrupt-doctype-public-identifier",
  [GTEXT_HTML_ERR_ABRUPT_DOCTYPE_SYSTEM_IDENTIFIER] =
      "abrupt-doctype-system-identifier",
  [GTEXT_HTML_ERR_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE] =
      "absence-of-digits-in-numeric-character-reference",
  [GTEXT_HTML_ERR_CDATA_IN_HTML_CONTENT] = "cdata-in-html-content",
  [GTEXT_HTML_ERR_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE] =
      "character-reference-outside-unicode-range",
  [GTEXT_HTML_ERR_CONTROL_CHARACTER_IN_INPUT_STREAM] =
      "control-character-in-input-stream",
  [GTEXT_HTML_ERR_CONTROL_CHARACTER_REFERENCE] =
      "control-character-reference",
  [GTEXT_HTML_ERR_DISALLOWED_PROCESSING_INSTRUCTION_TARGET] =
      "disallowed-processing-instruction-target",
  [GTEXT_HTML_ERR_DUPLICATE_ATTRIBUTE] = "duplicate-attribute",
  [GTEXT_HTML_ERR_END_TAG_WITH_ATTRIBUTES] = "end-tag-with-attributes",
  [GTEXT_HTML_ERR_END_TAG_WITH_TRAILING_SOLIDUS] =
      "end-tag-with-trailing-solidus",
  [GTEXT_HTML_ERR_EOF_BEFORE_TAG_NAME] = "eof-before-tag-name",
  [GTEXT_HTML_ERR_EOF_IN_CDATA] = "eof-in-cdata",
  [GTEXT_HTML_ERR_EOF_IN_COMMENT] = "eof-in-comment",
  [GTEXT_HTML_ERR_EOF_IN_DOCTYPE] = "eof-in-doctype",
  [GTEXT_HTML_ERR_EOF_IN_PROCESSING_INSTRUCTION] =
      "eof-in-processing-instruction",
  [GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT] =
      "eof-in-script-html-comment-like-text",
  [GTEXT_HTML_ERR_EOF_IN_TAG] = "eof-in-tag",
  [GTEXT_HTML_ERR_INCORRECTLY_CLOSED_COMMENT] = "incorrectly-closed-comment",
  [GTEXT_HTML_ERR_INCORRECTLY_OPENED_COMMENT] = "incorrectly-opened-comment",
  [GTEXT_HTML_ERR_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME] =
      "invalid-character-sequence-after-doctype-name",
  [GTEXT_HTML_ERR_INVALID_FIRST_CHARACTER_OF_PROCESSING_INSTRUCTION_TARGET] =
      "invalid-first-character-of-processing-instruction-target",
  [GTEXT_HTML_ERR_INVALID_FIRST_CHARACTER_OF_TAG_NAME] =
      "invalid-first-character-of-tag-name",
  [GTEXT_HTML_ERR_INVALID_PROCESSING_INSTRUCTION_TARGET] =
      "invalid-processing-instruction-target",
  [GTEXT_HTML_ERR_MISSING_ATTRIBUTE_VALUE] = "missing-attribute-value",
  [GTEXT_HTML_ERR_MISSING_DOCTYPE_NAME] = "missing-doctype-name",
  [GTEXT_HTML_ERR_MISSING_DOCTYPE_PUBLIC_IDENTIFIER] =
      "missing-doctype-public-identifier",
  [GTEXT_HTML_ERR_MISSING_DOCTYPE_SYSTEM_IDENTIFIER] =
      "missing-doctype-system-identifier",
  [GTEXT_HTML_ERR_MISSING_END_TAG_NAME] = "missing-end-tag-name",
  [GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER] =
      "missing-quote-before-doctype-public-identifier",
  [GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER] =
      "missing-quote-before-doctype-system-identifier",
  [GTEXT_HTML_ERR_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE] =
      "missing-semicolon-after-character-reference",
  [GTEXT_HTML_ERR_MISSING_WHITESPACE_AFTER_DOCTYPE_PUBLIC_KEYWORD] =
      "missing-whitespace-after-doctype-public-keyword",
  [GTEXT_HTML_ERR_MISSING_WHITESPACE_AFTER_DOCTYPE_SYSTEM_KEYWORD] =
      "missing-whitespace-after-doctype-system-keyword",
  [GTEXT_HTML_ERR_MISSING_WHITESPACE_BEFORE_DOCTYPE_NAME] =
      "missing-whitespace-before-doctype-name",
  [GTEXT_HTML_ERR_MISSING_WHITESPACE_BETWEEN_ATTRIBUTES] =
      "missing-whitespace-between-attributes",
  [GTEXT_HTML_ERR_MISSING_WHITESPACE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS] =
      "missing-whitespace-between-doctype-public-and-system-identifiers",
  [GTEXT_HTML_ERR_NESTED_COMMENT] = "nested-comment",
  [GTEXT_HTML_ERR_NONCHARACTER_CHARACTER_REFERENCE] =
      "noncharacter-character-reference",
  [GTEXT_HTML_ERR_NONCHARACTER_IN_INPUT_STREAM] =
      "noncharacter-in-input-stream",
  [GTEXT_HTML_ERR_NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS] =
      "non-void-html-element-start-tag-with-trailing-solidus",
  [GTEXT_HTML_ERR_NULL_CHARACTER_REFERENCE] = "null-character-reference",
  [GTEXT_HTML_ERR_SURROGATE_CHARACTER_REFERENCE] =
      "surrogate-character-reference",
  [GTEXT_HTML_ERR_SURROGATE_IN_INPUT_STREAM] = "surrogate-in-input-stream",
  [GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_AFTER_DOCTYPE_SYSTEM_IDENTIFIER] =
      "unexpected-character-after-doctype-system-identifier",
  [GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_IN_ATTRIBUTE_NAME] =
      "unexpected-character-in-attribute-name",
  [GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_IN_UNQUOTED_ATTRIBUTE_VALUE] =
      "unexpected-character-in-unquoted-attribute-value",
  [GTEXT_HTML_ERR_UNEXPECTED_EQUALS_SIGN_BEFORE_ATTRIBUTE_NAME] =
      "unexpected-equals-sign-before-attribute-name",
  [GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER] = "unexpected-null-character",
  [GTEXT_HTML_ERR_UNEXPECTED_SOLIDUS_IN_TAG] = "unexpected-solidus-in-tag",
  [GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE] =
      "unknown-named-character-reference",
  [GTEXT_HTML_ERR_INVALID_UTF8] = "invalid-utf-8-in-input-stream",
};

const char * gtext_html_error_code_name(GTEXT_HTML_ErrorCode code) {
  if ((unsigned)code >= (unsigned)GTEXT_HTML_ERR_COUNT
      || !error_names[code]) {
    return "unknown";
  }
  return error_names[code];
}
