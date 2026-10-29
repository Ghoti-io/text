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
 * The runner of tools/conformance/css_conditions_suite.py: one line on standard input per vector, `kind<TAB>query` with \\, \n and \t escaped; one line of answer
 * on standard output for each: `ser:<the serialization>` for a serialize vector, `match:1` or `match:0` for a reftest vector (the environment of the reftest
 * harness: a screen 800 by 600 at 1 dppx), `error:<status>` for a failure.
 */
#include <ghoti.io/text/css.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void unescape(char * s) {
  char * w = s;
  for (char * r = s; *r; r++) {
    if (*r == '\\' && r[1]) {
      r++;
      *w++ = *r == 'n' ? '\n' : (*r == 't' ? '\t' : *r);
    } else {
      *w++ = *r;
    }
  }
  *w = 0;
}

static void emit(const char * tag, const char * s, size_t n) {
  fputs(tag, stdout);
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\n') fputs("\\n", stdout);
    else if (s[i] == '\\') fputs("\\\\", stdout);
    else if (s[i] == '\t') fputs("\\t", stdout);
    else fputc(s[i], stdout);
  }
  fputc('\n', stdout);
}

/* The metrics of the font of the reftest harness (the test fonts' glyphs are as wide as they are tall), at the size asked. */
static bool metric(void * user, const GTEXT_CSS_Font_Query * q, GTEXT_CSS_Font_Metric m, double * px) {
  (void)user;
  *px = q->size_px * (m == GTEXT_CSS_METRIC_EX || m == GTEXT_CSS_METRIC_CH ? 0.5 : 1.0);
  return true;
}

int main(void) {
  GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(NULL);
  env.font_metric = metric;
  env.viewport.width = 800;
  env.viewport.height = 600;
  env.device.width = 800;
  env.device.height = 600;
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.media = &env;
  GTEXT_CSS_Conditions * conds = NULL;
  if (gtext_css_conditions_new(NULL, &co, &conds) != GTEXT_CSS_OK) {
    return 2;
  }
  char * line = NULL;
  size_t cap = 0;
  ssize_t n;
  while ((n = getline(&line, &cap, stdin)) >= 0) {
    if (n && line[n - 1] == '\n') line[--n] = 0;
    char * tab = strchr(line, '\t');
    if (!tab) {
      emit("error:", "no tab", 6);
      continue;
    }
    *tab = 0;
    char * query = tab + 1;
    unescape(query);
    if (strcmp(line, "serialize") == 0) {
      GTEXT_CSS_Media_List * l = NULL;
      GTEXT_CSS_Status st = gtext_css_media_list_parse(query, strlen(query), NULL, &l, NULL);
      char * out = NULL;
      size_t len = 0;
      if (st == GTEXT_CSS_OK) st = gtext_css_media_list_serialize_buffer(l, NULL, &out, &len);
      if (st != GTEXT_CSS_OK) {
        emit("error:", gtext_css_status_string(st), strlen(gtext_css_status_string(st)));
      } else {
        emit("ser:", out, len);
      }
      gtext_css_media_buffer_free(NULL, out);
      gtext_css_media_list_free(l);
    } else {
      bool m = false;
      GTEXT_CSS_Status st = gtext_css_conditions_matches_media(conds, query, strlen(query), NULL, &m);
      if (st != GTEXT_CSS_OK) {
        emit("error:", gtext_css_status_string(st), strlen(gtext_css_status_string(st)));
      } else {
        emit(m ? "match:1" : "match:0", "", 0);
      }
    }
  }
  free(line);
  gtext_css_conditions_free(conds);
  return 0;
}
