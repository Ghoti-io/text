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
/* This library's whole HTML pipeline through the public API only, for tools/oracle/html_pipeline_diff.py: one request per line, one
 * answer per line.
 *
 *   <mode> <transport|-> <c|t> <default|-> <ignore_meta 0|1> x<hex bytes>
 *
 * Modes `sniff`, `parse`, `bytes` and `split` are html_sniff_ours.c's (the encoding decision alone; the same answer line), so that the
 * label tables of the sniffing differential can be asked here too. The two modes of this file:
 *
 *   doc:<S>             parse the bytes as a document (gtext_html_parse(); the scripting flag is S, 0 or 1)
 *   frag:<S>:x<ctx>     parse the bytes as a fragment (gtext_html_parse_fragment()) in the context element whose text is the hex
 *                       (`td`, `svg path`, `math mi`: a namespace word and a name, or an HTML name)
 *
 * and the answer is, in one line,
 *
 *   OK <encoding> <source> <confidence> <bom length> <reparsed> <ignored flags> <dump> <text> <serialized> <dump2> <serialized2>
 *
 * every field after the flags a hex string with a leading `x`: `dump` the tree in the tree-construction dump format (the document's
 * children, or the fragment's), `text` what the Encoding module decodes the bytes after the byte order mark to under the encoding the
 * parser ended with (UTF-8, replacement for ill-formed input), `serialized` the tree written by gtext_html_serialize() at its defaults
 * (the children of the document or of the fragment), `dump2` the tree of parsing `serialized` again (UTF-8 under a certain label, the
 * same flag and context), `serialized2` that tree written. Or `error <status>` (a limit, or a failure that content never causes).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/encoding.h>
#include <ghoti.io/text/html.h>

#include "../conformance/html_tree_dump.h"

static int unhex(int c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static bool from_hex(const char * s, unsigned char ** out, size_t * n, bool x_prefix) {
  *out = NULL;
  *n = 0;
  if (!x_prefix && strcmp(s, "-") == 0) {
    return true;
  }
  if (!x_prefix && strcmp(s, "=") == 0) {
    s = "";
  }
  if (x_prefix) {
    if (*s != 'x') {
      return false;
    }
    s++;
  }
  const size_t digits = strlen(s);
  if (digits % 2 != 0) {
    return false;
  }
  unsigned char * b = malloc(digits / 2 + 1);
  if (!b) {
    return false;
  }
  for (size_t i = 0; i < digits / 2; i++) {
    const int hi = unhex(s[2 * i]);
    const int lo = unhex(s[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      free(b);
      return false;
    }
    b[i] = (unsigned char)(hi * 16 + lo);
  }
  b[digits / 2] = 0;
  *out = b;
  *n = digits / 2;
  return true;
}

static void put_hex(const void * p, size_t n) {
  static const char d[] = "0123456789abcdef";
  const unsigned char * b = p;
  putchar('x');
  for (size_t i = 0; i < n; i++) {
    putchar(d[b[i] >> 4]);
    putchar(d[b[i] & 15]);
  }
}

typedef struct {
  GTEXT_HTML_Namespace ns;
  char name[256];
  bool present;
} Context;

static bool parse_context(const char * hex, Context * c) {
  unsigned char * b = NULL;
  size_t n = 0;
  if (!from_hex(hex, &b, &n, true) || n == 0 || n >= sizeof(c->name)) {
    free(b);
    return false;
  }
  c->present = true;
  c->ns = GTEXT_HTML_NS_HTML;
  const char * name = (const char *)b;
  if (n > 4 && memcmp(b, "svg ", 4) == 0) {
    c->ns = GTEXT_HTML_NS_SVG;
    name += 4;
  }
  else if (n > 5 && memcmp(b, "math ", 5) == 0) {
    c->ns = GTEXT_HTML_NS_MATHML;
    name += 5;
  }
  snprintf(c->name, sizeof(c->name), "%s", name);
  free(b);
  return true;
}

static GTEXT_HTML_Parse_Options base_options(bool scripting) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.scripting = scripting;
  o.max_errors = 1;
  return o;
}

/* Parse; on OK the document (and for a fragment the fragment node) is returned. */
static GTEXT_HTML_Status do_parse(const unsigned char * bytes, size_t n, const GTEXT_HTML_Parse_Options * o, const Context * ctx,
    GTEXT_Markup_Document ** doc, GTEXT_Markup_Node ** frag, GTEXT_HTML_Result * r) {
  memset(r, 0, sizeof(*r));
  *doc = NULL;
  *frag = NULL;
  if (ctx && ctx->present) {
    GTEXT_HTML_Fragment_Context fc;
    memset(&fc, 0, sizeof(fc));
    fc.ns = ctx->ns;
    fc.local_name = ctx->name;
    fc.form_ancestor = strcmp(ctx->name, "form") == 0;
    return gtext_html_parse_fragment(&fc, bytes, n, o, doc, frag, r);
  }
  return gtext_html_parse(bytes, n, o, doc, r);
}

static char * dump_of(GTEXT_Markup_Document * doc, GTEXT_Markup_Node * frag, size_t * n) {
  return frag ? htd_dump_children(frag, n) : htd_dump(doc, n);
}

static GTEXT_HTML_Status serialize_of(GTEXT_Markup_Document * doc, GTEXT_Markup_Node * frag, bool scripting, char ** out, size_t * n) {
  const GTEXT_Markup_Node * top = frag ? frag : gtext_markup_document_root(doc);
  GTEXT_HTML_Serialize_Options so = gtext_html_serialize_options_default();
  so.scripting = scripting; /* whether the text of a noscript is raw text depends on the flag the document was parsed with */
  return gtext_html_serialize_buffer(top, GTEXT_HTML_SERIALIZE_CHILDREN, &so, out, n, NULL);
}

static int run_pipe(const char * mode, const unsigned char * tl, size_t tn, bool certain, const unsigned char * dl, bool ignore,
    const unsigned char * bytes, size_t n) {
  (void)tn;
  bool scripting = mode[4] == '1';
  Context ctx;
  memset(&ctx, 0, sizeof(ctx));
  if (strncmp(mode, "frag:", 5) == 0) {
    scripting = mode[5] == '1';
    const char * colon = strchr(mode + 6, ':');
    if (!colon || !parse_context(colon + 1, &ctx)) {
      return -1;
    }
  }
  GTEXT_HTML_Parse_Options o = base_options(scripting);
  o.transport_encoding = (const char *)tl;
  o.transport_encoding_certainty = certain ? GTEXT_HTML_CERTAIN : GTEXT_HTML_TENTATIVE;
  o.default_encoding = (const char *)dl;
  o.ignore_meta = ignore;
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * frag = NULL;
  GTEXT_HTML_Result r;
  GTEXT_HTML_Status st = do_parse(bytes, n, &o, &ctx, &doc, &frag, &r);
  if (st != GTEXT_HTML_OK) {
    printf("error %s\n", gtext_html_status_string(st));
    gtext_html_result_free(&r);
    return 0;
  }
  size_t dn = 0, sn = 0;
  char * dump = dump_of(doc, frag, &dn);
  char * ser = NULL;
  st = serialize_of(doc, frag, scripting, &ser, &sn);
  if (!dump || st != GTEXT_HTML_OK) {
    printf("error serialize-%s\n", gtext_html_status_string(st));
    free(dump);
    gtext_markup_document_free(doc);
    gtext_html_result_free(&r);
    return 0;
  }
  /* The text: the bytes after the mark, decoded as the parser ended up reading them. */
  char * text = NULL;
  size_t tx = 0;
  const GTEXT_Encoding e = r.encoding ? gtext_encoding_for_label(r.encoding, strlen(r.encoding)) : GTEXT_ENCODING_NONE;
  if (e != GTEXT_ENCODING_NONE) {
    GTEXT_Decoder_Options dopts = gtext_decoder_options_default();
    const size_t skip = r.bom_length <= n ? r.bom_length : 0;
    if (gtext_encoding_decode_buffer(e, &dopts, bytes + skip, n - skip, &text, &tx, NULL) != GTEXT_ENCODING_OK) {
      text = NULL;
    }
  }
  /* ... and the serialization parsed again, and written again. */
  GTEXT_HTML_Parse_Options o2 = base_options(scripting);
  o2.transport_encoding = "utf-8";
  o2.transport_encoding_certainty = GTEXT_HTML_CERTAIN;
  GTEXT_Markup_Document * doc2 = NULL;
  GTEXT_Markup_Node * frag2 = NULL;
  GTEXT_HTML_Result r2;
  st = do_parse((const unsigned char *)ser, sn, &o2, &ctx, &doc2, &frag2, &r2);
  size_t d2n = 0, s2n = 0;
  char * dump2 = NULL;
  char * ser2 = NULL;
  if (st == GTEXT_HTML_OK) {
    dump2 = dump_of(doc2, frag2, &d2n);
    if (serialize_of(doc2, frag2, scripting, &ser2, &s2n) != GTEXT_HTML_OK) {
      ser2 = NULL;
    }
  }
  printf("OK %s %d %d %zu %d %d%d ", r.encoding ? r.encoding : "-", (int)r.encoding_source, (int)r.encoding_confidence, r.bom_length,
      r.reparsed ? 1 : 0, r.transport_encoding_ignored ? 1 : 0, r.default_encoding_ignored ? 1 : 0);
  put_hex(dump, dn);
  putchar(' ');
  put_hex(text ? text : "", text ? tx : 0);
  putchar(' ');
  put_hex(ser, sn);
  putchar(' ');
  if (dump2) {
    put_hex(dump2, d2n);
  }
  else {
    printf("-");
  }
  putchar(' ');
  if (ser2) {
    put_hex(ser2, s2n);
  }
  else {
    printf("-");
  }
  putchar('\n');
  free(dump);
  free(dump2);
  gtext_html_serialize_buffer_free(doc, NULL, ser);
  if (ser2) {
    gtext_html_serialize_buffer_free(doc2, NULL, ser2);
  }
  gtext_encoding_buffer_free(text);
  gtext_markup_document_free(doc);
  gtext_markup_document_free(doc2);
  gtext_html_result_free(&r);
  if (st == GTEXT_HTML_OK) {
    gtext_html_result_free(&r2);
  }
  return 0;
}

static int run_request(char * line) {
  char * mode = strtok(line, " \n");
  char * transport = strtok(NULL, " \n");
  char * certainty = strtok(NULL, " \n");
  char * fallback = strtok(NULL, " \n");
  char * ignore = strtok(NULL, " \n");
  char * doc = strtok(NULL, " \n");
  if (!mode || !transport || !certainty || !fallback || !ignore || !doc || strtok(NULL, " \n")) {
    return -1;
  }
  unsigned char * tl = NULL;
  unsigned char * dl = NULL;
  unsigned char * bytes = NULL;
  size_t tn = 0, dn = 0, n = 0;
  if (!from_hex(transport, &tl, &tn, false) || !from_hex(fallback, &dl, &dn, false) || !from_hex(doc, &bytes, &n, true)
      || (strcmp(certainty, "c") != 0 && strcmp(certainty, "t") != 0) || (strcmp(ignore, "0") != 0 && strcmp(ignore, "1") != 0)) {
    free(tl);
    free(dl);
    free(bytes);
    return -1;
  }
  int rc = 0;
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.transport_encoding = (const char *)tl;
  o.transport_encoding_certainty = certainty[0] == 'c' ? GTEXT_HTML_CERTAIN : GTEXT_HTML_TENTATIVE;
  o.default_encoding = (const char *)dl;
  o.ignore_meta = ignore[0] == '1';
  if (strcmp(mode, "sniff") == 0) {
    GTEXT_HTML_Sniff_Result r;
    const GTEXT_HTML_Status st = gtext_html_sniff_encoding(bytes, n, &o, &r);
    if (st != GTEXT_HTML_OK) {
      printf("error %s\n", gtext_html_status_string(st));
    }
    else {
      printf("%s %d %d %zu 0 %d%d\n", r.encoding_name, (int)r.source, (int)r.confidence, r.bom_length,
          r.transport_encoding_ignored ? 1 : 0, r.default_encoding_ignored ? 1 : 0);
    }
  }
  else if (strncmp(mode, "doc:", 4) == 0 || strncmp(mode, "frag:", 5) == 0) {
    rc = run_pipe(mode, tl, tn, certainty[0] == 'c', dl, ignore[0] == '1', bytes, n);
  }
  else {
    rc = -1;
  }
  free(tl);
  free(dl);
  free(bytes);
  return rc;
}

int main(void) {
  char * line = NULL;
  size_t cap = 0;
  ssize_t got;
  setvbuf(stdout, NULL, _IOFBF, 1 << 16);
  while ((got = getline(&line, &cap, stdin)) > 0) {
    if (run_request(line) != 0) {
      printf("error E_REQUEST\n");
    }
    fflush(stdout);
  }
  fflush(stdout);
  free(line);
  return 0;
}
