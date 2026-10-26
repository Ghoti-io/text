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

// The runner of the CSS property vector gate (tools/conformance/css_property_suite.py).
//
// It reads requests on standard input, one a line: `<flags> TAB <property> TAB <value>`, the property and value written with \\ for
// a backslash, \n for a line feed and \t for a tab, the flags a (possibly empty) string of letters: `q` for quirks mode. For each it parses the
// value as the value of the property (gtext_css_parse_property_text, the generated registry), and writes one line:
//
//   UNKNOWN                          no such property
//   UNDECIDED                        the property is known and the library cannot judge it (the pin has no grammar)
//   INVALID                          the value is not one of the property's
//   OK TAB <outcome> TAB <text> TAB <again>
//                                    <outcome> is valid, wide or pending; <text> the serialization; <again> what the serialization
//                                    serializes as when it is parsed in its turn (`!INVALID` when it is refused)
//   ERR TAB <status>                 any other status
//
// With `-n` as the only argument the line instead names the property's nature: `SHORTHAND` or `LONGHAND` (an alias is what its target
// is) for a name the registry has, or `UNKNOWN`, so the scorer can tell a shorthand's vectors, whose serialization belongs to C2a.
// It reads no vector and no expectation: what is compared is the scorer's business.
#include <ghoti.io/text/css.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char * unescape(const char * s, size_t n, size_t * out_len) {
  char * r = malloc(n + 1);
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

static const char * outcome_name(GTEXT_CSS_Property_Outcome o) {
  switch (o) {
  case GTEXT_CSS_PROP_RESULT_VALID: return "valid";
  case GTEXT_CSS_PROP_RESULT_WIDE: return "wide";
  case GTEXT_CSS_PROP_RESULT_PENDING: return "pending";
  case GTEXT_CSS_PROP_RESULT_INVALID: return "invalid";
  default: return "unknown";
  }
}

/** Parse and serialize; returns the status of the parse, fills `text` (malloc) with the serialization when it is valid-ish. */
static GTEXT_CSS_Status one(const GTEXT_CSS_Registry * reg, const char * name, size_t nlen, const char * value, size_t vlen, bool quirks,
    GTEXT_CSS_Property_Outcome * outcome, char ** text, size_t * tlen) {
  GTEXT_CSS_Property_Options opt;
  memset(&opt, 0, sizeof opt);
  opt.quirks = quirks;
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  GTEXT_CSS_Status st = gtext_css_parse_property_text(reg, name, nlen, value, vlen, &opt, &pv);
  *outcome = pv.outcome;
  *text = NULL;
  *tlen = 0;
  if (st == GTEXT_CSS_OK && (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE ||
      pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING)) {
    st = gtext_css_property_value_serialize_buffer(&pv, NULL, text, tlen);
  }
  gtext_css_property_value_free(&pv);
  return st;
}

int main(int argc, char ** argv) {
  const bool nature = argc > 1 && strcmp(argv[1], "-n") == 0;
  GTEXT_CSS_Registry * reg = NULL;
  if (gtext_css_registry_new(NULL, &reg) != GTEXT_CSS_OK) {
    return 2;
  }
  char * line = NULL;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    char * t1 = memchr(line, '\t', (size_t)len);
    char * t2 = t1 ? memchr(t1 + 1, '\t', (size_t)len - (size_t)(t1 + 1 - line)) : NULL;
    if (!t1 || !t2) {
      puts("BAD");
      fflush(stdout);
      continue;
    }
    bool quirks = memchr(line, 'q', (size_t)(t1 - line)) != NULL;
    size_t nlen, vlen;
    char * name = unescape(t1 + 1, (size_t)(t2 - t1 - 1), &nlen);
    char * value = unescape(t2 + 1, (size_t)len - (size_t)(t2 + 1 - line), &vlen);
    if (nature) {
      GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(reg, name, nlen);
      GTEXT_CSS_Property_Info info;
      if (!id || !gtext_css_registry_property_info(reg, id, &info)) {
        puts("UNKNOWN");
      }
      else if (info.flags & GTEXT_CSS_PROP_ALIAS) {
        /* an alias is what its target is: `grid-gap` is `gap`, a shorthand */
        GTEXT_CSS_Property_Info target;
        const bool known = gtext_css_registry_property_info(reg, gtext_css_registry_resolve_alias(reg, id), &target);
        puts(known && (target.flags & GTEXT_CSS_PROP_SHORTHAND) ? "SHORTHAND" : "LONGHAND");
      }
      else if (info.flags & GTEXT_CSS_PROP_SHORTHAND) {
        puts("SHORTHAND");
      }
      else {
        puts("LONGHAND");
      }
      free(name);
      free(value);
      fflush(stdout);
      continue;
    }
    GTEXT_CSS_Property_Outcome outcome;
    char * text;
    size_t tlen;
    GTEXT_CSS_Status st = one(reg, name, nlen, value, vlen, quirks, &outcome, &text, &tlen);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      puts("UNDECIDED");
    }
    else if (st != GTEXT_CSS_OK) {
      printf("ERR\t%s\n", gtext_css_status_string(st));
    }
    else if (outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN) {
      puts("UNKNOWN");
    }
    else if (outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
      puts("INVALID");
    }
    else {
      GTEXT_CSS_Property_Outcome o2;
      char * again;
      size_t alen;
      GTEXT_CSS_Status st2 = one(reg, name, nlen, text, tlen, quirks, &o2, &again, &alen);
      fputs("OK\t", stdout);
      fputs(outcome_name(outcome), stdout);
      fputc('\t', stdout);
      escape_out(text, tlen);
      fputc('\t', stdout);
      if (st2 != GTEXT_CSS_OK || (o2 != GTEXT_CSS_PROP_RESULT_VALID && o2 != GTEXT_CSS_PROP_RESULT_WIDE && o2 != GTEXT_CSS_PROP_RESULT_PENDING)) {
        fputs("!INVALID", stdout);
      }
      else {
        escape_out(again, alen);
      }
      fputc('\n', stdout);
      gtext_css_serialize_buffer_free(NULL, again);
    }
    gtext_css_serialize_buffer_free(NULL, text);
    free(name);
    free(value);
    fflush(stdout);
  }
  free(line);
  gtext_css_registry_free(reg);
  return 0;
}
