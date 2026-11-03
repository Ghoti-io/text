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
 * The canonical dumps of a sheet (`cssom-sheet 1`) and of a rule with everything under it (`cssom-rule 1`), and their SHA-256 (the idiom of the declaration dump). A dump
 * is plain text, one line for the sheet and one for each rule in document order, with every number in decimal written by hand: the same bytes on every host, whatever the
 * addresses, the locale or the byte order. It reads and builds nothing; a declaration block that has not been built is `lazy`.
 */

#include <string.h>

#include <ghoti.io/security/sha256.h>

#include "cssom_rule_internal.h"

static GTEXT_CSS_Status put(const GTEXT_Markup_Sink * sink, const char * s, size_t n) {
  return n == 0 || sink->write(sink->user, s, n) == GTEXT_MARKUP_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_WRITE;
}

static GTEXT_CSS_Status puts_(const GTEXT_Markup_Sink * sink, const char * s) {
  return put(sink, s, strlen(s));
}

static GTEXT_CSS_Status put_u64(const GTEXT_Markup_Sink * sink, uint64_t v) {
  char b[24];
  size_t n = sizeof b;
  do {
    b[--n] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  return put(sink, b + n, sizeof b - n);
}

/// A string between quotes with the bytes that would break a line or a quote written as \xHH.
static GTEXT_CSS_Status put_string(const GTEXT_Markup_Sink * sink, const char * s) {
  static const char hex[] = "0123456789abcdef";
  GTEXT_CSS_Status st = puts_(sink, "\"");
  for (const unsigned char * p = (const unsigned char *)s; st == GTEXT_CSS_OK && *p; p++) {
    if (*p < 0x20 || *p == 0x7f || *p == '"' || *p == '\\') {
      const char two[2] = {hex[*p >> 4], hex[*p & 15]};
      st = put(sink, "\\x", 2);
      if (st == GTEXT_CSS_OK) {
        st = put(sink, two, 2);
      }
    }
    else {
      st = put(sink, (const char *)p, 1);
    }
  }
  return st == GTEXT_CSS_OK ? puts_(sink, "\"") : st;
}

static const char * const source_names[] = {"parsed", "inserted", "modified"};

static const char * const diag_names[] = {"charset", "unknown-at-rule", "invalid-at-rule", "invalid-selector", "invalid-keyframe", "misplaced-import", "misplaced-namespace"};

static GTEXT_CSS_Status dump_rule(const css_rule * r, size_t depth, const GTEXT_Markup_Sink * sink) {
  GTEXT_CSS_Status st = puts_(sink, "rule ");
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, r->obj.id);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " ");
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, css_kind((GTEXT_CSSOM_Rule_Kind)r->kind)->name);
  }
  if (st == GTEXT_CSS_OK && r->kind == GTEXT_CSSOM_RULE_LAYER) {
    st = puts_(sink, r->has_block ? " block" : " statement");
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " version ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, r->version);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " depth ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, depth);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " children ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, r->rules.count);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, r->nested ? " nested " : " plain ");
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, source_names[r->source_kind]);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " source ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, r->source ? r->source->id : 0);
  }
  const GTEXT_CSS_Span sp = css_rule_span(r);
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " line ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sp.line);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " column ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sp.column);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " bytes ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sp.offset);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, "-");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sp.end);
  }
  if (st == GTEXT_CSS_OK && (css_kind((GTEXT_CSSOM_Rule_Kind)r->kind)->flags & CSS_KIND_STYLE_PROPERTIES)) {
    st = puts_(sink, " style ");
    if (st == GTEXT_CSS_OK) {
      if (!r->style) {
        st = puts_(sink, "lazy");
      }
      else {
        st = puts_(sink, "version ");
        if (st == GTEXT_CSS_OK) {
          st = put_u64(sink, r->style->version);
        }
      }
    }
  }
  return st == GTEXT_CSS_OK ? puts_(sink, "\n") : st;
}

/// The rules of a list and everything under them in document order, with the explicit walk of the parent pointers. `stop` (a rule, or NULL) is the root of a dump of one rule.
static GTEXT_CSS_Status dump_below(const struct GTEXT_CSSOM_Rule_List * list, size_t depth, const css_rule * stop, const GTEXT_Markup_Sink * sink) {
  const css_rule * r = css_rule_list_first(list);
  while (r) {
    GTEXT_CSS_Status st = dump_rule(r, depth, sink);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (r->rules.count) {
      r = css_rule_list_first(&r->rules);
      depth++;
      continue;
    }
    for (;;) {
      const css_rule * next = css_rule_list_next(r);
      if (next) {
        r = next;
        break;
      }
      const css_obj * owner = r->list->owner;
      if (owner->kind != GTEXT_CSSOM_OBJECT_RULE || (const css_rule *)(uintptr_t)owner == stop) {
        return GTEXT_CSS_OK;
      }
      r = (const css_rule *)(uintptr_t)owner;
      depth--;
    }
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status rule_dump_walk(const GTEXT_CSSOM_Rule * rule, const GTEXT_Markup_Sink * sink) {
  GTEXT_CSS_Status st = puts_(sink, "cssom-rule 1\n");
  if (st == GTEXT_CSS_OK) {
    st = dump_rule(rule, 0, sink);
  }
  if (st == GTEXT_CSS_OK && rule->rules.count) {
    st = dump_below(&rule->rules, 1, rule, sink);
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_rule_dump(const GTEXT_CSSOM_Rule * rule, const GTEXT_Markup_Sink * sink) {
  if (!rule || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  /* the sink is the caller's code, run in the middle of a walk over the rules: a mutation from it is refused, as from any callback */
  css_core * core = rule->obj.core;
  core->in_callback++;
  const GTEXT_CSS_Status st = rule_dump_walk(rule, sink);
  core->in_callback--;
  return st;
}

static GTEXT_CSS_Status sheet_dump_walk(const GTEXT_CSSOM_Sheet * sheet, const GTEXT_Markup_Sink * sink) {
  GTEXT_CSS_Status st = puts_(sink, "cssom-sheet 1\nsheet ");
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sheet->obj.id);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " version ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sheet->version);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, sheet->disabled ? " disabled 1" : " disabled 0");
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, sheet->origin_clean ? " origin-clean 1" : " origin-clean 0");
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, sheet->constructed ? " constructed 1" : " constructed 0");
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, sheet->alternate ? " alternate 1" : " alternate 0");
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " rules ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sheet->rules.count);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, " total ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, sheet->rule_total);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, "\n");
  }
  const struct {
    const char * name;
    const char * value;
  } strings[] = {{"href", sheet->href}, {"title", sheet->title}, {"base-url", sheet->base_url}};
  for (size_t i = 0; st == GTEXT_CSS_OK && i < 3; i++) {
    if (strings[i].value) {
      st = puts_(sink, strings[i].name);
      if (st == GTEXT_CSS_OK) {
        st = puts_(sink, " ");
      }
      if (st == GTEXT_CSS_OK) {
        st = put_string(sink, strings[i].value);
      }
      if (st == GTEXT_CSS_OK) {
        st = puts_(sink, "\n");
      }
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, "diagnostics ");
    if (st == GTEXT_CSS_OK) {
      st = put_u64(sink, sheet->diag_total);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, "\n");
    }
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < sheet->ndiags; i++) {
    st = puts_(sink, "diag ");
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, diag_names[sheet->diags[i].kind]);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, " ");
    }
    if (st == GTEXT_CSS_OK) {
      st = put_u64(sink, sheet->diags[i].span.line);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, ":");
    }
    if (st == GTEXT_CSS_OK) {
      st = put_u64(sink, sheet->diags[i].span.column);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, "\n");
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = dump_below(&sheet->rules, 0, NULL, sink);
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_sheet_dump(const GTEXT_CSSOM_Sheet * sheet, const GTEXT_Markup_Sink * sink) {
  if (!sheet || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  /* the sink is the caller's code, run in the middle of a walk over the rules: a mutation from it is refused, as from any callback */
  css_core * core = sheet->obj.core;
  core->in_callback++;
  const GTEXT_CSS_Status st = sheet_dump_walk(sheet, sink);
  core->in_callback--;
  return st;
}

typedef struct {
  GSEC_Sha256 ctx;
  bool failed;
} hasher;

static GTEXT_Markup_Status hash_write(void * user, const char * data, size_t n) {
  hasher * h = user;
  if (gsec_sha256_update(&h->ctx, data, n) != GSEC_OK) {
    h->failed = true;
    return GTEXT_MARKUP_E_OOM;
  }
  return GTEXT_MARKUP_OK;
}

static GTEXT_CSS_Status hash_of(GTEXT_CSS_Status (*dump)(const void *, const GTEXT_Markup_Sink *), const void * object, unsigned char hash[GTEXT_CSSOM_HASH_LENGTH]) {
  hasher h;
  h.failed = false;
  if (gsec_sha256_init(&h.ctx) != GSEC_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &h;
  sink.write = hash_write;
  const GTEXT_CSS_Status st = dump(object, &sink);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_WRITE ? GTEXT_CSS_E_INTERNAL : st;
  }
  if (gsec_sha256_final(&h.ctx, hash) != GSEC_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status sheet_dump_thunk(const void * o, const GTEXT_Markup_Sink * sink) {
  return gtext_cssom_sheet_dump(o, sink);
}

static GTEXT_CSS_Status rule_dump_thunk(const void * o, const GTEXT_Markup_Sink * sink) {
  return gtext_cssom_rule_dump(o, sink);
}

GTEXT_CSS_Status gtext_cssom_sheet_hash(const GTEXT_CSSOM_Sheet * sheet, unsigned char hash[GTEXT_CSSOM_HASH_LENGTH]) {
  if (!sheet || !hash) {
    return GTEXT_CSS_E_INVALID;
  }
  return hash_of(sheet_dump_thunk, sheet, hash);
}

GTEXT_CSS_Status gtext_cssom_rule_hash(const GTEXT_CSSOM_Rule * rule, unsigned char hash[GTEXT_CSSOM_HASH_LENGTH]) {
  if (!rule || !hash) {
    return GTEXT_CSS_E_INVALID;
  }
  return hash_of(rule_dump_thunk, rule, hash);
}
