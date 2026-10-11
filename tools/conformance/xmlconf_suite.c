/* Read an XML document on stdin; print what the parser did with it.
 *
 * One line, for tools/conformance/xmlconf_suite.py to read:
 *
 *   OK
 *   FAIL <GTEXT_XML_E_...> <offset> <line> <column> <message>
 *
 * "OK" is printed only when the tree the parser built also passes the tree's
 * own invariant checker, so a parse that succeeds by building something broken
 * is a failure here and not a pass. The bytes are read whole, as the parser
 * takes them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/text/xml.h>

#include "../../src/markup/markup_internal.h"

#define NAME(x) case x: return #x

static const char *name_of(GTEXT_XML_Status s) {
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
  }
  return "GTEXT_XML_E_UNKNOWN";
}

int main(void) {
  size_t cap = 1 << 16, n = 0;
  char *buf = malloc(cap);
  if (!buf) { printf("FAIL GTEXT_XML_E_OOM 0 0 0 runner\n"); return 0; }
  for (;;) {
    size_t got = fread(buf + n, 1, cap - n, stdin);
    n += got;
    if (n < cap) break;
    cap *= 2;
    char *grown = realloc(buf, cap);
    if (!grown) { printf("FAIL GTEXT_XML_E_OOM 0 0 0 runner\n"); return 0; }
    buf = grown;
  }

  GTEXT_Markup_Document *doc = NULL;
  GTEXT_XML_Error err;
  const GTEXT_XML_Status status = gtext_xml_parse(buf, n, NULL, &doc, &err);
  if (status != GTEXT_XML_OK) {
    printf("FAIL %s %zu %zu %zu %s\n", name_of(status), err.byte_offset,
        err.line, err.column, err.message ? err.message : "?");
    free(buf);
    return 0;
  }
  const char *problem = markup_check_invariants(doc);
  if (problem) {
    printf("FAIL INVARIANT 0 0 0 %s\n", problem);
  }
  else {
    printf("OK\n");
  }
  gtext_markup_document_free(doc);
  free(buf);
  return 0;
}
