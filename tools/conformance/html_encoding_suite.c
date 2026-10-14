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
/* The HTML encoding vectors, driven through the PUBLIC API, one request per line.
 *
 *   html-encoding-runner
 *
 * A request is
 *
 *   <mode> x<hex input>
 *
 * with `sniff` (gtext_html_sniff_encoding(): the prescan alone, what html5lib's input stream decides) or `parse` (the whole parser:
 * the encoding it ends with, after a restart if there was one, what html5lib's `HTMLParser.documentEncoding` reports), `parser-bytes`
 * (the incremental parser fed a byte at a time), `parser-split:<n>` (two feeds cut at n). The caller's default is windows-1252, as
 * the vectors' own first test says ("you may have to set your defaults appropriately"). The answer is
 *
 *   ENCODING <name>      the Encoding Standard's name
 *   SOURCE <n>           GTEXT_Markup_Encoding_Source
 *   CONFIDENCE <n>       GTEXT_HTML_Encoding_Certainty
 *   REPARSED <0|1>
 *   END DONE
 *
 * (or END E_REQUEST for a request that is not understood, END <status> for a parse that failed).
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

static unsigned char * from_hex(const char * s, size_t * n) {
  if (!s || *s != 'x') {
    return NULL;
  }
  s++;
  const size_t digits = strlen(s);
  if (digits % 2 != 0) {
    return NULL;
  }
  const size_t len = digits / 2;
  unsigned char * out = malloc(len + 1);
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < len; i++) {
    const int hi = unhex(s[2 * i]);
    const int lo = unhex(s[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      free(out);
      return NULL;
    }
    out[i] = (unsigned char)(hi * 16 + lo);
  }
  out[len] = 0;
  *n = len;
  return out;
}

static int run_request(char * line) {
  char * mode = strtok(line, " \n");
  char * hex = strtok(NULL, " \n");
  if (!mode || !hex || strtok(NULL, " \n")) {
    return -1;
  }
  size_t n = 0;
  unsigned char * input = from_hex(hex, &n);
  if (!input) {
    return -1;
  }
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.default_encoding = "windows-1252";
  if (strcmp(mode, "sniff") == 0) {
    GTEXT_HTML_Sniff_Result r;
    const GTEXT_HTML_Status st = gtext_html_sniff_encoding(input, n, &opts, &r);
    if (st != GTEXT_HTML_OK) {
      printf("END %s\n", gtext_html_status_string(st));
      free(input);
      return 0;
    }
    printf("ENCODING %s\nSOURCE %d\nCONFIDENCE %d\nREPARSED 0\nEND DONE\n", r.encoding_name, (int)r.source, (int)r.confidence);
    free(input);
    return 0;
  }
  size_t split = (size_t)-1;
  bool bytes = false;
  bool whole = false;
  char * end = NULL;
  if (strcmp(mode, "parse") == 0) {
    whole = true;
  }
  else if (strcmp(mode, "parser-bytes") == 0) {
    bytes = true;
  }
  else if (strncmp(mode, "parser-split:", 13) == 0 && mode[13] >= '0' && mode[13] <= '9') {
    split = (size_t)strtoull(mode + 13, &end, 10);
    if (*end || split > n) {
      free(input);
      return -1;
    }
  }
  else {
    free(input);
    return -1;
  }
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result r;
  memset(&r, 0, sizeof(r));
  GTEXT_HTML_Status st;
  if (whole) {
    st = gtext_html_parse(input, n, &opts, &doc, &r);
  }
  else {
    GTEXT_HTML_Parser * p = NULL;
    st = gtext_html_parser_new(&opts, &p);
    if (st == GTEXT_HTML_OK) {
      if (bytes) {
        for (size_t i = 0; i < n && st == GTEXT_HTML_OK; i++) {
          st = gtext_html_parser_feed(p, input + i, 1);
        }
      }
      else {
        st = gtext_html_parser_feed(p, input, split);
        if (st == GTEXT_HTML_OK) {
          st = gtext_html_parser_feed(p, input + split, n - split);
        }
      }
      const GTEXT_HTML_Status fs = gtext_html_parser_finish(p, &doc, &r);
      if (st == GTEXT_HTML_OK) {
        st = fs;
      }
      gtext_html_parser_free(p);
    }
  }
  if (st != GTEXT_HTML_OK) {
    printf("END %s\n", gtext_html_status_string(st));
  }
  else {
    printf("ENCODING %s\nSOURCE %d\nCONFIDENCE %d\nREPARSED %d\nEND DONE\n", r.encoding, (int)r.encoding_source,
        (int)r.encoding_confidence, r.reparsed ? 1 : 0);
  }
  gtext_markup_document_free(doc);
  gtext_html_result_free(&r);
  free(input);
  return 0;
}

int main(void) {
  char * line = NULL;
  size_t cap = 0;
  ssize_t got;
  setvbuf(stdout, NULL, _IOFBF, 1 << 16);
  while ((got = getline(&line, &cap, stdin)) > 0) {
    if (run_request(line) != 0) {
      printf("END E_REQUEST\n");
    }
    fflush(stdout);
  }
  free(line);
  return 0;
}
