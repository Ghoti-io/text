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

/* The HTML tree builder, driven from text on stdin, one request per line.
 *
 *   html-tree-runner
 *
 * A request is
 *
 *   <mode> <scripting> x<hex input> [ctx:x<hex context> [form] [quirks | limited-quirks]]
 *
 * With a context the input is parsed by the HTML fragment parsing algorithm in that context element
 * (the vectors' `#document-fragment` line: `td`, `svg path`, `math mi`; the words `svg ` and `math ` say
 * the namespace, as the vectors' README does), `form` says the context element has a form ancestor, and
 * the last word the mode of its document (no-quirks without one). The DUMP is then the fragment's.
 *
 * where `mode` is `whole` (the input in one feed), `bytes` (one byte per feed),
 * `chunks:<seed>` (feeds of 1 to 9 bytes chosen by a generator from the seed), `split:<k>`
 * (two feeds, cut after k bytes), or `cuts` (the answer for the whole input, and then the
 * same input cut every way this runner knows - every two-way split, a byte at a time,
 * pieces of 2 to 9 bytes - each of which must give the identical answer), and `scripting`
 * is 0 or 1. A request that is not exactly this is answered `END E_REQUEST` and run as
 * nothing. The answer is, for the whole-input run:
 *
 *   STATUS <name>                       OK, or the failure (E_LIMIT_DEPTH, ...)
 *   ERRORS <kept> <dropped> <tokenizer> the parse errors, then one line each:
 *   X <name> <line> <column> <offset>
 *   MODE <document mode>                no-quirks, limited-quirks, quirks (OK only)
 *   INVARIANTS <OK | message>           the tree's invariant checker (OK only)
 *   DUMP x<hex of the tree-construction dump>   (OK only)
 *   CUT OK | CUT DIFF <what differed>   (mode `cuts` only)
 *   END DONE
 *
 * One process answers many requests: the scorer sends thousands. Every chunk is a separate heap
 * block freed as soon as the builder has taken it, so a builder that kept a pointer into a
 * chunk is a use-after-free for the sanitizers.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/html/html_tree_internal.h"
#include "../../src/markup/markup_internal.h"
#include "html_tree_dump.h"

static const char * status_name(GTEXT_HTML_TreeStatus s) {
  switch (s) {
    case GTEXT_HTML_TREE_OK: return "OK";
    case GTEXT_HTML_TREE_E_INVALID: return "E_INVALID";
    case GTEXT_HTML_TREE_E_STATE: return "E_STATE";
    case GTEXT_HTML_TREE_E_OOM: return "E_OOM";
    case GTEXT_HTML_TREE_E_REPROCESS: return "E_REPROCESS";
    case GTEXT_HTML_TREE_E_ENCODING: return "E_ENCODING";
    case GTEXT_HTML_TREE_E_LIMIT_DEPTH: return "E_LIMIT_DEPTH";
    case GTEXT_HTML_TREE_E_LIMIT_NODES: return "E_LIMIT_NODES";
    case GTEXT_HTML_TREE_E_LIMIT_INPUT: return "E_LIMIT_INPUT";
    case GTEXT_HTML_TREE_E_LIMIT_NAME: return "E_LIMIT_NAME";
    case GTEXT_HTML_TREE_E_LIMIT_VALUE: return "E_LIMIT_VALUE";
    case GTEXT_HTML_TREE_E_LIMIT_COMMENT: return "E_LIMIT_COMMENT";
    case GTEXT_HTML_TREE_E_LIMIT_TEXT: return "E_LIMIT_TEXT";
    case GTEXT_HTML_TREE_E_LIMIT_ATTRIBUTES: return "E_LIMIT_ATTRIBUTES";
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
  GTEXT_HTML_TreeStatus status;
  char * dump;
  size_t dump_length;
  const char * invariants; /* NULL when it holds (or there is no tree) */
  int mode;
  size_t nerrors;
  GTEXT_HTML_TreeError * errors;
  size_t dropped;
  size_t tokenizer_errors;
} Result;

static void result_free(Result * r) {
  free(r->dump);
  free(r->errors);
  memset(r, 0, sizeof(*r));
}

/* Feed `input` in pieces: `pieces` is NULL for the whole, else the lengths to use in turn
   (cycled), or if `split` >= 0 two pieces cut there. */
static void run(const unsigned char * input, size_t n, bool scripting, int how,
    unsigned long long seed, size_t split, const Ctx * ctx, Result * out) {
  memset(out, 0, sizeof(*out));
  GTEXT_HTML_TreeOptions opts;
  memset(&opts, 0, sizeof(opts));
  opts.scripting = scripting;
  opts.max_errors = 1u << 20;
  opts.hash_seed = 12345;
  GTEXT_HTML_Tree * tree = NULL;
  if (ctx && ctx->present) {
    GTEXT_HTML_FragmentContext fc;
    memset(&fc, 0, sizeof(fc));
    fc.ns = ctx->ns;
    fc.local_name = ctx->name;
    fc.local_name_length = ctx->name_length;
    fc.form_ancestor = ctx->form;
    fc.quirks = ctx->quirks;
    fc.scripting = scripting;
    GTEXT_HTML_TreeStatus st = GTEXT_HTML_TREE_OK;
    tree = gtext_html_tree_new_fragment(&opts, &fc, &st);
    if (!tree) {
      out->status = st;
      return;
    }
  }
  else {
    tree = gtext_html_tree_new(&opts);
  }
  if (!tree) {
    out->status = GTEXT_HTML_TREE_E_OOM;
    return;
  }
  GTEXT_HTML_TreeStatus s = GTEXT_HTML_TREE_OK;
  size_t at = 0;
  if (how == 3) {
    /* Two feeds, cut after `split` bytes (the first may be empty). */
    for (int part = 0; part < 2 && s == GTEXT_HTML_TREE_OK; part++) {
      const size_t take = part == 0 ? split : n - split;
      unsigned char * chunk = malloc(take ? take : 1);
      if (!chunk) {
        s = GTEXT_HTML_TREE_E_OOM;
        break;
      }
      memcpy(chunk, input + (part == 0 ? 0 : split), take);
      s = gtext_html_tree_feed(tree, chunk, take);
      free(chunk);
    }
    at = n;
  }
  while (at < n && s == GTEXT_HTML_TREE_OK) {
    size_t take = n - at;
    if (how == 1) {
      take = 1;
    }
    else if (how == 2) {
      take = 1 + lcg(&seed) % 9;
    }
    else if (how == 4) {
      take = 2 + (size_t)seed;
    }
    if (take > n - at) {
      take = n - at;
    }
    unsigned char * chunk = malloc(take ? take : 1);
    if (!chunk) {
      s = GTEXT_HTML_TREE_E_OOM;
      break;
    }
    memcpy(chunk, input + at, take);
    at += take;
    s = gtext_html_tree_feed(tree, chunk, take);
    free(chunk);
  }
  if (s == GTEXT_HTML_TREE_OK) {
    s = gtext_html_tree_finish(tree);
  }
  out->status = s;
  out->nerrors = gtext_html_tree_error_count(tree);
  out->dropped = gtext_html_tree_errors_dropped(tree);
  if (out->nerrors) {
    out->errors = malloc(out->nerrors * sizeof(*out->errors));
    if (out->errors) {
      for (size_t i = 0; i < out->nerrors; i++) {
        out->errors[i] = *gtext_html_tree_error_at(tree, i);
        if (out->errors[i].code < GTEXT_HTML_TREE_ERROR_BASE) {
          out->tokenizer_errors++;
        }
      }
    }
  }
  if (s == GTEXT_HTML_TREE_OK && ctx && ctx->present) {
    GTEXT_Markup_Document * doc = NULL;
    GTEXT_Markup_Node * frag = gtext_html_tree_take_fragment(tree, &doc);
    if (doc && frag) {
      out->mode = (int)gtext_markup_document_mode(doc);
      out->invariants = markup_check_invariants(doc);
      out->dump = htd_dump_children(frag, &out->dump_length);
      gtext_markup_document_free(doc);
    }
  }
  else if (s == GTEXT_HTML_TREE_OK) {
    GTEXT_Markup_Document * doc = gtext_html_tree_take_document(tree);
    if (doc) {
      out->mode = (int)gtext_markup_document_mode(doc);
      out->invariants = markup_check_invariants(doc);
      out->dump = htd_dump(doc, &out->dump_length);
      gtext_markup_document_free(doc);
    }
  }
  gtext_html_tree_free(tree);
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
        || a->errors[i].column != b->errors[i].column || a->errors[i].offset != b->errors[i].offset) {
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
      if ((cn >= 4 && memcmp(ctx_text, "svg ", 4) == 0 && cn == 4)
          || (cn >= 5 && memcmp(ctx_text, "math ", 5) == 0 && cn == 5)) {
        /* A namespace word with no local name is not a context (and is not the HTML element "svg "). */
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
  printf("ERRORS %zu %zu %zu\n", whole.nerrors, whole.dropped, whole.tokenizer_errors);
  for (size_t i = 0; i < whole.nerrors && whole.errors; i++) {
    printf("X %s %zu %zu %zu\n", gtext_html_tree_error_name(whole.errors[i].code),
        whole.errors[i].line, whole.errors[i].column, whole.errors[i].offset);
  }
  if (whole.status == GTEXT_HTML_TREE_OK) {
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
