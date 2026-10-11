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
 * Memory, interned names, UTF-8 checks and the growable buffer behind the
 * markup tree.
 */

#include <ghoti.io/text/macros.h>

#include <ghoti.io/unicode/utf.h>
#include <stdint.h>
#include <string.h>

#include "markup_internal.h"

/* ---- memory ------------------------------------------------------------- */

void * markup_alloc(struct GTEXT_Markup_Document * doc, size_t size) {
  return gtext_allocator_calloc(&doc->allocator, 1, size);
}

void * markup_realloc(
    struct GTEXT_Markup_Document * doc, void * ptr, size_t size) {
  return gtext_allocator_realloc(&doc->allocator, ptr, size);
}

void markup_free(struct GTEXT_Markup_Document * doc, void * ptr) {
  gtext_allocator_free(&doc->allocator, ptr);
}

char * markup_copy(
    struct GTEXT_Markup_Document * doc, const char * bytes, size_t len) {
  if (len == SIZE_MAX) {
    return NULL;
  }
  char * copy = gtext_allocator_malloc(&doc->allocator, len + 1);
  if (!copy) {
    return NULL;
  }
  if (len > 0) {
    memcpy(copy, bytes, len);
  }
  copy[len] = '\0';
  return copy;
}

/* ---- UTF-8 -------------------------------------------------------------- */

GTEXT_Markup_Status markup_check_utf8(const char * bytes, size_t len) {
  if (len == 0) {
    return GTEXT_MARKUP_OK;
  }
  if (!bytes) {
    return GTEXT_MARKUP_E_INVALID;
  }
  return guni_utf8_validate(bytes, len, NULL) == GUNI_OK
      ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_ENCODING;
}

/* ---- atoms -------------------------------------------------------------- */

/* FNV-1a, 64 bit, started from a per-document seed. The seed is the document's
   own address mixed with a stack address, so it differs between runs under
   address-space randomisation. That is not a cryptographic defence and does not
   pretend to be one: it removes the precomputed-collision attack, where an
   attacker builds a million names that all land in one chain without needing to
   know anything about the process. Names are attacker-chosen in a parsed
   document, which is why the table is not a plain unseeded hash. */
static size_t hash_bytes(size_t seed, const char * bytes, size_t len) {
  uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)seed;
  for (size_t i = 0; i < len; i++) {
    h ^= (unsigned char)bytes[i];
    h *= 0x100000001b3ull;
  }
  /* A last mix, so that the low bits - which pick the bucket - depend on every
     byte rather than mostly on the last few. */
  h ^= h >> 32;
  h *= 0x9E3779B97F4A7C15ull;
  h ^= h >> 29;
  return (size_t)h;
}

static void grow_table(struct GTEXT_Markup_Document * doc) {
  /* Best effort. A table that cannot grow is slower, not wrong, so a failed
     allocation here is not a failure of the intern that asked for it. */
  const size_t wanted = doc->bucket_count ? doc->bucket_count * 2 : 64;
  if (wanted <= doc->bucket_count || wanted > SIZE_MAX / sizeof(markup_atom *)) {
    return;
  }
  markup_atom ** buckets = gtext_allocator_calloc(
      &doc->allocator, wanted, sizeof(markup_atom *));
  if (!buckets) {
    return;
  }
  for (size_t i = 0; i < doc->bucket_count; i++) {
    markup_atom * a = doc->buckets[i];
    while (a) {
      markup_atom * next = a->next;
      const size_t slot = a->hash & (wanted - 1);
      a->next = buckets[slot];
      buckets[slot] = a;
      a = next;
    }
  }
  markup_free(doc, doc->buckets);
  doc->buckets = buckets;
  doc->bucket_count = wanted;
}

markup_atom * markup_atom_find(
    const struct GTEXT_Markup_Document * doc, const char * bytes, size_t len) {
  if (!doc->bucket_count) {
    return NULL;
  }
  const size_t hash = hash_bytes(doc->seed, bytes, len);
  for (markup_atom * a = doc->buckets[hash & (doc->bucket_count - 1)]; a;
      a = a->next) {
    if (a->hash == hash && a->length == len && memcmp(a->text, bytes, len) == 0) {
      return a;
    }
  }
  return NULL;
}

markup_atom * markup_atom_intern(
    struct GTEXT_Markup_Document * doc, const char * bytes, size_t len) {
  markup_atom * existing = markup_atom_find(doc, bytes, len);
  if (existing) {
    existing->refs++;
    return existing;
  }
  if (len > SIZE_MAX - sizeof(markup_atom) - 1) {
    return NULL;
  }
  if (!doc->bucket_count || doc->atom_count >= doc->bucket_count) {
    grow_table(doc);
    if (!doc->bucket_count) {
      return NULL;
    }
  }
  markup_atom * atom = markup_alloc(doc, sizeof(markup_atom) + len + 1);
  if (!atom) {
    return NULL;
  }
  atom->hash = hash_bytes(doc->seed, bytes, len);
  atom->refs = 1;
  atom->length = len;
  if (len > 0) {
    memcpy(atom->text, bytes, len);
  }
  atom->text[len] = '\0';
  const size_t slot = atom->hash & (doc->bucket_count - 1);
  atom->next = doc->buckets[slot];
  doc->buckets[slot] = atom;
  doc->atom_count++;
  return atom;
}

void markup_atom_release(struct GTEXT_Markup_Document * doc, markup_atom * atom) {
  if (!atom) {
    return;
  }
  if (--atom->refs > 0) {
    return;
  }
  markup_atom ** link = &doc->buckets[atom->hash & (doc->bucket_count - 1)];
  while (*link && *link != atom) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = atom->next;
    doc->atom_count--;
  }
  markup_free(doc, atom);
}

/* ---- the extended walk -------------------------------------------------- */

struct GTEXT_Markup_Node * markup_ext_first(const struct GTEXT_Markup_Node * n) {
  if (n->kind == GTEXT_MARKUP_KIND_ELEMENT) {
    const markup_element * e = (const markup_element *)n;
    if (e->content) {
      return e->content;
    }
  }
  return n->first_child;
}

struct GTEXT_Markup_Node * markup_ext_next(const struct GTEXT_Markup_Node * n) {
  if (n->flags & MARKUP_F_CONTENT) {
    return n->parent->first_child;
  }
  /* `next` is a detached-list link when there is no parent, and the root and a
     detached subtree's top have none to give. */
  return n->parent ? n->next : NULL;
}

/* ---- a growable buffer -------------------------------------------------- */

void markup_buf_init(markup_buf * buf, const GTEXT_Allocator * allocator) {
  buf->allocator = allocator;
  buf->data = NULL;
  buf->length = 0;
  buf->capacity = 0;
}

GTEXT_Markup_Status markup_buf_append(
    markup_buf * buf, const char * bytes, size_t len) {
  if (len > SIZE_MAX - buf->length - 1) {
    return GTEXT_MARKUP_E_OOM;
  }
  const size_t need = buf->length + len + 1;
  if (need > buf->capacity) {
    size_t cap = buf->capacity ? buf->capacity : 64;
    while (cap < need) {
      if (cap > SIZE_MAX / 2) {
        cap = need;
        break;
      }
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(buf->allocator, buf->data, cap);
    if (!grown) {
      return GTEXT_MARKUP_E_OOM;
    }
    buf->data = grown;
    buf->capacity = cap;
  }
  if (len > 0) {
    memcpy(buf->data + buf->length, bytes, len);
  }
  buf->length += len;
  buf->data[buf->length] = '\0';
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status markup_buf_sink_write(
    void * user, const char * bytes, size_t len) {
  return markup_buf_append((markup_buf *)user, bytes, len);
}

void markup_buf_free(markup_buf * buf) {
  gtext_allocator_free(buf->allocator, buf->data);
  buf->data = NULL;
  buf->length = 0;
  buf->capacity = 0;
}
