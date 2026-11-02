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
 * The declaration block (css_shorthand.h): an ordered list of declarations, shorthands expanded into longhands, and CSSOM's serialization of it.
 */

#include <stdlib.h>
#include <string.h>

#include "css_shorthand_internal.h"

#define DEFAULT_MAX_DECLARATIONS ((size_t)1 << 20)
#define LIMIT_MAX_DECLARATIONS ((size_t)1 << 28)
#define DEFAULT_MAX_OUTPUT ((size_t)64 << 20)

GTEXT_CSS_Declaration_Options gtext_css_declaration_options_default(void) {
  GTEXT_CSS_Declaration_Options o;
  memset(&o, 0, sizeof o);
  o.max_declarations = DEFAULT_MAX_DECLARATIONS;
  o.max_output_bytes = DEFAULT_MAX_OUTPUT;
  return o;
}

const GTEXT_CSS_Registry * css_block_registry(const GTEXT_CSS_Declaration_Block * block) {
  return block ? block->registry : NULL;
}

GTEXT_CSS_Status gtext_css_block_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Declaration_Options * options,
    GTEXT_CSS_Declaration_Block ** out) {
  if (!registry || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Declaration_Options o = options ? *options : gtext_css_declaration_options_default();
  if (o.max_declarations == 0) {
    o.max_declarations = DEFAULT_MAX_DECLARATIONS;
  }
  if (o.max_declarations > LIMIT_MAX_DECLARATIONS) {
    o.max_declarations = LIMIT_MAX_DECLARATIONS;
  }
  if (o.max_output_bytes == 0) {
    o.max_output_bytes = DEFAULT_MAX_OUTPUT;
  }
  const GTEXT_Allocator * al = o.allocator ? o.allocator : css_registry_allocator(registry);
  GTEXT_Allocator a = *al;
  GTEXT_CSS_Declaration_Block * b = gtext_allocator_calloc(&a, 1, sizeof *b);
  if (!b) {
    return GTEXT_CSS_E_OOM;
  }
  b->registry = registry;
  b->alloc = a;
  b->options = o;
  b->options.allocator = NULL;
  *out = b;
  return GTEXT_CSS_OK;
}

void gtext_css_block_free(GTEXT_CSS_Declaration_Block * block) {
  if (!block) {
    return;
  }
  GTEXT_Allocator a = block->alloc;
  for (size_t i = 0; i < block->count; i++) {
    gtext_css_value_set_free(block->items[i].set);
    gtext_allocator_free(&a, block->items[i].custom_name);
    gtext_allocator_free(&a, block->items[i].raw);
  }
  gtext_allocator_free(&a, block->covered);
  gtext_allocator_free(&a, block->items);
  gtext_allocator_free(&a, block->index);
  gtext_allocator_free(&a, block);
}

/* ---- the index ------------------------------------------------------------------------------------------------------------ */

#define INDEX_TOMB SIZE_MAX

static uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

/// The hash of a key: a custom property's name (case-sensitive bytes), else the property id. Fixed constants, no address: the same on every host.
static uint64_t key_hash(GTEXT_CSS_Property_Id property, const char * name, size_t length) {
  if (!name) {
    return mix64((uint64_t)property);
  }
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < length; i++) {
    h = (h ^ (unsigned char)name[i]) * 0x100000001b3ull;
  }
  return mix64(h ^ (uint64_t)length);
}

static bool key_equal(const blk_item * it, GTEXT_CSS_Property_Id property, const char * name, size_t length) {
  if (name) {
    return it->custom_name && it->custom_length == length && memcmp(it->custom_name, name, length) == 0;
  }
  return !it->custom_name && it->property == property;
}

/// The entry of a key: the position in the table of the entry that names it, or of the place a new one goes (the first tombstone passed, else the empty entry that ended
/// the probe). `found` says which.
static size_t index_probe(GTEXT_CSS_Declaration_Block * b, GTEXT_CSS_Property_Id property, const char * name, size_t length, bool * found) {
  const size_t mask = b->index_cap - 1;
  size_t pos = (size_t)key_hash(property, name, length) & mask;
  size_t tomb = SIZE_MAX;
  *found = false;
  for (size_t step = 0; step < b->index_cap; step++) {
    b->probes++;
    const size_t e = b->index[pos];
    if (e == 0) {
      return tomb != SIZE_MAX ? tomb : pos;
    }
    if (e == INDEX_TOMB) {
      if (tomb == SIZE_MAX) {
        tomb = pos;
      }
    }
    else if (key_equal(&b->items[e - 1], property, name, length)) {
      *found = true;
      return pos;
    }
    pos = (pos + 1) & mask;
  }
  return tomb;   /* the table is full of tombstones and entries but one: the caller keeps it below three quarters, so a free place always exists */
}

/// Make the entry of the item in slot `slot` (a live one) name it, chaining the item to the one the key named before.
static void index_put(GTEXT_CSS_Declaration_Block * b, size_t slot) {
  blk_item * it = &b->items[slot];
  bool found = false;
  const size_t pos = index_probe(b, it->custom_name ? 0 : it->property, it->custom_name, it->custom_length, &found);
  if (found) {
    it->prev = b->index[pos];
  }
  else {
    it->prev = 0;
    if (b->index[pos] == INDEX_TOMB) {
      b->index_tombs--;
    }
    else {
      b->index_used++;
    }
  }
  b->index[pos] = slot + 1;
}

/// Build the table afresh over the live items, with room for `extra` more keys. Only the table is allocated, so this is the one step of the index that can fail.
static GTEXT_CSS_Status index_build(GTEXT_CSS_Declaration_Block * b, size_t extra, bool force) {
  size_t need = b->live + extra;
  size_t cap = 16;
  while (cap / 4 * 3 < need + 1) {
    if (cap > SIZE_MAX / 4) {
      return GTEXT_CSS_E_OOM;
    }
    cap *= 2;
  }
  if (!force && b->index && cap <= b->index_cap && b->index_cap / 4 * 3 >= b->index_used + extra + 1) {
    return GTEXT_CSS_OK;
  }
  if (b->index && cap <= b->index_cap) {
    cap = b->index_cap;   /* rebuilding in place keeps the size: the tombstones go, nothing is allocated */
    memset(b->index, 0, cap * sizeof *b->index);
  }
  else {
    size_t * table = gtext_allocator_calloc(&b->alloc, cap, sizeof *table);
    if (!table) {
      return GTEXT_CSS_E_OOM;
    }
    gtext_allocator_free(&b->alloc, b->index);
    b->index = table;
  }
  b->index_cap = cap;
  b->index_used = 0;
  b->index_tombs = 0;
  for (size_t i = 0; i < b->count; i++) {
    if (!b->items[i].dead) {
      index_put(b, i);
    }
  }
  return GTEXT_CSS_OK;
}

/// The index, built if it is not, with room for `extra` more keys.
static GTEXT_CSS_Status index_ready(GTEXT_CSS_Declaration_Block * b, size_t extra) {
  if (!b->index) {
    return index_build(b, extra, true);
  }
  return index_build(b, extra, false);
}

/// The slot + 1 of the last live item of a key; 0 for none. Without an index it is a scan from the end.
static size_t block_lookup(const GTEXT_CSS_Declaration_Block * cb, GTEXT_CSS_Property_Id property, const char * name, size_t length) {
  GTEXT_CSS_Declaration_Block * b = (GTEXT_CSS_Declaration_Block *)cb;
  if (b->index) {
    bool found = false;
    const size_t pos = index_probe(b, property, name, length, &found);
    return found ? b->index[pos] : 0;
  }
  for (size_t i = b->count; i-- > 0;) {
    b->probes++;
    if (!b->items[i].dead && key_equal(&b->items[i], property, name, length)) {
      return i + 1;
    }
  }
  return 0;
}

/// Squeeze the dead slots out, keeping the order of the rest, and rebuild the index in the table it has (nothing is allocated).
static void block_compact(GTEXT_CSS_Declaration_Block * b) {
  if (b->live == b->count) {
    return;
  }
  size_t to = 0;
  for (size_t i = 0; i < b->count; i++) {
    if (!b->items[i].dead) {
      if (to != i) {
        b->items[to] = b->items[i];
      }
      to++;
    }
  }
  b->count = to;
  if (b->index) {
    index_build(b, 0, true);
  }
}

void css_block_compact(GTEXT_CSS_Declaration_Block * block) {
  if (block) {
    block_compact(block);
  }
}

static GTEXT_CSS_Status block_reserve(GTEXT_CSS_Declaration_Block * b, size_t more) {
  if (more > b->options.max_declarations || b->live > b->options.max_declarations - more) {
    return GTEXT_CSS_E_LIMIT_DECLARATIONS;
  }
  if (b->index) {
    GTEXT_CSS_Status st = index_ready(b, more);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  if (b->count + more <= b->cap) {
    return GTEXT_CSS_OK;
  }
  if (b->count != b->live && b->count + more > b->cap) {
    block_compact(b);   /* room that the dead slots hold, before the array grows */
    if (b->count + more <= b->cap) {
      return GTEXT_CSS_OK;
    }
  }
  size_t cap = b->cap ? b->cap : 8;
  while (cap < b->count + more) {
    cap *= 2;
  }
  blk_item * grown = gtext_allocator_realloc(&b->alloc, b->items, cap * sizeof *grown);
  if (!grown) {
    return GTEXT_CSS_E_OOM;
  }
  b->items = grown;
  b->cap = cap;
  return GTEXT_CSS_OK;
}

/// A slot that was just filled by an append (count already advanced): count it, and name it in the index when there is one. block_reserve() made room for both.
static void block_appended(GTEXT_CSS_Declaration_Block * b) {
  b->live++;
  if (b->index) {
    index_put(b, b->count - 1);
  }
}

static void fill_result(GTEXT_CSS_Shorthand_Result * r, GTEXT_CSS_Status st) {
  if (r) {
    r->status = st;
    r->message = gtext_css_status_string(st);
  }
}

static bool is_all(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info info;
  return gtext_css_registry_property_info(registry, id, &info) && info.name_length == 3 && memcmp(info.name, "all", 3) == 0;
}

GTEXT_CSS_Status gtext_css_block_add(GTEXT_CSS_Declaration_Block * block, const GTEXT_CSS_Property_Value * value, bool important,
    GTEXT_CSS_Shorthand_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    fill_result(result, GTEXT_CSS_OK);
  }
  if (!block || !value) {
    return GTEXT_CSS_E_INVALID;
  }
  if (value->outcome != GTEXT_CSS_PROP_RESULT_VALID && value->outcome != GTEXT_CSS_PROP_RESULT_WIDE &&
      value->outcome != GTEXT_CSS_PROP_RESULT_PENDING) {
    if (result) {
      result->detail = "only a valid, wide or pending value is added to a block";
    }
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Registry * reg = block->registry;
  const GTEXT_CSS_Property_Id prop = value->property;
  GTEXT_CSS_Property_Info info;
  if (!prop || !gtext_css_registry_property_info(reg, prop, &info)) {
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  if (info.flags & GTEXT_CSS_PROP_CUSTOM) {
    if (result) {
      result->detail = "a custom property is added with gtext_css_block_add_custom()";
    }
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  const bool shorthand = (info.flags & GTEXT_CSS_PROP_SHORTHAND) && !is_all(reg, prop);
  GTEXT_CSS_Status st;
  if (!shorthand) {
    st = block_reserve(block, 1);
    if (st != GTEXT_CSS_OK) {
      fill_result(result, st);
      return st;
    }
    blk_item * it = &block->items[block->count++];
    memset(it, 0, sizeof *it);
    it->property = prop;
    it->important = important;
    it->outcome = value->outcome;
    it->wide = value->wide;
    it->set = gtext_css_value_set_retain(value->set);
    it->declaration = block->next_declaration++;
    block_appended(block);
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Longhand_List * list = NULL;
  st = gtext_css_shorthand_expand(reg, value, &block->alloc, &list, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const size_t n = gtext_css_longhand_list_count(list);
  st = block_reserve(block, n);
  if (st != GTEXT_CSS_OK) {
    gtext_css_longhand_list_free(list);
    fill_result(result, st);
    return st;
  }
  const size_t decl = block->next_declaration++;
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Longhand lh;
    gtext_css_longhand_list_at(list, i, &lh);
    blk_item * it = &block->items[block->count++];
    memset(it, 0, sizeof *it);
    it->property = lh.property;
    it->important = important;
    it->outcome = lh.outcome;
    it->wide = lh.wide;
    it->set = gtext_css_value_set_retain(lh.set);
    it->shorthand = prop;
    it->declaration = decl;
    block_appended(block);
  }
  gtext_css_longhand_list_free(list);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_block_add_custom(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length,
    const GTEXT_CSS_Property_Value * value, bool important) {
  if (!block || !value || !name || name_length < 3 || name[0] != '-' || name[1] != '-') {
    return GTEXT_CSS_E_INVALID;
  }
  if (value->outcome != GTEXT_CSS_PROP_RESULT_VALID && value->outcome != GTEXT_CSS_PROP_RESULT_WIDE && value->outcome != GTEXT_CSS_PROP_RESULT_PENDING) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = block_reserve(block, 1);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  char * copy = gtext_allocator_malloc(&block->alloc, name_length + 1);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(copy, name, name_length);
  copy[name_length] = '\0';
  const GTEXT_CSS_Property_Id custom = gtext_css_registry_find_property(block->registry, "--x", 3);
  blk_item * it = &block->items[block->count++];
  memset(it, 0, sizeof *it);
  it->property = custom;
  it->custom_name = copy;
  it->custom_length = name_length;
  it->important = important;
  it->outcome = value->outcome;
  it->wide = value->wide;
  it->set = gtext_css_value_set_retain(value->set);
  it->declaration = block->next_declaration++;
  block_appended(block);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_block_push(GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, const char * custom_name, size_t custom_length,
    bool important, GTEXT_CSS_Property_Outcome outcome, GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, GTEXT_CSS_Property_Id shorthand,
    size_t declaration) {
  GTEXT_CSS_Status st = block_reserve(block, 1);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  char * copy = NULL;
  if (custom_name) {
    copy = gtext_allocator_malloc(&block->alloc, custom_length + 1);
    if (!copy) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(copy, custom_name, custom_length);
    copy[custom_length] = '\0';
  }
  blk_item * it = &block->items[block->count++];
  memset(it, 0, sizeof *it);
  it->property = property;
  it->custom_name = copy;
  it->custom_length = custom_name ? custom_length : 0;
  it->important = important;
  it->outcome = outcome;
  it->wide = wide;
  it->set = gtext_css_value_set_retain(set);
  it->shorthand = shorthand;
  it->declaration = declaration;
  if (declaration >= block->next_declaration) {
    block->next_declaration = declaration + 1;
  }
  block_appended(block);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_block_add_text(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length, const char * text,
    size_t text_length, bool important, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Outcome * outcome,
    GTEXT_CSS_Shorthand_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    fill_result(result, GTEXT_CSS_OK);
  }
  if (!block || !name || (!text && text_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  if (options) {
    po = *options;
  }
  if (!po.allocator) {
    po.allocator = &block->alloc;
  }
  GTEXT_CSS_Property_Value pv;
  GTEXT_CSS_Status st = gtext_css_parse_property_text(block->registry, name, name_length, text ? text : "", text_length, &po, &pv);
  if (outcome) {
    *outcome = pv.outcome;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_property_value_free(&pv);
    fill_result(result, st);
    return st;
  }
  if (pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN || pv.outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
    gtext_css_property_value_free(&pv);
    return GTEXT_CSS_OK;
  }
  if (name_length >= 2 && name[0] == '-' && name[1] == '-') {
    if (!css_custom_name_ok(name, name_length)) {
      /* as a declaration that is not valid is: not added, and not an error */
      if (outcome) {
        *outcome = GTEXT_CSS_PROP_RESULT_INVALID;
      }
      gtext_css_property_value_free(&pv);
      return GTEXT_CSS_OK;
    }
    st = gtext_css_block_add_custom(block, name, name_length, &pv, important);
  }
  else {
    st = gtext_css_block_add(block, &pv, important, result);
  }
  gtext_css_property_value_free(&pv);
  return st;
}

size_t gtext_css_block_count(const GTEXT_CSS_Declaration_Block * block) {
  return block ? block->live : 0;
}

static void item_out(const blk_item * it, GTEXT_CSS_Declaration_Item * out) {
  out->property = it->property;
  out->custom_name.data = it->custom_name;
  out->custom_name.length = it->custom_length;
  out->important = it->important;
  out->outcome = it->outcome;
  out->wide = it->wide;
  out->set = it->set;
  out->shorthand = it->shorthand;
  out->declaration = it->declaration;
  out->opaque = it->opaque;
  out->raw.data = it->raw;
  out->raw.length = it->raw_length;
}

bool gtext_css_block_item(const GTEXT_CSS_Declaration_Block * block, size_t index, GTEXT_CSS_Declaration_Item * out) {
  if (!block || !out || index >= block->live) {
    return false;
  }
  if (block->live != block->count) {
    /* dead slots are in the way (a block that had declarations removed and has not been compacted): the index-th live one */
    size_t seen = 0;
    for (size_t i = 0; i < block->count; i++) {
      if (!block->items[i].dead && seen++ == index) {
        item_out(&block->items[i], out);
        return true;
      }
    }
    return false;
  }
  item_out(&block->items[index], out);
  return true;
}

/* ---- editing: set, remove, find, clone ---------------------------------------------------------------------------------------- */

static void change_reset(GTEXT_CSS_Block_Change * change) {
  if (change) {
    gtext_css_block_change_free(change);
    memset(change, 0, sizeof *change);
  }
}

void gtext_css_block_change_free(GTEXT_CSS_Block_Change * change) {
  if (change && change->ids) {
    GTEXT_Allocator a = change->alloc;
    gtext_allocator_free(&a, change->ids);
    change->ids = NULL;
    change->count = 0;
  }
}

uint64_t gtext_css_block_probes(const GTEXT_CSS_Declaration_Block * block) {
  return block ? block->probes : 0;
}

/// Room for `n` changed ids in the change (allocated from the block's allocator before anything is altered).
static GTEXT_CSS_Status change_reserve(const GTEXT_CSS_Declaration_Block * b, GTEXT_CSS_Block_Change * change, size_t n) {
  if (!change || n == 0) {
    return GTEXT_CSS_OK;
  }
  change->alloc = b->alloc;
  GTEXT_Allocator a = b->alloc;
  change->ids = gtext_allocator_malloc(&a, n * sizeof *change->ids);
  return change->ids ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

static void change_note(GTEXT_CSS_Block_Change * change, GTEXT_CSS_Property_Id id) {
  if (change) {
    change->changed = true;
    if (change->ids && id) {
      change->ids[change->count++] = id;
    }
  }
}

/// Whether the item says exactly what the new declaration would: the same kind of value, the same text, the same importance.
static GTEXT_CSS_Status item_same(const GTEXT_CSS_Declaration_Block * b, const blk_item * it, GTEXT_CSS_Property_Outcome outcome,
    GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, bool important, bool * same) {
  *same = false;
  if (it->opaque || it->important != important || it->outcome != outcome) {
    return GTEXT_CSS_OK;
  }
  if (outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    *same = it->wide == wide;
    return GTEXT_CSS_OK;
  }
  if (it->set == set) {
    *same = true;
    return GTEXT_CSS_OK;
  }
  if (!it->set || !set) {
    return GTEXT_CSS_OK;
  }
  if (it->custom_name || outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
    const GTEXT_CSS_Value * x = gtext_css_value_set_root(it->set);
    const GTEXT_CSS_Value * y = gtext_css_value_set_root(set);
    const size_t xl = x && x->text.data ? x->text.length : 0;
    const size_t yl = y && y->text.data ? y->text.length : 0;
    *same = xl == yl && (xl == 0 || memcmp(x->text.data, y->text.data, xl) == 0);
    return GTEXT_CSS_OK;
  }
  char *a = NULL, *c = NULL;
  size_t al = 0, cl = 0;
  const GTEXT_Allocator * da = &b->alloc;
  GTEXT_CSS_Status st = css_item_text(b->registry, da, it->property, it->outcome, it->wide, it->set, &a, &al);
  if (st == GTEXT_CSS_OK) {
    st = css_item_text(b->registry, da, it->property, outcome, wide, set, &c, &cl);
  }
  if (st == GTEXT_CSS_OK) {
    *same = al == cl && memcmp(a, c, al) == 0;
  }
  GTEXT_Allocator al_ = b->alloc;
  gtext_allocator_free(&al_, a);
  gtext_allocator_free(&al_, c);
  return st;
}

/// Overwrite an item in place: its value, importance and provenance; its place in the order, its key and its chain stay.
static void item_replace(blk_item * it, GTEXT_CSS_Property_Outcome outcome, GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, bool important,
    GTEXT_CSS_Property_Id shorthand, size_t declaration) {
  GTEXT_CSS_Value_Set * old = it->set;
  it->set = gtext_css_value_set_retain(set);
  gtext_css_value_set_free(old);
  it->outcome = outcome;
  it->wide = wide;
  it->important = important;
  it->shorthand = shorthand;
  it->declaration = declaration;
}

static bool value_usable(const GTEXT_CSS_Property_Value * value) {
  return value && (value->outcome == GTEXT_CSS_PROP_RESULT_VALID || value->outcome == GTEXT_CSS_PROP_RESULT_WIDE ||
      value->outcome == GTEXT_CSS_PROP_RESULT_PENDING);
}

static bool kill_key(GTEXT_CSS_Declaration_Block * b, GTEXT_CSS_Property_Id property, const char * name, size_t length);
static bool opaque_covers_after(const GTEXT_CSS_Declaration_Block * b, size_t from, GTEXT_CSS_Property_Id longhand);

GTEXT_CSS_Status gtext_css_block_set(GTEXT_CSS_Declaration_Block * block, const GTEXT_CSS_Property_Value * value, bool important,
    GTEXT_CSS_Block_Change * change, GTEXT_CSS_Shorthand_Result * result) {
  return css_block_set_ex(block, value, important, false, change, result);
}

GTEXT_CSS_Status css_block_set_ex(GTEXT_CSS_Declaration_Block * block, const GTEXT_CSS_Property_Value * value, bool important, bool keep_important,
    GTEXT_CSS_Block_Change * change, GTEXT_CSS_Shorthand_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    fill_result(result, GTEXT_CSS_OK);
  }
  change_reset(change);
  if (!block || !value_usable(value)) {
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Registry * reg = block->registry;
  const GTEXT_CSS_Property_Id prop = value->property;
  GTEXT_CSS_Property_Info info;
  if (!prop || !gtext_css_registry_property_info(reg, prop, &info) || (info.flags & GTEXT_CSS_PROP_CUSTOM)) {
    if (result && prop && (info.flags & GTEXT_CSS_PROP_CUSTOM)) {
      result->detail = "a custom property is set with gtext_css_block_set_custom()";
    }
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  const bool shorthand = (info.flags & GTEXT_CSS_PROP_SHORTHAND) && !is_all(reg, prop);
  GTEXT_CSS_Longhand_List * list = NULL;
  size_t n = 1;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (shorthand) {
    st = gtext_css_shorthand_expand(reg, value, &block->alloc, &list, result);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    n = gtext_css_longhand_list_count(list);
  }
  /* everything that can fail comes first: the index, the places of the existing declarations, whether each differs, room for the new ones, the change list */
  size_t * slots = NULL;
  unsigned char * same = NULL;
  unsigned char * moved = NULL;   /* a longhand declared before an opaque shorthand that sets it: the new declaration goes after the shorthand */
  size_t fresh = 0;
  st = index_ready(block, n);
  if (st == GTEXT_CSS_OK && n) {
    slots = gtext_allocator_calloc(&block->alloc, n, sizeof *slots + 2);
    if (!slots) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      same = (unsigned char *)(slots + n);
      moved = same + n;
    }
  }
  /* a declaration of the whole shorthand supersedes the opaque one that kept its place */
  const size_t superseded = shorthand ? block_lookup(block, prop, NULL, 0) : 0;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < n; i++) {
    GTEXT_CSS_Longhand lh;
    if (shorthand) {
      gtext_css_longhand_list_at(list, i, &lh);
    }
    else {
      memset(&lh, 0, sizeof lh);
      lh.property = prop;
      lh.outcome = value->outcome;
      lh.wide = value->wide;
      lh.set = value->set;
    }
    slots[i] = block_lookup(block, lh.property, NULL, 0);
    if (!slots[i]) {
      fresh++;
      continue;
    }
    if (block->items[slots[i] - 1].opaque || opaque_covers_after(block, slots[i] - 1, lh.property)) {
      moved[i] = 1;
      fresh++;
      continue;
    }
    if (block->parse_mode && !(keep_important && !important && block->items[slots[i] - 1].important)) {
      moved[i] = 1;   /* a parsed list: the later declaration goes after everything before it, and the earlier one is gone */
      fresh++;
      continue;
    }
    bool is_same = false;
    if (keep_important && !important && block->items[slots[i] - 1].important) {
      is_same = true;   /* a list of declarations: a later declaration does not override an earlier important one of the same property */
    }
    else {
      st = item_same(block, &block->items[slots[i] - 1], lh.outcome, lh.wide, lh.set, important, &is_same);
    }
    same[i] = is_same;
  }
  if (st == GTEXT_CSS_OK) {
    st = block_reserve(block, fresh);   /* may squeeze out dead slots: the places are asked again, the comparisons stand */
  }
  if (st == GTEXT_CSS_OK) {
    st = change_reserve(block, change, n);
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < n; i++) {
    GTEXT_CSS_Longhand lh;
    if (shorthand) {
      gtext_css_longhand_list_at(list, i, &lh);
    }
    else {
      lh.property = prop;
    }
    slots[i] = block_lookup(block, lh.property, NULL, 0);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&block->alloc, slots);
    gtext_css_longhand_list_free(list);
    change_reset(change);
    fill_result(result, st);
    return st;
  }
  /* nothing below can fail */
  size_t decl = 0;
  bool numbered = false;
  if (superseded) {
    kill_key(block, prop, NULL, 0);
    change_note(change, 0);
  }
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Longhand lh;
    if (shorthand) {
      gtext_css_longhand_list_at(list, i, &lh);
    }
    else {
      memset(&lh, 0, sizeof lh);
      lh.property = prop;
      lh.outcome = value->outcome;
      lh.wide = value->wide;
      lh.set = value->set;
    }
    if (slots[i] && same[i]) {
      continue;
    }
    if (!numbered) {
      decl = block->next_declaration++;
      numbered = true;
    }
    if (moved[i]) {
      kill_key(block, lh.property, NULL, 0);
      slots[i] = 0;
    }
    if (slots[i]) {
      item_replace(&block->items[slots[i] - 1], lh.outcome, lh.wide, lh.set, important, shorthand ? prop : 0, decl);
    }
    else {
      blk_item * it = &block->items[block->count++];
      memset(it, 0, sizeof *it);
      it->property = lh.property;
      it->important = important;
      it->outcome = lh.outcome;
      it->wide = lh.wide;
      it->set = gtext_css_value_set_retain(lh.set);
      it->shorthand = shorthand ? prop : 0;
      it->declaration = decl;
      block_appended(block);
    }
    change_note(change, lh.property);
  }
  gtext_allocator_free(&block->alloc, slots);
  gtext_css_longhand_list_free(list);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_block_set_custom(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length,
    const GTEXT_CSS_Property_Value * value, bool important, GTEXT_CSS_Block_Change * change) {
  return css_block_set_custom_ex(block, name, name_length, value, important, false, change);
}

GTEXT_CSS_Status css_block_set_custom_ex(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length, const GTEXT_CSS_Property_Value * value,
    bool important, bool keep_important, GTEXT_CSS_Block_Change * change) {
  change_reset(change);
  if (!block || !value_usable(value) || !name || name_length < 3 || name[0] != '-' || name[1] != '-') {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = index_ready(block, 1);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t at = block_lookup(block, 0, name, name_length);
  if (at && block->parse_mode && !(keep_important && !important && block->items[at - 1].important)) {
    kill_key(block, 0, name, name_length);   /* a parsed list: the later declaration is appended, the earlier one goes */
    at = 0;
  }
  if (at) {
    bool is_same = false;
    if (keep_important && !important && block->items[at - 1].important) {
      return GTEXT_CSS_OK;
    }
    st = item_same(block, &block->items[at - 1], value->outcome, value->wide, value->set, important, &is_same);
    if (st != GTEXT_CSS_OK || is_same) {
      return st;
    }
    item_replace(&block->items[at - 1], value->outcome, value->wide, value->set, important, 0, block->next_declaration++);
    change_note(change, 0);
    return GTEXT_CSS_OK;
  }
  st = block_reserve(block, 1);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  char * copy = gtext_allocator_malloc(&block->alloc, name_length + 1);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(copy, name, name_length);
  copy[name_length] = '\0';
  blk_item * it = &block->items[block->count++];
  memset(it, 0, sizeof *it);
  it->property = gtext_css_registry_find_property(block->registry, "--x", 3);
  it->custom_name = copy;
  it->custom_length = name_length;
  it->important = important;
  it->outcome = value->outcome;
  it->wide = value->wide;
  it->set = gtext_css_value_set_retain(value->set);
  it->declaration = block->next_declaration++;
  block_appended(block);
  change_note(change, 0);
  return GTEXT_CSS_OK;
}

/// Whether a property is a shorthand that is written as its longhands (`all` is one declaration).
static bool is_shorthand_id(const GTEXT_CSS_Registry * reg, GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info info;
  return gtext_css_registry_property_info(reg, id, &info) && (info.flags & GTEXT_CSS_PROP_SHORTHAND) && !is_all(reg, id);
}

/// Count (`delta` 1) or uncount (-1) an opaque shorthand against each longhand it sets. The table exists once any opaque shorthand was added.
static void cover_adjust(GTEXT_CSS_Declaration_Block * b, GTEXT_CSS_Property_Id shorthand, int delta) {
  if (!b->covered) {
    return;
  }
  GTEXT_CSS_Property_Id leaves[256];
  size_t n = 0;
  if (!css_shorthand_leaves(b->registry, shorthand, leaves, 256, &n)) {
    return;
  }
  const size_t np = gtext_css_registry_property_count(b->registry);
  for (size_t i = 0; i < n; i++) {
    if (leaves[i] && leaves[i] <= np) {
      b->covered[leaves[i]] = (uint32_t)((int64_t)b->covered[leaves[i]] + delta);
    }
  }
}

/// Whether a live opaque shorthand after the slot `from` sets the longhand: a declaration of the longhand before that shorthand is overridden by it.
static bool opaque_covers_after(const GTEXT_CSS_Declaration_Block * b, size_t from, GTEXT_CSS_Property_Id longhand) {
  const size_t np = gtext_css_registry_property_count(b->registry);
  if (!b->covered || longhand > np || b->covered[longhand] == 0) {
    return false;
  }
  for (size_t j = from + 1; j < b->count; j++) {
    const blk_item * it = &b->items[j];
    if (it->dead || !it->opaque || !is_shorthand_id(b->registry, it->property)) {
      continue;
    }
    GTEXT_CSS_Property_Id leaves[256];
    size_t n = 0;
    if (css_shorthand_leaves(b->registry, it->property, leaves, 256, &n)) {
      for (size_t i = 0; i < n; i++) {
        if (leaves[i] == longhand) {
          return true;
        }
      }
    }
  }
  return false;
}

/// Kill every live item of a key (the whole chain), release what they hold, and unname the key. True when there was one.
static bool kill_key(GTEXT_CSS_Declaration_Block * b, GTEXT_CSS_Property_Id property, const char * name, size_t length) {
  bool found = false;
  const size_t pos = index_probe(b, property, name, length, &found);
  if (!found) {
    return false;
  }
  size_t s = b->index[pos];
  b->index[pos] = INDEX_TOMB;
  b->index_tombs++;
  while (s) {
    blk_item * it = &b->items[s - 1];
    const size_t next = it->prev;
    if (it->opaque && b->opaque_shorthands && is_shorthand_id(b->registry, it->property)) {
      cover_adjust(b, it->property, -1);
      b->opaque_shorthands--;
    }
    gtext_css_value_set_free(it->set);
    gtext_allocator_free(&b->alloc, it->custom_name);
    gtext_allocator_free(&b->alloc, it->raw);
    it->raw = NULL;
    it->raw_length = 0;
    it->opaque = false;
    it->set = NULL;
    it->custom_name = NULL;
    it->custom_length = 0;
    it->prev = 0;
    it->dead = true;
    b->live--;
    s = next;
  }
  return true;
}

GTEXT_CSS_Status gtext_css_block_remove(GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, const char * name, size_t name_length,
    GTEXT_CSS_Block_Change * change) {
  change_reset(change);
  if (!block || (name ? name_length < 3 || name[0] != '-' || name[1] != '-' : !property)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Property_Id leaves[257];
  size_t nleaf = 1;
  if (name) {
    leaves[0] = 0;
  }
  else {
    const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(block->registry, property);
    GTEXT_CSS_Property_Info info;
    if (!id || !gtext_css_registry_property_info(block->registry, id, &info) || (info.flags & GTEXT_CSS_PROP_CUSTOM)) {
      return GTEXT_CSS_E_INVALID;
    }
    if ((info.flags & GTEXT_CSS_PROP_SHORTHAND) && !is_all(block->registry, id)) {
      if (!css_shorthand_leaves(block->registry, id, leaves, 256, &nleaf)) {
        if (!block_lookup(block, id, NULL, 0)) {
          return GTEXT_CSS_E_UNSUPPORTED;
        }
        nleaf = 0;   /* a shorthand with no rule for its longhands, kept as written: only the declaration itself can go */
      }
      leaves[nleaf++] = id;   /* an opaque declaration of the shorthand itself */
    }
    else {
      leaves[0] = id;
    }
  }
  GTEXT_CSS_Status st = index_ready(block, 0);
  if (st == GTEXT_CSS_OK) {
    st = change_reserve(block, change, nleaf);
  }
  if (st != GTEXT_CSS_OK) {
    change_reset(change);
    return st;
  }
  for (size_t i = 0; i < nleaf; i++) {
    if (kill_key(block, name ? 0 : leaves[i], name, name_length)) {
      change_note(change, name ? 0 : leaves[i]);
    }
  }
  if (block->count - block->live > block->count / 2 && block->count >= 16) {
    block_compact(block);
  }
  return GTEXT_CSS_OK;
}

bool gtext_css_block_find(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, const char * name, size_t name_length,
    GTEXT_CSS_Declaration_Item * out) {
  if (!block || !out || (name ? name_length == 0 : !property)) {
    return false;
  }
  const GTEXT_CSS_Property_Id id = name ? 0 : gtext_css_registry_resolve_alias(block->registry, property);
  const size_t at = block_lookup(block, id, name, name_length);
  if (!at) {
    return false;
  }
  item_out(&block->items[at - 1], out);
  return true;
}

/// Append an opaque item (the room was made by the caller's reserve; the copy and the coverage table are the only allocations).
static GTEXT_CSS_Status push_opaque(GTEXT_CSS_Declaration_Block * b, GTEXT_CSS_Property_Id property, const char * raw, size_t raw_length, bool important, size_t declaration) {
  const bool sh = is_shorthand_id(b->registry, property);
  GTEXT_CSS_Status st = block_reserve(b, 1);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (sh && !b->covered) {
    b->covered = gtext_allocator_calloc(&b->alloc, gtext_css_registry_property_count(b->registry) + 1, sizeof *b->covered);
    if (!b->covered) {
      return GTEXT_CSS_E_OOM;
    }
  }
  char * copy = gtext_allocator_malloc(&b->alloc, raw_length + 1);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(copy, raw, raw_length);
  copy[raw_length] = '\0';
  blk_item * it = &b->items[b->count++];
  memset(it, 0, sizeof *it);
  it->property = property;
  it->important = important;
  it->opaque = true;
  it->raw = copy;
  it->raw_length = raw_length;
  it->declaration = declaration;
  if (declaration >= b->next_declaration) {
    b->next_declaration = declaration + 1;
  }
  block_appended(b);
  if (sh) {
    b->opaque_shorthands++;
    cover_adjust(b, property, 1);
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_block_set_opaque(GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, const char * raw, size_t raw_length,
    bool important, GTEXT_CSS_Block_Change * change) {
  return css_block_set_opaque_ex(block, property, raw, raw_length, important, false, change);
}

GTEXT_CSS_Status css_block_set_opaque_ex(GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, const char * raw, size_t raw_length, bool important,
    bool keep_important, GTEXT_CSS_Block_Change * change) {
  change_reset(change);
  if (!block || !raw || raw_length == 0 || !property) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(block->registry, property);
  GTEXT_CSS_Property_Info info;
  if (!id || !gtext_css_registry_property_info(block->registry, id, &info) || (info.flags & GTEXT_CSS_PROP_CUSTOM) || is_all(block->registry, id)) {
    return GTEXT_CSS_E_INVALID;
  }
  const bool sh = is_shorthand_id(block->registry, id);
  GTEXT_CSS_Status st = index_ready(block, 1);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t at = block_lookup(block, id, NULL, 0);
  if (at && block->parse_mode && !(keep_important && !important && block->items[at - 1].important)) {
    kill_key(block, id, NULL, 0);   /* a parsed list: the later declaration is appended, the earlier one goes */
    at = 0;
  }
  if (at) {
    blk_item * it = &block->items[at - 1];
    if (keep_important && !important && it->important) {
      return GTEXT_CSS_OK;
    }
    if (it->opaque && it->important == important && it->raw_length == raw_length && memcmp(it->raw, raw, raw_length) == 0) {
      return GTEXT_CSS_OK;
    }
    if (sh && !block->covered) {
      block->covered = gtext_allocator_calloc(&block->alloc, gtext_css_registry_property_count(block->registry) + 1, sizeof *block->covered);
      if (!block->covered) {
        return GTEXT_CSS_E_OOM;
      }
    }
    char * copy = gtext_allocator_malloc(&block->alloc, raw_length + 1);
    if (!copy) {
      return GTEXT_CSS_E_OOM;
    }
    st = change_reserve(block, change, 1);
    if (st != GTEXT_CSS_OK) {
      gtext_allocator_free(&block->alloc, copy);
      return st;
    }
    memcpy(copy, raw, raw_length);
    copy[raw_length] = '\0';
    gtext_css_value_set_free(it->set);
    it->set = NULL;
    gtext_allocator_free(&block->alloc, it->raw);
    if (!it->opaque && sh) {
      block->opaque_shorthands++;
      cover_adjust(block, id, 1);
    }
    it->opaque = true;
    it->raw = copy;
    it->raw_length = raw_length;
    it->important = important;
    it->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    it->shorthand = 0;
    it->declaration = block->next_declaration++;
    change_note(change, sh ? 0 : id);
    return GTEXT_CSS_OK;
  }
  st = change_reserve(block, change, 1);
  if (st == GTEXT_CSS_OK) {
    st = push_opaque(block, id, raw, raw_length, important, block->next_declaration);
  }
  if (st != GTEXT_CSS_OK) {
    change_reset(change);
    return st;
  }
  change_note(change, sh ? 0 : id);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_block_clone(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Declaration_Block ** out) {
  if (!block || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Declaration_Options o = block->options;
  o.allocator = &block->alloc;
  GTEXT_CSS_Declaration_Block * c = NULL;
  GTEXT_CSS_Status st = gtext_css_block_new(block->registry, &o, &c);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = block_reserve(c, block->live);
  for (size_t i = 0; st == GTEXT_CSS_OK && i < block->count; i++) {
    const blk_item * it = &block->items[i];
    if (it->dead) {
      continue;
    }
    if (it->opaque) {
      st = push_opaque(c, it->property, it->raw, it->raw_length, it->important, it->declaration);
      continue;
    }
    st = css_block_push(c, it->property, it->custom_name, it->custom_length, it->important, it->outcome, it->wide, it->set, it->shorthand, it->declaration);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(c);
    return st;
  }
  c->next_declaration = block->next_declaration;
  *out = c;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_block_prepare(GTEXT_CSS_Declaration_Block * block) {
  return index_ready(block, 0);
}

/// Whether the live item at `a` of `x` and the one at `b` of `y` are the same declaration: the same value text and importance.
static GTEXT_CSS_Status items_equal(const GTEXT_CSS_Declaration_Block * x, const blk_item * a, const GTEXT_CSS_Declaration_Block * y, const blk_item * b, bool * same) {
  if (a->opaque || b->opaque) {
    *same = a->opaque && b->opaque && a->important == b->important && a->raw_length == b->raw_length && (a->raw_length == 0 || memcmp(a->raw, b->raw, a->raw_length) == 0);
    return GTEXT_CSS_OK;
  }
  if (a->outcome != b->outcome || a->important != b->important || (a->outcome == GTEXT_CSS_PROP_RESULT_WIDE && a->wide != b->wide)) {
    *same = false;
    return GTEXT_CSS_OK;
  }
  if (a->outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    *same = true;
    return GTEXT_CSS_OK;
  }
  /* item_same() reads the first block's registry and allocator, which are the second's too (a block is cloned or made from one document) */
  (void)y;
  return item_same(x, a, b->outcome, b->wide, b->set, b->important, same);
}

static int cust_name_cmp(const void * a, const void * b) {
  const GTEXT_CSS_String * x = a;
  const GTEXT_CSS_String * y = b;
  const size_t m = x->length < y->length ? x->length : y->length;
  const int c = m ? memcmp(x->data, y->data, m) : 0;
  if (c) {
    return c;
  }
  return x->length < y->length ? -1 : x->length > y->length ? 1 : 0;
}

static int id_cmp(const void * a, const void * b) {
  const GTEXT_CSS_Property_Id x = *(const GTEXT_CSS_Property_Id *)a;
  const GTEXT_CSS_Property_Id y = *(const GTEXT_CSS_Property_Id *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

void css_block_diff_free(css_block_delta * diff) {
  if (diff) {
    GTEXT_Allocator a = diff->alloc;
    gtext_allocator_free(&a, diff->ids);
    gtext_allocator_free(&a, diff->customs);
    memset(diff, 0, sizeof *diff);
  }
}

GTEXT_CSS_Status css_block_diff(GTEXT_CSS_Declaration_Block * from, GTEXT_CSS_Declaration_Block * to, css_block_delta * diff) {
  memset(diff, 0, sizeof *diff);
  diff->alloc = from->alloc;
  GTEXT_CSS_Status st = index_ready(from, 0);
  if (st == GTEXT_CSS_OK) {
    st = index_ready(to, 0);
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const size_t cap = from->live + to->live;
  GTEXT_Allocator a = from->alloc;
  if (cap) {
    diff->ids = gtext_allocator_malloc(&a, cap * sizeof *diff->ids);
    diff->customs = gtext_allocator_malloc(&a, cap * sizeof *diff->customs);
    if (!diff->ids || !diff->customs) {
      css_block_diff_free(diff);
      return GTEXT_CSS_E_OOM;
    }
  }
  for (int pass = 0; pass < 2 && st == GTEXT_CSS_OK; pass++) {
    GTEXT_CSS_Declaration_Block * x = pass == 0 ? from : to;
    GTEXT_CSS_Declaration_Block * y = pass == 0 ? to : from;
    for (size_t i = 0; i < x->count && st == GTEXT_CSS_OK; i++) {
      const blk_item * it = &x->items[i];
      if (it->dead) {
        continue;
      }
      const GTEXT_CSS_Property_Id key = it->custom_name ? 0 : it->property;
      if (block_lookup(x, key, it->custom_name, it->custom_length) != i + 1) {
        continue;   /* the last declaration of each key counts on both sides */
      }
      const size_t at = block_lookup(y, key, it->custom_name, it->custom_length);
      bool same = false;
      if (at) {
        if (pass == 1) {
          continue;   /* a key both blocks have was judged in the first pass */
        }
        st = items_equal(x, it, y, &y->items[at - 1], &same);
      }
      if (st != GTEXT_CSS_OK || same) {
        continue;
      }
      if (it->custom_name) {
        diff->customs[diff->custom_count].data = it->custom_name;
        diff->customs[diff->custom_count].length = it->custom_length;
        diff->custom_count++;
      }
      else {
        diff->ids[diff->id_count++] = it->property;   /* one per key: only the last declaration of a property is judged */
      }
    }
  }
  if (st != GTEXT_CSS_OK) {
    css_block_diff_free(diff);
    return st;
  }
  if (diff->id_count > 1) {
    qsort(diff->ids, diff->id_count, sizeof *diff->ids, id_cmp);
  }
  if (diff->custom_count > 1) {
    qsort(diff->customs, diff->custom_count, sizeof *diff->customs, cust_name_cmp);
  }
  return GTEXT_CSS_OK;
}

/* ---- serialization ------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_block_shorthand_value(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id shorthand, char ** out,
    size_t * out_length, bool * important) {
  if (!block || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(block->registry, shorthand);
  GTEXT_CSS_Property_Info info;
  if (!id || !gtext_css_registry_property_info(block->registry, id, &info) || !(info.flags & GTEXT_CSS_PROP_SHORTHAND)) {
    return GTEXT_CSS_E_INVALID;
  }
  const size_t kept = block_lookup(block, id, NULL, 0);
  if (kept && block->items[kept - 1].opaque) {
    /* a shorthand kept as written is its own value */
    const blk_item * it = &block->items[kept - 1];
    GTEXT_Allocator da = *gtext_allocator_default();
    char * copy = gtext_allocator_malloc(&da, it->raw_length + 1);
    if (!copy) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(copy, it->raw, it->raw_length);
    copy[it->raw_length] = '\0';
    *out = copy;
    *out_length = it->raw_length;
    if (important) {
      *important = it->important;
    }
    return GTEXT_CSS_OK;
  }
  if (gtext_css_shorthand_kind(block->registry, id) == GTEXT_CSS_SHORTHAND_UNIMPLEMENTED) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  GTEXT_CSS_Shorthand_Result r;
  memset(&r, 0, sizeof r);
  bool all_wide = false;
  GTEXT_CSS_Wide_Keyword wide = GTEXT_CSS_WIDE_NONE;
  bool imp = false;
  GTEXT_CSS_Status st = css_block_shorthand_text(block, id, &all_wide, &wide, out, out_length, &imp, &r);
  if (st == GTEXT_CSS_OK && important) {
    *important = imp;
  }
  return st;
}

GTEXT_CSS_Status gtext_css_block_get_property_value(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, char ** out,
    size_t * out_length, bool * important) {
  if (!block || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(block->registry, property);
  GTEXT_CSS_Property_Info info;
  if (!id || !gtext_css_registry_property_info(block->registry, id, &info)) {
    return GTEXT_CSS_E_INVALID;
  }
  if ((info.flags & GTEXT_CSS_PROP_SHORTHAND) && !is_all(block->registry, id)) {
    return gtext_css_block_shorthand_value(block, id, out, out_length, important);
  }
  const size_t at = block_lookup(block, id, NULL, 0);
  if (at) {
    const blk_item * it = &block->items[at - 1];
    if (important) {
      *important = it->important;
    }
    if (it->opaque) {
      GTEXT_Allocator da = *gtext_allocator_default();
      char * copy = gtext_allocator_malloc(&da, it->raw_length + 1);
      if (!copy) {
        return GTEXT_CSS_E_OOM;
      }
      memcpy(copy, it->raw, it->raw_length);
      copy[it->raw_length] = '\0';
      *out = copy;
      *out_length = it->raw_length;
      return GTEXT_CSS_OK;
    }
    if (it->outcome != GTEXT_CSS_PROP_RESULT_PENDING) {
      char * t = NULL;
      size_t n = 0;
      const GTEXT_Allocator * da = gtext_allocator_default();
      GTEXT_CSS_Status st = css_item_text(block->registry, da, id, it->outcome, it->wide, it->set, &t, &n);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      *out = t;
      *out_length = n;
      return GTEXT_CSS_OK;
    }
  }
  GTEXT_Allocator a = *gtext_allocator_default();
  char * empty = gtext_allocator_malloc(&a, 1);
  if (!empty) {
    return GTEXT_CSS_E_OOM;
  }
  empty[0] = '\0';
  *out = empty;
  *out_length = 0;
  if (important) {
    *important = false;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_block_text(const GTEXT_CSS_Declaration_Block * block, tbuf * out);

GTEXT_CSS_Status gtext_css_block_serialize(const GTEXT_CSS_Declaration_Block * block, const GTEXT_Markup_Sink * sink) {
  if (!block || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  tbuf out;
  tb_init(&out, &block->alloc);
  GTEXT_CSS_Status st = css_block_text(block, &out);
  if (st == GTEXT_CSS_OK && out.len > block->options.max_output_bytes) {
    st = GTEXT_CSS_E_LIMIT_OUTPUT;
  }
  if (st == GTEXT_CSS_OK && out.len && sink->write(sink->user, out.data, out.len) != GTEXT_MARKUP_OK) {
    st = GTEXT_CSS_E_WRITE;
  }
  tb_free(&out);
  return st;
}

GTEXT_CSS_Status gtext_css_block_serialize_buffer(const GTEXT_CSS_Declaration_Block * block, char ** out, size_t * out_length) {
  if (!block || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  tbuf b;
  tb_init(&b, gtext_allocator_default());
  GTEXT_CSS_Status st = css_block_text(block, &b);
  if (st == GTEXT_CSS_OK && b.len > block->options.max_output_bytes) {
    st = GTEXT_CSS_E_LIMIT_OUTPUT;
  }
  if (st != GTEXT_CSS_OK) {
    tb_free(&b);
    return st;
  }
  size_t n = 0;
  char * s = tb_take(&b, &n);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  *out = s;
  *out_length = n;
  return GTEXT_CSS_OK;
}
