/* Read an XML document on stdin; print what the parser did with it.
 *
 *   xmlconf-runner [dom | whole | bytes]
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
 *
 * `dom` (the default) is gtext_xml_parse(). `whole` and `bytes` are the event
 * parser (xml_stream.h), fed the whole input in one call or one byte at a time.
 * They print OK only if the events, replayed into a tree, give a tree equal to
 * gtext_xml_parse()'s - every name, value and location - and the same line
 * otherwise, so the script can hold all three to one answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/text/xml.h>

#include "../../src/markup/markup_internal.h"
#include "../../tests/xml_replay.h"

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
    NAME(GTEXT_XML_E_INVALID_SEQUENCE);
    NAME(GTEXT_XML_E_ENCODING_MISMATCH);
    NAME(GTEXT_XML_E_INCOMPLETE);
    NAME(GTEXT_XML_E_STATE);
  }
  return "GTEXT_XML_E_UNKNOWN";
}

static void print_failure(GTEXT_XML_Status status, const GTEXT_XML_Error *err) {
  printf("FAIL %s %zu %zu %zu %s\n", name_of(status), err->byte_offset,
      err->line, err->column, err->message ? err->message : "?");
}

/* The input through the event parser, whole or a byte at a time, replayed. */
static void run_stream(const char *buf, size_t n, int by_bytes) {
  xml_replay replay;
  xml_replay_init(&replay);
  GTEXT_XML_Error err;
  memset(&err, 0, sizeof(err));
  GTEXT_XML_Stream *stream = gtext_xml_stream_new(NULL, xml_replay_event, &replay);
  if (!stream) {
    printf("FAIL GTEXT_XML_E_OOM 0 0 0 runner\n");
    return;
  }
  GTEXT_XML_Status status = GTEXT_XML_OK;
  if (by_bytes) {
    for (size_t i = 0; i < n && status == GTEXT_XML_OK; i++) {
      status = gtext_xml_stream_feed(stream, buf + i, 1, &err);
    }
  }
  else if (n > 0) {
    status = gtext_xml_stream_feed(stream, buf, n, &err);
  }
  if (status == GTEXT_XML_OK) {
    status = gtext_xml_stream_finish(stream, &err);
  }
  gtext_xml_stream_free(stream);
  if (status != GTEXT_XML_OK) {
    print_failure(status, &err);
    xml_replay_free(&replay);
    return;
  }
  if (replay.problem || !replay.doc || !replay.ended) {
    printf("FAIL INVARIANT 0 0 0 replay: %s\n",
        replay.problem ? replay.problem : "no document");
    xml_replay_free(&replay);
    return;
  }
  GTEXT_Markup_Document *dom = NULL;
  GTEXT_XML_Error dom_err;
  if (gtext_xml_parse(buf, n, NULL, &dom, &dom_err) != GTEXT_XML_OK) {
    printf("FAIL INVARIANT 0 0 0 the events were accepted and the DOM parse refused\n");
    xml_replay_free(&replay);
    return;
  }
  const char *problem = markup_check_invariants(replay.doc);
  if (!problem) {
    problem = xml_replay_compare(replay.doc, dom);
    if (problem) {
      printf("FAIL INVARIANT 0 0 0 replayed tree differs from the DOM parse: %s\n", problem);
      problem = "reported";
    }
  }
  else {
    printf("FAIL INVARIANT 0 0 0 %s\n", problem);
    problem = "reported";
  }
  if (!problem) {
    printf("OK\n");
  }
  gtext_markup_document_free(dom);
  xml_replay_free(&replay);
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "dom";
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

  if (strcmp(mode, "whole") == 0 || strcmp(mode, "bytes") == 0) {
    run_stream(buf, n, mode[0] == 'b');
    free(buf);
    return 0;
  }

  GTEXT_Markup_Document *doc = NULL;
  GTEXT_XML_Error err;
  const GTEXT_XML_Status status = gtext_xml_parse(buf, n, NULL, &doc, &err);
  if (status != GTEXT_XML_OK) {
    print_failure(status, &err);
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
