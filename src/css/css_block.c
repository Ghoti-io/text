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
  }
  gtext_allocator_free(&a, block->items);
  gtext_allocator_free(&a, block);
}

static GTEXT_CSS_Status block_reserve(GTEXT_CSS_Declaration_Block * b, size_t more) {
  if (more > b->options.max_declarations || b->count > b->options.max_declarations - more) {
    return GTEXT_CSS_E_LIMIT_DECLARATIONS;
  }
  if (b->count + more <= b->cap) {
    return GTEXT_CSS_OK;
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
  }
  gtext_css_longhand_list_free(list);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_block_add_custom(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length,
    const GTEXT_CSS_Property_Value * value, bool important) {
  if (!block || !value || !name || name_length < 3 || name[0] != '-' || name[1] != '-') {
    return GTEXT_CSS_E_INVALID;
  }
  if (value->outcome != GTEXT_CSS_PROP_RESULT_VALID && value->outcome != GTEXT_CSS_PROP_RESULT_WIDE) {
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
    st = gtext_css_block_add_custom(block, name, name_length, &pv, important);
  }
  else {
    st = gtext_css_block_add(block, &pv, important, result);
  }
  gtext_css_property_value_free(&pv);
  return st;
}

size_t gtext_css_block_count(const GTEXT_CSS_Declaration_Block * block) {
  return block ? block->count : 0;
}

bool gtext_css_block_item(const GTEXT_CSS_Declaration_Block * block, size_t index, GTEXT_CSS_Declaration_Item * out) {
  if (!block || !out || index >= block->count) {
    return false;
  }
  const blk_item * it = &block->items[index];
  out->property = it->property;
  out->custom_name.data = it->custom_name;
  out->custom_name.length = it->custom_length;
  out->important = it->important;
  out->outcome = it->outcome;
  out->wide = it->wide;
  out->set = it->set;
  out->shorthand = it->shorthand;
  out->declaration = it->declaration;
  return true;
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
  for (size_t i = block->count; i-- > 0;) {
    const blk_item * it = &block->items[i];
    if (it->property != id || it->custom_name) {
      continue;
    }
    if (important) {
      *important = it->important;
    }
    if (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
      break;
    }
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
