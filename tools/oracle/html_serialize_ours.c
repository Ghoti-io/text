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
/* This library's side of the serializer differential (tools/oracle/html_serialize_diff.py), one request per line.
 *
 *   html-serialize-ours
 *
 * The differential asks html5lib's serializer, parse5's serializer and this library's the same question about the
 * same tree, so the tree travels as a list of **ops** rather than as markup or as the tree-construction dump (which
 * sorts the attributes, and the order of an element's attributes is the order they are written in). The ops, separated by
 * spaces, fields by commas; a string is "x" and hex, the bare "x" is empty, "-" is NULL:
 *
 *   R,doc | R,frag                          the root (the first op)
 *   e,<ns>,<prefix>,<local>,<n>[,<ans>,<aprefix>,<alocal>,<value>]*n   open an element
 *   [   ]                                   the open element's template contents (the children inside are its content)
 *   x                                       close the element
 *   t,<data>  k,<data>  c,<data>            text, CDATA section, comment
 *   p,<target>,<data>                       processing instruction
 *   d,<name>,<public>,<system>              document type
 *
 * Requests:
 *
 *   parse <scripting 0|1> x<html> [ctx:x<context>]   parse (as a fragment in the context if given) and answer
 *                                                    TREE <ops>, STATUS OK and DIRECT <status> x<hex> (the tree written
 *                                                    as it is, to hold the rebuild to), or STATUS <name>
 *   ser <children|outer> <scripting 0|1> <ops...>    build the tree and serialize it with the standard's defaults
 *                                                    (the scripting flag as given): the root's children, or the root's
 *                                                    first child itself. Answer OUT x<hex>, STATUS <name>
 *
 * and every answer ends END DONE (or END E_REQUEST for a request that cannot be read).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/html.h>

static const char * status_name(GTEXT_HTML_Status s) {
  switch (s) {
    case GTEXT_HTML_OK: return "OK";
    case GTEXT_HTML_E_INVALID: return "E_INVALID";
    case GTEXT_HTML_E_OOM: return "E_OOM";
    case GTEXT_HTML_E_LIMIT_DEPTH: return "E_LIMIT_DEPTH";
    case GTEXT_HTML_E_LIMIT_OUTPUT: return "E_LIMIT_OUTPUT";
    case GTEXT_HTML_E_WRITE: return "E_WRITE";
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

/* *absent is set for "-"; NULL without it is a malformed field. The result is NUL-terminated. */
static unsigned char * from_hex(const char * s, size_t * n, bool * absent) {
  *absent = false;
  *n = 0;
  if (strcmp(s, "-") == 0) {
    *absent = true;
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

static void put_hex(FILE * f, const char * s, size_t n) {
  fputc('x', f);
  for (size_t i = 0; i < n; i++) {
    fprintf(f, "%02x", (unsigned char)s[i]);
  }
}

static void put_field(FILE * f, const char * s) {
  if (!s) {
    fputc('-', f);
  }
  else {
    put_hex(f, s, strlen(s));
  }
}

/* ---- tree to ops ----------------------------------------------------------------------------- */

static void dump_node(FILE * f, const GTEXT_Markup_Node * n);

static void dump_children(FILE * f, const GTEXT_Markup_Node * parent) {
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(parent); c; c = gtext_markup_node_next_sibling(c)) {
    dump_node(f, c);
  }
}

static void dump_node(FILE * f, const GTEXT_Markup_Node * n) {
  size_t len = 0;
  switch (gtext_markup_node_kind(n)) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      fputs(" e,", f);
      put_field(f, gtext_markup_element_namespace(n));
      fputc(',', f);
      put_field(f, gtext_markup_element_prefix(n));
      fputc(',', f);
      put_field(f, gtext_markup_element_local_name(n));
      const size_t na = gtext_markup_element_attribute_count(n);
      fprintf(f, ",%zu", na);
      for (size_t i = 0; i < na; i++) {
        GTEXT_Markup_Attribute a;
        gtext_markup_element_attribute_at(n, i, &a);
        fputc(',', f);
        put_field(f, a.ns);
        fputc(',', f);
        put_field(f, a.prefix);
        fputc(',', f);
        put_field(f, a.local);
        fputc(',', f);
        put_hex(f, a.value, a.value_length);
      }
      const GTEXT_Markup_Node * content = gtext_markup_element_template_content(n);
      if (content) {
        fputs(" [", f);
        dump_children(f, content);
        fputs(" ]", f);
      }
      dump_children(f, n);
      fputs(" x", f);
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA:
    case GTEXT_MARKUP_KIND_COMMENT: {
      const char * d = gtext_markup_node_data(n, &len);
      const GTEXT_Markup_Kind k = gtext_markup_node_kind(n);
      fprintf(f, " %c,", k == GTEXT_MARKUP_KIND_TEXT ? 't' : k == GTEXT_MARKUP_KIND_CDATA ? 'k' : 'c');
      put_hex(f, d ? d : "", len);
      break;
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const char * d = gtext_markup_node_data(n, &len);
      fputs(" p,", f);
      put_field(f, gtext_markup_pi_target(n));
      fputc(',', f);
      put_hex(f, d ? d : "", len);
      break;
    }
    case GTEXT_MARKUP_KIND_DOCTYPE:
      fputs(" d,", f);
      put_field(f, gtext_markup_doctype_name(n));
      fputc(',', f);
      put_field(f, gtext_markup_doctype_public_id(n));
      fputc(',', f);
      put_field(f, gtext_markup_doctype_system_id(n));
      break;
    default:
      break;
  }
}

/* ---- ops to tree ------------------------------------------------------------------------------ */

typedef struct {
  GTEXT_Markup_Document * doc;
  GTEXT_Markup_Node * root;
  GTEXT_Markup_Node ** stack; /* containers: the root, open elements, template contents */
  size_t n, cap;
  GTEXT_Markup_Node ** elements; /* parallel: the element a container belongs to, for `]` and `x` */
} Build;

static bool push(Build * b, GTEXT_Markup_Node * container) {
  if (b->n == b->cap) {
    const size_t cap = b->cap ? b->cap * 2 : 64;
    GTEXT_Markup_Node ** g = realloc(b->stack, cap * sizeof(*g));
    if (!g) {
      return false;
    }
    b->stack = g;
    b->cap = cap;
  }
  b->stack[b->n++] = container;
  return true;
}

/* Split `word` at commas, in place; returns the number of fields (at most max). */
static size_t split(char * word, char ** fields, size_t max) {
  size_t n = 0;
  char * p = word;
  while (n < max) {
    fields[n++] = p;
    char * c = strchr(p, ',');
    if (!c) {
      break;
    }
    *c = 0;
    p = c + 1;
  }
  return n;
}

static bool field_str(const char * f, unsigned char ** out, size_t * n) {
  bool absent;
  *out = from_hex(f, n, &absent);
  return *out != NULL || absent;
}

/* Apply one op. */
static bool apply(Build * b, char * word) {
  GTEXT_Markup_Node * top = b->stack[b->n - 1];
  GTEXT_Markup_Node * made = NULL;
  if (strcmp(word, "x") == 0) {
    if (b->n < 2) {
      return false;
    }
    b->n--;
    return true;
  }
  if (strcmp(word, "[") == 0) {
    /* The contents of the element just opened: the container below the top is the element's parent, so the
       element is the top one. */
    GTEXT_Markup_Node * content = NULL;
    if (gtext_markup_node_kind(top) != GTEXT_MARKUP_KIND_ELEMENT
        || gtext_markup_element_ensure_template_content(top, &content) != GTEXT_MARKUP_OK) {
      return false;
    }
    return push(b, content);
  }
  if (strcmp(word, "]") == 0) {
    if (b->n < 2) {
      return false;
    }
    b->n--;
    return true;
  }
  size_t commas = 0;
  for (const char * p = word; *p; p++) {
    commas += *p == ',';
  }
  char ** f = malloc((commas + 1) * sizeof(*f));
  if (!f) {
    return false;
  }
  const size_t nf = split(word, f, commas + 1);
  const char kind = f[0][0];
  if (f[0][1] != 0) {
    free(f);
    return false;
  }
  unsigned char *a = NULL, *c = NULL, *d = NULL;
  size_t an = 0, cn = 0, dn = 0;
  bool ok = true;
  switch (kind) {
    case 't':
    case 'k':
    case 'c': {
      if (nf != 2 || !field_str(f[1], &a, &an)) {
        free(f);
        return false;
      }
      GTEXT_Markup_Status st = kind == 't' ? gtext_markup_create_text(b->doc, (const char *)a, an, &made)
          : kind == 'k' ? gtext_markup_create_cdata(b->doc, (const char *)a, an, &made)
                        : gtext_markup_create_comment(b->doc, (const char *)a, an, &made);
      ok = st == GTEXT_MARKUP_OK && gtext_markup_append_child(top, made) == GTEXT_MARKUP_OK;
      break;
    }
    case 'p': {
      if (nf != 3 || !field_str(f[1], &a, &an) || !field_str(f[2], &c, &cn) || !a) {
        ok = false;
        break;
      }
      ok = gtext_markup_create_processing_instruction(b->doc, (const char *)a, (const char *)c, cn, &made) == GTEXT_MARKUP_OK
          && gtext_markup_append_child(top, made) == GTEXT_MARKUP_OK;
      break;
    }
    case 'd': {
      if (nf != 4 || !field_str(f[1], &a, &an) || !field_str(f[2], &c, &cn) || !field_str(f[3], &d, &dn)) {
        ok = false;
        break;
      }
      ok = gtext_markup_create_doctype(b->doc, a ? (const char *)a : "", (const char *)c, (const char *)d, &made) == GTEXT_MARKUP_OK
          && gtext_markup_append_child(top, made) == GTEXT_MARKUP_OK;
      break;
    }
    case 'e': {
      if (nf < 5) {
        ok = false;
        break;
      }
      const size_t na = (size_t)strtoul(f[4], NULL, 10);
      if (nf != 5 + 4 * na) {
        ok = false;
        break;
      }
      unsigned char * prefix = NULL;
      size_t pn = 0;
      if (!field_str(f[1], &a, &an) || !field_str(f[2], &prefix, &pn) || !field_str(f[3], &c, &cn) || !c) {
        free(a);
        free(prefix);
        free(c);
        free(f);
        return false;
      }
      ok = gtext_markup_create_element(b->doc, (const char *)a, (const char *)prefix, (const char *)c, &made) == GTEXT_MARKUP_OK;
      for (size_t i = 0; ok && i < na; i++) {
        unsigned char *ans = NULL, *apx = NULL, *aln = NULL, *av = NULL;
        size_t l1, l2, l3, l4;
        if (!field_str(f[5 + 4 * i], &ans, &l1) || !field_str(f[6 + 4 * i], &apx, &l2) || !field_str(f[7 + 4 * i], &aln, &l3)
            || !field_str(f[8 + 4 * i], &av, &l4) || !aln || !av) {
          ok = false;
        }
        else {
          ok = gtext_markup_element_set_attribute(made, (const char *)ans, (const char *)apx, (const char *)aln,
                   (const char *)av, l4) == GTEXT_MARKUP_OK;
        }
        free(ans);
        free(apx);
        free(aln);
        free(av);
      }
      ok = ok && gtext_markup_append_child(top, made) == GTEXT_MARKUP_OK && push(b, made);
      free(prefix);
      break;
    }
    default:
      ok = false;
      break;
  }
  free(a);
  free(c);
  free(d);
  free(f);
  return ok;
}

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

static int run_ser(char * rest) {
  char * save = NULL;
  char * scope = strtok_r(rest, " \n", &save);
  char * script = strtok_r(NULL, " \n", &save);
  char * first = strtok_r(NULL, " \n", &save);
  if (!scope || !script || !first || (strcmp(scope, "children") != 0 && strcmp(scope, "outer") != 0)
      || (strcmp(script, "0") != 0 && strcmp(script, "1") != 0)) {
    return -1;
  }
  Build b;
  memset(&b, 0, sizeof(b));
  GTEXT_Markup_Options mo = gtext_markup_options_default();
  mo.max_depth = 0;
  mo.max_nodes = 0;
  mo.max_attributes = 0;
  if (gtext_markup_document_new(&mo, &b.doc) != GTEXT_MARKUP_OK) {
    return -1;
  }
  int result = -1;
  if (strcmp(first, "R,doc") == 0) {
    b.root = gtext_markup_document_root(b.doc);
  }
  else if (strcmp(first, "R,frag") == 0) {
    if (gtext_markup_create_fragment(b.doc, &b.root) != GTEXT_MARKUP_OK) {
      goto done;
    }
  }
  else {
    goto done;
  }
  if (!push(&b, b.root)) {
    goto done;
  }
  for (char * w = strtok_r(NULL, " \n", &save); w; w = strtok_r(NULL, " \n", &save)) {
    if (!apply(&b, w)) {
      goto done;
    }
  }
  {
    GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
    o.scripting = script[0] == '1';
    o.max_depth = 1000000;
    Out out = {NULL, 0, 0};
    const GTEXT_Markup_Sink sink = {out_write, &out};
    const GTEXT_Markup_Node * target = b.root;
    GTEXT_HTML_Serialize_Scope sc = GTEXT_HTML_SERIALIZE_CHILDREN;
    if (strcmp(scope, "outer") == 0) {
      target = gtext_markup_node_first_child(b.root);
      sc = GTEXT_HTML_SERIALIZE_OUTER;
      if (!target) {
        goto done;
      }
    }
    const GTEXT_HTML_Status st = gtext_html_serialize(target, sc, &o, &sink, NULL);
    printf("OUT ");
    put_hex(stdout, out.data ? out.data : "", out.length);
    printf("\nSTATUS %s\n", status_name(st));
    free(out.data);
    printf("END DONE\n");
    result = 0;
  }
done:
  free(b.stack);
  gtext_markup_document_free(b.doc);
  return result;
}

static int run_parse(char * rest) {
  char * save = NULL;
  char * script = strtok_r(rest, " \n", &save);
  char * hex = strtok_r(NULL, " \n", &save);
  char * ctxw = strtok_r(NULL, " \n", &save);
  if (!script || !hex || (strcmp(script, "0") != 0 && strcmp(script, "1") != 0)) {
    return -1;
  }
  bool absent;
  size_t n = 0;
  unsigned char * input = from_hex(hex, &n, &absent);
  if (!input) {
    return -1;
  }
  unsigned char * ctx_text = NULL;
  GTEXT_HTML_Fragment_Context fc;
  memset(&fc, 0, sizeof(fc));
  bool fragment = false;
  if (ctxw) {
    size_t cn = 0;
    if (strncmp(ctxw, "ctx:", 4) != 0 || !(ctx_text = from_hex(ctxw + 4, &cn, &absent)) || cn == 0
        || strlen((const char *)ctx_text) != cn) {
      free(input);
      free(ctx_text);
      return -1;
    }
    fragment = true;
    fc.ns = GTEXT_HTML_NS_HTML;
    fc.local_name = (const char *)ctx_text;
    if (cn > 4 && memcmp(ctx_text, "svg ", 4) == 0) {
      fc.ns = GTEXT_HTML_NS_SVG;
      fc.local_name += 4;
    }
    else if (cn > 5 && memcmp(ctx_text, "math ", 5) == 0) {
      fc.ns = GTEXT_HTML_NS_MATHML;
      fc.local_name += 5;
    }
  }
  GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
  po.scripting = script[0] == '1';
  po.count_errors_only = true;
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * frag = NULL;
  GTEXT_HTML_Result res;
  memset(&res, 0, sizeof(res));
  const GTEXT_HTML_Status st = fragment ? gtext_html_parse_fragment(&fc, input, n, &po, &doc, &frag, &res)
                                         : gtext_html_parse(input, n, &po, &doc, &res);
  gtext_html_result_free(&res);
  if (st != GTEXT_HTML_OK) {
    printf("STATUS %s\n", status_name(st));
  }
  else {
    printf("TREE R,%s", fragment ? "frag" : "doc");
    dump_children(stdout, fragment ? frag : gtext_markup_document_root(doc));
    printf("\nSTATUS OK\n");
    /* The tree as it is, written directly: the differential holds the tree it rebuilds from the ops to this. */
    GTEXT_HTML_Serialize_Options so = gtext_html_serialize_options_default();
    so.scripting = po.scripting;
    so.max_depth = 1000000;
    Out out = {NULL, 0, 0};
    const GTEXT_Markup_Sink sink = {out_write, &out};
    const GTEXT_HTML_Status ss =
        gtext_html_serialize(fragment ? frag : gtext_markup_document_root(doc), GTEXT_HTML_SERIALIZE_CHILDREN, &so, &sink, NULL);
    printf("DIRECT %s ", status_name(ss));
    put_hex(stdout, out.data ? out.data : "", out.length);
    printf("\n");
    free(out.data);
  }
  printf("END DONE\n");
  gtext_markup_document_free(doc);
  free(input);
  free(ctx_text);
  return 0;
}

int main(void) {
  char * line = NULL;
  size_t cap = 0;
  ssize_t got;
  setvbuf(stdout, NULL, _IOFBF, 1 << 16);
  while ((got = getline(&line, &cap, stdin)) > 0) {
    char * save = NULL;
    char * kind = strtok_r(line, " \n", &save);
    int rc = -1;
    if (kind && strcmp(kind, "parse") == 0) {
      rc = run_parse(save);
    }
    else if (kind && strcmp(kind, "ser") == 0) {
      rc = run_ser(save);
    }
    if (rc != 0) {
      printf("END E_REQUEST\n");
    }
    fflush(stdout);
  }
  free(line);
  return 0;
}
