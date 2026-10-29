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

/**
 * @file
 *
 * Scores the at-rule layer against the vectors tools/css/extract-wpt-atrules.py reads from the pinned web-platform-tests. Usage: css_atrules_suite <vectors.tsv> <exceptions>.
 *
 * Each vector is built into a rule and parsed: a name or a selector is valid when the rule (or its keyframe block) is kept, a source list is valid when the descriptor survives.
 * A failure must be a line of the exceptions file (kind, input, reason); a stale line is an error.
 */

#include <ghoti.io/text/css.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// The helper of css-counter-styles/counter-style-at-rule/support/counter-style-testcommon.js: the rule it builds around the descriptor under test.
static void counter_rule(char * text, size_t cap, const char * descriptor, const char * value) {
  char rest[512] = "";
  if (strcmp(descriptor, "system") == 0) {
    if (strcmp(value, "additive") == 0) {
      snprintf(rest, sizeof rest, "; additive-symbols: 1 \"I\"");
    }
    else if (strncmp(value, "extends", 7) != 0) {
      snprintf(rest, sizeof rest, "; symbols: \"X\" \"Y\"");
    }
  }
  else if (strcmp(descriptor, "symbols") == 0) {
    snprintf(rest, sizeof rest, "; system: symbolic");
  }
  else if (strcmp(descriptor, "additive-symbols") == 0) {
    snprintf(rest, sizeof rest, "; system: additive");
  }
  else {
    snprintf(rest, sizeof rest, "; system: symbolic; symbols: \"X\" \"Y\"");
  }
  snprintf(text, cap, "@counter-style foo { %s: %s%s }", descriptor, value, rest);
}

static int rule_valid(const GTEXT_CSS_Registry * reg, const char * kind, const char * input) {
  char text[8192];
  char desc[128] = "", value[4096] = "", expected[4096] = "";
  const int descriptor_kind = strcmp(kind, "counter-descriptor") == 0 || strcmp(kind, "face-descriptor") == 0;
  if (descriptor_kind) {
    const char * t1 = strchr(input, '\x1f');
    const char * t2 = t1 ? strchr(t1 + 1, '\x1f') : NULL;
    if (!t1 || !t2) {
      return 0;
    }
    snprintf(desc, sizeof desc, "%.*s", (int)(t1 - input), input);
    snprintf(value, sizeof value, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
    snprintf(expected, sizeof expected, "%s", t2 + 1);
    if (kind[0] == 'c') {
      counter_rule(text, sizeof text, desc, value);
    }
    else {
      snprintf(text, sizeof text, "@font-face { %s: %s }", desc, value);
    }
  }
  else if (strcmp(kind, "counter-name") == 0) {
    snprintf(text, sizeof text, "@counter-style %s { system: symbolic; symbols: 'X' 'Y'; }", input);
  }
  else if (strcmp(kind, "keyframes-name") == 0) {
    snprintf(text, sizeof text, "@keyframes %s { }", input);
  }
  else if (strcmp(kind, "keyframe-selector") == 0) {
    snprintf(text, sizeof text, "@keyframes --foo { %s {} }", input);
  }
  else {
    snprintf(text, sizeof text, "@font-face { src: %s }", input);
  }
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result res;
  memset(&res, 0, sizeof res);
  if (gtext_css_parse_stylesheet(text, strlen(text), NULL, &tree, &res) != GTEXT_CSS_OK || !tree || tree->rule_count != 1) {
    gtext_css_tree_free(tree);
    gtext_css_result_free(&res);
    return 0;
  }
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Atrule_Result ar;
  GTEXT_CSS_Atrule_Options o = gtext_css_atrule_options_default();
  int ok = 0;
  if (gtext_css_atrule_parse(reg, &tree->rules[0], &o, &a, &ar) == GTEXT_CSS_OK && ar.valid && a) {
    if (strcmp(kind, "keyframes-name") == 0 || strcmp(kind, "counter-name") == 0) {
      ok = 1;
    }
    else if (descriptor_kind) {
      GTEXT_CSS_Atrule_Descriptor d;
      ok = gtext_css_atrule_descriptor_find(a, desc, strlen(desc), &d);
      if (ok && expected[0]) {
        if (kind[0] == 'c') {
          /* the page asks for `descriptor: expected` somewhere in the rule's cssText */
          char * buf = NULL;
          size_t n = 0;
          char want[8300];
          snprintf(want, sizeof want, "%s: %s", desc, expected);
          ok = gtext_css_atrule_serialize_buffer(a, NULL, &buf, &n) == GTEXT_CSS_OK && buf && strstr(buf, want) != NULL;
          gtext_css_atrule_buffer_free(NULL, buf);
        }
        else {
          ok = strlen(expected) == d.text.length && memcmp(expected, d.text.data, d.text.length) == 0;
        }
      }
    }
    else if (strcmp(kind, "keyframe-selector") == 0) {
      ok = gtext_css_atrule_child_count(a) == 1;
    }
    else {
      GTEXT_CSS_Atrule_Descriptor d;
      ok = gtext_css_atrule_descriptor_find(a, "src", 3, &d);
    }
  }
  gtext_css_atrule_free(a);
  gtext_css_tree_free(tree);
  gtext_css_result_free(&res);
  return ok;
}

int main(int argc, char ** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s vectors.tsv exceptions\n", argv[0]);
    return 2;
  }
  GTEXT_CSS_Registry * reg = NULL;
  if (gtext_css_registry_new(NULL, &reg) != GTEXT_CSS_OK) {
    fprintf(stderr, "no registry\n");
    return 2;
  }
  char ** exc = NULL;
  int *used = NULL, nexc = 0;
  FILE * ef = fopen(argv[2], "r");
  char line[16384];
  while (ef && fgets(line, sizeof line, ef)) {
    if (line[0] == '#' || line[0] == '\n') {
      continue;
    }
    line[strcspn(line, "\n")] = 0;
    exc = realloc(exc, (size_t)(nexc + 1) * sizeof *exc);
    used = realloc(used, (size_t)(nexc + 1) * sizeof *used);
    exc[nexc] = strdup(line);
    used[nexc++] = 0;
  }
  if (ef) {
    fclose(ef);
  }
  FILE * vf = fopen(argv[1], "r");
  if (!vf) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  int total = 0, pass = 0, listed = 0, bad = 0;
  while (fgets(line, sizeof line, vf)) {
    if (line[0] == '#') {
      continue;
    }
    line[strcspn(line, "\n")] = 0;
    char *kind = line, *page = strchr(kind, '\t');
    if (!page) {
      continue;
    }
    *page++ = 0;
    char * input = strchr(page, '\t');
    if (!input) {
      continue;
    }
    *input++ = 0;
    char * want = strrchr(input, '\t');
    if (!want) {
      continue;
    }
    *want++ = 0;
    total++;
    const int got = rule_valid(reg, kind, input);
    if (got == (strcmp(want, "valid") == 0)) {
      pass++;
      continue;
    }
    char key[16384];
    snprintf(key, sizeof key, "%s\t%s\t", kind, input);
    int found = 0;
    for (int i = 0; i < nexc; i++) {
      if (strncmp(exc[i], key, strlen(key)) == 0) {
        used[i] = 1;
        found = 1;
      }
    }
    if (found) {
      listed++;
    }
    else {
      bad++;
      printf("FAIL %s %s (expected %s)\n", kind, input, want);
    }
  }
  fclose(vf);
  for (int i = 0; i < nexc; i++) {
    if (!used[i]) {
      bad++;
      printf("STALE exception: %s\n", exc[i]);
    }
  }
  printf("css-atrules: %d vectors, %d pass, %d listed exceptions, %d problems\n", total, pass, listed, bad);
  return bad ? 1 : 0;
}
