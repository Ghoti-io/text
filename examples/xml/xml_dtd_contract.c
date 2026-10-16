/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 */

#include <ghoti.io/text/xml.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const unsigned char * bytes;
  size_t length;
  size_t offset;
  size_t close_count;
} Memory_Source;

static const unsigned char external_subset[] =
    "<!ENTITY label 'resolved by the caller'>";
static const int resource_identity = 0;

static GTEXT_XML_Source_Read_Result read_source(void * handle,
    unsigned char * buffer, size_t capacity, size_t * bytes_read) {
  Memory_Source * source = handle;
  size_t remaining = source->length - source->offset;
  size_t amount = remaining < capacity ? remaining : capacity;
  if (amount == 0) {
    *bytes_read = 0;
    return GTEXT_XML_SOURCE_EOF;
  }
  memcpy(buffer, source->bytes + source->offset, amount);
  source->offset += amount;
  *bytes_read = amount;
  return GTEXT_XML_SOURCE_DATA;
}

static void close_source(void * handle) {
  Memory_Source * source = handle;
  source->close_count++;
}

/* The application maps exactly one known identifier to a fixed in-memory DTD.
 * This callback performs no filesystem, URL or network lookup. */
static GTEXT_XML_Resolver_Result resolve_known_subset(
    const GTEXT_XML_Resolve_Request * request, void * user_data,
    GTEXT_XML_External_Source * out_source) {
  Memory_Source * source = user_data;
  if (request->kind != GTEXT_XML_RESOURCE_EXTERNAL_SUBSET ||
      request->system_id == NULL ||
      strcmp(request->system_id, "urn:example:story-2-16.dtd") != 0) {
    return GTEXT_XML_RESOLVER_DECLINED;
  }

  source->bytes = external_subset;
  source->length = sizeof(external_subset) - 1;
  source->offset = 0;
  out_source->handle = source;
  out_source->identity = &resource_identity;
  out_source->base_uri = "urn:example:story-2-16.dtd";
  out_source->read = read_source;
  out_source->close = close_source;
  return GTEXT_XML_RESOLVER_OPENED;
}

int main(void) {
  static const char dtd_off_document[] =
      "<!DOCTYPE message [<!ENTITY label 'not expanded'>]>"
      "<message>&label;</message>";
  static const char resolved_document[] =
      "<?xml version='1.1'?>"
      "<!DOCTYPE message SYSTEM 'urn:example:story-2-16.dtd'>"
      "<message>&label;</message>";
  GTEXT_Markup_Document * document = NULL;
  GTEXT_XML_Error error;

  /* NULL options select DTD_OFF. No tree is published when that mode
   * encounters a non-predefined entity reference. */
  GTEXT_XML_Status status = gtext_xml_parse(dtd_off_document,
      sizeof(dtd_off_document) - 1, NULL, &document, &error);
  if (status != GTEXT_XML_E_ENTITY || document != NULL) {
    fprintf(stderr, "DTD-off contract failed: %s\n",
        gtext_xml_status_string(status));
    gtext_xml_error_free(&error);
    return 1;
  }
  gtext_xml_error_free(&error);

  Memory_Source source = {0};
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  options.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  options.resolver = resolve_known_subset;
  options.resolver_user_data = &source;
  options.base_uri = "urn:example:document.xml";

  status = gtext_xml_parse(resolved_document, sizeof(resolved_document) - 1,
      &options, &document, &error);
  if (status != GTEXT_XML_OK) {
    fprintf(stderr, "DTD processing failed at %zu:%zu: %s\n", error.line,
        error.column, error.message);
    gtext_xml_error_free(&error);
    return 1;
  }

  GTEXT_Markup_Node * root = gtext_markup_document_element(document);
  char * text = NULL;
  size_t text_length = 0;
  GTEXT_Markup_Status markup_status = gtext_markup_node_text_content(root,
      NULL, &text, &text_length);
  const char * version = gtext_markup_document_xml_version(document, NULL);
  int ok = markup_status == GTEXT_MARKUP_OK && version != NULL &&
      strcmp(version, "1.1") == 0 && text_length == strlen("resolved by the caller") &&
      strcmp(text, "resolved by the caller") == 0 && source.close_count == 1;

  if (!ok) {
    fprintf(stderr, "resolved DTD result or source cleanup contract failed\n");
  } else {
    printf("XML %s: %s; resolver source closed %zu time\n", version, text,
        source.close_count);
  }

  gtext_markup_string_free(NULL, text);
  gtext_markup_document_free(document);
  gtext_xml_error_free(&error);
  return ok ? 0 : 1;
}
