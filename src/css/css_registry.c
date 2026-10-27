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
 * The registry: the generated tables (tables/), the caller's registrations over them, and the lazy,
 * lock-guarded compilation of grammars.
 *
 * State and its guard. A registry is one struct that a builder is a handle on. Everything that never changes after
 * the freeze (the caller's entries, the name index, the override list) is written only before it, by one thread.
 * What changes afterward is the grammar cache: for each property, descriptor and type, the parsed definition
 * (kept so that `<'name'>` and `<type>` references can hand it to a link) and the compiled grammar. The cache is
 * written under `lock`, and a compiled grammar is published by storing its state in an atomic after the object is
 * complete, so that a reader which sees a state other than "not compiled" sees the whole object without taking
 * the lock. A compiled grammar is immutable.
 *
 * Nothing here recurses in the registry's own code. The grammar engine does not either.
 */

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/cutil/mutex.h>
#include <ghoti.io/text/css/css_function.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/macros.h>

#include <stdlib.h>
#include <string.h>

#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_tables.h"

#define REG_NAME_STACK 4096u
#define REG_DEFAULT_REGISTRATIONS 65536u
#define REG_MAX_REGISTRATIONS (1u << 24)
#define REG_DEFAULT_NAME 256u
#define REG_DEFAULT_INITIAL 65536u
#define REG_MAX_INITIAL (1u << 24)
#define REG_DEFAULT_LONGHANDS 1024u
#define REG_MAX_LONGHANDS 65536u
#define REG_DEFAULT_NODES (1u << 21)
#define REG_MAX_NODES (1u << 28)

/* ---- the state ---------------------------------------------------------------------------- */

/** The grammar of one thing, compiled when first asked for. */
typedef struct {
  GTEXT_CSS_Definition * def;  ///< The parsed text, kept for the references that resolve to it (under the lock).
  uint8_t def_state;           ///< 0 not parsed, 1 parsed, 2 the text is not a definition (under the lock).
  GTEXT_CSS_Status def_status; ///< State 2: why.
  size_t def_offset;           ///< State 2: where in the text.
  GTEXT_CSS_Compiled * compiled; ///< Set before `state` is.
  GCU_Atomic_Int state;        ///< A GTEXT_CSS_Grammar_State; read without the lock, written with it.
  GTEXT_CSS_Grammar_Result res; ///< The compile's report, set before `state`.
} lazy_grammar;

/** A property, alias or shorthand the caller registered. */
typedef struct {
  char * name;                 ///< Lower case, NUL-terminated.
  size_t name_length;
  char * grammar;              ///< NULL for an alias.
  size_t grammar_length;
  char * ctext;                ///< An override's grammar with its reference to its own name replaced by the definition it replaces, or NULL (then `grammar`).
  size_t ctext_length;
  char * initial;              ///< NULL for a shorthand and an alias.
  uint32_t flags;              ///< GTEXT_CSS_PROP_* (without GENERATED).
  GTEXT_CSS_Inherit inherited;
  GTEXT_CSS_Animation animation;
  GTEXT_CSS_Computed computed;
  GTEXT_CSS_Property_Id alias_of;
  GTEXT_CSS_Property_Id overridden;  ///< The property this one replaces, or 0.
  GTEXT_CSS_Property_Id * longhands;
  size_t longhand_count;
  GTEXT_CSS_Shorthand_Hook hook;
  void * hook_user;
  lazy_grammar lazy;
} cprop;

typedef struct {
  GTEXT_CSS_Property_Id id;
  GTEXT_CSS_Property_Id overridden;
} override_rec;

struct GTEXT_CSS_Registry {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Grammar_Options gopt;
  GTEXT_CSS_Match_Options mopt;
  size_t max_registrations, max_name, max_initial, max_longhands, max_nodes;
  size_t grammar_nodes;        ///< The nodes of the compiled grammars of the registrations so far (the budget `max_nodes` holds).
  size_t last_nodes;           ///< The nodes of the grammar the registration in hand compiled; charged when it succeeds.
  GCU_MUTEX_T lock;
  bool lock_made;

  cprop * cprops;              ///< The caller's, in order; id = generated count + 1 + index.
  size_t ncprops, capcprops;
  uint32_t * hash;             ///< Open addressing over the lower-cased names of the caller's entries: index + 1, 0 empty.
  size_t hash_cap;
  override_rec * overrides;
  size_t noverrides, capoverrides;

  lazy_grammar * gprops;       ///< One for each generated property.
  lazy_grammar * gtypes;       ///< One for each generated type: only `def` is used.
  lazy_grammar * gdescs;       ///< One for each generated descriptor.
  lazy_grammar pending;        ///< `<declaration-value>?`.
  css_creg_store cregs;        ///< The custom properties the caller registered (gtext_css_registry_register_custom()).

  size_t work;                 ///< Units of work done by registrations and by the lookups made through css_registry_find_counted(): probes, steps, bytes.
  GTEXT_CSS_Function_Types * ftypes;
  GTEXT_CSS_Property_Id scope; ///< While a link runs (under the lock): the property whose nested types are in scope.
  struct {
    GTEXT_CSS_Status status;
    char name[96];
    size_t offset;
  } rf;                        ///< While a link runs: why a definition the resolver was asked for could not be made.
};

struct GTEXT_CSS_Registry_Builder {
  GTEXT_CSS_Registry * reg;
};

/* ---- small helpers ------------------------------------------------------------------------ */

static void lower_copy(char * dst, const char * src, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)src[i];
    dst[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  }
}

static int cmp_bytes(const char * a, size_t al, const char * b, size_t bl) {
  const size_t n = al < bl ? al : bl;
  const int c = n ? memcmp(a, b, n) : 0;
  if (c != 0) {
    return c;
  }
  return al < bl ? -1 : al > bl ? 1 : 0;
}

static int cmp_tstr(const char * q, size_t ql, css_tstr s) {
  return cmp_bytes(q, ql, gtext_css_tbl_pool + s.off, s.len);
}

static char * dup_n(const GTEXT_Allocator * a, const char * s, size_t n) {
  char * p = gtext_allocator_malloc(a, n + 1);
  if (!p) {
    return NULL;
  }
  if (n) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

static void fill_result(GTEXT_CSS_Registry_Result * r, GTEXT_CSS_Status status, const char * name, size_t name_length) {
  if (!r) {
    return;
  }
  memset(r, 0, sizeof *r);
  r->status = status;
  r->message = gtext_css_status_string(status);
  r->detail = NULL;
  if (name) {
    const size_t n = name_length < sizeof r->name - 1 ? name_length : sizeof r->name - 1;
    memcpy(r->name, name, n);
    r->name[n] = '\0';
  }
}

static size_t clamp_limit(size_t v, size_t dflt, size_t maxv) {
  return v == 0 ? dflt : (v > maxv ? maxv : v);
}

/* ---- options ------------------------------------------------------------------------------ */

GTEXT_CSS_Registry_Options gtext_css_registry_options_default(void) {
  GTEXT_CSS_Registry_Options o;
  memset(&o, 0, sizeof o);
  o.max_registrations = REG_DEFAULT_REGISTRATIONS;
  o.max_name_bytes = REG_DEFAULT_NAME;
  o.max_initial_bytes = REG_DEFAULT_INITIAL;
  o.max_longhands = REG_DEFAULT_LONGHANDS;
  o.max_grammar_nodes = REG_DEFAULT_NODES;
  return o;
}

/* ---- the generated layer: lookups --------------------------------------------------------- */

size_t gtext_css_registry_generated_property_count(void) {
  return gtext_css_tbl_property_count;
}

/** The row (index) of the generated property with this lower-case name, or SIZE_MAX. `work`, if not NULL, counts the steps. */
static size_t gen_find(const char * lname, size_t length, size_t * work) {
  size_t lo = 0, hi = gtext_css_tbl_property_count;
  while (lo < hi) {
    if (work) {
      (*work)++;
    }
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_tstr(lname, length, gtext_css_tbl_properties[mid].name);
    if (c == 0) {
      return mid;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return (size_t)-1;
}

static uint64_t hash_name(const char * s, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ull;
  }
  return h;
}

/** The newest entry (index) of the caller's with this lower-case name, or SIZE_MAX. */
static size_t caller_find(const GTEXT_CSS_Registry * r, const char * lname, size_t length, size_t * work) {
  if (!r->hash_cap) {
    return (size_t)-1;
  }
  if (work) {
    *work += length;
  }
  size_t i = (size_t)(hash_name(lname, length) & (r->hash_cap - 1));
  for (;;) {
    if (work) {
      (*work)++;
    }
    const uint32_t slot = r->hash[i];
    if (!slot) {
      return (size_t)-1;
    }
    const cprop * c = &r->cprops[slot - 1];
    if (c->name_length == length && memcmp(c->name, lname, length) == 0) {
      return slot - 1;
    }
    i = (i + 1) & (r->hash_cap - 1);
  }
}

static bool hash_put(GTEXT_CSS_Registry * r, size_t index) {
  size_t * work = &r->work;
  /* grow at a load of one half; `index` is already in cprops */
  if ((r->ncprops + 1) * 2 > r->hash_cap) {
    const size_t cap = r->hash_cap ? r->hash_cap * 2 : 64;
    uint32_t * h = gtext_allocator_calloc(&r->alloc, cap, sizeof *h);
    if (!h) {
      return false;
    }
    /* re-insert the newest entry of each name only (an override replaced the slot of the older one) */
    for (size_t k = 0; k < r->ncprops; k++) {
      const cprop * c = &r->cprops[k];
      *work += c->name_length;
      size_t i = (size_t)(hash_name(c->name, c->name_length) & (cap - 1));
      for (;;) {
        (*work)++;
        if (!h[i]) {
          h[i] = (uint32_t)(k + 1);
          break;
        }
        const cprop * o = &r->cprops[h[i] - 1];
        if (o->name_length == c->name_length && memcmp(o->name, c->name, c->name_length) == 0) {
          h[i] = (uint32_t)(k + 1); /* later wins */
          break;
        }
        i = (i + 1) & (cap - 1);
      }
    }
    gtext_allocator_free(&r->alloc, r->hash);
    r->hash = h;
    r->hash_cap = cap;
  }
  const cprop * c = &r->cprops[index];
  *work += c->name_length;
  size_t i = (size_t)(hash_name(c->name, c->name_length) & (r->hash_cap - 1));
  for (;;) {
    (*work)++;
    if (!r->hash[i]) {
      r->hash[i] = (uint32_t)(index + 1);
      return true;
    }
    const cprop * o = &r->cprops[r->hash[i] - 1];
    if (o->name_length == c->name_length && memcmp(o->name, c->name, c->name_length) == 0) {
      r->hash[i] = (uint32_t)(index + 1);
      return true;
    }
    i = (i + 1) & (r->hash_cap - 1);
  }
}

static GTEXT_CSS_Property_Id find_lower_w(const GTEXT_CSS_Registry * r, const char * lname, size_t length, size_t * work) {
  if (length == 0) {
    return GTEXT_CSS_PROPERTY_NONE;
  }
  const size_t c = caller_find(r, lname, length, work);
  if (c != (size_t)-1) {
    return (GTEXT_CSS_Property_Id)(gtext_css_tbl_property_count + 1 + c);
  }
  size_t g = gen_find(lname, length, work);
  if (g == (size_t)-1 && length > 2 && lname[0] == '-' && lname[1] == '-') {
    g = gen_find("--*", 3, work); /* a custom property is the pattern of the pin */
  }
  return g == (size_t)-1 ? GTEXT_CSS_PROPERTY_NONE : (GTEXT_CSS_Property_Id)(g + 1);
}

static GTEXT_CSS_Property_Id find_lower(const GTEXT_CSS_Registry * r, const char * lname, size_t length) {
  return find_lower_w(r, lname, length, NULL);
}

GTEXT_CSS_Property_Id gtext_css_registry_find_property(const GTEXT_CSS_Registry * registry, const char * name, size_t length) {
  if (!registry || !name || length == 0 || length > REG_NAME_STACK) {
    return GTEXT_CSS_PROPERTY_NONE;
  }
  if (length > 2 && name[0] == '-' && name[1] == '-') {
    /* custom properties are case-sensitive: the name is never folded */
    return find_lower(registry, name, length) ;
  }
  char buf[REG_NAME_STACK];
  lower_copy(buf, name, length);
  return find_lower(registry, buf, length);
}

/** The property that now has the name of `id`: an override registered after `id` was made hides it, and a relation made by id (a shorthand's
 *  longhands, an alias's target, a logical group) must lead to what a lookup of the name finds. */
static GTEXT_CSS_Property_Id current_id(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id) {
  if (!r->noverrides || !id) {
    return id;
  }
  const char * name;
  size_t length;
  if ((size_t)id <= gtext_css_tbl_property_count) {
    name = css_tstr_c(gtext_css_tbl_properties[id - 1].name);
    length = gtext_css_tbl_properties[id - 1].name.len;
  }
  else {
    name = r->cprops[id - gtext_css_tbl_property_count - 1].name;
    length = r->cprops[id - gtext_css_tbl_property_count - 1].name_length;
  }
  const size_t c = caller_find(r, name, length, NULL);
  const GTEXT_CSS_Property_Id newest = c == (size_t)-1 ? id : (GTEXT_CSS_Property_Id)(gtext_css_tbl_property_count + 1 + c);
  return newest > id ? newest : id;
}

static bool id_valid(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id) {
  return id >= 1 && (size_t)id <= gtext_css_tbl_property_count + r->ncprops;
}

GTEXT_CSS_Property_Id gtext_css_registry_resolve_alias(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id) {
  if (!registry || !id_valid(registry, id)) {
    return GTEXT_CSS_PROPERTY_NONE;
  }
  for (size_t hops = 0; hops < 64; hops++) {
    GTEXT_CSS_Property_Id next;
    if ((size_t)id <= gtext_css_tbl_property_count) {
      next = current_id(registry, (GTEXT_CSS_Property_Id)gtext_css_tbl_properties[id - 1].alias_of);
    }
    else {
      next = current_id(registry, registry->cprops[id - gtext_css_tbl_property_count - 1].alias_of);
    }
    if (!next) {
      return id;
    }
    id = next;
  }
  return id; /* the generator and the registration both refuse a cycle; this keeps a defect from looping */
}

/* ---- property info ------------------------------------------------------------------------ */

bool gtext_css_registry_property_info(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id, GTEXT_CSS_Property_Info * info) {
  if (!r || !info || !id_valid(r, id)) {
    return false;
  }
  memset(info, 0, sizeof *info);
  info->id = id;
  if ((size_t)id <= gtext_css_tbl_property_count) {
    const css_tbl_property * p = &gtext_css_tbl_properties[id - 1];
    info->name = css_tstr_c(p->name);
    info->name_length = p->name.len;
    info->grammar = css_tstr_c(p->grammar);
    info->initial = css_tstr_c(p->initial);
    info->inherited = (GTEXT_CSS_Inherit)p->inherited;
    info->inherited_raw = css_tstr_c(p->inherited_raw);
    info->animation = (GTEXT_CSS_Animation)p->animation;
    info->animation_raw = css_tstr_c(p->animation_raw);
    info->computed = GTEXT_CSS_COMPUTED_UNSPECIFIED;
    info->computed_value = css_tstr_c(p->computed_value);
    info->applies_to = css_tstr_c(p->applies_to);
    info->percentages = css_tstr_c(p->percentages);
    info->canonical_order = css_tstr_c(p->canonical_order);
    info->logical_group = p->group ? css_tstr_c(gtext_css_tbl_groups[p->group - 1].name) : NULL;
    info->source = css_tstr_c(p->source);
    info->alias_of = current_id(r, (GTEXT_CSS_Property_Id)p->alias_of);
    info->longhand_count = p->longhand_count;
    info->reset_longhand_count = p->reset_count;
    info->style_name_count = p->style_count;
    info->flags = p->flags | GTEXT_CSS_PROP_GENERATED;
    return true;
  }
  const size_t ci = id - gtext_css_tbl_property_count - 1;
  const cprop * c = &r->cprops[ci];
  info->name = c->name;
  info->name_length = c->name_length;
  info->grammar = c->grammar;
  info->initial = c->initial;
  info->inherited = c->inherited;
  info->animation = c->animation;
  info->computed = c->computed;
  info->alias_of = current_id(r, c->alias_of);
  info->longhand_count = c->longhand_count;
  info->flags = c->flags | (c->overridden ? GTEXT_CSS_PROP_OVERRIDE : 0u);
  return true;
}

GTEXT_CSS_Property_Id gtext_css_registry_longhand(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id, size_t index) {
  if (!r || !id_valid(r, id)) {
    return GTEXT_CSS_PROPERTY_NONE;
  }
  if ((size_t)id <= gtext_css_tbl_property_count) {
    const css_tbl_property * p = &gtext_css_tbl_properties[id - 1];
    return index < p->longhand_count ? current_id(r, (GTEXT_CSS_Property_Id)gtext_css_tbl_property_refs[p->longhand_first + index]) : 0;
  }
  const cprop * c = &r->cprops[id - gtext_css_tbl_property_count - 1];
  return index < c->longhand_count ? current_id(r, c->longhands[index]) : 0;
}

GTEXT_CSS_Property_Id gtext_css_registry_reset_longhand(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id, size_t index) {
  if (!r || !id_valid(r, id) || (size_t)id > gtext_css_tbl_property_count) {
    return GTEXT_CSS_PROPERTY_NONE;
  }
  const css_tbl_property * p = &gtext_css_tbl_properties[id - 1];
  return index < p->reset_count ? current_id(r, (GTEXT_CSS_Property_Id)gtext_css_tbl_property_refs[p->reset_first + index]) : 0;
}

const char * gtext_css_registry_style_name(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id, size_t index) {
  if (!r || !id_valid(r, id) || (size_t)id > gtext_css_tbl_property_count) {
    return NULL;
  }
  const css_tbl_property * p = &gtext_css_tbl_properties[id - 1];
  return index < p->style_count ? css_tstr_c(gtext_css_tbl_style_names[p->style_first + index]) : NULL;
}

static const css_tbl_group * group_find(const char * group, size_t length) {
  if (!group || length == 0 || length > REG_NAME_STACK) {
    return NULL;
  }
  size_t lo = 0, hi = gtext_css_tbl_group_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_tstr(group, length, gtext_css_tbl_groups[mid].name);
    if (c == 0) {
      return &gtext_css_tbl_groups[mid];
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return NULL;
}

size_t gtext_css_registry_logical_group_size(const char * group, size_t length) {
  const css_tbl_group * g = group_find(group, length);
  return g ? g->count : 0;
}

GTEXT_CSS_Property_Id gtext_css_registry_logical_group_member(const GTEXT_CSS_Registry * r, const char * group, size_t length, size_t index) {
  const css_tbl_group * g = group_find(group, length);
  return r && g && index < g->count ? current_id(r, (GTEXT_CSS_Property_Id)gtext_css_tbl_group_members[g->first + index]) : 0;
}

bool gtext_css_registry_shorthand_hook(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id,
    GTEXT_CSS_Shorthand_Hook * hook, void ** user) {
  if (!r || !id_valid(r, id) || (size_t)id <= gtext_css_tbl_property_count) {
    return false;
  }
  const cprop * c = &r->cprops[id - gtext_css_tbl_property_count - 1];
  if (!c->hook) {
    return false;
  }
  if (hook) {
    *hook = c->hook;
  }
  if (user) {
    *user = c->hook_user;
  }
  return true;
}

size_t gtext_css_registry_property_count(const GTEXT_CSS_Registry * r) {
  return r ? gtext_css_tbl_property_count + r->ncprops : 0;
}

size_t gtext_css_registry_override_count(const GTEXT_CSS_Registry * r) {
  return r ? r->noverrides : 0;
}

bool gtext_css_registry_override_at(const GTEXT_CSS_Registry * r, size_t index, GTEXT_CSS_Property_Id * id,
    GTEXT_CSS_Property_Id * overridden) {
  if (!r || index >= r->noverrides) {
    return false;
  }
  if (id) {
    *id = r->overrides[index].id;
  }
  if (overridden) {
    *overridden = r->overrides[index].overridden;
  }
  return true;
}

/* ---- the lazy grammar cache --------------------------------------------------------------- */

static void lazy_init(lazy_grammar * lz) {
  memset(lz, 0, sizeof *lz);
  gcu_atomic_int_init(&lz->state, GTEXT_CSS_GRAMMAR_NOT_COMPILED);
}

static void lazy_free(lazy_grammar * lz) {
  gtext_css_compiled_free(lz->compiled);
  gtext_css_definition_free(lz->def);
  lz->compiled = NULL;
  lz->def = NULL;
}

/** Forgets a compiled grammar and the report of its compile, keeping the parsed text of the definition. */
static void lazy_forget_compiled(lazy_grammar * lz) {
  gtext_css_compiled_free(lz->compiled);
  lz->compiled = NULL;
  memset(&lz->res, 0, sizeof lz->res);
  gcu_atomic_int_store(&lz->state, GTEXT_CSS_GRAMMAR_NOT_COMPILED);
}

/** Parse a definition once (under the lock). OK with `*out` NULL means the text is not a definition (`r->rf` says why). */
static GTEXT_CSS_Status lazy_def(GTEXT_CSS_Registry * r, lazy_grammar * lz, const char * text, size_t length,
    const GTEXT_CSS_Definition ** out) {
  *out = NULL;
  if (lz->def_state == 1) {
    *out = lz->def;
    return GTEXT_CSS_OK;
  }
  if (lz->def_state == 2) {
    r->rf.status = lz->def_status;
    r->rf.offset = lz->def_offset;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Grammar_Options go = r->gopt;
  go.allocator = &r->alloc;
  GTEXT_CSS_Grammar_Result res;
  GTEXT_CSS_Definition * d = NULL;
  const GTEXT_CSS_Status st = gtext_css_definition_parse(text, length, &go, &d, &res);
  if (st == GTEXT_CSS_OK) {
    lz->def = d;
    lz->def_state = 1;
    *out = d;
    return GTEXT_CSS_OK;
  }
  if (st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_INVALID) {
    return st;
  }
  lz->def_state = 2; /* not a definition: cached with the reason */
  lz->def_status = st;
  lz->def_offset = res.offset;
  r->rf.status = st;
  r->rf.offset = res.offset;
  return GTEXT_CSS_OK;
}

/** The grammar text of a property id (alias already resolved): NULL when it has none. */
static const char * prop_text(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id, size_t * length) {
  if ((size_t)id <= gtext_css_tbl_property_count) {
    const css_tbl_property * p = &gtext_css_tbl_properties[id - 1];
    *length = p->grammar.len;
    return css_tstr_c(p->grammar);
  }
  const cprop * c = &r->cprops[id - gtext_css_tbl_property_count - 1];
  *length = c->ctext ? c->ctext_length : c->grammar_length;
  return c->ctext ? c->ctext : c->grammar;
}

static lazy_grammar * prop_lazy(GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id) {
  if ((size_t)id <= gtext_css_tbl_property_count) {
    return &r->gprops[id - 1];
  }
  return &r->cprops[id - gtext_css_tbl_property_count - 1].lazy;
}

static void set_rf_name(GTEXT_CSS_Registry * r, const char * name, size_t n) {
  const size_t k = n < sizeof r->rf.name - 1 ? n : sizeof r->rf.name - 1;
  memcpy(r->rf.name, name, k);
  r->rf.name[k] = '\0';
}

/** The definition of a property for a `<'name'>` reference: parsed on first use. */
static const GTEXT_CSS_Definition * prop_def(GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id, const char * name, size_t n) {
  id = gtext_css_registry_resolve_alias(r, id);
  if (!id) {
    return NULL;
  }
  size_t length = 0;
  const char * text = prop_text(r, id, &length);
  if (!text) {
    return NULL;
  }
  const GTEXT_CSS_Definition * d = NULL;
  const GTEXT_CSS_Status st = lazy_def(r, prop_lazy(r, id), text, length, &d);
  if (st != GTEXT_CSS_OK) {
    r->rf.status = st;
    set_rf_name(r, name, n);
    return NULL;
  }
  if (!d) {
    set_rf_name(r, name, n);
  }
  return d;
}

/** The first row of the type table with this name, or SIZE_MAX. */
static size_t type_first(const char * name, size_t length) {
  size_t lo = 0, hi = gtext_css_tbl_type_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (cmp_tstr(name, length, gtext_css_tbl_types[mid].name) <= 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  if (lo < gtext_css_tbl_type_count && cmp_tstr(name, length, gtext_css_tbl_types[lo].name) == 0) {
    return lo;
  }
  return (size_t)-1;
}

static const GTEXT_CSS_Definition * type_def(GTEXT_CSS_Registry * r, size_t row, const char * name, size_t n) {
  const css_tbl_type * t = &gtext_css_tbl_types[row];
  if (!t->grammar.len || (t->flags & GTEXT_CSS_TYPE_NATIVE)) {
    return NULL;
  }
  const GTEXT_CSS_Definition * d = NULL;
  const GTEXT_CSS_Status st = lazy_def(r, &r->gtypes[row], css_tstr_c(t->grammar), t->grammar.len, &d);
  if (st != GTEXT_CSS_OK) {
    r->rf.status = st;
    set_rf_name(r, name, n);
    return NULL;
  }
  if (!d) {
    set_rf_name(r, name, n);
  }
  return d;
}

/**
 * The pin's side of the resolver, called under the lock: properties for `<'name'>`; for `<name>` the type nested in the property being compiled,
 * then the global type. With `others`, a type the pin nests in another property (see reg_resolve_top()).
 */
static const GTEXT_CSS_Definition * pin_resolve(GTEXT_CSS_Registry * r, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length,
    bool others) {
  if (!name || length == 0) {
    return NULL;
  }
  if (kind == GTEXT_CSS_REF_PROPERTY) {
    if (length > REG_NAME_STACK) {
      return NULL;
    }
    char buf[REG_NAME_STACK];
    lower_copy(buf, name, length);
    const GTEXT_CSS_Property_Id id = find_lower(r, buf, length);
    return id ? prop_def(r, id, name, length) : NULL;
  }
  const size_t first = type_first(name, length);
  if (first == (size_t)-1) {
    return NULL;
  }
  size_t global = (size_t)-1, other = (size_t)-1;
  bool other_ambiguous = false;
  for (size_t i = first; i < gtext_css_tbl_type_count && cmp_tstr(name, length, gtext_css_tbl_types[i].name) == 0; i++) {
    if (gtext_css_tbl_types[i].scope == 0) {
      global = i;
    }
    else if (r->scope && gtext_css_tbl_types[i].scope == r->scope) {
      return type_def(r, i, name, length);
    }
    else if (other == (size_t)-1) {
      other = i;
    }
    else if (gtext_css_tbl_types[i].grammar.off != gtext_css_tbl_types[other].grammar.off) {
      other_ambiguous = true; /* strings are pooled once, so equal offsets are equal texts */
    }
  }
  if (global != (size_t)-1) {
    return type_def(r, global, name, length);
  }
  return others && other != (size_t)-1 && !other_ambiguous ? type_def(r, other, name, length) : NULL;
}

/** The fallback the function types ask first: the pin's own properties and types, without the other properties' nested types. */
static const GTEXT_CSS_Definition * reg_resolve(void * user, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length) {
  return pin_resolve(user, kind, name, length, false);
}

/**
 * The resolver a link is given: the pin (the property's own nested types, then the global types), the function types of the library, and
 * last the types the pin nests in some other property. The pin defines some types inside one property and names them from others
 * (`fill-opacity` uses `<opacity-value>` of `opacity`, `backdrop-filter` the functions of `filter`); with no global type of the name,
 * a definition made in other properties is used when they all say the same thing. It comes after the library's own function types
 * because the nested `rect()` of `clip` is the CSS 2 shape and not the basic shape.
 */
static const GTEXT_CSS_Definition * reg_resolve_top(void * user, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length) {
  GTEXT_CSS_Registry * r = user;
  const GTEXT_CSS_Definition * d = gtext_css_function_types_resolve(r->ftypes, kind, name, length);
  if (r->rf.status != GTEXT_CSS_OK || gtext_css_function_types_status(r->ftypes) != GTEXT_CSS_OK) {
    /* a definition of the pin could not be made (memory, or its text): the answer is "none", and not the library's own function
     * type of the same name, which would make the result depend on where an allocation failed */
    return NULL;
  }
  if (d || kind != GTEXT_CSS_REF_TYPE) {
    return d;
  }
  return pin_resolve(r, kind, name, length, true);
}

/**
 * Compile the grammar of one thing under the lock. A final answer is cached in `lz` and published through its state;
 * an out-of-memory is not.
 */
static GTEXT_CSS_Status compile_locked(GTEXT_CSS_Registry * r, lazy_grammar * lz, const char * text, size_t length,
    GTEXT_CSS_Property_Id scope) {
  if (gcu_atomic_int_load(&lz->state) != GTEXT_CSS_GRAMMAR_NOT_COMPILED) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Grammar_Result res;
  memset(&res, 0, sizeof res);
  res.status = GTEXT_CSS_OK;
  res.message = gtext_css_status_string(GTEXT_CSS_OK);
  GTEXT_CSS_Grammar_State final_state = GTEXT_CSS_GRAMMAR_OK;
  GTEXT_CSS_Compiled * compiled = NULL;
  if (!text) {
    final_state = GTEXT_CSS_GRAMMAR_NONE;
  }
  else {
    r->scope = scope;
    r->rf.status = GTEXT_CSS_OK;
    r->rf.name[0] = '\0';
    r->rf.offset = 0;
    const GTEXT_CSS_Definition * d = NULL;
    GTEXT_CSS_Status st = lazy_def(r, lz, text, length, &d);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (!d) {
      /* the text itself is not a definition */
      memset(&res, 0, sizeof res);
      res.status = r->rf.status;
      res.message = gtext_css_status_string(r->rf.status);
      res.offset = r->rf.offset;
      final_state = GTEXT_CSS_GRAMMAR_FAILED;
    }
    else {
      GTEXT_CSS_Grammar_Options go = r->gopt;
      go.allocator = &r->alloc;
      st = gtext_css_definition_link(d, reg_resolve_top, r, &go, &compiled, &res);
      if (st == GTEXT_CSS_OK) {
        final_state = GTEXT_CSS_GRAMMAR_OK;
      }
      else if (st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_INVALID) {
        return GTEXT_CSS_E_OOM;
      }
      else if (st == GTEXT_CSS_E_REFERENCE) {
        const GTEXT_CSS_Status fs = gtext_css_function_types_status(r->ftypes);
        if (fs == GTEXT_CSS_E_OOM) {
          return GTEXT_CSS_E_OOM;
        }
        if (r->rf.status == GTEXT_CSS_E_OOM) {
          return GTEXT_CSS_E_OOM;
        }
        if (r->rf.status != GTEXT_CSS_OK || fs != GTEXT_CSS_OK) {
          /* a definition the link asked for could not be made: a defect or a limit of that text, not a missing name */
          const GTEXT_CSS_Status why = r->rf.status != GTEXT_CSS_OK ? r->rf.status : fs;
          res.status = why;
          res.message = gtext_css_status_string(why);
          if (r->rf.name[0]) {
            const size_t n = strlen(r->rf.name);
            memset(res.name, 0, sizeof res.name);
            memcpy(res.name, r->rf.name, n < sizeof res.name - 1 ? n : sizeof res.name - 1);
          }
          final_state = GTEXT_CSS_GRAMMAR_FAILED;
        }
        else {
          final_state = GTEXT_CSS_GRAMMAR_UNRESOLVED;
        }
      }
      else {
        final_state = GTEXT_CSS_GRAMMAR_FAILED;
      }
    }
  }
  lz->compiled = compiled;
  lz->res = res;
  gcu_atomic_int_store(&lz->state, (int32_t)final_state);
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status compile_lazy(const GTEXT_CSS_Registry * cr, lazy_grammar * lz, const char * text, size_t length,
    GTEXT_CSS_Property_Id scope, GTEXT_CSS_Grammar_State * state, const GTEXT_CSS_Compiled ** compiled,
    GTEXT_CSS_Grammar_Result * res) {
  GTEXT_CSS_Registry * r = (GTEXT_CSS_Registry *)cr; /* the cache is the registry's one mutable part */
  int32_t s = gcu_atomic_int_load(&lz->state);
  if (s == GTEXT_CSS_GRAMMAR_NOT_COMPILED) {
    GCU_MUTEX_LOCK(r->lock);
    const GTEXT_CSS_Status st = compile_locked(r, lz, text, length, scope);
    GCU_MUTEX_UNLOCK(r->lock);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    s = gcu_atomic_int_load(&lz->state);
  }
  *state = (GTEXT_CSS_Grammar_State)s;
  *compiled = lz->compiled;
  if (res) {
    *res = lz->res;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_registry_property_compiled(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id,
    GTEXT_CSS_Grammar_State * state, const GTEXT_CSS_Compiled ** compiled, GTEXT_CSS_Grammar_Result * res) {
  if (!r || !state || !compiled || !id_valid(r, id)) {
    return GTEXT_CSS_E_INVALID;
  }
  id = gtext_css_registry_resolve_alias(r, id);
  size_t length = 0;
  const char * text = prop_text(r, id, &length);
  return compile_lazy(r, prop_lazy((GTEXT_CSS_Registry *)r, id), text, length, id, state, compiled, res);
}

GTEXT_CSS_Status css_registry_descriptor_compiled(const GTEXT_CSS_Registry * r, GTEXT_CSS_Descriptor_Id id,
    GTEXT_CSS_Grammar_State * state, const GTEXT_CSS_Compiled ** compiled, GTEXT_CSS_Grammar_Result * res) {
  if (!r || !state || !compiled || id < 1 || (size_t)id > gtext_css_tbl_descriptor_count) {
    return GTEXT_CSS_E_INVALID;
  }
  const css_tbl_descriptor * d = &gtext_css_tbl_descriptors[id - 1];
  /* a nested at-rule (`@swash { <declaration-list> }`) is the at-rule's syntax, not a value: it has no grammar to match */
  const char * text = d->kind == GTEXT_CSS_DESC_AT_RULE ? NULL : css_tstr_c(d->grammar);
  return compile_lazy(r, &((GTEXT_CSS_Registry *)r)->gdescs[id - 1], text, d->grammar.len, 0, state, compiled, res);
}

GTEXT_CSS_Status css_registry_pending_grammar(const GTEXT_CSS_Registry * r, const GTEXT_CSS_Compiled ** compiled) {
  GTEXT_CSS_Grammar_State s;
  static const char text[] = "<declaration-value>?";
  const GTEXT_CSS_Status st = compile_lazy(r, &((GTEXT_CSS_Registry *)r)->pending, text, sizeof text - 1, 0, &s, compiled, NULL);
  if (st == GTEXT_CSS_OK && s != GTEXT_CSS_GRAMMAR_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  return st;
}

size_t css_registry_work(const GTEXT_CSS_Registry * r) {
  return r ? r->work : 0;
}

GTEXT_CSS_Property_Id css_registry_find_counted(const GTEXT_CSS_Registry * cr, const char * name, size_t length, size_t * work) {
  GTEXT_CSS_Registry * r = (GTEXT_CSS_Registry *)cr;
  if (!r || !name || length == 0 || length > REG_NAME_STACK) {
    return GTEXT_CSS_PROPERTY_NONE;
  }
  char buf[REG_NAME_STACK];
  lower_copy(buf, name, length);
  *work += length;
  return find_lower_w(r, buf, length, work);
}

bool css_registry_resolvable(const GTEXT_CSS_Registry * cr, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length,
    GTEXT_CSS_Property_Id scope) {
  GTEXT_CSS_Registry * r = (GTEXT_CSS_Registry *)cr;
  GCU_MUTEX_LOCK(r->lock);
  r->scope = scope;
  r->rf.status = GTEXT_CSS_OK;
  r->rf.name[0] = '\0';
  const bool found = reg_resolve_top(r, kind, name, length) != NULL;
  GCU_MUTEX_UNLOCK(r->lock);
  return found;
}

const GTEXT_Allocator * css_registry_allocator(const GTEXT_CSS_Registry * r) {
  return &r->alloc;
}

const GTEXT_CSS_Match_Options * css_registry_match_options(const GTEXT_CSS_Registry * r) {
  return &r->mopt;
}

GTEXT_CSS_Status gtext_css_registry_property_state(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id id,
    GTEXT_CSS_Grammar_State * state, GTEXT_CSS_Grammar_Result * grammar_result) {
  const GTEXT_CSS_Compiled * c;
  GTEXT_CSS_Grammar_State s;
  const GTEXT_CSS_Status st = css_registry_property_compiled(r, id, &s, &c, grammar_result);
  if (st == GTEXT_CSS_OK && state) {
    *state = s;
  }
  return st;
}

/* ---- creation, destruction ---------------------------------------------------------------- */

static void reg_destroy(GTEXT_CSS_Registry * r) {
  if (!r) {
    return;
  }
  GTEXT_Allocator a = r->alloc;
  if (r->gprops) {
    for (size_t i = 0; i < gtext_css_tbl_property_count; i++) {
      lazy_free(&r->gprops[i]);
    }
  }
  if (r->gtypes) {
    for (size_t i = 0; i < gtext_css_tbl_type_count; i++) {
      lazy_free(&r->gtypes[i]);
    }
  }
  if (r->gdescs) {
    for (size_t i = 0; i < gtext_css_tbl_descriptor_count; i++) {
      lazy_free(&r->gdescs[i]);
    }
  }
  lazy_free(&r->pending);
  css_creg_store_free(&r->cregs);
  for (size_t i = 0; i < r->ncprops; i++) {
    cprop * c = &r->cprops[i];
    lazy_free(&c->lazy);
    gtext_allocator_free(&a, c->name);
    gtext_allocator_free(&a, c->grammar);
    gtext_allocator_free(&a, c->ctext);
    gtext_allocator_free(&a, c->initial);
    gtext_allocator_free(&a, c->longhands);
  }
  gtext_css_function_types_free(r->ftypes);
  gtext_allocator_free(&a, r->gprops);
  gtext_allocator_free(&a, r->gtypes);
  gtext_allocator_free(&a, r->gdescs);
  gtext_allocator_free(&a, r->cprops);
  gtext_allocator_free(&a, r->hash);
  gtext_allocator_free(&a, r->overrides);
  if (r->lock_made) {
    GCU_MUTEX_DESTROY(r->lock);
  }
  gtext_allocator_free(&a, r);
}

GTEXT_CSS_Status gtext_css_registry_builder_new(const GTEXT_CSS_Registry_Options * options, GTEXT_CSS_Registry_Builder ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * ap = options && options->allocator ? options->allocator : gtext_allocator_default();
  GTEXT_Allocator a = *ap;
  GTEXT_CSS_Registry_Builder * b = gtext_allocator_calloc(&a, 1, sizeof *b);
  GTEXT_CSS_Registry * r = gtext_allocator_calloc(&a, 1, sizeof *r);
  if (!b || !r) {
    gtext_allocator_free(&a, b);
    gtext_allocator_free(&a, r);
    return GTEXT_CSS_E_OOM;
  }
  r->alloc = a;
  r->gopt = options && options->grammar ? *options->grammar : gtext_css_grammar_options_default();
  r->gopt.allocator = NULL;
  r->mopt = options && options->match ? *options->match : gtext_css_match_options_default();
  r->mopt.allocator = NULL;
  const GTEXT_CSS_Registry_Options d = gtext_css_registry_options_default();
  r->max_registrations = clamp_limit(options ? options->max_registrations : 0, d.max_registrations, REG_MAX_REGISTRATIONS);
  r->max_name = clamp_limit(options ? options->max_name_bytes : 0, d.max_name_bytes, REG_NAME_STACK);
  r->max_initial = clamp_limit(options ? options->max_initial_bytes : 0, d.max_initial_bytes, REG_MAX_INITIAL);
  r->max_longhands = clamp_limit(options ? options->max_longhands : 0, d.max_longhands, REG_MAX_LONGHANDS);
  r->max_nodes = clamp_limit(options ? options->max_grammar_nodes : 0, d.max_grammar_nodes, REG_MAX_NODES);
  if (GCU_MUTEX_CREATE(r->lock) != 0) {
    gtext_allocator_free(&a, b);
    gtext_allocator_free(&a, r);
    return GTEXT_CSS_E_INVALID;
  }
  r->lock_made = true;
  r->gprops = gtext_allocator_calloc(&a, gtext_css_tbl_property_count, sizeof *r->gprops);
  r->gtypes = gtext_allocator_calloc(&a, gtext_css_tbl_type_count, sizeof *r->gtypes);
  r->gdescs = gtext_allocator_calloc(&a, gtext_css_tbl_descriptor_count, sizeof *r->gdescs);
  if (!r->gprops || !r->gtypes || !r->gdescs) {
    reg_destroy(r);
    gtext_allocator_free(&a, b);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < gtext_css_tbl_property_count; i++) {
    lazy_init(&r->gprops[i]);
  }
  for (size_t i = 0; i < gtext_css_tbl_type_count; i++) {
    lazy_init(&r->gtypes[i]);
  }
  for (size_t i = 0; i < gtext_css_tbl_descriptor_count; i++) {
    lazy_init(&r->gdescs[i]);
  }
  lazy_init(&r->pending);
  css_creg_store_init(&r->cregs, &r->alloc);
  GTEXT_CSS_Function_Types_Options fo;
  memset(&fo, 0, sizeof fo);
  fo.allocator = &r->alloc;
  fo.fallback = reg_resolve;
  fo.fallback_user = r;
  fo.grammar = &r->gopt;
  if (gtext_css_function_types_new(&fo, &r->ftypes) != GTEXT_CSS_OK) {
    reg_destroy(r);
    gtext_allocator_free(&a, b);
    return GTEXT_CSS_E_OOM;
  }
  b->reg = r;
  *out = b;
  return GTEXT_CSS_OK;
}

void gtext_css_registry_builder_free(GTEXT_CSS_Registry_Builder * b) {
  if (!b) {
    return;
  }
  GTEXT_Allocator a = b->reg->alloc;
  reg_destroy(b->reg);
  gtext_allocator_free(&a, b);
}

void gtext_css_registry_free(GTEXT_CSS_Registry * r) {
  reg_destroy(r);
}

GTEXT_CSS_Status gtext_css_registry_new(const GTEXT_CSS_Registry_Options * options, GTEXT_CSS_Registry ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Registry_Builder * b = NULL;
  GTEXT_CSS_Status st = gtext_css_registry_builder_new(options, &b);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = gtext_css_registry_freeze(b, out, NULL);
  if (st != GTEXT_CSS_OK) {
    gtext_css_registry_builder_free(b);
  }
  return st;
}

/* ---- registration ------------------------------------------------------------------------- */

static int id_compare(const void * a, const void * b) {
  const GTEXT_CSS_Property_Id x = *(const GTEXT_CSS_Property_Id *)a, y = *(const GTEXT_CSS_Property_Id *)b;
  return x < y ? -1 : x > y;
}

static bool name_ok(const char * name, size_t n) {
  if (n == 0) {
    return false;
  }
  if (n >= 2 && name[0] == '-' && name[1] == '-') {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)name[i];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!(alpha || c == '_' || c == '-' || (digit && i > 0))) {
      return false;
    }
  }
  if (name[0] == '-' && n == 1) {
    return false;
  }
  if (name[0] == '-' && n > 1 && (name[1] >= '0' && name[1] <= '9')) {
    return false; /* `-1x` is not an identifier */
  }
  return true;
}

/** Parse and link a grammar against the registry as it stands, keeping nothing (the validation of a registration). */
/** What a registration refused for the registry-wide node budget says (the status is GTEXT_CSS_E_LIMIT_REGISTRY, as for the other limits). */
static const char kNodesDetail[] = "the compiled grammars of the registered properties together are past max_grammar_nodes";

static GTEXT_CSS_Status validate_grammar(GTEXT_CSS_Registry * r, const char * text, size_t length, GTEXT_CSS_Registry_Result * out,
    GTEXT_CSS_Compiled ** compiled) {
  GTEXT_CSS_Grammar_Options go = r->gopt;
  go.allocator = &r->alloc;
  GTEXT_CSS_Definition * d = NULL;
  GTEXT_CSS_Grammar_Result gr;
  GTEXT_CSS_Status st = gtext_css_definition_parse(text, length, &go, &d, &gr);
  if (st == GTEXT_CSS_OK) {
    GCU_MUTEX_LOCK(r->lock);
    r->scope = 0;
    r->rf.status = GTEXT_CSS_OK;
    r->rf.name[0] = '\0';
    st = gtext_css_definition_link(d, reg_resolve_top, r, &go, compiled, &gr);
    r->work += gr.work + gr.nodes + length; /* the link's own work: what it walked, the nodes it made, the text it read */
    if (st == GTEXT_CSS_OK) {
      /* A compiled grammar carries the grammars it names, so a chain of properties that each name the one before holds the square of its
       * length in nodes. The registry holds them all after the freeze, and this is what bounds that. */
      if (gr.nodes > r->max_nodes - (r->grammar_nodes < r->max_nodes ? r->grammar_nodes : r->max_nodes)) {
        gtext_css_compiled_free(*compiled);
        *compiled = NULL;
        st = GTEXT_CSS_E_LIMIT_REGISTRY;
        gr.status = st;
        gr.message = gtext_css_status_string(st);
      }
      else {
        r->last_nodes = gr.nodes;
      }
    }
    if (st == GTEXT_CSS_E_REFERENCE) {
      const GTEXT_CSS_Status fs = gtext_css_function_types_status(r->ftypes);
      const GTEXT_CSS_Status why = r->rf.status != GTEXT_CSS_OK ? r->rf.status : fs;
      if (why != GTEXT_CSS_OK) {
        st = why;
        gr.status = why;
        gr.message = gtext_css_status_string(why);
      }
    }
    GCU_MUTEX_UNLOCK(r->lock);
    gtext_css_definition_free(d);
  }
  if (out) {
    out->grammar = gr;
    out->status = st;
    out->message = gtext_css_status_string(st);
  }
  return st;
}

/** Match the initial value text against a compiled grammar: OK only if it is a concrete value of it. */
static GTEXT_CSS_Status validate_initial(GTEXT_CSS_Registry * r, const GTEXT_CSS_Compiled * compiled, const char * text, size_t length,
    const char ** detail) {
  *detail = "the initial value is not a list of component values";
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &r->alloc;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text, length, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status result = GTEXT_CSS_OK;
  bool sub = false;
  st = gtext_css_has_arbitrary_substitution(tree->values, tree->value_count, &r->alloc, &sub);
  if (st != GTEXT_CSS_OK) {
    result = st;
  }
  else if (sub || tree->value_count == 0) {
    result = GTEXT_CSS_E_INVALID;
    *detail = sub ? "the initial value has a substitution function: it must be a concrete value" : "the initial value is empty";
  }
  else {
    /* a CSS-wide keyword is not a value of a property */
    size_t n = 0;
    const GTEXT_CSS_Component_Value * only = NULL;
    for (size_t i = 0; i < tree->value_count; i++) {
      if (tree->values[i].type != GTEXT_CSS_TOKEN_WHITESPACE) {
        n++;
        only = &tree->values[i];
      }
    }
    static const char * const wide[] = {"initial", "inherit", "unset", "revert", "revert-layer", "revert-rule"};
    if (n == 1 && only->type == GTEXT_CSS_TOKEN_IDENT) {
      for (size_t k = 0; k < sizeof wide / sizeof wide[0]; k++) {
        const size_t wl = strlen(wide[k]);
        if (only->value.length == wl) {
          char lc[16];
          lower_copy(lc, only->value.data, wl);
          if (memcmp(lc, wide[k], wl) == 0) {
            result = GTEXT_CSS_E_INVALID;
            *detail = "the initial value is a CSS-wide keyword";
          }
        }
      }
    }
    if (result == GTEXT_CSS_OK) {
      GTEXT_CSS_Match_Options mo = r->mopt;
      mo.allocator = &r->alloc;
      bool matched = false;
      st = gtext_css_match(compiled, tree->values, tree->value_count, &mo, &matched, NULL, NULL);
      result = st != GTEXT_CSS_OK ? st : (matched ? GTEXT_CSS_OK : GTEXT_CSS_E_INVALID);
      *detail = "the initial value does not match the grammar";
    }
  }
  gtext_css_tree_free(tree);
  return result;
}

/**
 * An override's grammar with its references to its own name (`<'width'>`) replaced by `[ the definition it replaces ]`, so that an
 * override can extend what it overrides and the grammar of any property that names the overridden one by name (`margin` names
 * `margin-top`) links the same way. Written once, at registration, from the definition as it stands: the one replaced, whether the
 * pin's or an earlier registration's (which has been expanded itself). `*out` is NULL when nothing was replaced.
 */
static GTEXT_CSS_Status expand_self(GTEXT_CSS_Registry * r, const char * lname, size_t nlen, const char * grammar, size_t glen,
    GTEXT_CSS_Property_Id old, char ** out, size_t * out_length) {
  *out = NULL;
  *out_length = 0;
  old = gtext_css_registry_resolve_alias(r, old);
  size_t olen = 0;
  const char * otext = old ? prop_text(r, old, &olen) : NULL;
  if (!otext) {
    return GTEXT_CSS_OK;
  }
  size_t cap = glen + 64, len = 0;
  char * buf = gtext_allocator_malloc(&r->alloc, cap);
  if (!buf) {
    return GTEXT_CSS_E_OOM;
  }
  size_t replaced = 0;
  for (size_t i = 0; i < glen;) {
    size_t take = 1;
    const char * piece = grammar + i;
    size_t piece_len = 1;
    size_t extra = 0;
    if (grammar[i] == '<' && i + 1 < glen && grammar[i + 1] == '\'') {
      size_t j = i + 2;
      while (j < glen && grammar[j] != '\'') {
        j++;
      }
      size_t k = j + 1;
      while (k < glen && (grammar[k] == ' ' || grammar[k] == '\t' || grammar[k] == '\n')) {
        k++;
      }
      if (j < glen && k < glen && grammar[k] == '>' && j - (i + 2) == nlen) {
        char tmp[REG_NAME_STACK];
        if (nlen <= sizeof tmp) {
          lower_copy(tmp, grammar + i + 2, nlen);
          if (memcmp(tmp, lname, nlen) == 0) {
            take = k + 1 - i;
            piece = otext;
            piece_len = olen;
            extra = 4; /* "[ " and " ]" */
            replaced++;
          }
        }
      }
    }
    if (len + piece_len + extra + 1 > cap) {
      cap = (len + piece_len + extra + 1) * 2;
      char * g = gtext_allocator_realloc(&r->alloc, buf, cap);
      if (!g) {
        gtext_allocator_free(&r->alloc, buf);
        return GTEXT_CSS_E_OOM;
      }
      buf = g;
    }
    if (extra) {
      memcpy(buf + len, "[ ", 2);
      len += 2;
    }
    memcpy(buf + len, piece, piece_len);
    len += piece_len;
    if (extra) {
      memcpy(buf + len, " ]", 2);
      len += 2;
    }
    r->work += piece_len;
    i += take;
  }
  if (!replaced) {
    gtext_allocator_free(&r->alloc, buf);
    return GTEXT_CSS_OK;
  }
  buf[len] = '\0';
  *out = buf;
  *out_length = len;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status reg_prepare_slot(GTEXT_CSS_Registry * r, const char * name, size_t name_length, bool override,
    GTEXT_CSS_Registry_Result * result, char ** lname_out, GTEXT_CSS_Property_Id * existing) {
  *lname_out = NULL;
  *existing = GTEXT_CSS_PROPERTY_NONE;
  r->last_nodes = 0;
  if (name_length > r->max_name) {
    fill_result(result, GTEXT_CSS_E_LIMIT_REGISTRY, name, name_length);
    return GTEXT_CSS_E_LIMIT_REGISTRY;
  }
  if (!name_ok(name, name_length)) {
    fill_result(result, GTEXT_CSS_E_INVALID, name, name_length);
    if (result) {
      result->detail = "the name is not a property name: letters, digits, `-` and `_`, not beginning with a digit or `--`";
    }
    return GTEXT_CSS_E_INVALID;
  }
  if (gtext_css_wide_keyword_lookup(name, name_length) != GTEXT_CSS_WIDE_NONE) {
    fill_result(result, GTEXT_CSS_E_INVALID, name, name_length);
    if (result) {
      result->detail = "the name is a CSS-wide keyword, which no property can be called";
    }
    return GTEXT_CSS_E_INVALID;
  }
  if (r->ncprops >= r->max_registrations) {
    fill_result(result, GTEXT_CSS_E_LIMIT_REGISTRY, name, name_length);
    return GTEXT_CSS_E_LIMIT_REGISTRY;
  }
  char * lname = dup_n(&r->alloc, name, name_length);
  if (!lname) {
    fill_result(result, GTEXT_CSS_E_OOM, name, name_length);
    return GTEXT_CSS_E_OOM;
  }
  lower_copy(lname, name, name_length);
  r->work += name_length;
  const GTEXT_CSS_Property_Id found = find_lower_w(r, lname, name_length, &r->work);
  if (found && !override) {
    gtext_allocator_free(&r->alloc, lname);
    fill_result(result, GTEXT_CSS_E_EXISTS, name, name_length);
    if (result) {
      result->id = found;
    }
    return GTEXT_CSS_E_EXISTS;
  }
  *existing = found;
  *lname_out = lname;
  return GTEXT_CSS_OK;
}

/** Add an entry (taking ownership of its strings) and index it; on failure frees them. */
static GTEXT_CSS_Status reg_add(GTEXT_CSS_Registry * r, cprop * entry, GTEXT_CSS_Property_Id existing, GTEXT_CSS_Registry_Result * result) {
  if (r->ncprops == r->capcprops) {
    const size_t cap = r->capcprops ? r->capcprops * 2 : 16;
    cprop * n = gtext_allocator_realloc(&r->alloc, r->cprops, cap * sizeof *n);
    if (!n) {
      goto oom;
    }
    r->cprops = n;
    r->capcprops = cap;
  }
  if (existing && r->noverrides == r->capoverrides) {
    const size_t cap = r->capoverrides ? r->capoverrides * 2 : 8;
    override_rec * n = gtext_allocator_realloc(&r->alloc, r->overrides, cap * sizeof *n);
    if (!n) {
      goto oom;
    }
    r->overrides = n;
    r->capoverrides = cap;
  }
  r->cprops[r->ncprops] = *entry;
  r->cprops[r->ncprops].overridden = existing;
  r->grammar_nodes += r->last_nodes;
  r->last_nodes = 0;
  r->work += entry->name_length + entry->grammar_length + 1;
  gcu_atomic_int_init(&r->cprops[r->ncprops].lazy.state, GTEXT_CSS_GRAMMAR_NOT_COMPILED);
  if (!hash_put(r, r->ncprops)) {
    goto oom;
  }
  const GTEXT_CSS_Property_Id id = (GTEXT_CSS_Property_Id)(gtext_css_tbl_property_count + 1 + r->ncprops);
  r->ncprops++;
  if (existing) {
    r->overrides[r->noverrides].id = id;
    r->overrides[r->noverrides].overridden = existing;
    r->noverrides++;
  }
  fill_result(result, GTEXT_CSS_OK, entry->name, entry->name_length);
  if (result) {
    result->id = id;
  }
  return GTEXT_CSS_OK;
oom:
  fill_result(result, GTEXT_CSS_E_OOM, entry->name, entry->name_length);
  gtext_allocator_free(&r->alloc, entry->name);
  gtext_allocator_free(&r->alloc, entry->grammar);
  gtext_allocator_free(&r->alloc, entry->ctext);
  gtext_allocator_free(&r->alloc, entry->initial);
  gtext_allocator_free(&r->alloc, entry->longhands);
  return GTEXT_CSS_E_OOM;
}

GTEXT_CSS_Status gtext_css_registry_register_property(GTEXT_CSS_Registry_Builder * builder, const GTEXT_CSS_Property_Def * def,
    GTEXT_CSS_Registry_Result * result) {
  if (!builder || !def || !def->name || !def->grammar || !def->initial) {
    fill_result(result, GTEXT_CSS_E_INVALID, NULL, 0);
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Registry * r = builder->reg;
  const size_t nl = strlen(def->name), gl = strlen(def->grammar), il = strlen(def->initial);
  char * lname;
  GTEXT_CSS_Property_Id existing;
  GTEXT_CSS_Status st = reg_prepare_slot(r, def->name, nl, def->override, result, &lname, &existing);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (il > r->max_initial) {
    st = GTEXT_CSS_E_LIMIT_REGISTRY;
    fill_result(result, st, def->name, nl);
    goto fail;
  }
  GTEXT_CSS_Compiled * compiled = NULL;
  char * ctext = NULL;
  size_t clen = 0;
  if (existing) {
    st = expand_self(r, lname, nl, def->grammar, gl, existing, &ctext, &clen);
    if (st != GTEXT_CSS_OK) {
      fill_result(result, st, def->name, nl);
      goto fail;
    }
  }
  st = validate_grammar(r, ctext ? ctext : def->grammar, ctext ? clen : gl, result, &compiled);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&r->alloc, ctext);
    if (result) {
      const GTEXT_CSS_Grammar_Result g = result->grammar;
      fill_result(result, st, def->name, nl);
      result->grammar = g;
      if (st == GTEXT_CSS_E_LIMIT_REGISTRY) {
        result->detail = kNodesDetail;
      }
      if (g.name[0]) {
        const size_t n = strlen(g.name);
        memset(result->name, 0, sizeof result->name);
        memcpy(result->name, g.name, n < sizeof result->name - 1 ? n : sizeof result->name - 1);
      }
    }
    goto fail;
  }
  const char * detail = NULL;
  st = validate_initial(r, compiled, def->initial, il, &detail);
  gtext_css_compiled_free(compiled);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&r->alloc, ctext);
    fill_result(result, st, def->name, nl);
    if (result && st == GTEXT_CSS_E_INVALID) {
      result->detail = detail;
    }
    goto fail;
  }
  cprop e;
  memset(&e, 0, sizeof e);
  e.name = lname;
  e.name_length = nl;
  e.grammar = dup_n(&r->alloc, def->grammar, gl);
  e.grammar_length = gl;
  e.ctext = ctext;
  e.ctext_length = clen;
  e.initial = dup_n(&r->alloc, def->initial, il);
  e.flags = def->quirk_unitless_length ? GTEXT_CSS_PROP_QUIRK_UNITLESS : 0;
  e.inherited = def->inherited ? GTEXT_CSS_INHERIT_YES : GTEXT_CSS_INHERIT_NO;
  e.animation = def->animation;
  e.computed = def->computed;
  if (!e.grammar || !e.initial) {
    gtext_allocator_free(&r->alloc, e.grammar);
    gtext_allocator_free(&r->alloc, e.ctext);
    gtext_allocator_free(&r->alloc, e.initial);
    gtext_allocator_free(&r->alloc, lname);
    fill_result(result, GTEXT_CSS_E_OOM, def->name, nl);
    return GTEXT_CSS_E_OOM;
  }
  return reg_add(r, &e, existing, result);
fail:
  gtext_allocator_free(&r->alloc, lname);
  return st;
}

GTEXT_CSS_Status gtext_css_registry_register_alias(GTEXT_CSS_Registry_Builder * builder, const char * alias, const char * target,
    bool override, GTEXT_CSS_Registry_Result * result) {
  if (!builder || !alias || !target) {
    fill_result(result, GTEXT_CSS_E_INVALID, NULL, 0);
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Registry * r = builder->reg;
  const size_t nl = strlen(alias), tl = strlen(target);
  char * lname;
  GTEXT_CSS_Property_Id existing;
  GTEXT_CSS_Status st = reg_prepare_slot(r, alias, nl, override, result, &lname, &existing);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_CSS_Property_Id t = gtext_css_registry_resolve_alias(r, gtext_css_registry_find_property(r, target, tl));
  GTEXT_CSS_Property_Info ti;
  const bool custom = t && gtext_css_registry_property_info(r, t, &ti) && (ti.flags & GTEXT_CSS_PROP_CUSTOM);
  if (!t || (existing && t == existing) || custom) {
    gtext_allocator_free(&r->alloc, lname);
    st = t ? GTEXT_CSS_E_INVALID : GTEXT_CSS_E_REFERENCE;
    fill_result(result, st, target, tl);
    if (result && st == GTEXT_CSS_E_INVALID) {
      result->detail = custom ? "an alias cannot name a custom property: custom properties are looked up by their own names"
                              : "a name cannot be made an alias of the property it replaces";
    }
    return st;
  }
  cprop e;
  memset(&e, 0, sizeof e);
  e.name = lname;
  e.name_length = nl;
  e.flags = GTEXT_CSS_PROP_ALIAS;
  e.alias_of = t;
  return reg_add(r, &e, existing, result);
}

GTEXT_CSS_Status gtext_css_registry_register_shorthand(GTEXT_CSS_Registry_Builder * builder, const GTEXT_CSS_Shorthand_Def * def,
    GTEXT_CSS_Registry_Result * result) {
  if (!builder || !def || !def->name || !def->grammar || !def->longhands || def->longhand_count == 0) {
    fill_result(result, GTEXT_CSS_E_INVALID, NULL, 0);
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Registry * r = builder->reg;
  const size_t nl = strlen(def->name), gl = strlen(def->grammar);
  if (def->longhand_count > r->max_longhands) {
    fill_result(result, GTEXT_CSS_E_LIMIT_REGISTRY, def->name, nl);
    return GTEXT_CSS_E_LIMIT_REGISTRY;
  }
  char * lname;
  GTEXT_CSS_Property_Id existing;
  GTEXT_CSS_Status st = reg_prepare_slot(r, def->name, nl, def->override, result, &lname, &existing);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Id * lh = gtext_allocator_malloc(&r->alloc, def->longhand_count * sizeof *lh);
  if (!lh) {
    gtext_allocator_free(&r->alloc, lname);
    fill_result(result, GTEXT_CSS_E_OOM, def->name, nl);
    return GTEXT_CSS_E_OOM;
  }
  size_t yes = 0, no = 0;
  for (size_t i = 0; i < def->longhand_count; i++) {
    const char * ln = def->longhands[i];
    const size_t ll = ln ? strlen(ln) : 0;
    GTEXT_CSS_Property_Id id = ln ? gtext_css_registry_resolve_alias(r, gtext_css_registry_find_property(r, ln, ll)) : 0;
    if (!id) {
      gtext_allocator_free(&r->alloc, lh);
      gtext_allocator_free(&r->alloc, lname);
      st = ln ? GTEXT_CSS_E_REFERENCE : GTEXT_CSS_E_INVALID;
      fill_result(result, st, ln, ll);
      return st;
    }
    if (existing && id == existing) {
      gtext_allocator_free(&r->alloc, lh);
      gtext_allocator_free(&r->alloc, lname);
      fill_result(result, GTEXT_CSS_E_INVALID, ln, ll);
      if (result) {
        result->detail = "a shorthand cannot name the property it replaces as a longhand";
      }
      return GTEXT_CSS_E_INVALID;
    }
    lh[i] = id;
    GTEXT_CSS_Property_Info info;
    (void)gtext_css_registry_property_info(r, id, &info);
    if (info.flags & GTEXT_CSS_PROP_CUSTOM) {
      gtext_allocator_free(&r->alloc, lh);
      gtext_allocator_free(&r->alloc, lname);
      fill_result(result, GTEXT_CSS_E_INVALID, ln, ll);
      if (result) {
        result->detail = "a custom property cannot be a longhand: shorthands expand into the properties of the registry";
      }
      return GTEXT_CSS_E_INVALID;
    }
    yes += info.inherited == GTEXT_CSS_INHERIT_YES;
    no += info.inherited == GTEXT_CSS_INHERIT_NO;
  }
  /* the same longhand twice is a mistake the expansion could not repair: found on a sorted copy, so that a long list costs n log n */
  {
    GTEXT_CSS_Property_Id * sorted = gtext_allocator_malloc(&r->alloc, def->longhand_count * sizeof *sorted);
    if (!sorted) {
      gtext_allocator_free(&r->alloc, lh);
      gtext_allocator_free(&r->alloc, lname);
      fill_result(result, GTEXT_CSS_E_OOM, def->name, nl);
      return GTEXT_CSS_E_OOM;
    }
    memcpy(sorted, lh, def->longhand_count * sizeof *sorted);
    qsort(sorted, def->longhand_count, sizeof *sorted, id_compare);
    GTEXT_CSS_Property_Id twice = 0;
    for (size_t i = 1; i < def->longhand_count && !twice; i++) {
      if (sorted[i] == sorted[i - 1]) {
        twice = sorted[i];
      }
    }
    gtext_allocator_free(&r->alloc, sorted);
    if (twice) {
      GTEXT_CSS_Property_Info ti;
      (void)gtext_css_registry_property_info(r, twice, &ti);
      gtext_allocator_free(&r->alloc, lh);
      gtext_allocator_free(&r->alloc, lname);
      fill_result(result, GTEXT_CSS_E_INVALID, ti.name, ti.name_length);
      if (result) {
        result->detail = "a longhand is named twice";
      }
      return GTEXT_CSS_E_INVALID;
    }
  }
  GTEXT_CSS_Compiled * compiled = NULL;
  char * ctext = NULL;
  size_t clen = 0;
  if (existing) {
    st = expand_self(r, lname, nl, def->grammar, gl, existing, &ctext, &clen);
  }
  if (st == GTEXT_CSS_OK) {
    st = validate_grammar(r, ctext ? ctext : def->grammar, ctext ? clen : gl, result, &compiled);
  }
  gtext_css_compiled_free(compiled);
  if (st != GTEXT_CSS_OK) {
    const GTEXT_CSS_Grammar_Result g = result ? result->grammar : (GTEXT_CSS_Grammar_Result){0};
    fill_result(result, st, def->name, nl);
    if (result) {
      result->grammar = g;
      if (st == GTEXT_CSS_E_LIMIT_REGISTRY) {
        result->detail = kNodesDetail;
      }
    }
    gtext_allocator_free(&r->alloc, ctext);
    gtext_allocator_free(&r->alloc, lh);
    gtext_allocator_free(&r->alloc, lname);
    return st;
  }
  cprop e;
  memset(&e, 0, sizeof e);
  e.name = lname;
  e.name_length = nl;
  e.grammar = dup_n(&r->alloc, def->grammar, gl);
  e.grammar_length = gl;
  e.ctext = ctext;
  e.ctext_length = clen;
  e.flags = GTEXT_CSS_PROP_SHORTHAND;
  e.inherited = yes == def->longhand_count ? GTEXT_CSS_INHERIT_YES : no == def->longhand_count ? GTEXT_CSS_INHERIT_NO : GTEXT_CSS_INHERIT_MIXED;
  e.animation = GTEXT_CSS_ANIM_SEE_LONGHANDS;
  e.longhands = lh;
  e.longhand_count = def->longhand_count;
  e.hook = def->hook;
  e.hook_user = def->hook_user;
  if (!e.grammar) {
    gtext_allocator_free(&r->alloc, ctext);
    gtext_allocator_free(&r->alloc, lh);
    gtext_allocator_free(&r->alloc, lname);
    fill_result(result, GTEXT_CSS_E_OOM, def->name, nl);
    return GTEXT_CSS_E_OOM;
  }
  return reg_add(r, &e, existing, result);
}

/* ---- freeze and prepare ------------------------------------------------------------------- */

/** A freeze that fails leaves the builder as a builder: what it compiled was compiled against the registrations so far, and a later
 * registration (an override of a name that a compiled grammar names) would make it stale, so every compiled grammar is forgotten,
 * and the work the attempt was charged is given back. The parsed definitions of the pin are not changed by any registration and stay. */
static void freeze_undo(GTEXT_CSS_Registry * r, size_t work_before) {
  for (size_t i = 0; i < gtext_css_tbl_property_count; i++) {
    lazy_forget_compiled(&r->gprops[i]);
  }
  for (size_t i = 0; i < gtext_css_tbl_type_count; i++) {
    lazy_forget_compiled(&r->gtypes[i]);
  }
  for (size_t i = 0; i < gtext_css_tbl_descriptor_count; i++) {
    lazy_forget_compiled(&r->gdescs[i]);
  }
  lazy_forget_compiled(&r->pending);
  for (size_t i = 0; i < r->ncprops; i++) {
    lazy_forget_compiled(&r->cprops[i].lazy);
  }
  r->work = work_before;
}

static GTEXT_CSS_Status freeze_run(GTEXT_CSS_Registry_Builder * builder, GTEXT_CSS_Registry ** out, GTEXT_CSS_Registry_Result * result);


/* ---- custom property registrations --------------------------------------------------------------------- */


static GTEXT_CSS_Status creg_fail_why(GTEXT_CSS_Registry_Result * result, GTEXT_CSS_Status st, const char * name, size_t nl, const char * detail, css_creg_why * why,
    css_creg_why w) {
  if (why) {
    *why = st == GTEXT_CSS_E_INVALID ? w : CSS_CREG_OTHER;
  }
  fill_result(result, st, name, nl);
  if (result) {
    result->detail = detail;
  }
  return st;
}

#define creg_fail(result, st, name, nl, detail) creg_fail_why(result, st, name, nl, detail, why, CSS_CREG_OTHER)

/// Whether a name is one of the CSS-wide keywords (`initial`, `inherit`, `unset`, `revert`, `revert-layer`, `revert-rule`), ASCII case-insensitively.
static bool is_css_wide_keyword(const GTEXT_CSS_String * s) {
  static const char * const names[] = {"initial", "inherit", "unset", "revert", "revert-layer", "revert-rule"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (css_ascii_ieq(s, names[i])) {
      return true;
    }
  }
  return false;
}

/// Whether the text, tokenized as written, holds a bad string or a bad url token anywhere.
static GTEXT_CSS_Status text_has_bad_token(const GTEXT_Allocator * alloc, const char * text, size_t length, bool * bad) {
  *bad = false;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = alloc;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text, length, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_OK; /* a limit of the parser is the caller's to meet when it parses the value */
  }
  GTEXT_Allocator a = *alloc;
  size_t cap = 16, top = 0;
  struct frame {
    const GTEXT_CSS_Component_Value * v;
    size_t n;
  } * stack = gtext_allocator_malloc(&a, cap * sizeof *stack);
  if (!stack) {
    gtext_css_tree_free(tree);
    return GTEXT_CSS_E_OOM;
  }
  stack[top++] = (struct frame){tree->values, tree->value_count};
  while (top > 0 && !*bad) {
    struct frame f = stack[--top];
    for (size_t i = 0; i < f.n; i++) {
      const GTEXT_CSS_Component_Value * v = &f.v[i];
      if (v->kind == GTEXT_CSS_VALUE_TOKEN && (v->type == GTEXT_CSS_TOKEN_BAD_STRING || v->type == GTEXT_CSS_TOKEN_BAD_URL)) {
        *bad = true;
        break;
      }
      if (v->child_count) {
        if (top == cap) {
          cap *= 2;
          struct frame * g = gtext_allocator_realloc(&a, stack, cap * sizeof *stack);
          if (!g) {
            gtext_allocator_free(&a, stack);
            gtext_css_tree_free(tree);
            return GTEXT_CSS_E_OOM;
          }
          stack = g;
        }
        stack[top++] = (struct frame){v->children, v->child_count};
      }
    }
  }
  gtext_allocator_free(&a, stack);
  gtext_css_tree_free(tree);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_registry_make_creg(const GTEXT_CSS_Registry * cr, const char * name, size_t name_length, const char * syntax, size_t syntax_length,
    bool inherits, const char * initial, size_t initial_length, css_creg ** out, GTEXT_CSS_Registry_Result * result, css_creg_why * why) {
  GTEXT_CSS_Registry * r = (GTEXT_CSS_Registry *)cr;
  *out = NULL;
  if (why) {
    *why = CSS_CREG_OK;
  }
  if (!css_creg_name_ok(name, name_length)) {
    return creg_fail_why(result, GTEXT_CSS_E_INVALID, name, name_length, "the name of a custom property begins with two dashes and has something after them", why, CSS_CREG_BAD_NAME);
  }
  if (name_length > r->max_name) {
    return creg_fail(result, GTEXT_CSS_E_LIMIT_REGISTRY, name, name_length, NULL);
  }
  if (initial && initial_length > r->max_initial) {
    return creg_fail(result, GTEXT_CSS_E_LIMIT_REGISTRY, name, name_length, NULL);
  }
  if (!syntax) {
    syntax = "*";
    syntax_length = 1;
  }
  css_creg * c = gtext_allocator_calloc(&r->alloc, 1, sizeof *c);
  if (!c) {
    return creg_fail(result, GTEXT_CSS_E_OOM, name, name_length, NULL);
  }
  c->alloc = r->alloc;
  c->inherits = inherits;
  c->name = dup_n(&r->alloc, name, name_length);
  c->name_len = name_length;
  c->syntax_text = dup_n(&r->alloc, syntax, syntax_length);
  c->syntax_len = syntax_length;
  if (!c->name || !c->syntax_text) {
    css_creg_free(c);
    return creg_fail(result, GTEXT_CSS_E_OOM, name, name_length, NULL);
  }
  GTEXT_CSS_Status st = css_syntax_parse(&r->alloc, syntax, syntax_length, &c->syntax, NULL);
  if (st == GTEXT_CSS_E_GRAMMAR) {
    css_creg_free(c);
    return creg_fail_why(result, GTEXT_CSS_E_INVALID, name, name_length, "the syntax is not a syntax string (Properties and Values API 1, 5.4)", why, CSS_CREG_BAD_SYNTAX);
  }
  if (st != GTEXT_CSS_OK) {
    css_creg_free(c);
    return creg_fail(result, st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_E_INVALID, name, name_length, NULL);
  }
  const bool universal = c->syntax->universal;
  if (!universal) {
    c->compiled = gtext_allocator_calloc(&r->alloc, c->syntax->ncomps, sizeof *c->compiled);
    if (!c->compiled) {
      css_creg_free(c);
      return creg_fail(result, GTEXT_CSS_E_OOM, name, name_length, NULL);
    }
    for (size_t i = 0; i < c->syntax->ncomps; i++) {
      char * g = NULL;
      size_t gl = 0;
      st = css_syntax_component_grammar(c->syntax, i, &g, &gl);
      if (st != GTEXT_CSS_OK) {
        css_creg_free(c);
        return creg_fail(result, st, name, name_length, NULL);
      }
      if (!g) {
        continue;
      }
      GTEXT_CSS_Registry_Result vr;
      memset(&vr, 0, sizeof vr);
      st = validate_grammar(r, g, gl, &vr, &c->compiled[i]);
      r->last_nodes = 0;
      gtext_allocator_free(&r->alloc, g);
      if (st != GTEXT_CSS_OK) {
        css_creg_free(c);
        fill_result(result, st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_LIMIT_REGISTRY ? st : GTEXT_CSS_E_INTERNAL, name, name_length);
        if (result) {
          result->grammar = vr.grammar;
          result->detail = "a component of the syntax could not be compiled";
        }
        return result ? result->status : st;
      }
    }
  }
  if (!initial) {
    if (!universal) {
      css_creg_free(c);
      return creg_fail_why(result, GTEXT_CSS_E_INVALID, name, name_length, "a syntax other than * needs an initial value", why, CSS_CREG_NO_INITIAL);
    }
  }
  else {
    const char * it = initial;
    size_t il = initial_length;
    /* "the initial value is parsed as written": a string that a line feed ends is a bad string, and cutting the white space off first would close it */
    bool bad_token = false;
    st = text_has_bad_token(&r->alloc, initial, initial_length, &bad_token);
    if (st != GTEXT_CSS_OK || bad_token) {
      css_creg_free(c);
      return creg_fail_why(result, st == GTEXT_CSS_OK ? GTEXT_CSS_E_INVALID : st, name, name_length,
          "the initial value has a bad string or a bad url in it, which no <declaration-value> holds", why, CSS_CREG_BAD_INITIAL);
    }
    css_trim_value_text(&it, &il);
    c->has_initial = true;
    c->initial = dup_n(&r->alloc, it, il);
    c->initial_len = il;
    if (!c->initial) {
      css_creg_free(c);
      return creg_fail(result, GTEXT_CSS_E_OOM, name, name_length, NULL);
    }
    GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
    po.allocator = &r->alloc;
    GTEXT_CSS_Tree * tree = NULL;
    st = gtext_css_parse_component_values(it, il, &po, &tree, NULL);
    if (st != GTEXT_CSS_OK) {
      css_creg_free(c);
      return creg_fail_why(result, st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_E_INVALID, name, name_length, "the initial value is not a list of component values", why, CSS_CREG_BAD_INITIAL);
    }
    bool sub = false;
    st = gtext_css_has_arbitrary_substitution(tree->values, tree->value_count, &r->alloc, &sub);
    const char * detail = NULL;
    css_creg_why w = CSS_CREG_BAD_INITIAL;
    bool good = false;
    GTEXT_CSS_Value_Set * typed = NULL;
    if (st == GTEXT_CSS_OK && tree->value_count == 1 && tree->values[0].kind == GTEXT_CSS_VALUE_TOKEN && tree->values[0].type == GTEXT_CSS_TOKEN_IDENT &&
        is_css_wide_keyword(&tree->values[0].value)) {
      /* a CSS-wide keyword, written as a value, is explicit defaulting and not a value that could be the initial one */
      detail = "the initial value is a CSS-wide keyword, which is not a value";
    }
    else if (st == GTEXT_CSS_OK && sub) {
      detail = "the initial value has a substitution function: it has to be computationally independent";
      w = CSS_CREG_SUBSTITUTION;
    }
    else if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Match_Options mo = r->mopt;
      mo.allocator = &r->alloc;
      if (universal) {
        const GTEXT_CSS_Compiled * pending = NULL;
        st = css_registry_pending_grammar(r, &pending);
        if (st == GTEXT_CSS_OK) {
          st = gtext_css_match(pending, tree->values, tree->value_count, &mo, &good, NULL, NULL);
        }
        if (st == GTEXT_CSS_OK && !good) {
          detail = "the initial value is not a valid token stream";
        }
      }
      else {
        css_syn_match m;
        st = css_syntax_match(c->syntax, c->compiled, tree->values, tree->value_count, &mo, &good, &m);
        if (st == GTEXT_CSS_OK && !good) {
          detail = "the initial value does not match the syntax";
        }
        else if (st == GTEXT_CSS_OK) {
          typed = m.set;
          bool independent = true;
          st = css_creg_independent(&r->alloc, tree->values, tree->value_count, typed, &independent);
          if (st == GTEXT_CSS_OK && !independent) {
            good = false;
            detail = "the initial value is not computationally independent: it needs the element (em, currentcolor ...)";
            w = CSS_CREG_NOT_INDEPENDENT;
          }
        }
      }
    }
    gtext_css_value_set_free(typed);
    gtext_css_tree_free(tree);
    if (st != GTEXT_CSS_OK || !good) {
      css_creg_free(c);
      return creg_fail_why(result, st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_INVALID, name, name_length, detail, why, w);
    }
  }
  *out = c;
  return GTEXT_CSS_OK;
}


GTEXT_CSS_Status css_registry_compile_syntax(const GTEXT_CSS_Registry * cr, const char * text, size_t length, css_compiled_syntax ** out) {
  GTEXT_CSS_Registry * r = (GTEXT_CSS_Registry *)cr;
  *out = NULL;
  css_compiled_syntax * c = gtext_allocator_calloc(&r->alloc, 1, sizeof *c);
  if (!c) {
    return GTEXT_CSS_E_OOM;
  }
  c->alloc = r->alloc;
  c->text = dup_n(&r->alloc, text, length);
  c->len = length;
  if (!c->text) {
    css_compiled_syntax_free(c);
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status st = css_syntax_parse(&r->alloc, text, length, &c->syntax, NULL);
  if (st != GTEXT_CSS_OK) {
    css_compiled_syntax_free(c);
    return st;
  }
  if (!c->syntax->universal) {
    c->compiled = gtext_allocator_calloc(&r->alloc, c->syntax->ncomps, sizeof *c->compiled);
    if (!c->compiled) {
      css_compiled_syntax_free(c);
      return GTEXT_CSS_E_OOM;
    }
    for (size_t i = 0; i < c->syntax->ncomps; i++) {
      char * g = NULL;
      size_t gl = 0;
      st = css_syntax_component_grammar(c->syntax, i, &g, &gl);
      if (st != GTEXT_CSS_OK) {
        css_compiled_syntax_free(c);
        return st;
      }
      if (!g) {
        continue;
      }
      st = validate_grammar(r, g, gl, NULL, &c->compiled[i]);
      r->last_nodes = 0;
      gtext_allocator_free(&r->alloc, g);
      if (st != GTEXT_CSS_OK) {
        css_compiled_syntax_free(c);
        return st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_E_INTERNAL;
      }
    }
  }
  *out = c;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_registry_register_custom(GTEXT_CSS_Registry_Builder * builder, const GTEXT_CSS_Custom_Def * def, GTEXT_CSS_Registry_Result * result) {
  css_creg_why * const why = NULL;
  if (!builder || !def || !def->name) {
    fill_result(result, GTEXT_CSS_E_INVALID, NULL, 0);
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Registry * r = builder->reg;
  const size_t nl = strlen(def->name);
  if (r->cregs.count >= r->max_registrations) {
    return creg_fail(result, GTEXT_CSS_E_LIMIT_REGISTRY, def->name, nl, NULL);
  }
  if (css_creg_store_find(&r->cregs, def->name, nl, &r->work)) {
    return creg_fail(result, GTEXT_CSS_E_EXISTS, def->name, nl, "a custom property of this name is registered already");
  }
  css_creg * c = NULL;
  GTEXT_CSS_Status st = css_registry_make_creg(r, def->name, nl, def->syntax, def->syntax ? strlen(def->syntax) : 0, def->inherits, def->initial_value,
      def->initial_value ? strlen(def->initial_value) : 0, &c, result, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = css_creg_store_add(&r->cregs, c);
  if (st != GTEXT_CSS_OK) {
    css_creg_free(c);
    return creg_fail(result, st, def->name, nl, NULL);
  }
  r->work += nl;
  fill_result(result, GTEXT_CSS_OK, def->name, nl);
  return GTEXT_CSS_OK;
}

const css_creg * css_registry_find_creg(const GTEXT_CSS_Registry * r, const char * name, size_t length, size_t * work) {
  return r ? css_creg_store_find(&r->cregs, name, length, work) : NULL;
}

const css_creg * css_registry_creg_at(const GTEXT_CSS_Registry * r, size_t index) {
  return r && index < r->cregs.count ? r->cregs.items[index] : NULL;
}

static void creg_info(const css_creg * c, GTEXT_CSS_Custom_Info * info) {
  info->name = c->name;
  info->name_length = c->name_len;
  info->syntax = c->syntax_text;
  info->syntax_length = c->syntax_len;
  info->universal = c->syntax->universal;
  info->inherits = c->inherits;
  info->has_initial = c->has_initial;
  info->initial_value = c->has_initial ? c->initial : NULL;
  info->initial_length = c->has_initial ? c->initial_len : 0;
  info->component_count = c->syntax->universal ? 0 : c->syntax->ncomps;
}

size_t gtext_css_registry_custom_count(const GTEXT_CSS_Registry * r) {
  return r ? r->cregs.count : 0;
}

bool gtext_css_registry_custom_at(const GTEXT_CSS_Registry * r, size_t index, GTEXT_CSS_Custom_Info * info) {
  const css_creg * c = css_registry_creg_at(r, index);
  if (!c || !info) {
    return false;
  }
  creg_info(c, info);
  return true;
}

bool gtext_css_registry_find_custom(const GTEXT_CSS_Registry * r, const char * name, size_t length, GTEXT_CSS_Custom_Info * info) {
  const css_creg * c = name ? css_registry_find_creg(r, name, length, NULL) : NULL;
  if (!c || !info) {
    return false;
  }
  creg_info(c, info);
  return true;
}

GTEXT_CSS_Status gtext_css_registry_freeze(GTEXT_CSS_Registry_Builder * builder, GTEXT_CSS_Registry ** out,
    GTEXT_CSS_Registry_Result * result) {
  if (!builder || !out) {
    fill_result(result, GTEXT_CSS_E_INVALID, NULL, 0);
    return GTEXT_CSS_E_INVALID;
  }
  const size_t work_before = builder->reg->work;
  const GTEXT_CSS_Status st = freeze_run(builder, out, result);
  if (st != GTEXT_CSS_OK) {
    freeze_undo(builder->reg, work_before);
  }
  return st;
}

static GTEXT_CSS_Status freeze_run(GTEXT_CSS_Registry_Builder * builder, GTEXT_CSS_Registry ** out, GTEXT_CSS_Registry_Result * result) {
  GTEXT_CSS_Registry * r = builder->reg;
  /* every registered grammar is compiled now: a redefinition made after a grammar that names it must not leave that grammar
   * unable to link unnoticed */
  for (size_t i = 0; i < r->ncprops; i++) {
    cprop * c = &r->cprops[i];
    if (!c->grammar) {
      continue;
    }
    GTEXT_CSS_Grammar_State s;
    const GTEXT_CSS_Compiled * comp;
    GTEXT_CSS_Grammar_Result gr;
    const GTEXT_CSS_Status st = compile_lazy(r, &c->lazy, c->ctext ? c->ctext : c->grammar, c->ctext ? c->ctext_length : c->grammar_length, (GTEXT_CSS_Property_Id)(gtext_css_tbl_property_count + 1 + i), &s, &comp, &gr);
    r->work += gr.work + gr.nodes + c->grammar_length;
    if (st != GTEXT_CSS_OK) {
      fill_result(result, st, c->name, c->name_length);
      return st;
    }
    if (s != GTEXT_CSS_GRAMMAR_OK) {
      const GTEXT_CSS_Status fs = s == GTEXT_CSS_GRAMMAR_UNRESOLVED ? GTEXT_CSS_E_REFERENCE : (gr.status != GTEXT_CSS_OK ? gr.status : GTEXT_CSS_E_GRAMMAR);
      fill_result(result, fs, c->name, c->name_length);
      if (result) {
        result->detail = c->overridden ? "the override no longer links with the properties of the registry that name it"
                                       : "a registered grammar no longer links with the properties registered after it";
        result->grammar = gr;
        result->id = (GTEXT_CSS_Property_Id)(gtext_css_tbl_property_count + 1 + i);
      }
      return fs;
    }
  }
  if (r->noverrides) {
    /* an override can change what a property of the pin links to (a grammar that names the overridden one): every such property is
     * compiled now, and one that no longer links is reported by name, with the override that is to blame. A gap of the pin itself
     * (a missing type, a text that is no definition) is not this. */
    for (size_t i = 1; i <= gtext_css_tbl_property_count; i++) {
      GTEXT_CSS_Grammar_State s;
      const GTEXT_CSS_Compiled * comp;
      GTEXT_CSS_Grammar_Result gr;
      const GTEXT_CSS_Status st = css_registry_property_compiled(r, (GTEXT_CSS_Property_Id)i, &s, &comp, &gr);
      if (st != GTEXT_CSS_OK) {
        fill_result(result, st, NULL, 0);
        return st;
      }
      if (s == GTEXT_CSS_GRAMMAR_FAILED && gr.status != GTEXT_CSS_E_GRAMMAR && gr.status != GTEXT_CSS_OK) {
        const css_tbl_property * p = &gtext_css_tbl_properties[i - 1];
        fill_result(result, gr.status, css_tstr_c(p->name), p->name.len);
        if (result) {
          result->grammar = gr;
          result->detail = "a property of the registry no longer links because of an override";
          result->id = r->overrides[0].id;
          for (size_t k = 0; k < r->noverrides; k++) {
            const cprop * oc = &r->cprops[r->overrides[k].id - gtext_css_tbl_property_count - 1];
            char needle[REG_NAME_STACK + 8];
            if (oc->name_length <= REG_NAME_STACK && p->grammar.len) {
              needle[0] = '<';
              needle[1] = '\'';
              memcpy(needle + 2, oc->name, oc->name_length);
              memcpy(needle + 2 + oc->name_length, "'>", 3);
              if (strstr(css_tstr_c(p->grammar), needle)) {
                result->id = r->overrides[k].id;
                break;
              }
            }
          }
        }
        return gr.status;
      }
    }
  }
  *out = r;
  gtext_allocator_free(&r->alloc, builder);
  fill_result(result, GTEXT_CSS_OK, NULL, 0);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_registry_prepare(GTEXT_CSS_Registry * registry) {
  if (!registry) {
    return GTEXT_CSS_E_INVALID;
  }
  const size_t total = gtext_css_registry_property_count(registry);
  for (size_t i = 1; i <= total; i++) {
    GTEXT_CSS_Grammar_State s;
    const GTEXT_CSS_Compiled * c;
    GTEXT_CSS_Status st = css_registry_property_compiled(registry, (GTEXT_CSS_Property_Id)i, &s, &c, NULL);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  for (size_t i = 1; i <= gtext_css_tbl_descriptor_count; i++) {
    GTEXT_CSS_Grammar_State s;
    const GTEXT_CSS_Compiled * c;
    const GTEXT_CSS_Status st = css_registry_descriptor_compiled(registry, (GTEXT_CSS_Descriptor_Id)i, &s, &c, NULL);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  const GTEXT_CSS_Compiled * p;
  GTEXT_CSS_Status st = css_registry_pending_grammar(registry, &p);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GCU_MUTEX_LOCK(registry->lock);
  st = gtext_css_function_types_prepare(registry->ftypes);
  GCU_MUTEX_UNLOCK(registry->lock);
  return st;
}

/* ---- the generated layer: at-rules, descriptors, types, selectors, keywords --------------- */

size_t gtext_css_registry_at_rule_count(void) {
  return gtext_css_tbl_at_rule_count;
}

size_t gtext_css_registry_descriptor_count(void) {
  return gtext_css_tbl_descriptor_count;
}

GTEXT_CSS_At_Rule_Id gtext_css_registry_find_at_rule(const char * name, size_t length) {
  if (!name || length == 0 || length > REG_NAME_STACK) {
    return 0;
  }
  char buf[REG_NAME_STACK];
  lower_copy(buf, name, length);
  size_t lo = 0, hi = gtext_css_tbl_at_rule_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_tstr(buf, length, gtext_css_tbl_at_rules[mid].name);
    if (c == 0) {
      return (GTEXT_CSS_At_Rule_Id)(mid + 1);
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return 0;
}

bool gtext_css_registry_at_rule_info(GTEXT_CSS_At_Rule_Id id, GTEXT_CSS_At_Rule_Info * info) {
  if (!info || id < 1 || (size_t)id > gtext_css_tbl_at_rule_count) {
    return false;
  }
  const css_tbl_at_rule * a = &gtext_css_tbl_at_rules[id - 1];
  memset(info, 0, sizeof *info);
  info->id = id;
  info->name = css_tstr_c(a->name);
  info->name_length = a->name.len;
  info->grammar = css_tstr_c(a->grammar);
  info->descriptor_count = a->count;
  info->declarations = a->declarations != 0;
  /* CSS Page 3: an @page rule holds declarations of page descriptors and of properties; CSS Mixins: the body of an @function holds
   * custom properties (its local variables) next to its `result` descriptor. No other at-rule of the pin is both. */
  info->properties = info->name_length == 5 && memcmp(info->name, "@page", 5) == 0;
  info->properties = info->properties || (info->name_length == 9 && memcmp(info->name, "@function", 9) == 0);
  return true;
}

GTEXT_CSS_Descriptor_Id gtext_css_registry_descriptor_at(GTEXT_CSS_At_Rule_Id at_rule, size_t index) {
  if (at_rule < 1 || (size_t)at_rule > gtext_css_tbl_at_rule_count) {
    return 0;
  }
  const css_tbl_at_rule * a = &gtext_css_tbl_at_rules[at_rule - 1];
  return index < a->count ? (GTEXT_CSS_Descriptor_Id)(a->first + index + 1) : 0;
}

GTEXT_CSS_Descriptor_Id gtext_css_registry_find_descriptor(GTEXT_CSS_At_Rule_Id at_rule, const char * name, size_t length) {
  if (at_rule < 1 || (size_t)at_rule > gtext_css_tbl_at_rule_count || !name || length == 0 || length > REG_NAME_STACK) {
    return 0;
  }
  char buf[REG_NAME_STACK];
  lower_copy(buf, name, length);
  const css_tbl_at_rule * a = &gtext_css_tbl_at_rules[at_rule - 1];
  size_t lo = a->first, hi = a->first + a->count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_tstr(buf, length, gtext_css_tbl_descriptors[mid].name);
    if (c == 0) {
      return (GTEXT_CSS_Descriptor_Id)(mid + 1);
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return 0;
}

bool gtext_css_registry_descriptor_info(GTEXT_CSS_Descriptor_Id id, GTEXT_CSS_Descriptor_Info * info) {
  if (!info || id < 1 || (size_t)id > gtext_css_tbl_descriptor_count) {
    return false;
  }
  const css_tbl_descriptor * d = &gtext_css_tbl_descriptors[id - 1];
  memset(info, 0, sizeof *info);
  info->id = id;
  info->at_rule = (GTEXT_CSS_At_Rule_Id)d->at_rule;
  info->name = css_tstr_c(d->name);
  info->name_length = d->name.len;
  info->grammar = css_tstr_c(d->grammar);
  info->initial = css_tstr_c(d->initial);
  info->kind = (GTEXT_CSS_Descriptor_Kind)d->kind;
  return true;
}

size_t gtext_css_registry_type_count(void) {
  return gtext_css_tbl_type_count;
}

static void type_fill(const css_tbl_type * t, GTEXT_CSS_Type_Info * info) {
  memset(info, 0, sizeof *info);
  info->name = css_tstr_c(t->name);
  info->name_length = t->name.len;
  info->grammar = css_tstr_c(t->grammar);
  info->scope = (GTEXT_CSS_Property_Id)t->scope;
  info->source = css_tstr_c(t->source);
  info->flags = t->flags;
}

bool gtext_css_registry_type_at(size_t index, GTEXT_CSS_Type_Info * info) {
  if (!info || index >= gtext_css_tbl_type_count) {
    return false;
  }
  type_fill(&gtext_css_tbl_types[index], info);
  return true;
}

bool gtext_css_registry_find_type(const char * name, size_t length, GTEXT_CSS_Property_Id scope, GTEXT_CSS_Type_Info * info) {
  if (!name || length == 0) {
    return false;
  }
  const size_t first = type_first(name, length);
  if (first == (size_t)-1) {
    return false;
  }
  size_t global = (size_t)-1;
  for (size_t i = first; i < gtext_css_tbl_type_count && cmp_tstr(name, length, gtext_css_tbl_types[i].name) == 0; i++) {
    if (gtext_css_tbl_types[i].scope == 0) {
      global = i;
    }
    else if (scope && gtext_css_tbl_types[i].scope == scope) {
      if (info) {
        type_fill(&gtext_css_tbl_types[i], info);
      }
      return true;
    }
  }
  if (global == (size_t)-1) {
    return false;
  }
  if (info) {
    type_fill(&gtext_css_tbl_types[global], info);
  }
  return true;
}

GTEXT_CSS_Status gtext_css_registry_type_state(const GTEXT_CSS_Registry * r, size_t index, GTEXT_CSS_Grammar_State * state,
    GTEXT_CSS_Grammar_Result * grammar_result) {
  if (!r || !state || index >= gtext_css_tbl_type_count) {
    return GTEXT_CSS_E_INVALID;
  }
  const css_tbl_type * t = &gtext_css_tbl_types[index];
  const GTEXT_CSS_Compiled * compiled;
  const char * text = (t->flags & GTEXT_CSS_TYPE_NATIVE) ? NULL : css_tstr_c(t->grammar);
  return compile_lazy(r, &((GTEXT_CSS_Registry *)r)->gtypes[index], text, t->grammar.len, (GTEXT_CSS_Property_Id)t->scope, state,
      &compiled, grammar_result);
}

GTEXT_CSS_Status gtext_css_registry_descriptor_state(const GTEXT_CSS_Registry * r, GTEXT_CSS_Descriptor_Id id,
    GTEXT_CSS_Grammar_State * state, GTEXT_CSS_Grammar_Result * grammar_result) {
  const GTEXT_CSS_Compiled * c;
  GTEXT_CSS_Grammar_State s;
  const GTEXT_CSS_Status st = css_registry_descriptor_compiled(r, id, &s, &c, grammar_result);
  if (st == GTEXT_CSS_OK && state) {
    *state = s;
  }
  return st;
}

size_t gtext_css_registry_selector_count(void) {
  return gtext_css_tbl_selector_count;
}

static void selector_fill(const css_tbl_selector * s, GTEXT_CSS_Selector_Info * info) {
  memset(info, 0, sizeof *info);
  info->name = css_tstr_c(s->name);
  info->name_length = s->name.len;
  info->syntax = css_tstr_c(s->syntax);
  info->source = css_tstr_c(s->source);
}

bool gtext_css_registry_selector_at(size_t index, GTEXT_CSS_Selector_Info * info) {
  if (!info || index >= gtext_css_tbl_selector_count) {
    return false;
  }
  selector_fill(&gtext_css_tbl_selectors[index], info);
  return true;
}

bool gtext_css_registry_find_selector(const char * name, size_t length, GTEXT_CSS_Selector_Info * info) {
  if (!name || length == 0 || length > REG_NAME_STACK) {
    return false;
  }
  char buf[REG_NAME_STACK];
  lower_copy(buf, name, length);
  size_t lo = 0, hi = gtext_css_tbl_selector_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_tstr(buf, length, gtext_css_tbl_selectors[mid].name);
    if (c == 0) {
      if (info) {
        selector_fill(&gtext_css_tbl_selectors[mid], info);
      }
      return true;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return false;
}

size_t gtext_css_registry_keyword_count(void) {
  return gtext_css_tbl_keyword_count;
}

const char * gtext_css_registry_keyword_at(size_t index, size_t * length) {
  if (index >= gtext_css_tbl_keyword_count) {
    return NULL;
  }
  if (length) {
    *length = gtext_css_tbl_keywords[index].len;
  }
  return css_tstr_c(gtext_css_tbl_keywords[index]);
}

uint32_t gtext_css_registry_keyword_lookup(const char * name, size_t length) {
  if (!name || length == 0 || length > REG_NAME_STACK) {
    return 0;
  }
  char buf[REG_NAME_STACK];
  lower_copy(buf, name, length);
  size_t lo = 0, hi = gtext_css_tbl_keyword_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = cmp_tstr(buf, length, gtext_css_tbl_keywords[mid]);
    if (c == 0) {
      return (uint32_t)(mid + 1);
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return 0;
}

size_t gtext_css_registry_pin_unit_count(void) {
  return gtext_css_tbl_unit_count;
}

bool gtext_css_registry_pin_unit_at(size_t index, const char ** name, const char ** measures) {
  if (index >= gtext_css_tbl_unit_count) {
    return false;
  }
  if (name) {
    *name = css_tstr_c(gtext_css_tbl_units[index].name);
  }
  if (measures) {
    *measures = css_tstr_c(gtext_css_tbl_units[index].measures);
  }
  return true;
}

size_t gtext_css_registry_pin_function_count(void) {
  return gtext_css_tbl_function_count;
}

const char * gtext_css_registry_pin_function_at(size_t index) {
  return index < gtext_css_tbl_function_count ? css_tstr_c(gtext_css_tbl_functions[index]) : NULL;
}

const char * gtext_css_registry_pin_commit(void) {
  return gtext_css_tbl_pin_commit;
}
