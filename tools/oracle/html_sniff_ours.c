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
/* This library's encoding decision, for tools/oracle/html_sniff_diff.py: one request per line, one answer per line.
 *
 *   <mode> <transport|-> <c|t> <default|-> <ignore_meta 0|1> x<hex document>
 *
 * mode `sniff` is gtext_html_sniff_encoding() (what the parser starts with), `parse` is the whole parser (what it ends with, after a
 * restart), `bytes` the incremental parser fed a byte at a time and `split` two feeds cut in the middle: those two must answer as `parse` does.
 * The labels are hex ("-" for none), because they may be any bytes; `c` says the transport label is certain, `t` that it is tentative.
 * The answer is
 *
 *   <encoding name> <source> <confidence> <bom length> <reparsed> <ignored flags>
 *
 * or `error <status>`.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/html.h>

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

/* Hex to bytes, with a NUL after them; "-" is none (NULL, n = 0). Returns false on a malformed token. */
static bool from_hex(const char * s, unsigned char ** out, size_t * n, bool x_prefix) {
  *out = NULL;
  *n = 0;
  if (!x_prefix && strcmp(s, "-") == 0) {
    return true;
  }
  if (!x_prefix && strcmp(s, "=") == 0) {  /* the empty label, which is a label (an empty token would not survive the split) */
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
  else if (strcmp(mode, "parse") == 0 || strcmp(mode, "bytes") == 0 || strcmp(mode, "split") == 0) {
    GTEXT_Markup_Document * d = NULL;
    GTEXT_HTML_Result r;
    memset(&r, 0, sizeof(r));
    GTEXT_HTML_Status st;
    if (mode[0] == 'p') {
      st = gtext_html_parse(bytes, n, &o, &d, &r);
    }
    else {
      GTEXT_HTML_Parser * p = NULL;
      st = gtext_html_parser_new(&o, &p);
      if (st == GTEXT_HTML_OK) {
        if (mode[0] == 'b') {
          for (size_t i = 0; i < n && st == GTEXT_HTML_OK; i++) {
            st = gtext_html_parser_feed(p, bytes + i, 1);
          }
        }
        else {
          st = gtext_html_parser_feed(p, bytes, n / 2);
          if (st == GTEXT_HTML_OK) {
            st = gtext_html_parser_feed(p, bytes + n / 2, n - n / 2);
          }
        }
        const GTEXT_HTML_Status fs = gtext_html_parser_finish(p, &d, &r);
        if (st == GTEXT_HTML_OK) {
          st = fs;
        }
        gtext_html_parser_free(p);
      }
    }
    if (st != GTEXT_HTML_OK) {
      printf("error %s\n", gtext_html_status_string(st));
    }
    else {
      printf("%s %d %d %zu %d %d%d\n", r.encoding, (int)r.encoding_source, (int)r.encoding_confidence, r.bom_length, r.reparsed ? 1 : 0,
          r.transport_encoding_ignored ? 1 : 0, r.default_encoding_ignored ? 1 : 0);
    }
    gtext_markup_document_free(d);
    gtext_html_result_free(&r);
  }
  else {
    free(tl);
    free(dl);
    free(bytes);
    return -1;
  }
  free(tl);
  free(dl);
  free(bytes);
  return 0;
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
    fflush(stdout);  /* a question asked of the runner while it runs (the label table) is answered at once */
  }
  fflush(stdout);
  free(line);
  return 0;
}
