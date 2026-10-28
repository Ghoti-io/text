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
 * `:lang()` (css_match.h): RFC 4647, 3.3.2, extended filtering, as Selectors 4 uses it. The range and the tag are compared ASCII case-insensitively as they are written; the
 * canonicalization to extlang form that Selectors 4 asks of both needs the IANA language subtag registry, which this library does not carry, so a range and a tag that differ
 * only by an extended language subtag (`zh-cmn-Hans` and `cmn-Hans`) are not the same here. A range that is not well-formed matches nothing; `:lang("")` matches only an
 * element whose language is not tagged; `*` matches any tagged element and not an untagged one.
 */

#include <string.h>

#include "css_match_internal.h"

static bool alnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/// The next subtag of [s, s+n) from *pos (a hyphen separated list): its start and length; false at the end.
static bool next_subtag(const char * s, size_t n, size_t * pos, size_t * start, size_t * len) {
  if (*pos > n) {
    return false;
  }
  size_t i = *pos;
  size_t j = i;
  while (j < n && s[j] != '-') {
    j++;
  }
  *start = i;
  *len = j - i;
  *pos = j + 1;
  return true;
}

/// An extended language range is well-formed (RFC 4647, 2.2): subtags of one to eight letters and digits, or `*`.
static bool range_ok(const char * r, size_t n) {
  size_t pos = 0, st, ln;
  bool any = false;
  while (next_subtag(r, n, &pos, &st, &ln)) {
    if (ln == 0) {
      return false;
    }
    if (ln == 1 && r[st] == '*') {
      any = true;
      continue;
    }
    if (ln > 8) {
      return false;
    }
    for (size_t k = 0; k < ln; k++) {
      if (!alnum((unsigned char)r[st + k])) {
        return false;
      }
    }
    any = true;
  }
  return any;
}

bool css_lang_range_matches(const char * range, size_t rlen, const char * tag, size_t tlen) {
  if (!range_ok(range, rlen) || tlen == 0) {
    return false;
  }
  size_t rp = 0, rs, rl, tp = 0, ts, tl;
  if (!next_subtag(range, rlen, &rp, &rs, &rl) || !next_subtag(tag, tlen, &tp, &ts, &tl)) {
    return false;
  }
  if (!(rl == 1 && range[rs] == '*') && !css_sel_ieq(range + rs, rl, tag + ts, tl)) {
    return false;
  }
  while (next_subtag(range, rlen, &rp, &rs, &rl)) {
    if (rl == 1 && range[rs] == '*') {
      continue;
    }
    for (;;) {
      if (!next_subtag(tag, tlen, &tp, &ts, &tl)) {
        return false;
      }
      if (css_sel_ieq(range + rs, rl, tag + ts, tl)) {
        break;
      }
      if (tl == 1) {
        return false; // a singleton subtag ends the language: the range cannot be skipped past it
      }
    }
  }
  return true;
}

bool css_match_lang(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El element) {
  GTEXT_CSS_String tag = {NULL, 0};
  const bool tagged = ctx->iface.lang(ctx->iface.user, element, &tag) && tag.data && tag.length > 0;
  for (size_t i = 0; i < s->string_count; i++) {
    const GTEXT_CSS_String * r = &s->strings[i];
    if (r->length == 0) {
      if (!tagged) {
        return true;
      }
      continue;
    }
    if (tagged && css_lang_range_matches(r->data, r->length, tag.data, tag.length)) {
      return true;
    }
  }
  return false;
}
