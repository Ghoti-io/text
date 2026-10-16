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

/* The accepted document written in every output encoding and read back: the
   tree must be equal, the declaration the same, and writing what was read must
   give the bytes that were read. Latin-1 and US-ASCII may refuse a document
   whose names, comments, instructions or CDATA hold characters they lack, and
   only with that status; everything else must write. */
static const struct {
  GTEXT_XML_Output_Encoding enc;
  const char *name;
  int may_refuse;
} round_trip_configs[] = {
  {GTEXT_XML_OUTPUT_UTF8, "utf-8", 0},
  {GTEXT_XML_OUTPUT_UTF16LE, "utf-16le", 0},
  {GTEXT_XML_OUTPUT_UTF16BE, "utf-16be", 0},
  {GTEXT_XML_OUTPUT_ISO_8859_1, "iso-8859-1", 1},
  {GTEXT_XML_OUTPUT_US_ASCII, "us-ascii", 1},
};

static void run_round_trip(const char *buf, size_t n) {
  GTEXT_Markup_Document *doc = NULL;
  GTEXT_XML_Error err;
  if (gtext_xml_parse(buf, n, NULL, &doc, &err) != GTEXT_XML_OK) {
    print_failure(err.code, &err);
    return;
  }
  int refused = 0;
  for (size_t i = 0; i < sizeof(round_trip_configs) / sizeof(round_trip_configs[0]); i++) {
    GTEXT_XML_Write_Options o = gtext_xml_write_options_default();
    o.encoding = round_trip_configs[i].enc;
    /* This gate checks semantic round-trip equivalence, so keep the source
       document's external identifiers rather than exercising reduced output. */
    o.emit_external_identifiers = true;
    GTEXT_XML_Write_Result r;
    char *out = NULL;
    size_t out_len = 0;
    GTEXT_XML_Status st = gtext_xml_write_buffer(doc, &o, &out, &out_len, &r);
    if (st == GTEXT_XML_E_UNREPRESENTABLE && round_trip_configs[i].may_refuse) {
      refused++;
      continue;
    }
    if (st != GTEXT_XML_OK) {
      printf("FAIL ROUNDTRIP 0 0 0 %s: write refused: %s at %s\n", round_trip_configs[i].name,
          r.message ? r.message : "?", r.path);
      gtext_markup_document_free(doc);
      return;
    }
    GTEXT_Markup_Document *back = NULL;
    st = gtext_xml_parse(out, out_len, NULL, &back, &err);
    const char *why = NULL;
    if (round_trip_configs[i].enc == GTEXT_XML_OUTPUT_UTF8) {
      /* A UTF-8 document that began with a byte order mark is written with one,
         and one that did not is not given one. */
      const int in_bom = n >= 3 && memcmp(buf, "\xEF\xBB\xBF", 3) == 0;
      const int out_bom = out_len >= 3 && memcmp(out, "\xEF\xBB\xBF", 3) == 0;
      if (in_bom && !out_bom) {
        why = "the UTF-8 byte order mark was not kept";
      }
      else if (!in_bom && out_bom) {
        why = "a UTF-8 byte order mark was added";
      }
    }
    if (why) {
      /* reported below */
    }
    else if (st != GTEXT_XML_OK) {
      why = "the output does not parse";
    }
    else if (!gtext_markup_equal(gtext_markup_document_root(doc), gtext_markup_document_root(back))) {
      why = "the output parses to a different tree";
    }
    else {
      GTEXT_Markup_Standalone sa, sb;
      const char *va = gtext_markup_document_xml_version(doc, &sa);
      const char *vb = gtext_markup_document_xml_version(back, &sb);
      /* A document with no declaration comes back with one only in ISO-8859-1,
         whose output can hold bytes that are not UTF-8 and so must say what it
         is; the version it says is 1.0. Everywhere else it comes back as it
         was. */
      const int forced = va == NULL && round_trip_configs[i].enc == GTEXT_XML_OUTPUT_ISO_8859_1;
      if (forced) {
        if (!vb || strcmp(vb, "1.0") != 0 || sb != GTEXT_MARKUP_STANDALONE_UNSPECIFIED) {
          why = "the declaration written for a document with none is not version 1.0";
        }
      }
      else if (sa != sb || (va == NULL) != (vb == NULL) || (va && strcmp(va, vb) != 0)) {
        why = "the XML declaration differs";
      }
      if (!why) {
        char *again = NULL;
        size_t again_len = 0;
        st = gtext_xml_write_buffer(back, &o, &again, &again_len, &r);
        if (st != GTEXT_XML_OK) {
          why = "the tree read back cannot be written";
        }
        else if (again_len != out_len || memcmp(again, out, out_len) != 0) {
          why = "writing what was read is not the same bytes";
        }
        gtext_xml_write_buffer_free(back, &o, again);
      }
    }
    gtext_xml_write_buffer_free(doc, &o, out);
    if (back) {
      gtext_markup_document_free(back);
    }
    if (why) {
      printf("FAIL ROUNDTRIP 0 0 0 %s: %s\n", round_trip_configs[i].name, why);
      gtext_markup_document_free(doc);
      return;
    }
  }
  gtext_markup_document_free(doc);
  printf("OK %d\n", refused);
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

  if (strcmp(mode, "roundtrip") == 0) {
    run_round_trip(buf, n);
    free(buf);
    return 0;
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
