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

// The runner of the selector vector gate (tools/conformance/css_selector_suite.py).
//
// It reads requests on standard input, one a line: `<value>` written with \\ for a backslash, \n for a line feed and \t for a tab. Each is read as the
// browsers read the selector of `sheet.insertRule(selector + "{}")`: a style rule whose prelude is the value, parsed by the rule parser, the prelude
// then read by gtext_css_selector_parse_values() as a complex selector list. For each it writes one line:
//
//   INVALID                          not a rule with a block, or a selector that is not valid
//   OK TAB <text> TAB <again>        <text> the serialization; <again> what the serialization is when it is read in its turn (`!INVALID` when refused)
//   ERR TAB <status>                 any other status
//
// It reads no vector and no expectation: what is compared is the scorer's business.
#include <ghoti.io/text/css.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char * unescape(const char * s, size_t n, size_t * out_len) {
  char * r = malloc(n + 3);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      i++;
      r[k++] = s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
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
    else if (s[i] == '\t') {
      fputs("\\t", stdout);
    }
    else {
      fputc(s[i], stdout);
    }
  }
}

/// Parse `value` + "{}" as a rule and its prelude as selectors; returns the status and the serialization (malloc) or NULL when invalid.
static GTEXT_CSS_Status one(const char * value, size_t length, char ** text, size_t * tlen) {
  *text = NULL;
  *tlen = 0;
  char * buf = malloc(length + 3);
  memcpy(buf, value, length);
  memcpy(buf + length, "{}", 3);
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_rule(buf, length + 2, NULL, &tree, NULL);
  free(buf);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (tree->syntax_error != GTEXT_CSS_SYNTAX_NONE || tree->rule_count != 1 || tree->rules[0].kind != GTEXT_CSS_RULE_QUALIFIED) {
    gtext_css_tree_free(tree);
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Selector_List * list = NULL;
  GTEXT_CSS_Selector_Result result;
  memset(&result, 0, sizeof result);
  st = gtext_css_selector_parse_values(tree->rules[0].prelude, tree->rules[0].prelude_count, NULL, &list, &result);
  if (st == GTEXT_CSS_OK && list) {
    st = gtext_css_selector_serialize_buffer(list, NULL, text, tlen);
  }
  gtext_css_selector_list_free(list);
  gtext_css_selector_result_free(&result);
  gtext_css_tree_free(tree);
  return st;
}

int main(void) {
  char * line = NULL;
  size_t cap = 0;
  ssize_t got;
  while ((got = getline(&line, &cap, stdin)) >= 0) {
    size_t n = (size_t)got;
    if (n && line[n - 1] == '\n') {
      n--;
    }
    size_t vlen;
    char * value = unescape(line, n, &vlen);
    char * text;
    size_t tlen;
    GTEXT_CSS_Status st = one(value, vlen, &text, &tlen);
    free(value);
    if (st != GTEXT_CSS_OK) {
      printf("ERR\t%s\n", gtext_css_status_string(st));
    }
    else if (!text) {
      puts("INVALID");
    }
    else {
      char * again;
      size_t alen;
      GTEXT_CSS_Status st2 = one(text, tlen, &again, &alen);
      fputs("OK\t", stdout);
      escape_out(text, tlen);
      fputc('\t', stdout);
      if (st2 != GTEXT_CSS_OK) {
        printf("ERR %s", gtext_css_status_string(st2));
      }
      else if (!again) {
        fputs("!INVALID", stdout);
      }
      else {
        escape_out(again, alen);
      }
      fputc('\n', stdout);
      free(again);
    }
    free(text);
  }
  free(line);
  return 0;
}
