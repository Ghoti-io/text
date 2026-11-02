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
 * The base of every CSSOM object (cssom_internal.h): internal and external counts with the keep-alive holds up the parent chain, the user slot and its
 * finalizer, and freeing without recursion.
 */

#include <string.h>

#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_internal.h"

void css_core_retain(css_core * core) {
  core->refs++;
}

css_core * css_obj_core(const void * object) {
  return object ? ((const css_obj *)object)->core : NULL;
}

GTEXT_CSS_Status css_core_room(css_core * core, size_t bytes) {
  if (core->counters.live_objects >= core->config.max_live_objects || bytes > core->config.max_live_bytes ||
      core->counters.live_bytes > core->config.max_live_bytes - bytes) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_core_room_bytes(const css_core * core, size_t bytes) {
  if (bytes > core->config.max_live_bytes || core->counters.live_bytes > core->config.max_live_bytes - bytes) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  return GTEXT_CSS_OK;
}

void css_obj_init(css_obj * o, css_core * core, GTEXT_CSSOM_Object_Kind kind, uint64_t internal, uint64_t external, size_t bytes, void (*destroy)(css_obj *)) {
  css_obj_init_id(o, core, kind, internal, external, bytes, destroy, core->next_id++);
}

void css_obj_init_id(css_obj * o, css_core * core, GTEXT_CSSOM_Object_Kind kind, uint64_t internal, uint64_t external, size_t bytes, void (*destroy)(css_obj *), uint64_t id) {
  memset(o, 0, sizeof *o);
  o->kind = (uint8_t)kind;
  o->core = core;
  o->internal = internal;
  o->external = external;
  o->id = id;
  o->destroy = destroy;
  o->bytes = bytes;
  core->counters.live_objects++;
  core->counters.live_bytes += bytes;
  css_core_retain(core);
}

bool css_obj_charge(css_obj * o, size_t bytes) {
  css_core * core = o->core;
  if (bytes > core->config.max_live_bytes || core->counters.live_bytes > core->config.max_live_bytes - bytes) {
    return false;
  }
  core->counters.live_bytes += bytes;
  o->bytes += bytes;
  return true;
}

void css_obj_credit(css_obj * o, size_t bytes) {
  if (bytes > o->bytes) {
    bytes = o->bytes;
  }
  o->bytes -= bytes;
  o->core->counters.live_bytes -= bytes;
}

/* ---- freeing: a queue, never recursion ---------------------------------------------------------------------------------------------------------------- */

static void run_finalizer(css_obj * o) {
  GTEXT_CSSOM_Finalizer f = o->finalizer;
  void * p = o->user;
  o->finalizer = NULL;
  o->user = NULL;
  if (f) {
    o->core->in_finalizer++;
    f(p, (GTEXT_CSSOM_Object_Kind)o->kind);
    o->core->in_finalizer--;
  }
}

/// Free every queued object: its finalizer once, then its own destruction (which releases its children, queueing those that reach zero).
static void drain(css_core * core) {
  if (core->freeing) {
    return;
  }
  core->freeing = true;
  css_core_retain(core);   /* the last object of the queue may be the last reference: the loop ends with the core alive */
  while (core->dead_head) {
    css_obj * o = core->dead_head;
    core->dead_head = o->next_dead;
    o->next_dead = NULL;
    o->queued = false;
    if (o->internal != 0 || o->external != 0) {
      continue;   /* taken again before it was reached */
    }
    if (!o->finalized) {
      o->finalized = true;
      /* the finalizer may release other objects and set their user slots; those that die are queued behind this one */
      run_finalizer(o);
    }
    core->counters.live_objects--;
    core->counters.live_bytes -= o->bytes;
    o->destroy(o);   /* frees the object's memory: `o` is gone */
    css_core_release(core);
  }
  core->freeing = false;
  css_core_release(core);
}

/// An object whose counts are both zero is queued to be freed.
static void maybe_free(css_obj * o) {
  if (o->internal == 0 && o->external == 0 && !o->queued && !o->finalized) {
    css_core * core = o->core;
    o->next_dead = core->dead_head;
    core->dead_head = o;
    o->queued = true;
  }
}

void css_obj_retain_internal(css_obj * o) {
  o->internal++;
}

void css_obj_release_internal(css_obj * o) {
  if (o->internal > 0) {
    o->internal--;
  }
  css_core * core = o->core;
  maybe_free(o);
  drain(core);
}

void css_obj_retain_external(css_obj * o) {
  /* the first external reference of an object holds its parent externally, and so up to the sheet */
  while (o) {
    const bool first = o->external == 0;
    o->external++;
    if (!first) {
      break;
    }
    o = o->parent;
  }
}

void css_obj_release_external(css_obj * o) {
  css_core * core = o->core;
  while (o) {
    if (o->external == 0) {
      break;
    }
    o->external--;
    css_obj * parent = o->external == 0 ? o->parent : NULL;
    maybe_free(o);
    if (!parent) {
      break;
    }
    o = parent;
  }
  drain(core);
}

void css_obj_set_parent(css_obj * o, css_obj * parent) {
  if (o->parent == parent) {
    return;
  }
  css_obj * old = o->parent;
  if (o->external > 0) {
    /* the hold moves: take the new one first, so that nothing between the two is freed */
    if (parent) {
      css_obj_retain_external(parent);
    }
    o->parent = parent;
    if (old) {
      css_obj_release_external(old);
    }
    return;
  }
  o->parent = parent;
}

/* ---- the user slot ---------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_object_set_user(void * object, void * pointer, GTEXT_CSSOM_Finalizer finalizer) {
  if (!object) {
    return GTEXT_CSS_E_INVALID;
  }
  css_obj * o = object;
  GTEXT_CSSOM_Finalizer old_f = o->finalizer;
  void * old_p = o->user;
  /* the new pair is stored first: the old finalizer may not see a half-replaced slot */
  o->user = pointer;
  o->finalizer = finalizer;
  if (old_f) {
    css_core * core = o->core;   /* the finalizer may release other objects, and the core outlives them while this call holds the object */
    const GTEXT_CSSOM_Object_Kind kind = (GTEXT_CSSOM_Object_Kind)o->kind;
    core->in_finalizer++;
    old_f(old_p, kind);
    core->in_finalizer--;
  }
  return GTEXT_CSS_OK;
}

void * gtext_cssom_object_user(const void * object) {
  return object ? ((const css_obj *)object)->user : NULL;
}

GTEXT_CSSOM_Object_Kind gtext_cssom_object_kind(const void * object) {
  return object ? (GTEXT_CSSOM_Object_Kind)((const css_obj *)object)->kind : GTEXT_CSSOM_OBJECT_NONE;
}
