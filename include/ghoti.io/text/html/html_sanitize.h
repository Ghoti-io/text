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
 */

/**
 * @file html_sanitize.h
 * @brief Sanitize untrusted HTML for insertion as HTML body/flow content.
 *
 * This is a conservative policy boundary, not a general HTML-context encoder.
 * The ordinary parser and serializer do not sanitize. A returned tree is
 * mutable and must be sanitized again after any mutation.
 */
#ifndef GHOTI_IO_GTEXT_HTML_HTML_SANITIZE_H
#define GHOTI_IO_GTEXT_HTML_HTML_SANITIZE_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/html/html_parse.h>
#include <ghoti.io/text/html/html_serialize.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Options for the built-in HTML flow-content policy. */
typedef struct {
  /** Parser configuration. Its finite parser limits also bound input/tree size. */
  GTEXT_HTML_Parse_Options parse;
  /** Keep safe links and add rel="nofollow". Enabled by default. */
  bool allow_links;
  /** Enable the separately constrained image policy. Disabled by default. */
  bool allow_images;
  /** Enable the separately constrained table policy. Disabled by default. */
  bool allow_tables;
  /** Maximum nodes and attributes examined. Zero selects 20,000,000. */
  size_t max_work_units;
  /** Maximum emitted bytes. Zero selects 64 MiB. */
  size_t max_output_bytes;
} GTEXT_HTML_Sanitize_Options;

/** Outcome of one sanitize operation. Strings are static and never freed. */
typedef struct {
  GTEXT_HTML_Status status;
  const char * message;
  /** Identifies the fixed policy behavior implemented by this library. */
  const char * policy_version;
  /** Nodes and attributes charged to the work bound. */
  size_t work_units;
} GTEXT_HTML_Sanitize_Result;

/** Return the conservative defaults (links on; images/tables off). */
GTEXT_API GTEXT_HTML_Sanitize_Options gtext_html_sanitize_options_default(void);

/**
 * Parse and sanitize an HTML fragment in a synthetic HTML `div` flow context.
 * On success `doc` owns a fresh fragment tree; free it with
 * gtext_markup_document_free(). Both output pointers are written only on
 * success. No safety guarantee applies after the returned tree is mutated.
 */
GTEXT_API GTEXT_HTML_Status gtext_html_sanitize_fragment(const void * data,
    size_t length, const GTEXT_HTML_Sanitize_Options * options,
    GTEXT_Markup_Document ** doc, GTEXT_Markup_Node ** fragment,
    GTEXT_HTML_Sanitize_Result * result);

/**
 * Sanitize and serialize atomically. The returned bytes are safe only for
 * HTML body/flow insertion and only as emitted by this call. Free with
 * gtext_html_sanitize_buffer_free().
 */
GTEXT_API GTEXT_HTML_Status gtext_html_sanitize_buffer(const void * data,
    size_t length, const GTEXT_HTML_Sanitize_Options * options,
    char ** output, size_t * output_length,
    GTEXT_HTML_Sanitize_Result * result);

/** Free a buffer returned by gtext_html_sanitize_buffer(). */
GTEXT_API void gtext_html_sanitize_buffer_free(char * output);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GTEXT_HTML_HTML_SANITIZE_H */
