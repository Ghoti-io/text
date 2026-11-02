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
 * Element-owned declarations (css_cssom_decl.h): the document's weak map from an element to its declaration, the validation of a declaration against the
 * element's `style` attribute by content, and the write-back.
 *
 * The attribute is the declaration's only storage. The declaration remembers the exact string it last parsed or wrote, and each call that reads or edits it
 * validates once: by the attribute's version when the binding has one, by nothing when the caller's tree has not moved since, else by comparing the attribute
 * with the remembered string (length, then bytes) and parsing it again when it differs. The write-back updates the remembered string, so a declaration never
 * parses its own write. The costs are counters (css_cssom_core.h).
 */

#include <string.h>

#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_internal.h"

/* ---- the weak map ------------------------------------------------------------------------------------------------------------------------------------ */

static size_t hash_ptr(const void * p) {
  uint64_t x = (uint64_t)(uintptr_t)p;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return (size_t)x;
}

/// The entry of an element; SIZE_MAX if there is none. Probing stops at an empty entry; a tombstone is passed.
static size_t map_find(const css_core * core, const void * element) {
  if (!core->inline_cap) {
    return SIZE_MAX;
  }
  const size_t mask = core->inline_cap - 1;
  size_t pos = hash_ptr(element) & mask;
  for (size_t step = 0; step < core->inline_cap; step++) {
    const css_inline_entry * e = &core->inline_map[pos];
    if (!e->element) {
      return SIZE_MAX;
    }
    if (e->element == element && e->decl) {
      return pos;
    }
    pos = (pos + 1) & mask;
  }
  return SIZE_MAX;
}

/// Room for one more entry: grows or cleans the table. The only allocation of the map.
static GTEXT_CSS_Status map_reserve(css_core * core) {
  if (core->inline_cap && (core->inline_used + 1) * 4 <= core->inline_cap * 3) {
    return GTEXT_CSS_OK;
  }
  size_t cap = core->inline_cap ? core->inline_cap : 16;
  while ((core->inline_live + 1) * 2 > cap) {
    cap *= 2;
  }
  css_inline_entry * table = gtext_allocator_calloc(&core->alloc, cap, sizeof *table);
  if (!table) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < core->inline_cap; i++) {
    const css_inline_entry * e = &core->inline_map[i];
    if (e->element && e->decl) {
      size_t pos = hash_ptr(e->element) & (cap - 1);
      while (table[pos].element) {
        pos = (pos + 1) & (cap - 1);
      }
      table[pos] = *e;
    }
  }
  gtext_allocator_free(&core->alloc, core->inline_map);
  core->inline_map = table;
  core->inline_cap = cap;
  core->inline_used = core->inline_live;
  return GTEXT_CSS_OK;
}

/// Add an entry; map_reserve() made room.
static void map_insert(css_core * core, const void * element, GTEXT_CSSOM_Decl * decl) {
  const size_t mask = core->inline_cap - 1;
  size_t pos = hash_ptr(element) & mask;
  while (core->inline_map[pos].element && core->inline_map[pos].decl) {
    pos = (pos + 1) & mask;
  }
  if (!core->inline_map[pos].element) {
    core->inline_used++;
  }
  core->inline_map[pos].element = element;
  core->inline_map[pos].decl = decl;
  core->inline_live++;
}

void css_inline_forget(css_core * core, GTEXT_CSSOM_Decl * decl) {
  if (decl->element && !decl->detached) {
    const size_t at = map_find(core, decl->element);
    if (at != SIZE_MAX && core->inline_map[at].decl == decl) {
      core->inline_map[at].decl = NULL;   /* a tombstone: the element stays so that the probe goes on past it */
      core->inline_live--;
    }
  }
  if (decl->owner_kind == GTEXT_CSSOM_DECL_ELEMENT && core->counters.inline_styles) {
    core->counters.inline_styles--;
  }
}

void css_inline_map_free(css_core * core) {
  GTEXT_Allocator a = core->alloc;
  gtext_allocator_free(&a, core->inline_map);
  core->inline_map = NULL;
  core->inline_cap = core->inline_used = core->inline_live = 0;
}

void gtext_cssom_document_element_released(GTEXT_CSSOM_Document * document, const void * element) {
  if (!document || !element) {
    return;
  }
  css_core * core = css_document_core(document);
  const size_t at = map_find(core, element);
  if (at == SIZE_MAX) {
    return;
  }
  GTEXT_CSSOM_Decl * d = core->inline_map[at].decl;
  core->inline_map[at].decl = NULL;
  core->inline_live--;
  d->detached = true;   /* it keeps what it held and never calls the binding again */
}

/* ---- validation ---------------------------------------------------------------------------------------------------------------------------------------- */

static void note_validated(GTEXT_CSSOM_Decl * d) {
  css_core * core = d->obj.core;
  d->validated = true;
  d->stale = false;
  d->validated_epoch = core->tree_epoch;
  d->validated_tree = css_core_tree_version(core, NULL);
}

/// A copy of a string that the declaration will remember, charged against the limit: NULL with the status (GTEXT_CSS_E_LIMIT_CSSOM or GTEXT_CSS_E_OOM), and nothing charged.
static char * remember_copy(GTEXT_CSSOM_Decl * d, const char * s, size_t n, bool * charged, GTEXT_CSS_Status * status) {
  css_core * core = d->obj.core;
  *charged = false;
  *status = GTEXT_CSS_OK;
  if (n > d->remembered_length && !css_obj_charge(&d->obj, n - d->remembered_length)) {
    *status = GTEXT_CSS_E_LIMIT_CSSOM;
    return NULL;
  }
  *charged = n > d->remembered_length;
  char * copy = css_core_dup(core, s, n);
  if (!copy) {
    *status = GTEXT_CSS_E_OOM;
    if (*charged) {
      css_obj_credit(&d->obj, n - d->remembered_length);
      *charged = false;
    }
  }
  return copy;
}

static void remember_commit(GTEXT_CSSOM_Decl * d, char * copy, size_t n, bool present) {
  css_core * core = d->obj.core;
  const size_t old = d->remembered_length;
  gtext_allocator_free(&core->alloc, d->remembered);
  d->remembered = copy;
  d->remembered_length = n;
  d->remembered_present = present;
  if (n < old) {
    css_obj_credit(&d->obj, old - n);
  }
  d->has_version = d->binding.attribute_version != NULL;
  d->remembered_version = d->has_version ? d->binding.attribute_version(d->binding.user, d->element) : 0;
  note_validated(d);
}

GTEXT_CSS_Status css_decl_validate(GTEXT_CSSOM_Decl * d) {
  if (d->owner_kind != GTEXT_CSSOM_DECL_ELEMENT || d->detached) {
    return GTEXT_CSS_OK;
  }
  css_core * core = d->obj.core;
  core->counters.inline_reads++;
  bool has_hook = false;
  const uint64_t tv = css_core_tree_version(core, &has_hook);
  if (d->validated && !d->stale) {
    if (d->binding.attribute_version) {
      if (d->has_version && d->binding.attribute_version(d->binding.user, d->element) == d->remembered_version) {
        return GTEXT_CSS_OK;
      }
    }
    else if (core->config.inline_trust_notifications) {
      return GTEXT_CSS_OK;
    }
    else if ((has_hook || core->tree_epoch > 0) && d->validated_epoch == core->tree_epoch && (!has_hook || d->validated_tree == tv)) {
      return GTEXT_CSS_OK;
    }
  }
  core->counters.inline_compares++;
  const char * value = NULL;
  size_t length = 0;
  bool present = false;
  GTEXT_CSS_Status st = d->binding.get_attribute(d->binding.user, d->element, &value, &length, &present);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!present) {
    value = "";
    length = 0;
  }
  if (d->validated && present == d->remembered_present && length == d->remembered_length &&
      (length == 0 || memcmp(value, d->remembered, length) == 0)) {
    d->has_version = d->binding.attribute_version != NULL;
    d->remembered_version = d->has_version ? d->binding.attribute_version(d->binding.user, d->element) : 0;
    note_validated(d);
    return GTEXT_CSS_OK;
  }
  /* it changed behind the object's back: read it again */
  core->counters.inline_reparses++;
  GTEXT_CSS_Declaration_Block * block = NULL;
  st = css_decl_parse_block(core, value, length, core->config.quirks, &block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  bool charged = false;
  char * copy = remember_copy(d, value, length, &charged, &st);
  if (!copy) {
    gtext_css_block_free(block);
    return st;
  }
  gtext_css_block_free(d->block);
  d->block = block;
  d->version++;
  remember_commit(d, copy, length, present);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_decl_write_attribute(GTEXT_CSSOM_Decl * d, const char * text, size_t length, bool * written) {
  css_core * core = d->obj.core;
  bool charged = false;
  GTEXT_CSS_Status st;
  char * copy = remember_copy(d, text, length, &charged, &st);
  if (!copy) {
    return st;
  }
  st = d->binding.set_attribute(d->binding.user, d->element, text, length);
  core->counters.inline_writes++;
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&core->alloc, copy);
    if (charged) {
      css_obj_credit(&d->obj, length - d->remembered_length);
    }
    return st;
  }
  remember_commit(d, copy, length, true);
  *written = true;
  return GTEXT_CSS_OK;
}

/* ---- the declaration of an element ------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_inline_style(GTEXT_CSSOM_Document * document, const void * element, const GTEXT_CSSOM_Attribute_Binding * binding,
    GTEXT_CSSOM_Decl ** out) {
  if (!document || !element || !binding || !out || binding->size < offsetof(GTEXT_CSSOM_Attribute_Binding, remove_attribute) || !binding->get_attribute ||
      !binding->set_attribute) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  const size_t found = map_find(core, element);
  if (found != SIZE_MAX) {
    GTEXT_CSSOM_Decl * d = core->inline_map[found].decl;
    css_obj_retain_external(&d->obj);
    *out = d;
    return GTEXT_CSS_OK;
  }
  if (core->counters.inline_styles >= core->config.max_inline_styles) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  const char * value = NULL;
  size_t length = 0;
  bool present = false;
  GTEXT_CSS_Status st = binding->get_attribute(binding->user, element, &value, &length, &present);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!present) {
    value = "";
    length = 0;
  }
  st = map_reserve(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Declaration_Block * block = NULL;
  st = css_decl_parse_block(core, value, length, core->config.quirks, &block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Decl * d = NULL;
  st = css_decl_create(core, GTEXT_CSSOM_DECL_ELEMENT, 0, 1, &d);
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(block);
    return st;
  }
  /* the declaration exists: from here a failure releases it, and the destructor undoes what was done */
  gtext_css_block_free(d->block);
  d->block = block;
  const size_t n = binding->size < sizeof d->binding ? binding->size : sizeof d->binding;
  memcpy(&d->binding, binding, n);
  d->binding.size = sizeof d->binding;
  d->element = element;
  core->counters.inline_styles++;
  bool charged = false;
  char * copy = remember_copy(d, value, length, &charged, &st);
  if (!copy) {
    d->element = NULL;   /* not in the map yet */
    css_obj_release_external(&d->obj);
    return st;
  }
  remember_commit(d, copy, length, present);
  map_insert(core, element, d);
  *out = d;
  return GTEXT_CSS_OK;
}

void gtext_cssom_inline_style_attribute_changed(GTEXT_CSSOM_Document * document, const void * element) {
  if (!document || !element) {
    return;
  }
  css_core * core = css_document_core(document);
  const size_t at = map_find(core, element);
  if (at != SIZE_MAX) {
    core->inline_map[at].decl->stale = true;
  }
}
