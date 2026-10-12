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
/* The HTML tree vectors, driven through the PUBLIC API (include/ghoti.io/text/html.h), one request per line.
 *
 *   html-api-runner
 *
 * The same protocol as html_tree_suite.c, which drives the tree builder directly, so that the same
 * scorer (html_tree_suite.py) holds the public layer to the same vectors with the same exceptions list:
 * the layer over the builder passes the builder's answer through, or this gate says so. A request is
 *
 *   <mode> <scripting> x<hex input> [ctx:x<hex context> [form] [quirks | limited-quirks]]
 *
 * (see html_tree_suite.c for the words). The input is parsed by gtext_html_parse() for `whole`, and
 * by the incremental parser (gtext_html_parser_new/feed/finish) for every other mode; a context makes
 * it gtext_html_parse_fragment() and gtext_html_parser_new_fragment()/finish_fragment(). `cuts`
 * answers for the whole input and then for every cut the other runner knows, each of which must give the
 * identical answer. The answer lines are those of the other runner (STATUS, ERRORS, MODE, INVARIANTS,
 * DUMP, CUT, END); the status names are this API's (E_LIMIT_DEPTH, ...), and the third number of ERRORS
 * (the tokenizer's share) is 0 here, since the public error list does not say which stage raised an error.
 *
 * Every chunk is a separate heap block freed as soon as the parser has taken it, so a layer that kept a
 * pointer into a chunk is a use-after-free for the sanitizers.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/html.h>

#include "../../src/markup/markup_internal.h"
#include "html_tree_dump.h"

static const char * status_name(GTEXT_HTML_Status s) {
  switch (s) {
    case GTEXT_HTML_OK: return "OK";
    case GTEXT_HTML_E_INVALID: return "E_INVALID";
    case GTEXT_HTML_E_OOM: return "E_OOM";
    case GTEXT_HTML_E_STATE: return "E_STATE";
    case GTEXT_HTML_E_UNSUPPORTED_ENCODING: return "E_UNSUPPORTED_ENCODING";
    case GTEXT_HTML_E_INTERNAL: return "E_INTERNAL";
    case GTEXT_HTML_E_LIMIT_INPUT: return "E_LIMIT_INPUT";
    case GTEXT_HTML_E_LIMIT_DEPTH: return "E_LIMIT_DEPTH";
    case GTEXT_HTML_E_LIMIT_NODES: return "E_LIMIT_NODES";
    case GTEXT_HTML_E_LIMIT_NAME: return "E_LIMIT_NAME";
    case GTEXT_HTML_E_LIMIT_VALUE: return "E_LIMIT_VALUE";
    case GTEXT_HTML_E_LIMIT_COMMENT: return "E_LIMIT_COMMENT";
    case GTEXT_HTML_E_LIMIT_TEXT: return "E_LIMIT_TEXT";
    case GTEXT_HTML_E_LIMIT_ATTRIBUTES: return "E_LIMIT_ATTRIBUTES";
  }
  return "E_UNKNOWN";
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
  out[len] = 0; /* the public context takes a NUL-terminated name */
  *n = len;
  return out;
}

static unsigned long long lcg(unsigned long long * s) {
  *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
  return *s >> 33;
}

/* The context of a fragment request. */
typedef struct {
  bool present;
  GTEXT_HTML_Namespace ns;
  const char * name;
  size_t name_length;
  bool form;
  GTEXT_Markup_Mode quirks;
} Ctx;

/* What one run produced. */
typedef struct {
  GTEXT_HTML_Status status;
  char * dump;
  size_t dump_length;
  const char * invariants; /* NULL when it holds (or there is no tree) */
  int mode;
  size_t nerrors;
  GTEXT_HTML_Error * errors;
  size_t dropped;
} Result;

static void result_free(Result * r) {
  free(r->dump);
  free(r->errors);
  memset(r, 0, sizeof(*r));
}

/* Take what a finished parse produced. */
static void take(Result * out, GTEXT_HTML_Status s, GTEXT_Markup_Document * doc, GTEXT_Markup_Node * frag,
    GTEXT_HTML_Result * r) {
  out->status = s;
  if (r->status != s) {
    out->status = GTEXT_HTML_E_INTERNAL; /* the result must say what the call returned */
  }
  out->nerrors = gtext_html_result_error_count(r);
  out->dropped = gtext_html_result_errors_dropped(r);
  if (out->nerrors) {
    out->errors = malloc(out->nerrors * sizeof(*out->errors));
    if (out->errors) {
      for (size_t i = 0; i < out->nerrors; i++) {
        gtext_html_result_error_at(r, i, &out->errors[i]);
      }
    }
  }
  if (s == GTEXT_HTML_OK && doc) {
    out->mode = (int)gtext_markup_document_mode(doc);
    if ((int)r->mode != out->mode) {
      out->status = GTEXT_HTML_E_INTERNAL; /* the result's mode is the document's */
    }
    out->invariants = markup_check_invariants(doc);
    out->dump = frag ? htd_dump_children(frag, &out->dump_length) : htd_dump(doc, &out->dump_length);
  }
  else if (s == GTEXT_HTML_OK) {
    out->status = GTEXT_HTML_E_INTERNAL;
  }
  gtext_markup_document_free(doc);
  gtext_html_result_free(r);
}

/* Feed `input` in pieces: `how` is 0 for gtext_html_parse(), 1 a byte at a time, 2 random pieces from `seed`,
   3 two feeds cut at `split`, 4 pieces of 2 + seed bytes. */
static void run(const unsigned char * input, size_t n, bool scripting, int how, unsigned long long seed,
    size_t split, const Ctx * ctx, Result * out) {
  memset(out, 0, sizeof(*out));
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.scripting = scripting;
  opts.max_errors = 1u << 20;
  GTEXT_HTML_Fragment_Context fc;
  memset(&fc, 0, sizeof(fc));
  if (ctx && ctx->present) {
    fc.ns = ctx->ns;
    fc.local_name = ctx->name;
    fc.form_ancestor = ctx->form;
    fc.quirks = ctx->quirks;
  }
  const bool fragment = ctx && ctx->present;
  if (fragment && strlen(ctx->name) != ctx->name_length) {
    out->status = GTEXT_HTML_E_INTERNAL; /* the name would be cut short by the C string the API takes */
    return;
  }
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * frag = NULL;
  GTEXT_HTML_Result r;
  memset(&r, 0, sizeof(r));
  if (how == 0) {
    /* gtext_html_parse(): the input as one block (the heap block, so that an over-read is seen). */
    unsigned char * block = malloc(n ? n : 1);
    if (!block) {
      out->status = GTEXT_HTML_E_OOM;
      return;
    }
    memcpy(block, input, n);
    const GTEXT_HTML_Status s = fragment
        ? gtext_html_parse_fragment(&fc, block, n, &opts, &doc, &frag, &r)
        : gtext_html_parse(block, n, &opts, &doc, &r);
    free(block);
    take(out, s, doc, frag, &r);
    return;
  }
  GTEXT_HTML_Parser * p = NULL;
  GTEXT_HTML_Status s = fragment ? gtext_html_parser_new_fragment(&fc, &opts, &p) : gtext_html_parser_new(&opts, &p);
  if (s != GTEXT_HTML_OK) {
    out->status = s;
    return;
  }
  size_t at = 0;
  if (how == 3) {
    for (int part = 0; part < 2 && s == GTEXT_HTML_OK; part++) {
      const size_t amount = part == 0 ? split : n - split;
      unsigned char * chunk = malloc(amount ? amount : 1);
      if (!chunk) {
        s = GTEXT_HTML_E_OOM;
        break;
      }
      memcpy(chunk, input + (part == 0 ? 0 : split), amount);
      s = gtext_html_parser_feed(p, chunk, amount);
      free(chunk);
    }
    at = n;
  }
  while (at < n && s == GTEXT_HTML_OK) {
    size_t amount = n - at;
    if (how == 1) {
      amount = 1;
    }
    else if (how == 2) {
      amount = 1 + lcg(&seed) % 9;
    }
    else if (how == 4) {
      amount = 2 + (size_t)seed;
    }
    if (amount > n - at) {
      amount = n - at;
    }
    unsigned char * chunk = malloc(amount ? amount : 1);
    if (!chunk) {
      s = GTEXT_HTML_E_OOM;
      break;
    }
    memcpy(chunk, input + at, amount);
    at += amount;
    s = gtext_html_parser_feed(p, chunk, amount);
    free(chunk);
  }
  const GTEXT_HTML_Status fs = fragment ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r)
                                         : gtext_html_parser_finish(p, &doc, &r);
  /* A feed that failed is the finish's failure too (sticky). */
  take(out, s != GTEXT_HTML_OK ? s : fs, doc, frag, &r);
  if (s != GTEXT_HTML_OK && fs != s) {
    out->status = GTEXT_HTML_E_INTERNAL;
  }
  gtext_html_parser_free(p);
}

static const char * mode_name(int m) {
  return m == 0 ? "no-quirks" : m == 1 ? "limited-quirks" : "quirks";
}

/* NULL if the two are the same answer, else what differed. */
static const char * differ(const Result * a, const Result * b) {
  if (a->status != b->status) {
    return "status";
  }
  if (a->nerrors != b->nerrors || a->dropped != b->dropped) {
    return "error count";
  }
  for (size_t i = 0; i < a->nerrors && a->errors && b->errors; i++) {
    if (a->errors[i].code != b->errors[i].code || a->errors[i].line != b->errors[i].line
        || a->errors[i].column != b->errors[i].column || a->errors[i].byte_offset != b->errors[i].byte_offset) {
      return "an error";
    }
  }
  if (a->mode != b->mode) {
    return "document mode";
  }
  if ((a->dump == NULL) != (b->dump == NULL)
      || (a->dump && (a->dump_length != b->dump_length || memcmp(a->dump, b->dump, a->dump_length) != 0))) {
    return "tree";
  }
  return NULL;
}

static void put_hex(const char * s, size_t n) {
  printf("x");
  for (size_t i = 0; i < n; i++) {
    printf("%02x", (unsigned char)s[i]);
  }
}

static int run_request(char * line) {
  char * mode = strtok(line, " \n");
  char * script = strtok(NULL, " \n");
  char * hex = strtok(NULL, " \n");
  if (!mode || !script || !hex) {
    return -1;
  }
  Ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  unsigned char * ctx_text = NULL;
  for (char * word = strtok(NULL, " \n"); word; word = strtok(NULL, " \n")) {
    if (strncmp(word, "ctx:", 4) == 0 && !ctx.present) {
      size_t cn = 0;
      ctx_text = from_hex(word + 4, &cn);
      if (!ctx_text || cn == 0) {
        free(ctx_text);
        return -1;
      }
      ctx.present = true;
      ctx.ns = GTEXT_HTML_NS_HTML;
      ctx.name = (const char *)ctx_text;
      ctx.name_length = cn;
      if (strlen((const char *)ctx_text) != cn) {
        /* The public context is a C string: a name with a NUL in it would be cut short, so it is not a request. */
        free(ctx_text);
        return -1;
      }
      if ((cn >= 4 && memcmp(ctx_text, "svg ", 4) == 0 && cn == 4)
          || (cn >= 5 && memcmp(ctx_text, "math ", 5) == 0 && cn == 5)) {
        free(ctx_text);
        return -1;
      }
      if (cn > 4 && memcmp(ctx_text, "svg ", 4) == 0) {
        ctx.ns = GTEXT_HTML_NS_SVG;
        ctx.name += 4;
        ctx.name_length -= 4;
      }
      else if (cn > 5 && memcmp(ctx_text, "math ", 5) == 0) {
        ctx.ns = GTEXT_HTML_NS_MATHML;
        ctx.name += 5;
        ctx.name_length -= 5;
      }
    }
    else if (strcmp(word, "form") == 0 && ctx.present) {
      ctx.form = true;
    }
    else if (strcmp(word, "quirks") == 0 && ctx.present) {
      ctx.quirks = GTEXT_MARKUP_MODE_QUIRKS;
    }
    else if (strcmp(word, "limited-quirks") == 0 && ctx.present) {
      ctx.quirks = GTEXT_MARKUP_MODE_LIMITED_QUIRKS;
    }
    else {
      free(ctx_text);
      return -1;
    }
  }
  if (strcmp(script, "0") != 0 && strcmp(script, "1") != 0) {
    free(ctx_text);
    return -1;
  }
  const bool scripting = script[0] == '1';
  size_t n = 0;
  unsigned char * input = from_hex(hex, &n);
  if (!input) {
    free(ctx_text);
    return -1;
  }
  int how = 0;
  unsigned long long seed = 0;
  size_t split = 0;
  bool cuts = false;
  char * end = NULL;
  if (strcmp(mode, "whole") == 0) {
    how = 0;
  }
  else if (strcmp(mode, "bytes") == 0) {
    how = 1;
  }
  else if (strncmp(mode, "chunks:", 7) == 0 && mode[7] >= '0' && mode[7] <= '9') {
    seed = strtoull(mode + 7, &end, 10);
    if (*end) {
      free(input);
      free(ctx_text);
      return -1;
    }
    how = 2;
  }
  else if (strncmp(mode, "split:", 6) == 0 && mode[6] >= '0' && mode[6] <= '9') {
    split = (size_t)strtoull(mode + 6, &end, 10);
    if (*end || split > n) {
      free(input);
      free(ctx_text);
      return -1;
    }
    how = 3;
  }
  else if (strcmp(mode, "cuts") == 0) {
    cuts = true;
  }
  else {
    free(input);
    free(ctx_text);
    return -1;
  }
  Result whole;
  run(input, n, scripting, how, seed, split, &ctx, &whole);
  printf("STATUS %s\n", status_name(whole.status));
  printf("ERRORS %zu %zu 0\n", whole.nerrors, whole.dropped);
  for (size_t i = 0; i < whole.nerrors && whole.errors; i++) {
    printf("X %s %zu %zu %zu\n", gtext_html_error_name(whole.errors[i].code), whole.errors[i].line,
        whole.errors[i].column, whole.errors[i].byte_offset);
  }
  if (whole.status == GTEXT_HTML_OK) {
    printf("MODE %s\n", mode_name(whole.mode));
    printf("INVARIANTS %s\n", whole.invariants ? whole.invariants : "OK");
    printf("DUMP ");
    put_hex(whole.dump ? whole.dump : "", whole.dump ? whole.dump_length : 0);
    printf("\n");
  }
  if (cuts) {
    const char * what = NULL;
    char which[64] = "";
    Result r;
    for (size_t k = 0; k <= n && !what; k++) {
      run(input, n, scripting, 3, 0, k, &ctx, &r);
      what = differ(&whole, &r);
      if (what) {
        snprintf(which, sizeof(which), "split at %zu", k);
      }
      result_free(&r);
    }
    if (!what) {
      run(input, n, scripting, 1, 0, 0, &ctx, &r);
      what = differ(&whole, &r);
      if (what) {
        snprintf(which, sizeof(which), "a byte at a time");
      }
      result_free(&r);
    }
    for (unsigned long long p = 2; p <= 9 && !what; p++) {
      run(input, n, scripting, 4, p - 2, 0, &ctx, &r);
      what = differ(&whole, &r);
      if (what) {
        snprintf(which, sizeof(which), "pieces of %llu", p);
      }
      result_free(&r);
    }
    for (unsigned long long sd = 1; sd <= 4 && !what; sd++) {
      run(input, n, scripting, 2, sd, 0, &ctx, &r);
      what = differ(&whole, &r);
      if (what) {
        snprintf(which, sizeof(which), "random pieces, seed %llu", sd);
      }
      result_free(&r);
    }
    if (what) {
      printf("CUT DIFF %s: %s\n", which, what);
    }
    else {
      printf("CUT OK\n");
    }
  }
  result_free(&whole);
  free(input);
  free(ctx_text);
  printf("END DONE\n");
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
