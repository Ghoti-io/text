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
/* The HTML serializer's runner, one request per line, for html_serializer_suite.py.
 *
 *   html-serializer-runner
 *
 * Two kinds of request. Every string in a request is hex ("x" and then pairs of digits; the bare "x" is the
 * empty string, "-" is NULL where a string may be absent), so that nothing in a vector can be mistaken for the
 * protocol.
 *
 *   tok <option>... -- <token> ...
 *
 * runs html5lib's serializer vectors through the token-stream serializer. An option is `name=value`
 * (see apply_option()); a token is comma-separated fields:
 *
 *   S,<ns>,<name>,<n>,{<ans>,<prefix>,<local>,<value>}*n    a start tag (<n> is decimal)
 *   M,...                                                    an empty tag (same fields)
 *   E,<ns>,<name>                                            an end tag
 *   C,<data>   K,<data>                                      characters, a comment
 *   D,<name>,<public>,<system>                               a document type
 *   P,<target>,<data>                                        a processing instruction
 *
 * and is answered with OUT <hex of the output>, COUNTS (omitted start, omitted end, minimized, replaced, injected),
 * STATUS and END DONE.
 *
 *   rt <scripting> x<input> [ctx:x<context> [form] [quirks | limited-quirks]]
 *
 * parses the input (as a document, or as a fragment in the context), serializes the tree with the standard's own
 * defaults, parses that and serializes it again, and answers with the two trees in the vectors' dump format
 * (DUMP1, DUMP2), the two outputs (SER1, SER2), the two document modes, whether gtext_markup_equal() holds
 * (EQUAL) and the statuses. A fragment is serialized as the contents of an element of the context's name, which
 * is what `innerHTML` does and what makes the raw text of a `style` context raw.
 */
#include <stdbool.h>
#include <errno.h>
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
    case GTEXT_HTML_E_LIMIT_DEPTH: return "E_LIMIT_DEPTH";
    case GTEXT_HTML_E_WRITE: return "E_WRITE";
    case GTEXT_HTML_E_LIMIT_OUTPUT: return "E_LIMIT_OUTPUT";
    case GTEXT_HTML_E_OPTIONS: return "E_OPTIONS";
    default: return "E_OTHER";
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

/* A hex string to a NUL-terminated heap block. "-" is NULL (`*absent` set). NULL and *absent false: malformed. */
static unsigned char * from_hex(const char * s, size_t * n, bool * absent) {
  *absent = false;
  if (strcmp(s, "-") == 0) {
    *absent = true;
    *n = 0;
    return NULL;
  }
  if (*s != 'x') {
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

static void put_hex(const char * s, size_t n) {
  printf("x");
  for (size_t i = 0; i < n; i++) {
    printf("%02x", (unsigned char)s[i]);
  }
}

/* ---- collecting the output ------------------------------------------------ */

typedef struct {
  char * data;
  size_t length, capacity;
} Out;

static GTEXT_Markup_Status out_write(void * user, const char * bytes, size_t len) {
  Out * o = user;
  if (o->length + len + 1 > o->capacity) {
    size_t cap = o->capacity ? o->capacity : 256;
    while (cap < o->length + len + 1) {
      cap *= 2;
    }
    char * g = realloc(o->data, cap);
    if (!g) {
      return GTEXT_MARKUP_E_OOM;
    }
    o->data = g;
    o->capacity = cap;
  }
  memcpy(o->data + o->length, bytes, len);
  o->length += len;
  o->data[o->length] = 0;
  return GTEXT_MARKUP_OK;
}

/* ---- tok ------------------------------------------------------------------ */

static bool apply_option(GTEXT_HTML_Serialize_Options * o, char * word, unsigned char ** keep) {
  char * eq = strchr(word, '=');
  if (!eq) {
    return false;
  }
  *eq = 0;
  const char * name = word;
  const char * value = eq + 1;
  const bool on = strcmp(value, "1") == 0;
  if (strcmp(name, "quote_attr_values") == 0) {
    if (strcmp(value, "always") == 0) {
      o->quote_attr_values = GTEXT_HTML_QUOTE_ALWAYS;
    }
    else if (strcmp(value, "when_needed") == 0) {
      o->quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
    }
    else {
      o->quote_attr_values = (GTEXT_HTML_Quote_Mode)99;
    }
  }
  else if (strcmp(name, "quote_char") == 0) {
    o->quote_char = strcmp(value, "apos") == 0 ? '\'' : strcmp(value, "quote") == 0 ? '"'
        : strcmp(value, "other") == 0 ? '#' : 0;
  }
  else if (strcmp(name, "use_best_quote_char") == 0) {
    o->use_best_quote_char = on;
  }
  else if (strcmp(name, "omit_optional_tags") == 0) {
    o->omit_optional_tags = on;
  }
  else if (strcmp(name, "minimize_boolean_attributes") == 0) {
    o->minimize_boolean_attributes = on;
  }
  else if (strcmp(name, "use_trailing_solidus") == 0) {
    o->use_trailing_solidus = on;
  }
  else if (strcmp(name, "space_before_trailing_solidus") == 0) {
    o->space_before_trailing_solidus = on;
  }
  else if (strcmp(name, "escape_rcdata") == 0) {
    o->escape_rcdata = on;
  }
  else if (strcmp(name, "strip_whitespace") == 0) {
    o->strip_whitespace = on;
  }
  else if (strcmp(name, "inject_meta_charset") == 0) {
    o->inject_meta_charset = on;
  }
  else if (strcmp(name, "alphabetical_attributes") == 0) {
    o->alphabetical_attributes = on;
  }
  else if (strcmp(name, "doctype_identifiers") == 0) {
    o->doctype_identifiers = on;
  }
  else if (strcmp(name, "scripting") == 0) {
    o->scripting = on;
  }
  else if (strcmp(name, "encoding") == 0) {
    size_t n;
    bool absent;
    *keep = from_hex(value, &n, &absent);
    if (!*keep) {
      return false;
    }
    o->encoding = (const char *)*keep;
  }
  else {
    return false;
  }
  return true;
}

/* A hex field that may be "-": NULL for absent. Returns false on a malformed field. */
static bool field(char ** cursor, unsigned char ** out, size_t * n, char sep) {
  char * start = *cursor;
  char * end = start;
  while (*end && *end != sep) {
    end++;
  }
  const bool more = *end == sep;
  if (more) {
    *end = 0;
  }
  bool absent;
  *out = from_hex(start, n, &absent);
  *cursor = more ? end + 1 : end;
  return *out != NULL || absent;
}

static int run_tok(char * rest) {
  GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
  unsigned char * keep = NULL;
  char * save = NULL;
  char * word = strtok_r(rest, " \n", &save);
  while (word && strcmp(word, "--") != 0) {
    if (!apply_option(&o, word, &keep)) {
      free(keep);
      return -1;
    }
    word = strtok_r(NULL, " \n", &save);
  }
  if (!word) {
    free(keep);
    return -1;
  }
  Out out = {NULL, 0, 0};
  const GTEXT_Markup_Sink sink = {out_write, &out};
  GTEXT_HTML_Serializer * ser = NULL;
  GTEXT_HTML_Status st = gtext_html_serializer_new(&o, &sink, &ser);
  if (st != GTEXT_HTML_OK) {
    printf("STATUS %s\nEND DONE\n", status_name(st));
    free(keep);
    return 0;
  }
  int bad = 0;
  for (word = strtok_r(NULL, " \n", &save); word && !bad && st == GTEXT_HTML_OK; word = strtok_r(NULL, " \n", &save)) {
    char kind = word[0];
    if (word[1] != ',' && word[1] != 0) {
      bad = 1;
      break;
    }
    char * cur = word[1] ? word + 2 : word + 1;
    unsigned char *a = NULL, *b = NULL, *c = NULL;
    size_t an = 0, bn = 0, cn = 0;
    switch (kind) {
      case 'S':
      case 'M': {
        if (!field(&cur, &a, &an, ',') || !field(&cur, &b, &bn, ',')) {
          bad = 1;
          break;
        }
        char * count = cur;
        while (*cur && *cur != ',') {
          cur++;
        }
        if (*cur == ',') {
          *cur++ = 0;
        }
        if (*count < '0' || *count > '9') {
          bad = 1;
          break;
        }
        char * count_end = NULL;
        errno = 0;
        const unsigned long n_parsed = strtoul(count, &count_end, 10);
        if (errno != 0 || *count_end != '\0' || n_parsed > 100000) {
          bad = 1;
          break;
        }
        const size_t n = (size_t)n_parsed;
        GTEXT_Markup_Attribute * attrs = calloc(n ? n : 1, sizeof(*attrs));
        unsigned char ** ptrs = calloc(n * 4 + 1, sizeof(*ptrs));
        if (!attrs || !ptrs) {
          free(attrs);
          free(ptrs);
          fprintf(stderr, "html serializer runner: out of memory\n");
          exit(2);
        }
        for (size_t i = 0; i < n && !bad; i++) {
          size_t l1, l2, l3, l4;
          if (!field(&cur, &ptrs[4 * i], &l1, ',') || !field(&cur, &ptrs[4 * i + 1], &l2, ',')
              || !field(&cur, &ptrs[4 * i + 2], &l3, ',') || !field(&cur, &ptrs[4 * i + 3], &l4, ',')) {
            bad = 1;
            break;
          }
          attrs[i].ns = (const char *)ptrs[4 * i];
          attrs[i].prefix = (const char *)ptrs[4 * i + 1];
          attrs[i].local = (const char *)ptrs[4 * i + 2];
          attrs[i].value = (const char *)ptrs[4 * i + 3];
          attrs[i].value_length = l4;
        }
        if (!bad) {
          st = kind == 'S' ? gtext_html_serializer_start_tag(ser, (const char *)a, (const char *)b, attrs, n)
                           : gtext_html_serializer_empty_tag(ser, (const char *)a, (const char *)b, attrs, n);
        }
        for (size_t i = 0; i < n * 4; i++) {
          free(ptrs[i]);
        }
        free(ptrs);
        free(attrs);
        break;
      }
      case 'E':
        if (!field(&cur, &a, &an, ',') || !field(&cur, &b, &bn, ',')) {
          bad = 1;
          break;
        }
        st = gtext_html_serializer_end_tag(ser, (const char *)a, (const char *)b);
        break;
      case 'C':
      case 'K':
        if (!field(&cur, &a, &an, ',')) {
          bad = 1;
          break;
        }
        st = kind == 'C' ? gtext_html_serializer_characters(ser, (const char *)a, an)
                         : gtext_html_serializer_comment(ser, (const char *)a, an);
        break;
      case 'D':
        if (!field(&cur, &a, &an, ',') || !field(&cur, &b, &bn, ',') || !field(&cur, &c, &cn, ',')) {
          bad = 1;
          break;
        }
        st = gtext_html_serializer_doctype(ser, (const char *)a, (const char *)b, (const char *)c);
        break;
      case 'P':
        if (!field(&cur, &a, &an, ',') || !field(&cur, &b, &bn, ',')) {
          bad = 1;
          break;
        }
        st = gtext_html_serializer_processing_instruction(ser, (const char *)a, (const char *)b, bn);
        break;
      default:
        bad = 1;
        break;
    }
    if (!bad && *cur != '\0') {
      bad = 1; /* more fields than the token has */
    }
    free(a);
    free(b);
    free(c);
  }
  GTEXT_HTML_Serialize_Result r;
  if (!bad) {
    const GTEXT_HTML_Status fs = gtext_html_serializer_finish(ser, &r);
    if (st == GTEXT_HTML_OK) {
      st = fs;
    }
  }
  gtext_html_serializer_free(ser);
  if (bad) {
    free(out.data);
    free(keep);
    return -1;
  }
  printf("OUT ");
  put_hex(out.data ? out.data : "", out.length);
  printf("\nCOUNTS %zu %zu %zu %zu %zu\n", r.omitted_start_tags, r.omitted_end_tags, r.minimized_attributes,
      r.meta_charset_replaced, r.meta_charset_injected);
  printf("STATUS %s\nEND DONE\n", status_name(st));
  free(out.data);
  free(keep);
  return 0;
}

/* ---- rt ---------------------------------------------------------------------- */

typedef struct {
  bool present;
  GTEXT_HTML_Namespace ns;
  const char * name;
  bool form;
  GTEXT_Markup_Mode quirks;
} Ctx;

typedef struct {
  GTEXT_HTML_Status parse_status;
  GTEXT_HTML_Status ser_status;
  char * dump;
  size_t dump_length;
  char * ser;
  size_t ser_length;
  int mode;
  GTEXT_Markup_Document * doc;
  GTEXT_Markup_Node * frag;
} Round;

static void round_free(Round * r) {
  free(r->dump);
  free(r->ser);
  gtext_markup_document_free(r->doc);
  memset(r, 0, sizeof(*r));
}

static const char * ns_uri(GTEXT_HTML_Namespace ns) {
  return ns == GTEXT_HTML_NS_SVG ? GTEXT_MARKUP_NS_SVG : ns == GTEXT_HTML_NS_MATHML ? GTEXT_MARKUP_NS_MATHML
                                                                                   : GTEXT_MARKUP_NS_HTML;
}

/* Parse `input`, dump the tree and serialize it with the standard's defaults (scripting as the parse). */
static void one_round(const unsigned char * input, size_t n, bool scripting, const Ctx * ctx, Round * r) {
  memset(r, 0, sizeof(*r));
  GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
  po.scripting = scripting;
  po.count_errors_only = true;
  GTEXT_HTML_Result res;
  memset(&res, 0, sizeof(res));
  GTEXT_HTML_Fragment_Context fc;
  memset(&fc, 0, sizeof(fc));
  if (ctx->present) {
    fc.ns = ctx->ns;
    fc.local_name = ctx->name;
    fc.form_ancestor = ctx->form;
    fc.quirks = ctx->quirks;
    r->parse_status = gtext_html_parse_fragment(&fc, input, n, &po, &r->doc, &r->frag, &res);
  }
  else {
    r->parse_status = gtext_html_parse(input, n, &po, &r->doc, &res);
  }
  gtext_html_result_free(&res);
  if (r->parse_status != GTEXT_HTML_OK) {
    return;
  }
  r->mode = (int)gtext_markup_document_mode(r->doc);
  GTEXT_HTML_Serialize_Options so = gtext_html_serialize_options_default();
  so.scripting = scripting;
  so.max_depth = 100000;
  Out out = {NULL, 0, 0};
  const GTEXT_Markup_Sink sink = {out_write, &out};
  if (ctx->present) {
    r->dump = htd_dump_children(r->frag, &r->dump_length);
    /* The contents of an element of the context's name: what innerHTML serializes. */
    GTEXT_Markup_Node * holder = NULL;
    if (gtext_markup_create_element(r->doc, ns_uri(ctx->ns), NULL, ctx->name, &holder) != GTEXT_MARKUP_OK) {
      r->ser_status = GTEXT_HTML_E_INTERNAL;
      return;
    }
    GTEXT_Markup_Node * into = holder;
    if (ctx->ns == GTEXT_HTML_NS_HTML && strcmp(ctx->name, "template") == 0) {
      if (gtext_markup_element_ensure_template_content(holder, &into) != GTEXT_MARKUP_OK) {
        r->ser_status = GTEXT_HTML_E_INTERNAL;
        return;
      }
    }
    for (GTEXT_Markup_Node * c = gtext_markup_node_first_child(r->frag); c; c = gtext_markup_node_next_sibling(c)) {
      GTEXT_Markup_Node * copy = NULL;
      if (gtext_markup_clone(r->doc, c, true, &copy) != GTEXT_MARKUP_OK
          || gtext_markup_append_child(into, copy) != GTEXT_MARKUP_OK) {
        r->ser_status = GTEXT_HTML_E_INTERNAL;
        return;
      }
    }
    r->ser_status = gtext_html_serialize(holder, GTEXT_HTML_SERIALIZE_CHILDREN, &so, &sink, NULL);
  }
  else {
    r->dump = htd_dump(r->doc, &r->dump_length);
    r->ser_status = gtext_html_serialize(gtext_markup_document_root(r->doc), GTEXT_HTML_SERIALIZE_CHILDREN, &so, &sink, NULL);
  }
  r->ser = out.data;
  r->ser_length = out.length;
}

static void print_round(const char * tag, const Round * r) {
  printf("%s %s %s %d\n", tag, status_name(r->parse_status), status_name(r->ser_status), r->mode);
}

static int run_rt(char * rest) {
  char * save = NULL;
  char * script = strtok_r(rest, " \n", &save);
  char * hex = strtok_r(NULL, " \n", &save);
  if (!script || !hex || (strcmp(script, "0") != 0 && strcmp(script, "1") != 0)) {
    return -1;
  }
  const bool scripting = script[0] == '1';
  size_t n = 0;
  bool absent;
  unsigned char * input = from_hex(hex, &n, &absent);
  if (!input) {
    return -1;
  }
  Ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  unsigned char * ctx_text = NULL;
  for (char * word = strtok_r(NULL, " \n", &save); word; word = strtok_r(NULL, " \n", &save)) {
    if (strncmp(word, "ctx:", 4) == 0 && !ctx.present) {
      size_t cn = 0;
      ctx_text = from_hex(word + 4, &cn, &absent);
      if (!ctx_text || cn == 0 || strlen((const char *)ctx_text) != cn) {
        free(input);
        free(ctx_text);
        return -1;
      }
      ctx.present = true;
      ctx.ns = GTEXT_HTML_NS_HTML;
      ctx.name = (const char *)ctx_text;
      if (cn > 4 && memcmp(ctx_text, "svg ", 4) == 0) {
        ctx.ns = GTEXT_HTML_NS_SVG;
        ctx.name += 4;
      }
      else if (cn > 5 && memcmp(ctx_text, "math ", 5) == 0) {
        ctx.ns = GTEXT_HTML_NS_MATHML;
        ctx.name += 5;
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
      free(input);
      free(ctx_text);
      return -1;
    }
  }
  Round a, b;
  one_round(input, n, scripting, &ctx, &a);
  memset(&b, 0, sizeof(b));
  print_round("R1", &a);
  if (a.dump) {
    printf("DUMP1 ");
    put_hex(a.dump, a.dump_length);
    printf("\n");
  }
  if (a.ser_status == GTEXT_HTML_OK && a.ser) {
    printf("SER1 ");
    put_hex(a.ser, a.ser_length);
    printf("\n");
    one_round((const unsigned char *)a.ser, a.ser_length, scripting, &ctx, &b);
    print_round("R2", &b);
    if (b.dump) {
      printf("DUMP2 ");
      put_hex(b.dump, b.dump_length);
      printf("\n");
    }
    if (b.ser_status == GTEXT_HTML_OK && b.ser) {
      printf("SER2 ");
      put_hex(b.ser, b.ser_length);
      printf("\n");
    }
    if (a.doc && b.doc && a.parse_status == GTEXT_HTML_OK && b.parse_status == GTEXT_HTML_OK) {
      const GTEXT_Markup_Node * x = ctx.present ? a.frag : gtext_markup_document_root(a.doc);
      const GTEXT_Markup_Node * y = ctx.present ? b.frag : gtext_markup_document_root(b.doc);
      printf("EQUAL %d\n", gtext_markup_equal(x, y) ? 1 : 0);
    }
  }
  round_free(&a);
  round_free(&b);
  free(input);
  free(ctx_text);
  printf("END DONE\n");
  return 0;
}

static int run_request(char * line) {
  char * save = NULL;
  char * kind = strtok_r(line, " \n", &save);
  if (!kind) {
    return -1;
  }
  char * rest = save;
  if (strcmp(kind, "tok") == 0) {
    return run_tok(rest);
  }
  if (strcmp(kind, "rt") == 0) {
    return run_rt(rest);
  }
  return -1;
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
