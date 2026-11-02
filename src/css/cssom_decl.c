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
 * `CSSStyleDeclaration` (css_cssom_decl.h): the declaration object of every owner, over the declaration block, with CSSOM's operations in the order the
 * specification gives their steps. The element-owned half (validation by content, write-back) is cssom_inline.c; the IDL attribute names cssom_decl_idl.c.
 *
 * A mutation is made in three parts so that the strong guarantee holds: everything that can fail is done first (the parse, the clone of an element's block, the
 * serialization and the copy that the attribute's remembered string will be, the owner's `prepare`), then the caller's attribute is written, and only then
 * is the object changed, by pointer swaps and an infallible commit.
 */

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/security/sha256.h>

#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_internal.h"

#define SIZEOF_DECL sizeof(GTEXT_CSSOM_Decl)

/* ---- the object ------------------------------------------------------------------------------------------------------------------------------------ */

static void decl_destroy(css_obj * o) {
  GTEXT_CSSOM_Decl * d = (GTEXT_CSSOM_Decl *)o;
  css_core * core = o->core;
  GTEXT_Allocator a = core->alloc;
  if (d->owner_kind == GTEXT_CSSOM_DECL_ELEMENT) {
    css_inline_forget(core, d);
  }
  gtext_allocator_free(&a, d->remembered);
  gtext_css_block_free(d->block);
  gtext_allocator_free(&a, d);
}

GTEXT_CSS_Status css_decl_create(css_core * core, GTEXT_CSSOM_Decl_Owner owner_kind, uint64_t internal, uint64_t external, GTEXT_CSSOM_Decl ** out) {
  GTEXT_CSS_Status st = css_core_room(core, SIZEOF_DECL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_Allocator a = core->alloc;
  GTEXT_CSSOM_Decl * d = gtext_allocator_calloc(&a, 1, SIZEOF_DECL);
  if (!d) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.allocator = &core->alloc;
  o.max_declarations = core->config.max_declarations;
  o.max_output_bytes = core->config.max_output_bytes;
  st = gtext_css_block_new(core->registry, &o, &d->block);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&a, d);
    return st;
  }
  css_obj_init(&d->obj, core, GTEXT_CSSOM_OBJECT_DECLARATION, internal, external, SIZEOF_DECL, decl_destroy);
  d->owner_kind = owner_kind;
  d->readonly = owner_kind == GTEXT_CSSOM_DECL_COMPUTED;
  *out = d;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_decl_new(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Decl ** out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  return css_decl_create(css_document_core(document), GTEXT_CSSOM_DECL_FREE, 0, 1, out);
}

GTEXT_CSSOM_Decl * gtext_cssom_decl_retain(GTEXT_CSSOM_Decl * decl) {
  if (decl) {
    css_obj_retain_external(&decl->obj);
  }
  return decl;
}

void gtext_cssom_decl_release(GTEXT_CSSOM_Decl * decl) {
  if (decl) {
    css_obj_release_external(&decl->obj);
  }
}

GTEXT_CSSOM_Decl_Owner gtext_cssom_decl_owner(const GTEXT_CSSOM_Decl * decl) {
  return decl ? decl->owner_kind : GTEXT_CSSOM_DECL_FREE;
}

bool gtext_cssom_decl_readonly(const GTEXT_CSSOM_Decl * decl) {
  return decl ? decl->readonly : true;
}

uint64_t gtext_cssom_decl_version(const GTEXT_CSSOM_Decl * decl) {
  return decl ? decl->version : 0;
}

GTEXT_CSSOM_Document * gtext_cssom_decl_document(const GTEXT_CSSOM_Decl * decl) {
  return decl ? decl->obj.core->document : NULL;
}

/* ---- names and values --------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  bool custom;          ///< A name beginning `--`.
  bool known;           ///< The registry has it (a custom property: its name can be written and read back).
  bool shorthand;       ///< A shorthand other than `all`.
  GTEXT_CSS_Property_Id id;   ///< The property, an alias followed; 0 for a custom property.
  const char * name;    ///< A custom property's name as given.
  size_t length;
} pref;

static bool is_all_info(const GTEXT_CSS_Property_Info * info) {
  return info->name_length == 3 && memcmp(info->name, "all", 3) == 0;
}

static void pref_resolve(const css_core * core, const char * name, size_t length, pref * p) {
  memset(p, 0, sizeof *p);
  if (!name) {
    return;
  }
  if (length >= 2 && name[0] == '-' && name[1] == '-') {
    p->custom = true;
    p->name = name;
    p->length = length;
    p->known = css_custom_name_ok(name, length);
    return;
  }
  GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(core->registry, name, length);
  if (!id) {
    return;
  }
  id = gtext_css_registry_resolve_alias(core->registry, id);
  GTEXT_CSS_Property_Info info;
  if (!id || !gtext_css_registry_property_info(core->registry, id, &info) || (info.flags & GTEXT_CSS_PROP_CUSTOM)) {
    return;
  }
  p->known = true;
  p->id = id;
  p->shorthand = (info.flags & GTEXT_CSS_PROP_SHORTHAND) && !is_all_info(&info);
}

/// The text of a declaration as getPropertyValue writes it: the empty string for a longhand left pending by a shorthand.
static GTEXT_CSS_Status item_value_text(css_core * core, const GTEXT_CSS_Declaration_Item * it, char ** out, size_t * n) {
  if (it->opaque) {
    *out = css_core_dup(core, it->raw.data, it->raw.length);
    *n = it->raw.length;
    return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (it->outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    const char * k = gtext_css_wide_keyword_name(it->wide);
    *out = css_core_dup(core, k, strlen(k));
    *n = strlen(k);
    return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (it->custom_name.data || (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING && !it->shorthand)) {
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(it->set);
    const size_t len = root && root->text.data ? root->text.length : 0;
    *out = css_core_dup(core, len ? root->text.data : "", len);
    *n = len;
    return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
    *out = css_core_dup(core, "", 0);
    *n = 0;
    return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  char * t = NULL;
  size_t len = 0;
  GTEXT_CSS_Status st = css_item_text(core->registry, &core->alloc, it->property, it->outcome, it->wide, it->set, &t, &len);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *out = t;
  *n = len;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status empty_string(css_core * core, char ** out, size_t * n) {
  *out = css_core_dup(core, "", 0);
  *n = 0;
  return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

/// A buffer the block layer made from the default allocator, copied to the core's and freed.
static GTEXT_CSS_Status adopt_buffer(css_core * core, char * buf, size_t n, char ** out) {
  char * mine = css_core_dup(core, buf, n);
  gtext_css_serialize_buffer_free(NULL, buf);
  *out = mine;
  return mine ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

/* ---- reading ------------------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_decl_length(GTEXT_CSSOM_Decl * decl, size_t * out) {
  if (!decl || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = css_decl_validate(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *out = gtext_css_block_count(decl->block);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_decl_item(GTEXT_CSSOM_Decl * decl, size_t index, char ** out, size_t * out_length) {
  if (!decl || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = decl->obj.core;
  GTEXT_CSS_Status st = css_decl_validate(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_block_compact(decl->block);
  GTEXT_CSS_Declaration_Item it;
  size_t n = 0;
  if (!gtext_css_block_item(decl->block, index, &it)) {
    st = empty_string(core, out, &n);
  }
  else if (it.custom_name.data) {
    *out = css_core_dup(core, it.custom_name.data, it.custom_name.length);
    n = it.custom_name.length;
    st = *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  else {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(core->registry, it.property, &info);
    *out = css_core_dup(core, info.name, info.name_length);
    n = info.name_length;
    st = *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK && out_length) {
    *out_length = n;
  }
  return st;
}

/// getPropertyValue for a resolved name, without the validation (the callers did it).
static GTEXT_CSS_Status value_of(GTEXT_CSSOM_Decl * decl, const pref * p, char ** out, size_t * out_length) {
  css_core * core = decl->obj.core;
  GTEXT_CSS_Status st;
  size_t n = 0;
  if (!p->known) {
    st = empty_string(core, out, &n);
  }
  else if (p->shorthand) {
    char * buf = NULL;
    st = gtext_css_block_shorthand_value(decl->block, p->id, &buf, &n, NULL);
    if (st == GTEXT_CSS_OK) {
      st = adopt_buffer(core, buf, n, out);
    }
  }
  else {
    GTEXT_CSS_Declaration_Item it;
    if (gtext_css_block_find(decl->block, p->id, p->custom ? p->name : NULL, p->length, &it)) {
      st = item_value_text(core, &it, out, &n);
    }
    else {
      st = empty_string(core, out, &n);
    }
  }
  if (st == GTEXT_CSS_OK && out_length) {
    *out_length = n;
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_decl_get_property_value(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, char ** out, size_t * out_length) {
  if (!decl || !out || (!name && name_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = css_decl_validate(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  pref p;
  pref_resolve(decl->obj.core, name, name_length, &p);
  return value_of(decl, &p, out, out_length);
}

GTEXT_CSS_Status gtext_cssom_decl_get_property_priority(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, const char ** out) {
  if (!decl || !out || (!name && name_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = decl->obj.core;
  GTEXT_CSS_Status st = css_decl_validate(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *out = "";
  pref p;
  pref_resolve(core, name, name_length, &p);
  if (!p.known) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Declaration_Item it;
  if (!p.shorthand) {
    if (gtext_css_block_find(decl->block, p.id, p.custom ? p.name : NULL, p.length, &it) && it.important) {
      *out = "important";
    }
    return GTEXT_CSS_OK;
  }
  if (gtext_css_block_find(decl->block, p.id, NULL, 0, &it) && it.opaque) {
    if (it.important) {
      *out = "important";
    }
    return GTEXT_CSS_OK;   /* a shorthand kept as written */
  }
  /* CSSOM getPropertyPriority: for a shorthand, important when every longhand it maps to is in the declarations and is important */
  GTEXT_CSS_Property_Id leaves[256];
  size_t n = 0;
  if (!css_shorthand_leaves(core->registry, p.id, leaves, 256, &n) || n == 0) {
    return GTEXT_CSS_OK;
  }
  for (size_t i = 0; i < n; i++) {
    if (!gtext_css_block_find(decl->block, leaves[i], NULL, 0, &it) || !it.important) {
      return GTEXT_CSS_OK;
    }
  }
  *out = "important";
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_decl_value_set(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, GTEXT_CSS_Value_Set ** out,
    GTEXT_CSS_Wide_Keyword * wide, bool * important, bool * found) {
  if (!decl || !out || !found || (!name && name_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = css_decl_validate(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  pref p;
  pref_resolve(decl->obj.core, name, name_length, &p);
  *found = false;
  GTEXT_CSS_Declaration_Item it;
  if (!p.known || p.shorthand || !gtext_css_block_find(decl->block, p.id, p.custom ? p.name : NULL, p.length, &it) || it.opaque) {
    return GTEXT_CSS_OK;   /* a declaration kept as written has no typed value */
  }
  *out = it.set ? gtext_css_value_set_retain(it.set) : NULL;
  if (wide) {
    *wide = it.outcome == GTEXT_CSS_PROP_RESULT_WIDE ? it.wide : GTEXT_CSS_WIDE_NONE;
  }
  if (important) {
    *important = it.important;
  }
  *found = true;
  return GTEXT_CSS_OK;
}

/* ---- cssText ------------------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_decl_css_text(GTEXT_CSSOM_Decl * decl, char ** out, size_t * out_length) {
  if (!decl || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = decl->obj.core;
  GTEXT_CSS_Status st = css_decl_validate(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  tbuf b;
  tb_init(&b, &core->alloc);
  st = css_block_text(decl->block, &b);
  if (st == GTEXT_CSS_OK && b.len > core->config.max_output_bytes) {
    st = GTEXT_CSS_E_LIMIT_OUTPUT;
  }
  if (st != GTEXT_CSS_OK) {
    tb_free(&b);
    return st;
  }
  size_t n = 0;
  char * text = tb_take(&b, &n);
  if (!text) {
    return GTEXT_CSS_E_OOM;
  }
  *out = text;
  if (out_length) {
    *out_length = n;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_decl_serialize(GTEXT_CSSOM_Decl * decl, const GTEXT_Markup_Sink * sink) {
  if (!decl || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  char * text = NULL;
  size_t n = 0;
  GTEXT_CSS_Status st = gtext_cssom_decl_css_text(decl, &text, &n);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (n && sink->write(sink->user, text, n) != GTEXT_MARKUP_OK) {
    st = GTEXT_CSS_E_WRITE;
  }
  gtext_cssom_buffer_free(&decl->obj.core->alloc, text);
  return st;
}

/* ---- parsing a list of declarations ---------------------------------------------------------------------------------------------------------- */

/// A declaration the library cannot judge, kept as written (CSSOM keeps it; the cascade drops it only for computing): the property of the registry and the value's tokens
/// written out and trimmed. A name the registry does not have is not a declaration.
static GTEXT_CSS_Status keep_opaque(css_core * core, GTEXT_CSS_Declaration_Block * block, const GTEXT_CSS_Declaration * d) {
  GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(core->registry, d->name.data, d->name.length);
  if (id) {
    id = gtext_css_registry_resolve_alias(core->registry, id);
  }
  if (!id) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &core->alloc;
  char * text = NULL;
  size_t n = 0;
  GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(d->value, d->value_count, &so, &text, &n);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const char * p = text;
  size_t m = n;
  css_trim_value_text(&p, &m);
  if (m != 0) {
    st = css_block_set_opaque_ex(block, id, p, m, d->important, true, NULL);
    if (st == GTEXT_CSS_OK) {
      core->counters.unjudgeable_preserved++;
    }
  }
  gtext_css_serialize_buffer_free(&so, text);
  return st;
}

GTEXT_CSS_Status css_decl_parse_block(css_core * core, const char * text, size_t length, bool quirks, GTEXT_CSS_Declaration_Block ** out) {
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &core->alloc;
  po.max_input_bytes = core->config.max_input_bytes;
  po.count_errors_only = true;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_declaration_list(text ? text : "", length, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.allocator = &core->alloc;
  o.max_declarations = core->config.max_declarations;
  o.max_output_bytes = core->config.max_output_bytes;
  GTEXT_CSS_Declaration_Block * block = NULL;
  st = gtext_css_block_new(core->registry, &o, &block);
  if (st != GTEXT_CSS_OK) {
    gtext_css_tree_free(tree);
    return st;
  }
  if (tree->declaration_count > core->config.max_work) {
    st = GTEXT_CSS_E_LIMIT_WORK;
  }
  GTEXT_CSS_Property_Options pro;
  memset(&pro, 0, sizeof pro);
  pro.allocator = &core->alloc;
  pro.quirks = quirks;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < tree->declaration_count; i++) {
    const GTEXT_CSS_Declaration * d = &tree->declarations[i];
    GTEXT_CSS_Property_Value pv;
    memset(&pv, 0, sizeof pv);
    st = gtext_css_parse_property_declaration(core->registry, d, &pro, &pv);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      /* a property the registry has and the library cannot judge: the cascade drops the declaration for computing, but a declaration block keeps it, as written */
      gtext_css_property_value_free(&pv);
      st = keep_opaque(core, block, d);
      continue;
    }
    if (st == GTEXT_CSS_OK && pv.outcome != GTEXT_CSS_PROP_RESULT_UNKNOWN && pv.outcome != GTEXT_CSS_PROP_RESULT_INVALID) {
      if (d->name.length >= 2 && d->name.data[0] == '-' && d->name.data[1] == '-') {
        if (css_custom_name_ok(d->name.data, d->name.length)) {
          st = css_block_set_custom_ex(block, d->name.data, d->name.length, &pv, d->important, true, NULL);
        }
      }
      else {
        GTEXT_CSS_Shorthand_Result sr;
        st = css_block_set_ex(block, &pv, d->important, true, NULL, &sr);
        if (st == GTEXT_CSS_E_UNSUPPORTED) {
          st = keep_opaque(core, block, d);   /* a shorthand the library cannot split: kept as one declaration */
        }
      }
    }
    gtext_css_property_value_free(&pv);
  }
  gtext_css_tree_free(tree);
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(block);
    return st;
  }
  *out = block;
  return GTEXT_CSS_OK;
}

/* ---- mutating --------------------------------------------------------------------------------------------------------------------------------------- */

typedef enum { EDIT_SET, EDIT_SET_CUSTOM, EDIT_REMOVE, EDIT_REMOVE_CUSTOM } edit_op;

typedef struct {
  edit_op op;
  const GTEXT_CSS_Property_Value * value;
  bool important;
  GTEXT_CSS_Property_Id id;
  const char * name;
  size_t length;
} edit;

static GTEXT_CSS_Status run_edit(GTEXT_CSS_Declaration_Block * block, const edit * e, GTEXT_CSS_Block_Change * change, GTEXT_CSSOM_Outcome * out) {
  GTEXT_CSS_Status st;
  switch (e->op) {
  case EDIT_SET: {
    GTEXT_CSS_Shorthand_Result sr;
    st = gtext_css_block_set(block, e->value, e->important, change, &sr);
    if (st == GTEXT_CSS_E_UNSUPPORTED && out) {
      out->property = e->id;
      out->detail = sr.detail ? sr.detail : "the value of this property cannot be judged";
    }
    return st;
  }
  case EDIT_SET_CUSTOM:
    return gtext_css_block_set_custom(block, e->name, e->length, e->value, e->important, change);
  case EDIT_REMOVE:
    return gtext_css_block_remove(block, e->id, NULL, 0, change);
  default:
    return gtext_css_block_remove(block, 0, e->name, e->length, change);
  }
}

static bool element_live(const GTEXT_CSSOM_Decl * d) {
  return d->owner_kind == GTEXT_CSSOM_DECL_ELEMENT && !d->detached;
}

/// The start of a mutator, in the order the steps come: the library's checks, the read-only check ("If the readonly flag is set, then throw a
/// NoModificationAllowedError"), the owner's, and the validation of an element's attribute.
static GTEXT_CSS_Status mutate_begin(GTEXT_CSSOM_Decl * d, GTEXT_CSSOM_Outcome * out) {
  css_core * core = d->obj.core;
  css_outcome_reset(out, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (d->readonly) {
    return GTEXT_CSS_E_NO_MODIFICATION;
  }
  if (d->ops && d->ops->prepare) {
    st = d->ops->prepare(d->owner, d);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  return css_decl_validate(d);
}

static int id_cmp(const void * a, const void * b) {
  const GTEXT_CSS_Property_Id x = *(const GTEXT_CSS_Property_Id *)a;
  const GTEXT_CSS_Property_Id y = *(const GTEXT_CSS_Property_Id *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

/// After a change is made: bump the declaration's version, tell the owner, and send the one event.
static void finish_change(GTEXT_CSSOM_Decl * d, GTEXT_CSS_Property_Id * ids, size_t id_count, const GTEXT_CSS_String * customs, size_t custom_count,
    bool attribute_written, GTEXT_CSSOM_Outcome * out) {
  css_core * core = d->obj.core;
  d->version++;
  if (id_count > 1) {
    qsort(ids, id_count, sizeof *ids, id_cmp);
  }
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = d->owner_kind == GTEXT_CSSOM_DECL_ELEMENT ? GTEXT_CSSOM_EVENT_INLINE_STYLE_CHANGED : GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED;
  ev.declaration = d;
  ev.element = d->owner_kind == GTEXT_CSSOM_DECL_ELEMENT ? d->element : NULL;
  ev.properties = ids;
  ev.property_count = id_count;
  ev.custom_names = customs;
  ev.custom_count = custom_count;
  ev.attribute_written = attribute_written;
  for (size_t i = 0; i < id_count; i++) {
    GTEXT_CSS_Property_Info info;
    if (gtext_css_registry_property_info(core->registry, ids[i], &info) && is_all_info(&info)) {
      ev.all_changed = true;
    }
  }
  if (d->ops && d->ops->commit) {
    d->ops->commit(d->owner, d, &ev);
  }
  if (out) {
    out->kind = GTEXT_CSSOM_CHANGED;
    out->attribute_written = attribute_written;
  }
  css_cssom_commit(core, &ev, out);
}

/// Apply one edit: to the block in place, or for an element-owned declaration to a clone that is written to the attribute first.
static GTEXT_CSS_Status apply_edit(GTEXT_CSSOM_Decl * d, const edit * e, GTEXT_CSSOM_Outcome * out) {
  css_core * core = d->obj.core;
  GTEXT_CSS_Block_Change change;
  memset(&change, 0, sizeof change);
  GTEXT_CSS_String custom;
  const bool is_custom = e->op == EDIT_SET_CUSTOM || e->op == EDIT_REMOVE_CUSTOM;
  custom.data = e->name;
  custom.length = e->length;
  GTEXT_CSS_Status st;
  if (!element_live(d)) {
    st = run_edit(d->block, e, &change, out);
    if (st == GTEXT_CSS_OK && change.changed) {
      finish_change(d, change.ids, change.count, is_custom ? &custom : NULL, is_custom ? 1 : 0, false, out);
    }
    gtext_css_block_change_free(&change);
    return st;
  }
  GTEXT_CSS_Declaration_Block * work = NULL;
  st = gtext_css_block_clone(d->block, &work);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = run_edit(work, e, &change, out);
  if (st != GTEXT_CSS_OK || !change.changed) {
    gtext_css_block_free(work);
    gtext_css_block_change_free(&change);
    return st;
  }
  tbuf text;
  tb_init(&text, &core->alloc);
  st = css_block_text(work, &text);
  if (st == GTEXT_CSS_OK && text.len > core->config.max_output_bytes) {
    st = GTEXT_CSS_E_LIMIT_OUTPUT;
  }
  bool written = false;
  GTEXT_CSS_Declaration_Block * old = d->block;
  if (st == GTEXT_CSS_OK) {
    st = css_decl_write_attribute(d, text.data ? text.data : "", text.len, &written);
  }
  tb_free(&text);
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(work);
    gtext_css_block_change_free(&change);
    return st;
  }
  d->block = work;
  gtext_css_block_free(old);
  finish_change(d, change.ids, change.count, is_custom ? &custom : NULL, is_custom ? 1 : 0, written, out);
  gtext_css_block_change_free(&change);
  return GTEXT_CSS_OK;
}

static bool ascii_ci_important(const char * s, size_t n) {
  static const char word[] = "important";
  if (n != 9) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != word[i]) {
      return false;
    }
  }
  return true;
}

static void ignore(GTEXT_CSSOM_Outcome * out, GTEXT_CSSOM_Outcome_Kind kind, GTEXT_CSS_Property_Id property, const char * detail) {
  if (out) {
    out->kind = kind;
    out->property = property;
    out->detail = detail;
  }
}

/// removeProperty after the start of the mutator: "Let value be the return value of invoking getPropertyValue() with property as argument. Let removed be false. ... If removed
/// is true, update style attribute for the CSS declaration block. Return value."
static GTEXT_CSS_Status remove_after_begin(GTEXT_CSSOM_Decl * decl, const pref * p, char ** old_value, size_t * old_length, GTEXT_CSSOM_Outcome * out) {
  css_core * core = decl->obj.core;
  char * value = NULL;
  size_t value_length = 0;
  GTEXT_CSS_Status st = value_of(decl, p, &value, &value_length);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (p->known) {
    edit e;
    memset(&e, 0, sizeof e);
    e.op = p->custom ? EDIT_REMOVE_CUSTOM : EDIT_REMOVE;
    e.id = p->id;
    e.name = p->name;
    e.length = p->length;
    st = apply_edit(decl, &e, out);
  }
  if (st != GTEXT_CSS_OK || !old_value) {
    gtext_cssom_buffer_free(&core->alloc, value);
    return st;
  }
  *old_value = value;
  if (old_length) {
    *old_length = value_length;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_decl_remove_property(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, char ** old_value, size_t * old_length,
    GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (!decl || (!name && name_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  /* "1. If the readonly flag is set, then throw a NoModificationAllowedError exception. 2. If property is not a custom property, let property be property converted to ASCII lowercase." */
  GTEXT_CSS_Status st = mutate_begin(decl, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  pref p;
  pref_resolve(decl->obj.core, name, name_length, &p);
  return remove_after_begin(decl, &p, old_value, old_length, out);
}

GTEXT_CSS_Status gtext_cssom_decl_set_property(GTEXT_CSSOM_Decl * decl, const char * name, size_t name_length, const char * value, size_t value_length,
    const char * priority, size_t priority_length, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (!decl || (!name && name_length) || (!value && value_length) || (!priority && priority_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = decl->obj.core;
  /* 1. "If the readonly flag is set, then throw a NoModificationAllowedError exception." */
  GTEXT_CSS_Status st = mutate_begin(decl, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  /* 2. "If property is not a custom property: let property be property converted to ASCII lowercase. If property is not a case-sensitive match for a supported CSS
        property, then return." */
  pref p;
  pref_resolve(core, name, name_length, &p);
  if (!p.known) {
    ignore(out, GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY, 0, "not a supported property");
    return GTEXT_CSS_OK;
  }
  /* 3. "If value is the empty string, invoke removeProperty() with property as argument and return." */
  if (value_length == 0) {
    return remove_after_begin(decl, &p, NULL, NULL, out);
  }
  /* 4. "If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return." */
  if (priority_length != 0 && !ascii_ci_important(priority, priority_length)) {
    ignore(out, GTEXT_CSSOM_IGNORED_BAD_PRIORITY, p.id, "the priority is neither empty nor important");
    return GTEXT_CSS_OK;
  }
  if (value_length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  /* 5. "Let component value list be the result of parsing value for property property." 6. "If component value list is null, then return." (An invalid value, a
        value that holds `!important`, which the grammar of no property accepts.) */
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &core->alloc;
  po.quirks = decl->owner_kind == GTEXT_CSSOM_DECL_ELEMENT && core->config.quirks;
  GTEXT_CSS_Property_Value pv;
  st = gtext_css_parse_property_text(core->registry, p.custom ? p.name : name, p.custom ? p.length : name_length, value, value_length, &po, &pv);
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    out->property = p.id;
    out->detail = "the value of this property cannot be judged";
    gtext_css_property_value_free(&pv);
    return st;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_property_value_free(&pv);
    return st;
  }
  if (pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN || pv.outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
    const bool unknown = pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN;
    gtext_css_property_value_free(&pv);
    ignore(out, unknown ? GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY : GTEXT_CSSOM_IGNORED_INVALID_VALUE, p.id,
        unknown ? "not a supported property" : "the value is not valid for the property");
    return GTEXT_CSS_OK;
  }
  /* 7. onwards: "Let updated be false" ... set a CSS declaration for the property, or for each longhand of a shorthand. */
  edit e;
  memset(&e, 0, sizeof e);
  e.op = p.custom ? EDIT_SET_CUSTOM : EDIT_SET;
  e.value = &pv;
  e.important = priority_length != 0;
  e.id = p.id;
  e.name = p.name;
  e.length = p.length;
  st = apply_edit(decl, &e, out);
  gtext_css_property_value_free(&pv);
  return st;
}

GTEXT_CSS_Status gtext_cssom_decl_set_css_text(GTEXT_CSSOM_Decl * decl, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (!decl || (!text && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = decl->obj.core;
  /* "1. If the readonly flag is set, then throw a NoModificationAllowedError exception." */
  GTEXT_CSS_Status st = mutate_begin(decl, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  /* "2. Empty the declarations. 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the declarations, in specified order." */
  GTEXT_CSS_Declaration_Block * neu = NULL;
  st = css_decl_parse_block(core, text, length, decl->owner_kind == GTEXT_CSSOM_DECL_ELEMENT && core->config.quirks, &neu);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  tbuf before, after;
  tb_init(&before, &core->alloc);
  tb_init(&after, &core->alloc);
  css_block_delta diff;
  memset(&diff, 0, sizeof diff);
  bool have_diff = false;
  st = css_block_text(decl->block, &before);
  if (st == GTEXT_CSS_OK) {
    st = css_block_text(neu, &after);
  }
  if (st == GTEXT_CSS_OK && (before.len > core->config.max_output_bytes || after.len > core->config.max_output_bytes)) {
    st = GTEXT_CSS_E_LIMIT_OUTPUT;
  }
  const bool changed = st == GTEXT_CSS_OK && (before.len != after.len || (after.len && memcmp(before.data, after.data, after.len) != 0));
  if (st == GTEXT_CSS_OK && changed) {
    st = css_block_diff(decl->block, neu, &diff);
    have_diff = st == GTEXT_CSS_OK;
  }
  bool written = false;
  if (st == GTEXT_CSS_OK && element_live(decl)) {
    /* "4. Update style attribute for the CSS declaration block." Unconditional: also for text that changes nothing. */
    st = css_decl_write_attribute(decl, after.data ? after.data : "", after.len, &written);
  }
  tb_free(&before);
  tb_free(&after);
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(neu);
    if (have_diff) {
      css_block_diff_free(&diff);
    }
    return st;
  }
  if (!changed) {
    gtext_css_block_free(neu);
    out->attribute_written = written;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Declaration_Block * old = decl->block;
  decl->block = neu;
  /* the names of the custom properties in the event point into the old block and the new: the old one outlives the delivery */
  finish_change(decl, diff.ids, diff.id_count, diff.customs, diff.custom_count, written, out);
  gtext_css_block_free(old);
  css_block_diff_free(&diff);
  return GTEXT_CSS_OK;
}

/* ---- the dump ------------------------------------------------------------------------------------------------------------------------------------- */

static const char * const owner_names[] = {"free", "rule", "element", "computed"};

static GTEXT_CSS_Status put(const GTEXT_Markup_Sink * sink, const char * s, size_t n) {
  return n == 0 || sink->write(sink->user, s, n) == GTEXT_MARKUP_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_WRITE;
}

static GTEXT_CSS_Status puts_(const GTEXT_Markup_Sink * sink, const char * s) {
  return put(sink, s, strlen(s));
}

/// A number in decimal, without the host's formatting.
static GTEXT_CSS_Status put_u64(const GTEXT_Markup_Sink * sink, uint64_t v) {
  char b[24];
  size_t n = sizeof b;
  do {
    b[--n] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  return put(sink, b + n, sizeof b - n);
}

GTEXT_CSS_Status gtext_cssom_decl_dump(const GTEXT_CSSOM_Decl * decl, const GTEXT_Markup_Sink * sink) {
  if (!decl || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = decl->obj.core;
  GTEXT_CSS_Status st = puts_(sink, "cssom-decl 1\nowner ");
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, owner_names[decl->owner_kind]);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, decl->readonly ? "\nreadonly 1\nversion " : "\nreadonly 0\nversion ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, decl->version);
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, "\ndeclarations ");
  }
  if (st == GTEXT_CSS_OK) {
    st = put_u64(sink, (uint64_t)gtext_css_block_count(decl->block));
  }
  if (st == GTEXT_CSS_OK) {
    st = puts_(sink, "\n");
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < gtext_css_block_count(decl->block); i++) {
    GTEXT_CSS_Declaration_Item it;
    gtext_css_block_item(decl->block, i, &it);
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(core->registry, it.property, &info);
    static const char * const outcomes[] = {"unknown", "valid", "wide", "pending", "invalid"};
    const char * outcome_name = it.opaque ? "opaque" : outcomes[it.outcome];
    st = puts_(sink, "decl ");
    if (st == GTEXT_CSS_OK) {
      st = it.custom_name.data ? put(sink, it.custom_name.data, it.custom_name.length) : put(sink, info.name, info.name_length);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, it.important ? " important " : " normal ");
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, outcome_name);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, " ");
    }
    char * v = NULL;
    size_t n = 0;
    if (st == GTEXT_CSS_OK) {
      st = item_value_text(core, &it, &v, &n);
    }
    if (st == GTEXT_CSS_OK) {
      st = put(sink, v, n);
      gtext_cssom_buffer_free(&core->alloc, v);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, "\n");
    }
  }
  if (st == GTEXT_CSS_OK) {
    tbuf b;
    tb_init(&b, &core->alloc);
    st = css_block_text(decl->block, &b);
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, "cssText ");
    }
    if (st == GTEXT_CSS_OK) {
      st = put(sink, b.data, b.len);
    }
    if (st == GTEXT_CSS_OK) {
      st = puts_(sink, "\n");
    }
    tb_free(&b);
  }
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

GTEXT_CSS_Status gtext_cssom_decl_hash(const GTEXT_CSSOM_Decl * decl, unsigned char hash[GTEXT_CSSOM_HASH_LENGTH]) {
  if (!decl || !hash) {
    return GTEXT_CSS_E_INVALID;
  }
  hasher h;
  h.failed = false;
  if (gsec_sha256_init(&h.ctx) != GSEC_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &h;
  sink.write = hash_write;
  const GTEXT_CSS_Status st = gtext_cssom_decl_dump(decl, &sink);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_WRITE ? GTEXT_CSS_E_INTERNAL : st;
  }
  if (gsec_sha256_final(&h.ctx, hash) != GSEC_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  return GTEXT_CSS_OK;
}
