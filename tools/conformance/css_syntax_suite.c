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

/*
 * The runner of the CSS Syntax vector gate (tools/conformance/css_syntax_suite.py).
 *
 * One request per line on stdin:
 *
 *     <entry> <options> <hex of the input>
 *
 * <entry> is one of stylesheet, stylesheet_contents, declaration_list,
 * block_contents, rule, declaration, component_value, component_values,
 * comma_list, anb, declaration_value (a stylesheet: the first rule's first
 * declaration's value), outline (a stylesheet: its structure, for the WPT
 * tables that assert on rules and declarations). <options> is "-" or a
 * comma-separated list of: wtf8 (read
 * the input as WTF-8). The input may be empty ("-" for none).
 *
 * For each request it prints:
 *
 *     RESULT <json>     the result, in css-parsing-tests' representation
 *     SERIAL <hex>      the serialization (numbers as written), "-" if none
 *     CSSOM <hex>       the same with CSSOM's numbers
 *     ROUNDTRIP ok | <what failed>
 *     ERRORS <n>        parse errors raised
 *     END DONE          or END E_REQUEST for a line it does not understand,
 *                       or END <status> for a parse that failed
 *
 * The representation is css-parsing-tests' (its README, "Result
 * representation"), with what this parser has and the suite's 2021 model
 * does not: a rule's block, which the current draft parses into declarations
 * and rules, is written as the suite expects it, as the component values of
 * the block's text (read again by gtext_css_parse_component_values(), which
 * is what the 2021 algorithm returned). The comparison and every
 * normalization are in the scorer.
 */

#include <ghoti.io/text/css.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css_tree_canon.h"

static const char * g_input;
static size_t g_len;
static GTEXT_CSS_Parse_Options g_opts;

static void json_str(const char * s, size_t n) {
  putchar('"');
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      printf("\\%c", c);
    }
    else if (c < 0x20 || c == 0x7F) {
      printf("\\u%04x", c);
    }
    else {
      putchar(c);
    }
  }
  putchar('"');
}

static void json_cstr(const char * s) {
  json_str(s, strlen(s));
}

static void json_num(double d) {
  if (d != d) {
    printf("NaN");
  }
  else if (d > 1.7976931348623157e308) {
    printf("1e999");
  }
  else if (d < -1.7976931348623157e308) {
    printf("-1e999");
  }
  else {
    printf("%.17g", d);
  }
}

static const char * repr_type(const GTEXT_CSS_String * repr) {
  for (size_t i = 0; i < repr->length; i++) {
    if (repr->data[i] == '.' || repr->data[i] == 'e' || repr->data[i] == 'E') {
      return "number";
    }
  }
  return "integer";
}

static void values(const GTEXT_CSS_Component_Value * v, size_t n, int * first);

static void sep(int * first) {
  if (!*first) {
    putchar(',');
  }
  *first = 0;
}

static void value(const GTEXT_CSS_Component_Value * v, int * first) {
  sep(first);
  if (v->kind == GTEXT_CSS_VALUE_FUNCTION) {
    printf("[\"function\",");
    json_str(v->value.data, v->value.length);
    int f = 0;
    values(v->children, v->child_count, &f);
    printf("]");
    return;
  }
  if (v->kind == GTEXT_CSS_VALUE_BLOCK) {
    printf("[\"%s\"", v->type == GTEXT_CSS_TOKEN_OPEN_CURLY ? "{}" : v->type == GTEXT_CSS_TOKEN_OPEN_SQUARE ? "[]" : "()");
    int f = 0;
    values(v->children, v->child_count, &f);
    printf("]");
    return;
  }
  switch (v->type) {
  case GTEXT_CSS_TOKEN_IDENT:
    printf("[\"ident\",");
    json_str(v->value.data, v->value.length);
    printf("]");
    break;
  case GTEXT_CSS_TOKEN_AT_KEYWORD:
    printf("[\"at-keyword\",");
    json_str(v->value.data, v->value.length);
    printf("]");
    break;
  case GTEXT_CSS_TOKEN_HASH:
    printf("[\"hash\",");
    json_str(v->value.data, v->value.length);
    printf(",\"%s\"]", (v->flags & GTEXT_CSS_FLAG_HASH_ID) ? "id" : "unrestricted");
    break;
  case GTEXT_CSS_TOKEN_STRING:
    printf("[\"string\",");
    json_str(v->value.data, v->value.length);
    printf("]");
    if (v->flags & GTEXT_CSS_FLAG_UNCLOSED) {
      printf(",[\"error\",\"eof-in-string\"]");
    }
    break;
  case GTEXT_CSS_TOKEN_BAD_STRING:
    printf("[\"error\",\"bad-string\"]");
    break;
  case GTEXT_CSS_TOKEN_URL:
    printf("[\"url\",");
    json_str(v->value.data, v->value.length);
    printf("]");
    if (v->flags & GTEXT_CSS_FLAG_UNCLOSED) {
      printf(",[\"error\",\"eof-in-url\"]");
    }
    break;
  case GTEXT_CSS_TOKEN_BAD_URL:
    printf("[\"error\",\"bad-url\"]");
    break;
  case GTEXT_CSS_TOKEN_DELIM:
    json_str(v->value.data, v->value.length);
    break;
  case GTEXT_CSS_TOKEN_NUMBER:
  case GTEXT_CSS_TOKEN_PERCENTAGE:
  case GTEXT_CSS_TOKEN_DIMENSION:
    printf("[\"%s\",", v->type == GTEXT_CSS_TOKEN_NUMBER ? "number" : v->type == GTEXT_CSS_TOKEN_PERCENTAGE ? "percentage" : "dimension");
    json_str(v->repr.data, v->repr.length);
    putchar(',');
    json_num(v->number);
    if (v->type == GTEXT_CSS_TOKEN_PERCENTAGE) {
      printf(",\"%s\"", repr_type(&v->repr));
    }
    else {
      printf(",\"%s\"", (v->flags & GTEXT_CSS_FLAG_INTEGER) ? "integer" : "number");
    }
    if (v->type == GTEXT_CSS_TOKEN_DIMENSION) {
      putchar(',');
      json_str(v->unit.data, v->unit.length);
    }
    printf("]");
    break;
  case GTEXT_CSS_TOKEN_UNICODE_RANGE:
    printf("[\"unicode-range\",%u,%u]", (unsigned)v->range_start, (unsigned)v->range_end);
    break;
  case GTEXT_CSS_TOKEN_WHITESPACE: printf("\" \""); break;
  case GTEXT_CSS_TOKEN_CDO: printf("\"<!--\""); break;
  case GTEXT_CSS_TOKEN_CDC: printf("\"-->\""); break;
  case GTEXT_CSS_TOKEN_COLON: printf("\":\""); break;
  case GTEXT_CSS_TOKEN_SEMICOLON: printf("\";\""); break;
  case GTEXT_CSS_TOKEN_COMMA: printf("\",\""); break;
  case GTEXT_CSS_TOKEN_CLOSE_CURLY: printf("[\"error\",\"}\"]"); break;
  case GTEXT_CSS_TOKEN_CLOSE_PAREN: printf("[\"error\",\")\"]"); break;
  case GTEXT_CSS_TOKEN_CLOSE_SQUARE: printf("[\"error\",\"]\"]"); break;
  default: printf("[\"unexpected\",%d]", (int)v->type); break;
  }
}

static void values(const GTEXT_CSS_Component_Value * v, size_t n, int * first) {
  for (size_t i = 0; i < n; i++) {
    value(&v[i], first);
  }
}

static void value_list(const GTEXT_CSS_Component_Value * v, size_t n) {
  putchar('[');
  int f = 1;
  values(v, n, &f);
  putchar(']');
}

/* A rule's block as the 2021 model has it: the component values of its text. */
static void block_values(const GTEXT_CSS_Rule * r) {
  const size_t begin = r->block_span.offset + 1;
  size_t end = r->block_span.end;
  if (!r->unclosed && end > begin) {
    end--;
  }
  if (end < begin) {
    end = begin;
  }
  GTEXT_CSS_Tree * t = NULL;
  GTEXT_CSS_Parse_Options o = g_opts;
  o.validity.declaration = NULL;
  o.validity.rule = NULL;
  if (gtext_css_parse_component_values(g_input + begin, end - begin, &o, &t, NULL) != GTEXT_CSS_OK) {
    printf("[\"unreadable block\"]");
    return;
  }
  value_list(t->values, t->value_count);
  gtext_css_tree_free(t);
}

static void decl(const GTEXT_CSS_Declaration * d) {
  printf("[\"declaration\",");
  json_str(d->name.data, d->name.length);
  putchar(',');
  value_list(d->value, d->value_count);
  printf(",%s]", d->important ? "true" : "false");
}

static void rule(const GTEXT_CSS_Rule * r) {
  if (r->kind == GTEXT_CSS_RULE_AT) {
    printf("[\"at-rule\",");
    json_str(r->name.data, r->name.length);
    putchar(',');
    value_list(r->prelude, r->prelude_count);
    putchar(',');
    if (r->has_block) {
      block_values(r);
    }
    else {
      printf("null");
    }
    putchar(']');
    return;
  }
  if (r->kind == GTEXT_CSS_RULE_QUALIFIED) {
    printf("[\"qualified rule\",");
    value_list(r->prelude, r->prelude_count);
    putchar(',');
    block_values(r);
    putchar(']');
    return;
  }
  /* A nested declarations rule is only ever inside a block, which this
     representation writes as text. */
  printf("[\"nested declarations\"]");
}

/* ---- the outline: the structure of a stylesheet, for the WPT tables ---- */

static void print_serialized(const GTEXT_CSS_Component_Value * v, size_t n, bool trim) {
  char * text = NULL;
  size_t len = 0;
  if (gtext_css_serialize_values_buffer(v, n, NULL, &text, &len) != GTEXT_CSS_OK) {
    printf("null");
    return;
  }
  size_t b = 0, e = len;
  if (trim) {
    while (b < e && text[b] == ' ') {
      b++;
    }
    while (e > b && text[e - 1] == ' ') {
      e--;
    }
  }
  json_str(text + b, e - b);
  gtext_css_serialize_buffer_free(NULL, text);
}

static void outline_decls(const GTEXT_CSS_Declaration * d, size_t n) {
  putchar('[');
  for (size_t i = 0; i < n; i++) {
    if (i) {
      putchar(',');
    }
    putchar('[');
    json_str(d[i].name.data, d[i].name.length);
    putchar(',');
    if (d[i].original_text.data) {
      json_str(d[i].original_text.data, d[i].original_text.length);
    }
    else {
      print_serialized(d[i].value, d[i].value_count, false);
    }
    printf(",%s]", d[i].important ? "true" : "false");
  }
  putchar(']');
}

static void outline_rule(const GTEXT_CSS_Rule * r) {
  if (r->kind == GTEXT_CSS_RULE_NESTED_DECLARATIONS) {
    printf("{\"n\":");
    outline_decls(r->declarations, r->declaration_count);
    putchar('}');
    return;
  }
  if (r->kind == GTEXT_CSS_RULE_QUALIFIED) {
    printf("{\"q\":");
    print_serialized(r->prelude, r->prelude_count, true);
    printf(",\"d\":");
    outline_decls(r->declarations, r->declaration_count);
    printf(",\"r\":[");
    for (size_t i = 0; i < r->rule_count; i++) {
      if (i) {
        putchar(',');
      }
      outline_rule(&r->rules[i]);
    }
    printf("]}");
    return;
  }
  printf("{\"at\":");
  json_str(r->name.data, r->name.length);
  printf(",\"p\":");
  print_serialized(r->prelude, r->prelude_count, true);
  if (r->has_block) {
    printf(",\"c\":[");
    for (size_t i = 0; i < r->content_count; i++) {
      if (i) {
        putchar(',');
      }
      if (r->contents[i].kind == GTEXT_CSS_ITEM_RULE) {
        outline_rule(r->contents[i].rule);
      }
      else {
        printf("{\"n\":");
        outline_decls(r->contents[i].declarations, r->contents[i].declaration_count);
        putchar('}');
      }
    }
    putchar(']');
  }
  putchar('}');
}

static const char * syntax_name(GTEXT_CSS_Syntax_Error e) {
  switch (e) {
  case GTEXT_CSS_SYNTAX_EMPTY: return "empty";
  case GTEXT_CSS_SYNTAX_INVALID: return "invalid";
  case GTEXT_CSS_SYNTAX_EXTRA_INPUT: return "extra-input";
  default: return "none";
  }
}

static void print_tree(const GTEXT_CSS_Tree * t) {
  if (t->syntax_error != GTEXT_CSS_SYNTAX_NONE) {
    printf("[\"error\",\"%s\"]", syntax_name(t->syntax_error));
    return;
  }
  switch (t->kind) {
  case GTEXT_CSS_TREE_STYLESHEET:
  case GTEXT_CSS_TREE_STYLESHEET_CONTENTS:
    putchar('[');
    for (size_t i = 0; i < t->rule_count; i++) {
      if (i) {
        putchar(',');
      }
      rule(&t->rules[i]);
    }
    putchar(']');
    return;
  case GTEXT_CSS_TREE_RULE:
    rule(&t->rules[0]);
    return;
  case GTEXT_CSS_TREE_DECLARATION_LIST:
    putchar('[');
    for (size_t i = 0; i < t->declaration_count; i++) {
      if (i) {
        putchar(',');
      }
      decl(&t->declarations[i]);
    }
    putchar(']');
    return;
  case GTEXT_CSS_TREE_DECLARATION:
    decl(&t->declarations[0]);
    return;
  case GTEXT_CSS_TREE_BLOCK_CONTENTS: {
    putchar('[');
    int first = 1;
    for (size_t i = 0; i < t->item_count; i++) {
      if (t->items[i].kind == GTEXT_CSS_ITEM_RULE) {
        sep(&first);
        rule(t->items[i].rule);
      }
      else {
        for (size_t k = 0; k < t->items[i].declaration_count; k++) {
          sep(&first);
          decl(&t->items[i].declarations[k]);
        }
      }
    }
    putchar(']');
    return;
  }
  case GTEXT_CSS_TREE_COMPONENT_VALUE: {
    int f = 1;
    values(t->values, t->value_count, &f);
    return;
  }
  case GTEXT_CSS_TREE_COMPONENT_VALUE_LIST:
    value_list(t->values, t->value_count);
    return;
  case GTEXT_CSS_TREE_COMMA_LIST:
    putchar('[');
    for (size_t i = 0; i < t->list_count; i++) {
      if (i) {
        putchar(',');
      }
      value_list(t->lists[i].values, t->lists[i].count);
    }
    putchar(']');
    return;
  }
}

static int hexval(int c) {
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

static void put_hex(const char * s, size_t n) {
  if (n == 0) {
    printf("-");
    return;
  }
  for (size_t i = 0; i < n; i++) {
    printf("%02x", (unsigned char)s[i]);
  }
}

static void serial(const GTEXT_CSS_Tree * t, GTEXT_CSS_Number_Style numbers, const char * label) {
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.numbers = numbers;
  char * text = NULL;
  size_t len = 0;
  printf("%s ", label);
  if (gtext_css_serialize_tree_buffer(t, &so, &text, &len) == GTEXT_CSS_OK) {
    put_hex(text, len);
    gtext_css_serialize_buffer_free(&so, text);
  }
  else {
    printf("FAILED");
  }
  putchar('\n');
}

typedef GTEXT_CSS_Status (*parse_fn)(const char *, size_t, const GTEXT_CSS_Parse_Options *, GTEXT_CSS_Tree **,
    GTEXT_CSS_Result *);

static parse_fn entry_fn(const char * entry) {
  static const struct {
    const char * name;
    parse_fn fn;
  } table[] = {
      {"stylesheet", gtext_css_parse_stylesheet},
      {"stylesheet_contents", gtext_css_parse_stylesheet_contents},
      {"declaration_list", gtext_css_parse_declaration_list},
      {"block_contents", gtext_css_parse_block_contents},
      {"rule", gtext_css_parse_rule},
      {"declaration", gtext_css_parse_declaration},
      {"component_value", gtext_css_parse_component_value},
      {"component_values", gtext_css_parse_component_values},
      {"comma_list", gtext_css_parse_comma_list},
      {"anb", gtext_css_parse_component_values},
      {"declaration_value", gtext_css_parse_stylesheet},
      {"outline", gtext_css_parse_stylesheet},
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    if (strcmp(table[i].name, entry) == 0) {
      return table[i].fn;
    }
  }
  return NULL;
}

static void request(char * line) {
  char * entry = strtok(line, " \t\r\n");
  char * opts = entry ? strtok(NULL, " \t\r\n") : NULL;
  char * hex = opts ? strtok(NULL, " \t\r\n") : NULL;
  char * extra = hex ? strtok(NULL, " \t\r\n") : NULL;
  parse_fn fn = entry ? entry_fn(entry) : NULL;
  if (!fn || !opts || !hex || extra) {
    printf("END E_REQUEST\n");
    return;
  }
  g_opts = gtext_css_parse_options_default();
  if (strcmp(opts, "-") != 0) {
    for (char * o = strtok(opts, ","); o; o = strtok(NULL, ",")) {
      if (strcmp(o, "wtf8") == 0) {
        g_opts.input_wtf8 = true;
      }
      else {
        printf("END E_REQUEST\n");
        return;
      }
    }
  }
  size_t n = 0;
  static char buf[1 << 20];
  if (strcmp(hex, "-") != 0) {
    const size_t hl = strlen(hex);
    if (hl % 2 || hl / 2 > sizeof buf) {
      printf("END E_REQUEST\n");
      return;
    }
    for (size_t i = 0; i < hl; i += 2) {
      const int a = hexval(hex[i]), b = hexval(hex[i + 1]);
      if (a < 0 || b < 0) {
        printf("END E_REQUEST\n");
        return;
      }
      buf[n++] = (char)(a * 16 + b);
    }
  }
  g_input = buf;
  g_len = n;
  GTEXT_CSS_Tree * t = NULL;
  GTEXT_CSS_Result r;
  memset(&r, 0, sizeof r);
  const GTEXT_CSS_Status st = fn(buf, n, &g_opts, &t, &r);
  if (st != GTEXT_CSS_OK) {
    printf("END %s\n", gtext_css_status_string(st));
    gtext_css_result_free(&r);
    return;
  }
  printf("RESULT ");
  if (strcmp(entry, "anb") == 0) {
    int64_t a, b;
    if (gtext_css_anb_parse(t->values, t->value_count, &a, &b)) {
      char s[64];
      const size_t len = gtext_css_anb_serialize(a, b, s, sizeof s);
      printf("[%lld,%lld]\nSERIAL ", (long long)a, (long long)b);
      put_hex(s, len);
      printf("\n");
    }
    else {
      printf("null\nSERIAL -\n");
    }
    printf("CSSOM -\n");
  }
  else if (strcmp(entry, "outline") == 0) {
    putchar('[');
    for (size_t i = 0; i < t->rule_count; i++) {
      if (i) {
        putchar(',');
      }
      outline_rule(&t->rules[i]);
    }
    printf("]\nSERIAL -\nCSSOM -\n");
  }
  else if (strcmp(entry, "declaration_value") == 0) {
    /* What getPropertyValue() reads of a custom property: the first
       declaration of the first rule, its value serialized. */
    if (t->rule_count == 0 || t->rules[0].declaration_count == 0) {
      printf("null\nSERIAL -\nCSSOM -\n");
    }
    else {
      const GTEXT_CSS_Declaration * d = &t->rules[0].declarations[0];
      value_list(d->value, d->value_count);
      printf("\nSERIAL ");
      char * text = NULL;
      size_t len = 0;
      if (gtext_css_serialize_values_buffer(d->value, d->value_count, NULL, &text, &len) == GTEXT_CSS_OK) {
        put_hex(text, len);
        gtext_css_serialize_buffer_free(NULL, text);
      }
      else {
        printf("FAILED");
      }
      printf("\nCSSOM -\n");
    }
  }
  else {
    print_tree(t);
    putchar('\n');
    serial(t, GTEXT_CSS_NUMBERS_SOURCE, "SERIAL");
    serial(t, GTEXT_CSS_NUMBERS_CSSOM, "CSSOM");
  }
  char * detail = NULL;
  const char * rt = css_round_trip(t, &g_opts, &detail);
  if (rt) {
    printf("ROUNDTRIP %s:", rt);
    for (char * c = detail; c && *c; c++) {
      putchar(*c == '\n' ? ' ' : *c);
    }
    putchar('\n');
  }
  else {
    printf("ROUNDTRIP ok\n");
  }
  free(detail);
  printf("ERRORS %zu\n", gtext_css_result_error_count(&r) + gtext_css_result_errors_dropped(&r));
  printf("END DONE\n");
  gtext_css_result_free(&r);
  gtext_css_tree_free(t);
}

int main(void) {
  static char line[(1 << 21) + 256];
  while (fgets(line, sizeof line, stdin)) {
    request(line);
    fflush(stdout);
  }
  return 0;
}
