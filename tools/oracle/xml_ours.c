/**
 * @file
 *
 * This library's answer to the questions tools/oracle/containers/xml/driver.c
 * puts to libxml2 and expat, in the same framed, hex-encoded protocol, so that
 * tools/oracle/xml_diff.py compares two event lists and not two texts.
 *
 * Read `<len>\n` then that many bytes, repeated; `-1\n` ends the stream. Per
 * document:
 *
 *     BEGIN
 *     OURS ACCEPT | REFUSE <status> <offset> <line> <column> | DECLINE <status> ...
 *     [DUMP ... ENDDUMP]                 only on ACCEPT
 *     END
 *
 * **REFUSE and DECLINE are different statements.** REFUSE is a well-formedness
 * verdict: the document is not XML. DECLINE is a statement about the parser: an
 * encoding or version it does not read, a limit, an allocation. A DECLINE says
 * nothing about the document, and the comparison keeps the two apart.
 *
 * The dump lines are the ones the reference driver prints (see its header). A
 * namespace declaration, which this library keeps as an attribute in the xmlns
 * namespace, is printed as an `N` line, because that is what the references do.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/xml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_hex(const char * bytes, size_t len) {
  if (!bytes) {
    fputs("-", stdout);
    return;
  }
  if (!len) {
    fputs(".", stdout);
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char) bytes[i]);
  }
}

static void put_str(const char * s) {
  put_hex(s, s ? strlen(s) : 0);
}

#define NAME(x) case x: return #x

static const char * name_of(GTEXT_XML_Status s) {
  switch (s) {
    NAME(GTEXT_XML_OK);
    NAME(GTEXT_XML_E_INVALID);
    NAME(GTEXT_XML_E_OOM);
    NAME(GTEXT_XML_E_SYNTAX);
    NAME(GTEXT_XML_E_UNEXPECTED_EOF);
    NAME(GTEXT_XML_E_UTF8);
    NAME(GTEXT_XML_E_CHARACTER);
    NAME(GTEXT_XML_E_MISMATCHED_TAG);
    NAME(GTEXT_XML_E_DUPLICATE_ATTRIBUTE);
    NAME(GTEXT_XML_E_ENTITY);
    NAME(GTEXT_XML_E_NAMESPACE);
    NAME(GTEXT_XML_E_DOCTYPE);
    NAME(GTEXT_XML_E_UNSUPPORTED_ENCODING);
    NAME(GTEXT_XML_E_UNSUPPORTED_VERSION);
    NAME(GTEXT_XML_E_LIMIT_INPUT);
    NAME(GTEXT_XML_E_LIMIT_DEPTH);
    NAME(GTEXT_XML_E_LIMIT_ATTRIBUTES);
    NAME(GTEXT_XML_E_LIMIT_VALUE);
    NAME(GTEXT_XML_E_LIMIT_NAME);
    NAME(GTEXT_XML_E_LIMIT_NODES);
    NAME(GTEXT_XML_E_LIMIT_NAMESPACES);
    NAME(GTEXT_XML_E_INVALID_SEQUENCE);
    NAME(GTEXT_XML_E_ENCODING_MISMATCH);
    default: return "GTEXT_XML_E_OTHER";
  }
}

static int is_verdict(GTEXT_XML_Status s) {
  switch (s) {
    case GTEXT_XML_E_SYNTAX:
    case GTEXT_XML_E_UNEXPECTED_EOF:
    case GTEXT_XML_E_UTF8:
    case GTEXT_XML_E_CHARACTER:
    case GTEXT_XML_E_MISMATCHED_TAG:
    case GTEXT_XML_E_DUPLICATE_ATTRIBUTE:
    case GTEXT_XML_E_ENTITY:
    case GTEXT_XML_E_NAMESPACE:
    case GTEXT_XML_E_DOCTYPE:
    case GTEXT_XML_E_INVALID_SEQUENCE:
    case GTEXT_XML_E_ENCODING_MISMATCH:
      return 1;
    default:
      return 0;
  }
}

static void put_data(const char * tag, GTEXT_Markup_Node * n) {
  size_t len = 0;
  const char * d = gtext_markup_node_data(n, &len);
  printf("%s ", tag);
  put_hex(d ? d : "", len);
  printf("\n");
}

static void dump(GTEXT_Markup_Document * doc) {
  printf("DUMP\n");
  GTEXT_Markup_Standalone sa = GTEXT_MARKUP_STANDALONE_UNSPECIFIED;
  const char * version = gtext_markup_document_xml_version(doc, &sa);
  if (version) {
    printf("X ");
    put_str(version);
    printf(" %d\n", sa == GTEXT_MARKUP_STANDALONE_UNSPECIFIED ? -1 : sa == GTEXT_MARKUP_STANDALONE_YES ? 1 : 0);
  }
  GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
  GTEXT_Markup_Node * n = gtext_markup_node_first_child(root);
  while (n) {
    switch (gtext_markup_node_kind(n)) {
      case GTEXT_MARKUP_KIND_DOCTYPE:
        printf("D ");
        put_str(gtext_markup_doctype_name(n));
        printf(" ");
        put_str(gtext_markup_doctype_public_id(n));
        printf(" ");
        put_str(gtext_markup_doctype_system_id(n));
        printf("\n");
        break;
      case GTEXT_MARKUP_KIND_ELEMENT: {
        printf("S ");
        put_str(gtext_markup_element_namespace(n));
        printf(" ");
        put_str(gtext_markup_element_local_name(n));
        printf(" ");
        put_str(gtext_markup_element_prefix(n));
        printf("\n");
        const size_t count = gtext_markup_element_attribute_count(n);
        for (size_t i = 0; i < count; i++) {
          GTEXT_Markup_Attribute a;
          gtext_markup_element_attribute_at(n, i, &a);
          if (a.ns && strcmp(a.ns, GTEXT_MARKUP_NS_XMLNS) == 0) {
            /* xmlns="u" is prefix NULL and local xmlns; xmlns:p="u" is prefix
               xmlns and local p. */
            printf("N ");
            if (a.prefix) {
              put_str(a.local);
            }
            else {
              put_str(NULL);
            }
            printf(" ");
            put_hex(a.value, a.value_length);
            printf("\n");
          }
          else {
            printf("A ");
            put_str(a.ns);
            printf(" ");
            put_str(a.local);
            printf(" ");
            put_str(a.prefix);
            printf(" ");
            put_hex(a.value, a.value_length);
            printf("\n");
          }
        }
        break;
      }
      case GTEXT_MARKUP_KIND_TEXT:
        put_data("T", n);
        break;
      case GTEXT_MARKUP_KIND_CDATA:
        put_data("C", n);
        break;
      case GTEXT_MARKUP_KIND_COMMENT:
        put_data("M", n);
        break;
      case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
        size_t len = 0;
        const char * d = gtext_markup_node_data(n, &len);
        printf("P ");
        put_str(gtext_markup_pi_target(n));
        printf(" ");
        put_hex(d ? d : "", len);
        printf("\n");
        break;
      }
      default:
        printf("R ");
        put_str("?");
        printf("\n");
        break;
    }
    GTEXT_Markup_Node * c = gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
        ? gtext_markup_node_first_child(n) : NULL;
    if (c) {
      n = c;
      continue;
    }
    for (;;) {
      if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
          && !gtext_markup_node_first_child(n)) {
        printf("E\n");
      }
      GTEXT_Markup_Node * next = gtext_markup_node_next_sibling(n);
      if (next) {
        n = next;
        break;
      }
      n = gtext_markup_node_parent(n);
      if (!n || n == root) {
        n = NULL;
        break;
      }
      printf("E\n");
    }
  }
  printf("ENDDUMP\n");
}

int main(void) {
  char header[64];
  while (fgets(header, sizeof(header), stdin)) {
    long len = strtol(header, NULL, 10);
    if (len < 0) {
      break;
    }
    char * data = malloc((size_t) len + 1);
    if (!data) {
      return 3;
    }
    if (len && fread(data, 1, (size_t) len, stdin) != (size_t) len) {
      free(data);
      return 3;
    }
    data[len] = '\0';
    printf("BEGIN\n");
    GTEXT_Markup_Document * doc = NULL;
    GTEXT_XML_Error err;
    memset(&err, 0, sizeof(err));
    GTEXT_XML_Status st = gtext_xml_parse(data, (size_t) len, NULL, &doc, &err);
    if (st == GTEXT_XML_OK) {
      printf("OURS ACCEPT\n");
      dump(doc);
      gtext_markup_document_free(doc);
    }
    else {
      printf("OURS %s %s %zu %zu %zu\n", is_verdict(st) ? "REFUSE" : "DECLINE",
          name_of(st), err.byte_offset, err.line, err.column);
    }
    gtext_xml_error_free(&err);
    printf("END\n");
    free(data);
  }
  return 0;
}
