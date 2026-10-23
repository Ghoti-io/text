// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2026 Corey Pennycuff
//
// This file is part of Ghoti.io Text.
//
// Ghoti.io Text is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// The runner of the CSS values gate (tools/conformance/css_values_suite.py).
//
// It reads requests on standard input, one a line: `<grammar> TAB <input>`, both written with \\ for a backslash and
// \n for a line feed. For each it compiles the grammar (against the function types), parses the input as component
// values, matches, and writes one line: `NO` (no match), `OK TAB <the value, written>`, or `ERR TAB <status>`. A line it
// cannot read is `BAD`. It reads no vector and no expectation: what is compared is the scorer's business.
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_function.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char * unescape(const char * s, size_t n, size_t * out_len) {
  char * r = malloc(n + 1);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      i++;
      r[k++] = s[i] == 'n' ? '\n' : s[i];
    }
    else {
      r[k++] = s[i];
    }
  }
  r[k] = '\0';
  *out_len = k;
  return r;
}

static void escape_out(const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\') {
      fputs("\\\\", stdout);
    }
    else if (s[i] == '\n') {
      fputs("\\n", stdout);
    }
    else {
      fputc(s[i], stdout);
    }
  }
}

typedef struct {
  char * text;
  GTEXT_CSS_Compiled * compiled;
} cached;

int main(void) {
  GTEXT_CSS_Function_Types * types = NULL;
  if (gtext_css_function_types_new(NULL, &types) != GTEXT_CSS_OK) {
    return 2;
  }
  cached cache[64];
  size_t ncache = 0;
  char * line = NULL;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    char * tab = memchr(line, '\t', (size_t)len);
    if (!tab) {
      puts("BAD");
      fflush(stdout);
      continue;
    }
    size_t glen, ilen;
    char * grammar = unescape(line, (size_t)(tab - line), &glen);
    char * input = unescape(tab + 1, (size_t)len - (size_t)(tab + 1 - line), &ilen);
    GTEXT_CSS_Compiled * c = NULL;
    for (size_t i = 0; i < ncache; i++) {
      if (strcmp(cache[i].text, grammar) == 0) {
        c = cache[i].compiled;
        break;
      }
    }
    if (!c) {
      if (gtext_css_compile(grammar, glen, gtext_css_function_types_resolve, types, NULL, &c, NULL) != GTEXT_CSS_OK || ncache == 64) {
        puts("BAD");
        free(grammar);
        free(input);
        fflush(stdout);
        continue;
      }
      cache[ncache].text = strdup(grammar);
      cache[ncache].compiled = c;
      ncache++;
    }
    GTEXT_CSS_Tree * tree = NULL;
    GTEXT_CSS_Result res;
    memset(&res, 0, sizeof res);
    if (gtext_css_parse_component_values(input, ilen, NULL, &tree, &res) != GTEXT_CSS_OK) {
      puts("ERR\tparse");
    }
    else {
      bool matched = false;
      GTEXT_CSS_Value_Set * set = NULL;
      const GTEXT_CSS_Status st = gtext_css_match(c, tree->values, tree->value_count, NULL, &matched, &set, NULL);
      if (st != GTEXT_CSS_OK) {
        printf("ERR\t%s\n", gtext_css_status_string(st));
      }
      else if (!matched) {
        puts("NO");
      }
      else {
        char * buf = NULL;
        size_t blen = 0;
        const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
        if (!root || gtext_css_serialize_value_buffer(root, NULL, &buf, &blen) != GTEXT_CSS_OK) {
          puts("ERR\tserialize");
        }
        else {
          fputs("OK\t", stdout);
          escape_out(buf, blen);
          fputc('\n', stdout);
          gtext_css_serialize_buffer_free(NULL, buf);
        }
      }
      gtext_css_value_set_free(set);
    }
    gtext_css_tree_free(tree);
    gtext_css_result_free(&res);
    free(grammar);
    free(input);
    fflush(stdout);
  }
  free(line);
  for (size_t i = 0; i < ncache; i++) {
    free(cache[i].text);
    gtext_css_compiled_free(cache[i].compiled);
  }
  gtext_css_function_types_free(types);
  return 0;
}
