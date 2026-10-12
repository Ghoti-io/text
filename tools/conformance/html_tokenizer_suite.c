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

/* The HTML tokenizer, driven from text on stdin, one request per line.
 *
 *   html-tokenizer-runner
 *
 * A request is
 *
 *   <mode> <state> <lasttag> x<hex input> [strict]
 *
 * where `mode` is `whole` (the input in one feed), `bytes` (one byte per feed),
 * `chunks:<seed>` (feeds of 1 to 9 bytes, chosen by a generator from the seed),
 * or `limits:<seed>` (as chunks, with small limits chosen by the seed); `state`
 * is one of data, rcdata, rawtext, script, plaintext, cdata; `lasttag` is `-`
 * (no start tag emitted) or `x<hex of the name>`. The input is WTF-8 (the vectors
 * hold lone surrogates), unless the request ends in `strict`, when it is strict UTF-8. A
 * request that is not exactly this - an unknown mode or state, a seed that is not a number,
 * hex that is odd or has a digit that is not one, a stray word - is answered `END E_REQUEST`
 * and run as nothing. The answer is the token stream and the errors, one line
 * each, ending in `END <status>`:
 *
 *   C x<text>  [W][N]          a run of characters (W: all white space, N: a NUL)
 *   S x<name> <self> <n> (x<name> x<value>)*   start tag
 *   E x<name> <self> <n> (x<name> x<value>)*   end tag
 *   M x<data>                  comment
 *   D <flags> x<name> x<public> x<system>       DOCTYPE; flags are N (has name), P, S, Q (force quirks)
 *   P x<target> x<data>        processing instruction
 *   F                          end of file
 *   X <code> <line> <column> <offset>           a parse error
 *
 * One process answers many requests: the scorer sends thousands.
 *
 * Every chunk is a separate heap block that is freed as soon as the tokenizer
 * has asked for more, so a tokenizer that kept a pointer into a chunk it was
 * told to release is a use-after-free for the sanitizers.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/html/html_tokenizer_internal.h"

static void put_hex(const char * s, size_t n) {
  printf("x");
  for (size_t i = 0; i < n; i++) {
    printf("%02x", (unsigned char)s[i]);
  }
}

static void put_attrs(const GTEXT_HTML_Token * t) {
  printf(" %d %zu", t->self_closing ? 1 : 0, t->attribute_count);
  for (size_t i = 0; i < t->attribute_count; i++) {
    printf(" ");
    put_hex(t->attributes[i].name, t->attributes[i].name_length);
    printf(" ");
    put_hex(t->attributes[i].value, t->attributes[i].value_length);
  }
}

static void put_token(const GTEXT_HTML_Token * t) {
  switch (t->type) {
    case GTEXT_HTML_TOKEN_CHARACTER:
      printf("C ");
      put_hex(t->data, t->data_length);
      if (t->all_whitespace) {
        printf(" W");
      }
      if (t->has_null) {
        printf(" N");
      }
      break;
    case GTEXT_HTML_TOKEN_START_TAG:
      printf("S ");
      put_hex(t->name, t->name_length);
      put_attrs(t);
      break;
    case GTEXT_HTML_TOKEN_END_TAG:
      printf("E ");
      put_hex(t->name, t->name_length);
      put_attrs(t);
      break;
    case GTEXT_HTML_TOKEN_COMMENT:
      printf("M ");
      put_hex(t->data, t->data_length);
      break;
    case GTEXT_HTML_TOKEN_DOCTYPE:
      printf("D %s%s%s%s ", t->has_name ? "N" : "-", t->has_public_id ? "P" : "-",
          t->has_system_id ? "S" : "-", t->force_quirks ? "Q" : "-");
      put_hex(t->name, t->name_length);
      printf(" ");
      put_hex(t->public_id, t->public_id_length);
      printf(" ");
      put_hex(t->system_id, t->system_id_length);
      break;
    case GTEXT_HTML_TOKEN_PROCESSING_INSTRUCTION:
      printf("P ");
      put_hex(t->name, t->name_length);
      printf(" ");
      put_hex(t->data, t->data_length);
      break;
    case GTEXT_HTML_TOKEN_EOF:
      printf("F");
      break;
  }
  printf("\n");
}

static const char * status_name(GTEXT_HTML_TokenizerStatus s) {
  switch (s) {
    case GTEXT_HTML_TOKENIZER_TOKEN: return "TOKEN";
    case GTEXT_HTML_TOKENIZER_NEED_INPUT: return "NEED_INPUT";
    case GTEXT_HTML_TOKENIZER_DONE: return "DONE";
    case GTEXT_HTML_TOKENIZER_OK: return "OK";
    case GTEXT_HTML_TOKENIZER_E_INVALID: return "E_INVALID";
    case GTEXT_HTML_TOKENIZER_E_STATE: return "E_STATE";
    case GTEXT_HTML_TOKENIZER_E_OOM: return "E_OOM";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT: return "E_LIMIT_INPUT";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_NAME: return "E_LIMIT_NAME";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE: return "E_LIMIT_VALUE";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT: return "E_LIMIT_COMMENT";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_TEXT: return "E_LIMIT_TEXT";
    case GTEXT_HTML_TOKENIZER_E_LIMIT_ATTRIBUTES: return "E_LIMIT_ATTRIBUTES";
  }
  return "E_UNKNOWN";
}

/* Print tokens until the tokenizer says something else. */
static GTEXT_HTML_TokenizerStatus drain(GTEXT_HTML_Tokenizer * tok) {
  const GTEXT_HTML_Token * token;
  for (;;) {
    const GTEXT_HTML_TokenizerStatus s = gtext_html_tokenizer_next(tok, &token);
    if (s != GTEXT_HTML_TOKENIZER_TOKEN) {
      return s;
    }
    put_token(token);
  }
}

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

/* "x4142" -> "AB". Returns a malloc'd block and its length, or NULL if the text is not an x and
   an even number of hex digits (or the allocation failed): the request is refused, not guessed at. */
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
  *n = len;
  return out;
}

/* A decimal seed after `prefix`, and nothing else. */
static int parse_seed(const char * text, unsigned long long * out) {
  if (*text < '0' || *text > '9') {
    return -1;
  }
  char * end = NULL;
  *out = strtoull(text, &end, 10);
  return *end == '\0' ? 0 : -1;
}

static unsigned long long lcg(unsigned long long * s) {
  *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
  return *s >> 33;
}

/* 0: answered; -1: the request is malformed, and nothing was run. */
static int run_request(char * line) {
  char * mode = strtok(line, " \n");
  char * state = strtok(NULL, " \n");
  char * last = strtok(NULL, " \n");
  char * hex = strtok(NULL, " \n");
  char * flag = strtok(NULL, " \n");
  if (!mode || !state || !last || !hex || strtok(NULL, " \n")) {
    return -1;
  }
  bool strict = false;
  if (flag) {
    if (strcmp(flag, "strict") != 0) {
      return -1;
    }
    strict = true;
  }
  GTEXT_HTML_TokenizerState st;
  if (strcmp(state, "data") == 0) {
    st = GTEXT_HTML_STATE_DATA;
  }
  else if (strcmp(state, "rcdata") == 0) {
    st = GTEXT_HTML_STATE_RCDATA;
  }
  else if (strcmp(state, "rawtext") == 0) {
    st = GTEXT_HTML_STATE_RAWTEXT;
  }
  else if (strcmp(state, "script") == 0) {
    st = GTEXT_HTML_STATE_SCRIPT_DATA;
  }
  else if (strcmp(state, "plaintext") == 0) {
    st = GTEXT_HTML_STATE_PLAINTEXT;
  }
  else if (strcmp(state, "cdata") == 0) {
    st = GTEXT_HTML_STATE_CDATA_SECTION;
  }
  else {
    return -1;
  }
  size_t n = 0;
  unsigned char * input = from_hex(hex, &n);
  if (!input) {
    return -1;
  }
  size_t ln = 0;
  unsigned char * name = NULL;
  if (strcmp(last, "-") != 0) {
    name = from_hex(last, &ln);
    if (!name) {
      free(input);
      return -1;
    }
  }
  GTEXT_HTML_TokenizerOptions opts;
  memset(&opts, 0, sizeof(opts));
  opts.wtf8 = !strict;
  opts.max_errors = 1u << 20;
  unsigned long long seed = 0;
  int chunked = 0;
  int bad = 0;
  if (strncmp(mode, "chunks:", 7) == 0) {
    bad = parse_seed(mode + 7, &seed);
    chunked = 2;
  }
  else if (strncmp(mode, "limits:", 7) == 0) {
    bad = parse_seed(mode + 7, &seed);
    chunked = 2;
    unsigned long long g = seed * 7919 + 17;
    opts.max_name_bytes = 1 + lcg(&g) % 12;
    opts.max_value_bytes = 1 + lcg(&g) % 24;
    opts.max_comment_bytes = 1 + lcg(&g) % 24;
    opts.max_text_bytes = 1 + lcg(&g) % 48;
    opts.max_attributes = 1 + lcg(&g) % 4;
    opts.max_errors = 1 + lcg(&g) % 6;
    opts.max_input_bytes = n > 4 ? 4 + lcg(&g) % (n - 3) : 0;
  }
  else if (strcmp(mode, "bytes") == 0) {
    chunked = 1;
  }
  else if (strcmp(mode, "whole") != 0) {
    bad = -1;
  }
  if (bad) {
    free(input);
    free(name);
    return -1;
  }
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&opts);
  if (!tok) {
    printf("END E_OOM\n");
    free(input);
    free(name);
    return 0;
  }
  gtext_html_tokenizer_set_state(tok, st);
  if (name) {
    gtext_html_tokenizer_set_last_start_tag(tok, (const char *)name, ln);
    free(name);
  }

  GTEXT_HTML_TokenizerStatus status = drain(tok);
  size_t at = 0;
  while (status == GTEXT_HTML_TOKENIZER_NEED_INPUT && at < n) {
    size_t take = n - at;
    if (chunked == 1) {
      take = 1;
    }
    else if (chunked == 2) {
      take = 1 + lcg(&seed) % 9;
      if (take > n - at) {
        take = n - at;
      }
    }
    unsigned char * chunk = malloc(take);
    if (!chunk) {
      status = GTEXT_HTML_TOKENIZER_E_OOM;
      break;
    }
    memcpy(chunk, input + at, take);
    at += take;
    const GTEXT_HTML_TokenizerStatus fed =
        gtext_html_tokenizer_feed(tok, chunk, take);
    status = fed == GTEXT_HTML_TOKENIZER_OK ? drain(tok) : fed;
    free(chunk);
  }
  if (status == GTEXT_HTML_TOKENIZER_NEED_INPUT) {
    gtext_html_tokenizer_finish(tok);
    status = drain(tok);
  }
  for (size_t i = 0; i < gtext_html_tokenizer_error_count(tok); i++) {
    const GTEXT_HTML_ParseError * e = gtext_html_tokenizer_error_at(tok, i);
    printf("X %s %zu %zu %zu\n", gtext_html_error_code_name(e->code), e->line,
        e->column, e->offset);
  }
  printf("DROPPED %zu\n", gtext_html_tokenizer_errors_dropped(tok));
  printf("END %s\n", status_name(status));
  gtext_html_tokenizer_free(tok);
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
