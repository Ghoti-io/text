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
 * URI-reference resolution, RFC 3986 section 5.
 *
 * JSON Schema's reference model is built on this and not on JSON Pointer.
 * `$id` sets a base URI, `$ref` is a URI-reference resolved against whatever
 * base is in scope, and the pointer in a fragment is the last step rather
 * than the whole mechanism. A resolver that treats `$ref` as a pointer gets
 * the common case right and every schema with an `$id` in it wrong.
 *
 * The strings here are plain C strings rather than arena values: they outlive
 * no document, they are built and thrown away during compilation, and putting
 * them in the schema's arena would tie their lifetime to something they have
 * no relation to.
 *
 * They come from the caller's allocator all the same.  Every function here
 * that allocates takes the allocator as a trailing parameter rather than
 * reading it from anything: this file is reached only from json_schema.c,
 * which has the compiling schema's allocator in hand at every call, and a
 * string returned from one allocator and released through another is the
 * failure mode that an allocator assigned after the fact invites (see the
 * note on csv_field_buffer_init in src/csv/csv_stream_buffer.c).
 */

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>

#include "json_internal.h"

void json_uri_parts_split(const char * uri, size_t len, json_uri_parts * out) {
  memset(out, 0, sizeof(*out));
  size_t i = 0;

  /* scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" - and the colon
   * must come before any slash, question mark or hash, or it belongs to a
   * path segment rather than to a scheme. */
  if (len > 0
      && ((uri[0] >= 'a' && uri[0] <= 'z') || (uri[0] >= 'A' && uri[0] <= 'Z'))) {
    size_t k = 0;
    while (k < len) {
      char c = uri[k];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
          || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        k++;
        continue;
      }
      break;
    }
    if (k < len && k > 0 && uri[k] == ':') {
      out->scheme = uri;
      out->scheme_len = k;
      out->has_scheme = 1;
      i = k + 1;
    }
  }

  if (i + 1 < len && uri[i] == '/' && uri[i + 1] == '/') {
    i += 2;
    size_t start = i;
    while (i < len && uri[i] != '/' && uri[i] != '?' && uri[i] != '#') {
      i++;
    }
    out->authority = uri + start;
    out->authority_len = i - start;
    out->has_authority = 1;
  }

  size_t path_start = i;
  while (i < len && uri[i] != '?' && uri[i] != '#') {
    i++;
  }
  out->path = uri + path_start;
  out->path_len = i - path_start;

  if (i < len && uri[i] == '?') {
    i++;
    size_t start = i;
    while (i < len && uri[i] != '#') {
      i++;
    }
    out->query = uri + start;
    out->query_len = i - start;
    out->has_query = 1;
  }
  if (i < len && uri[i] == '#') {
    out->fragment = uri + i + 1;
    out->fragment_len = len - i - 1;
    out->has_fragment = 1;
  }
}

/*
 * RFC 3986 section 5.2.4, remove_dot_segments, written into `out` which the
 * caller sized at least `len` bytes plus a terminator.
 *
 * The output is never longer than the input, because every step either copies
 * a segment or removes one.
 */
static size_t json_uri_remove_dot_segments(
    const char * path, size_t len, char * out) {
  size_t in = 0;
  size_t written = 0;

  while (in < len) {
    if (path[in] == '/') {
      /* Find the end of this segment. */
      size_t start = in + 1;
      size_t end = start;
      while (end < len && path[end] != '/') {
        end++;
      }
      size_t seg = end - start;
      if (seg == 1 && path[start] == '.') {
        in = end;
        if (in == len) {
          out[written++] = '/'; // "/." ends in a slash
        }
        continue;
      }
      if (seg == 2 && path[start] == '.' && path[start + 1] == '.') {
        /* Back up over the last segment written, if there is one. */
        while (written > 0 && out[written - 1] != '/') {
          written--;
        }
        if (written > 0) {
          written--; // the slash itself
        }
        in = end;
        if (in == len) {
          out[written++] = '/';
        }
        continue;
      }
      out[written++] = '/';
      memcpy(out + written, path + start, seg);
      written += seg;
      in = end;
      continue;
    }
    /* A relative first segment, which only reaches here for a path with no
     * leading slash; "." and ".." at the front are dropped. */
    size_t end = in;
    while (end < len && path[end] != '/') {
      end++;
    }
    size_t seg = end - in;
    if ((seg == 1 && path[in] == '.') || (seg == 2 && path[in] == '.'
            && path[in + 1] == '.')) {
      in = end;
      if (in < len) {
        in++; // and the slash after it
      }
      continue;
    }
    memcpy(out + written, path + in, seg);
    written += seg;
    in = end;
  }
  out[written] = '\0';
  return written;
}

char * json_uri_resolve(
    const char * base, size_t base_len, const char * ref, size_t ref_len,
    const GTEXT_Allocator * alloc) {
  json_uri_parts b;
  json_uri_parts r;
  json_uri_parts t;
  char * merged = NULL;
  size_t merged_len = 0;

  json_uri_parts_split(ref, ref_len, &r);
  json_uri_parts_split(base ? base : "", base ? base_len : 0, &b);

  memset(&t, 0, sizeof(t));
  /* RFC 3986 section 5.2.2, with strict resolution: a reference carrying a
   * scheme is already absolute and the base contributes nothing. */
  if (r.has_scheme) {
    t = r;
    merged = (char *)gtext_allocator_malloc(alloc, r.path_len + 1);
    if (!merged) {
      return NULL;
    }
    merged_len = json_uri_remove_dot_segments(r.path, r.path_len, merged);
  }
  else {
    t.scheme = b.scheme;
    t.scheme_len = b.scheme_len;
    t.has_scheme = b.has_scheme;
    if (r.has_authority) {
      t.authority = r.authority;
      t.authority_len = r.authority_len;
      t.has_authority = 1;
      merged = (char *)gtext_allocator_malloc(alloc, r.path_len + 1);
      if (!merged) {
        return NULL;
      }
      merged_len = json_uri_remove_dot_segments(r.path, r.path_len, merged);
      t.query = r.query;
      t.query_len = r.query_len;
      t.has_query = r.has_query;
    }
    else {
      t.authority = b.authority;
      t.authority_len = b.authority_len;
      t.has_authority = b.has_authority;
      if (r.path_len == 0) {
        merged = (char *)gtext_allocator_malloc(alloc, b.path_len + 1);
        if (!merged) {
          return NULL;
        }
        memcpy(merged, b.path, b.path_len);
        merged[b.path_len] = '\0';
        merged_len = b.path_len;
        /* An empty reference path keeps the base's query unless the
         * reference brought one of its own. */
        if (r.has_query) {
          t.query = r.query;
          t.query_len = r.query_len;
          t.has_query = 1;
        }
        else {
          t.query = b.query;
          t.query_len = b.query_len;
          t.has_query = b.has_query;
        }
      }
      else {
        t.query = r.query;
        t.query_len = r.query_len;
        t.has_query = r.has_query;
        if (r.path[0] == '/') {
          merged = (char *)gtext_allocator_malloc(alloc, r.path_len + 1);
          if (!merged) {
            return NULL;
          }
          merged_len = json_uri_remove_dot_segments(r.path, r.path_len, merged);
        }
        else {
          /* section 5.3's merge: everything up to the base's last slash. */
          size_t keep = 0;
          if (b.has_authority && b.path_len == 0) {
            keep = 0;
          }
          else {
            for (size_t i = b.path_len; i > 0; i--) {
              if (b.path[i - 1] == '/') {
                keep = i;
                break;
              }
            }
          }
          size_t joined_len = keep + r.path_len;
          char * joined = (char *)gtext_allocator_malloc(alloc, joined_len + 2);
          if (!joined) {
            return NULL;
          }
          size_t at = 0;
          if (b.has_authority && b.path_len == 0) {
            joined[at++] = '/';
          }
          memcpy(joined + at, b.path, keep);
          at += keep;
          memcpy(joined + at, r.path, r.path_len);
          at += r.path_len;
          joined[at] = '\0';
          merged = (char *)gtext_allocator_malloc(alloc, at + 1);
          if (!merged) {
            gtext_allocator_free(alloc, joined);
            return NULL;
          }
          merged_len = json_uri_remove_dot_segments(joined, at, merged);
          gtext_allocator_free(alloc, joined);
        }
      }
    }
  }
  t.fragment = r.fragment;
  t.fragment_len = r.fragment_len;
  t.has_fragment = r.has_fragment;

  /* section 5.3, recomposition. */
  size_t total = merged_len + 1;
  if (t.has_scheme) {
    total += t.scheme_len + 1;
  }
  if (t.has_authority) {
    total += t.authority_len + 2;
  }
  if (t.has_query) {
    total += t.query_len + 1;
  }
  if (t.has_fragment) {
    total += t.fragment_len + 1;
  }
  char * out = (char *)gtext_allocator_malloc(alloc, total);
  if (!out) {
    gtext_allocator_free(alloc, merged);
    return NULL;
  }
  size_t at = 0;
  if (t.has_scheme) {
    memcpy(out + at, t.scheme, t.scheme_len);
    at += t.scheme_len;
    out[at++] = ':';
  }
  if (t.has_authority) {
    out[at++] = '/';
    out[at++] = '/';
    memcpy(out + at, t.authority, t.authority_len);
    at += t.authority_len;
  }
  memcpy(out + at, merged, merged_len);
  at += merged_len;
  if (t.has_query) {
    out[at++] = '?';
    memcpy(out + at, t.query, t.query_len);
    at += t.query_len;
  }
  if (t.has_fragment) {
    out[at++] = '#';
    memcpy(out + at, t.fragment, t.fragment_len);
    at += t.fragment_len;
  }
  out[at] = '\0';
  gtext_allocator_free(alloc, merged);
  return out;
}

char * json_uri_without_fragment(
    const char * uri, size_t len, const GTEXT_Allocator * alloc) {
  const char * hash = (const char *)memchr(uri, '#', len);
  size_t keep = hash ? (size_t)(hash - uri) : len;
  char * out = (char *)gtext_allocator_malloc(alloc, keep + 1);
  if (!out) {
    return NULL;
  }
  memcpy(out, uri, keep);
  out[keep] = '\0';
  return out;
}

/*
 * Percent-decode, in place of a copy.
 *
 * A fragment is percent-encoded and a JSON Pointer is tilde-encoded, and they
 * are two different encodings applied in that order: RFC 6901 section 6 says
 * a pointer in a fragment is percent-decoded first and then read as a
 * pointer. So `#/$defs/percent%25field` names the member `percent%field`, and
 * a resolver that skipped this step looked for `percent%25field` and found
 * nothing.
 */
char * json_uri_percent_decode(const char * s, size_t len, size_t * out_len,
    const GTEXT_Allocator * alloc) {
  char * out = (char *)gtext_allocator_malloc(alloc, len + 1);
  if (!out) {
    return NULL;
  }
  size_t written = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '%' && i + 2 < len) {
      int hi = -1;
      int lo = -1;
      for (int pass = 0; pass < 2; pass++) {
        char c = s[i + 1 + pass];
        int v = -1;
        if (c >= '0' && c <= '9') {
          v = c - '0';
        }
        else if (c >= 'a' && c <= 'f') {
          v = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F') {
          v = c - 'A' + 10;
        }
        if (pass == 0) {
          hi = v;
        }
        else {
          lo = v;
        }
      }
      if (hi >= 0 && lo >= 0) {
        out[written++] = (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out[written++] = s[i];
  }
  out[written] = '\0';
  if (out_len) {
    *out_len = written;
  }
  return out;
}
