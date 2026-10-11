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
 * Lookups over the HTML named character reference table.
 *
 * WHATWG HTML, "Named character references" and the tokenizer's "named
 * character reference state".
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "charref_internal.h"

/* Order of a candidate against an entry's name: the bytes first, and a name
   that is a proper prefix of the other sorts first. That is the order
   tools/html/gen_charrefs.py sorts by, and the binary search below is only
   correct because the two agree. */
static int compare(const char * name, size_t len,
    const GTEXT_HTML_CharRef * entry) {
  const size_t other = entry->name_length;
  const int c = memcmp(name, gtext_html_charref_name(entry),
      len < other ? len : other);
  if (c != 0) {
    return c;
  }
  if (len == other) {
    return 0;
  }
  return len < other ? -1 : 1;
}

const GTEXT_HTML_CharRef * gtext_html_charref_find(
    const char * name, size_t len) {
  if (!name || len < gtext_html_charref_shortest
      || len > gtext_html_charref_longest) {
    return NULL;
  }
  size_t low = 0;
  size_t high = gtext_html_charref_count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    const int c = compare(name, len, &gtext_html_charref_table[mid]);
    if (c == 0) {
      return &gtext_html_charref_table[mid];
    }
    if (c < 0) {
      high = mid;
    }
    else {
      low = mid + 1;
    }
  }
  return NULL;
}

static int is_alnum(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
      || (c >= 'a' && c <= 'z');
}

const GTEXT_HTML_CharRef * gtext_html_charref_longest_match(
    const char * text, size_t len) {
  if (!text) {
    return NULL;
  }
  /* A name is letters and digits and then at most one `;`, so no match can be
     longer than the alphanumeric run here, plus one more byte if that run is
     followed by `;`. Starting there instead of at the longest name in the
     table turns thirty tries into one or two for ordinary text. The run is
     measured only as far as the longest name can reach: nothing past that can
     matter, and a megabyte of letters after an `&` must not cost a megabyte. */
  const size_t reach = len < gtext_html_charref_longest
      ? len : gtext_html_charref_longest;
  size_t run = 0;
  while (run < reach && is_alnum(text[run])) {
    run++;
  }
  size_t top = run;
  if (run < len && text[run] == ';' && run < gtext_html_charref_longest) {
    top = run + 1;
  }
  for (size_t n = top; n >= gtext_html_charref_shortest; n--) {
    const GTEXT_HTML_CharRef * entry = gtext_html_charref_find(text, n);
    if (entry) {
      return entry;
    }
  }
  return NULL;
}
