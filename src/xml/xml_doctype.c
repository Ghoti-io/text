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
 * The document type declaration and ordered internal syntax records.
 *
 * XML 1.0 (Fifth Edition) [28] doctypedecl, [75] ExternalID, [11]
 * SystemLiteral, [12] PubidLiteral, [28a]-[29] and the markup declarations of
 * the internal subset.
 *
 * The name and external identifiers are kept in a doctype node. In DTD
 * processing mode, each internal-subset construct is retained in source order
 * with its kind and physical location. Declaration evaluation is deliberately
 * separate; a rebuildable effective binding index is compiled from the
 * retained records after DTD processing is complete.
 */

#include <ghoti.io/text/macros.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "xml_internal.h"

/* The specific code for plain syntax, the generic ones when the character
   itself is the trouble. */
static GTEXT_XML_Status bad(xml_ctx * p, const char * message) {
  uint32_t cp = 0;
  size_t n = 0;
  if (xml_decode(p, p->pos, &cp, &n) == XML_CHAR_OK) {
    return xml_fail(p, GTEXT_XML_E_DOCTYPE, message);
  }
  return xml_unexpected(p, message);
}

static int pubid_char(unsigned char c, unsigned char quote) {
  if (c == quote) {
    return 0;
  }
  return c == 0x20 || c == 0x0D || c == 0x0A || (c >= 'a' && c <= 'z')
      || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
      || (c != 0 && strchr("-'()+,./:=?;!*#@$_%", c) != NULL);
}

/* A quoted literal at the cursor. Appends its value to `out` - line ends
   normalized, and for a public identifier white space collapsed and trimmed as
   the specification asks processors to - and leaves the cursor after the
   closing quote. */
static GTEXT_XML_Status literal(xml_ctx * p, markup_buf * out, int is_public) {
  const unsigned char quote = p->pos < p->length ? p->in[p->pos] : 0;
  if (quote != '"' && quote != '\'') {
    return bad(p, "a quoted literal was expected");
  }
  XML_CONSUME_OR_RETURN(p, 1);
  /* An empty literal is present and empty, which is not the same as absent:
     make sure there is a string to hand over. */
  GTEXT_XML_Status status = xml_append(p, out, "", 0);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const size_t start = p->pos;
  size_t i = start;
  while (i < p->length && p->in[i] != quote) {
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) {
      return work;
    }
    const unsigned char b = p->in[i];
    if (is_public) {
      if (!pubid_char(b, quote)) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return bad(p, "character not allowed in a public identifier");
      }
      i++;
      continue;
    }
    if (b >= 0x20 && b < 0x80
        && !(b == 0x7F && p->version == XML_VERSION_11)) {
      i++;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
      XML_CONSUME_OR_RETURN(p, i - p->pos);
      return xml_unexpected(p, "invalid character");
    }
    i += n;
  }
  if (i >= p->length) {
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated literal");
  }
  if (is_public) {
    size_t words = 0;
    size_t j = start;
    while (j < i) {
      status = xml_charge_work(p, 1);
      if (status != GTEXT_XML_OK) return status;
      while (j < i) {
        const size_t whitespace = xml_space_bytes(p, j);
        if (whitespace == 0 || j + whitespace > i) {
          break;
        }
        j += whitespace;
      }
      const size_t word = j;
      while (j < i && xml_space_bytes(p, j) == 0) {
        j++;
      }
      if (j > word) {
        if (words++ > 0) {
          status = xml_append(p, out, " ", 1);
          if (status != GTEXT_XML_OK) {
            return status;
          }
        }
        status = xml_append(p, out, (const char *)p->in + word, j - word);
        if (status != GTEXT_XML_OK) {
          return status;
        }
      }
    }
  }
  else {
    status = xml_append_normalized(p, out, start, i);
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  XML_CONSUME_OR_RETURN(p, i + 1 - p->pos);
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_bad(xml_ctx * p, const char * message) {
  if (p->partial && p->pos >= p->length)
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
  return xml_fail(p, GTEXT_XML_E_DOCTYPE, message);
}

static GTEXT_XML_Status dtd_expand_inline_reference(xml_ctx * p);
static GTEXT_XML_Status dtd_pop_finished_parameter_sources(xml_ctx * p);
static int dtd_pe_references_allowed(const xml_ctx * p);
static GTEXT_XML_Status dtd_consume_logical_keyword(xml_ctx * p,
    const char * keyword, size_t length);
static GTEXT_XML_Status dtd_expand_current_references(xml_ctx * p);

static size_t dtd_skip_space(xml_ctx * p) {
  size_t total = 0;
  for (;;) {
    total += xml_skip_space(p);
    if (p->err.code != GTEXT_XML_OK) return total;
    if (p->pos >= p->length && p->source_count > 1
        && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
      if (xml_source_pop(p) != GTEXT_XML_OK) return total;
      continue;
    }
    if (p->pos >= p->length || p->in[p->pos] != '%'
        || p->pos + 1 >= p->length
        || xml_space_bytes(p, p->pos + 1) != 0) return total;
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return total;
  }
}

#define DTD_SKIP_SPACE_OR_RETURN(p) do { \
  dtd_skip_space(p); \
  if ((p)->err.code != GTEXT_XML_OK) return (p)->err.code; \
} while (0)

static GTEXT_XML_Status dtd_need_space(xml_ctx * p) {
  size_t skipped = dtd_skip_space(p);
  if (p->err.code != GTEXT_XML_OK) return p->err.code;
  if (skipped == 0) return dtd_bad(p, "white space is required in a DTD declaration");
  return p->err.code;
}

static GTEXT_XML_Status dtd_name_impl(xml_ctx * p) {
  p->dtd_name_token_length = 0;
  for (;;) {
    if (p->pos >= p->length) {
      if (p->source_count > 1 && XML_SOURCE_IS_PARAMETER_ENTITY(
              p->sources[p->source_count - 1].kind)) {
        GTEXT_XML_Status s = xml_source_pop(p);
        if (s != GTEXT_XML_OK) return s;
        continue;
      }
      break;
    }
    if (p->in[p->pos] == '%' && dtd_pe_references_allowed(p)) {
      GTEXT_XML_Status s = dtd_expand_inline_reference(p);
      if (s != GTEXT_XML_OK) return s;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK)
      return xml_unexpected(p, "invalid character in DTD name");
    if (!(p->dtd_name_token_length ? markup_xml_name_char(cp)
            : markup_xml_name_start_char(cp))) break;
    GTEXT_XML_Status s = xml_charge_work(p, 1);
    if (s != GTEXT_XML_OK) return s;
    if (n > p->max_name_bytes
        || p->dtd_name_token_length > p->max_name_bytes - n)
      return xml_fail(p, GTEXT_XML_E_LIMIT_NAME, "DTD Name exceeds the name byte limit");
    s = xml_grow(p, (void **)&p->dtd_name_token,
        &p->dtd_name_token_capacity, 1,
        p->dtd_name_token_length + n + 1);
    if (s != GTEXT_XML_OK) return s;
    memcpy(p->dtd_name_token + p->dtd_name_token_length,
        p->in + p->pos, n);
    p->dtd_name_token_length += n;
    p->dtd_name_token[p->dtd_name_token_length] = 0;
    XML_CONSUME_OR_RETURN(p, n);
  }
  if (!p->dtd_name_token_length) return dtd_bad(p, "a Name was expected");
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_name(xml_ctx * p) {
  while (p->pos < p->length && p->in[p->pos] == '%') {
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return status;
  }
  return dtd_name_impl(p);
}

static GTEXT_XML_Status dtd_plain_name(xml_ctx * p) {
  size_t n = 0;
  GTEXT_XML_Status status = xml_scan_name(p, &n);
  if (status != GTEXT_XML_OK) return status;
  return xml_consume(p, n);
}

static GTEXT_XML_Status dtd_quoted(xml_ctx * p, int is_public) {
  markup_buf tmp;
  markup_buf_init(&tmp, p->allocator);
  GTEXT_XML_Status s = literal(p, &tmp, is_public);
  markup_buf_free(&tmp);
  return s;
}

static GTEXT_XML_Status dtd_charge_reference(xml_ctx * p) {
  return xml_charge(p, &p->references, p->max_references, 1,
      GTEXT_XML_E_LIMIT_REFERENCES, "XML reference limit exceeded");
}

static int dtd_xml_char(const xml_ctx * p, uint32_t c);

static xml_parameter_entity * dtd_find_parameter_entity(xml_ctx * p,
    const unsigned char * name, size_t name_length) {
  for (size_t i = 0; i < p->parameter_entity_count; i++) {
    xml_parameter_entity * item = &p->parameter_entities[i];
    if (item->name_length == name_length
        && memcmp(item->name, name, name_length) == 0) return item;
  }
  return NULL;
}

static GTEXT_XML_Status dtd_add_parameter_entity(xml_ctx * p,
    const unsigned char * name, size_t name_length,
    const unsigned char * replacement, size_t replacement_length) {
  /* The first declaration is the binding; later ones remain inspectable. */
  if (dtd_find_parameter_entity(p, name, name_length)) return GTEXT_XML_OK;
  GTEXT_XML_Status status = xml_grow(p,
      (void **)&p->parameter_entities, &p->parameter_entity_capacity,
      sizeof(*p->parameter_entities), p->parameter_entity_count + 1);
  if (status != GTEXT_XML_OK) return status;
  char * owned_name = xml_alloc(p, name_length + 1);
  unsigned char * owned_replacement = xml_alloc(p,
      replacement_length ? replacement_length : 1);
  if (!owned_name || !owned_replacement) {
    xml_release(p, owned_name);
    xml_release(p, owned_replacement);
    return xml_fail(p, GTEXT_XML_E_OOM,
        "could not retain parameter entity replacement text");
  }
  memcpy(owned_name, name, name_length);
  owned_name[name_length] = '\0';
  if (replacement_length) memcpy(owned_replacement, replacement,
      replacement_length);
  xml_parameter_entity * item =
      &p->parameter_entities[p->parameter_entity_count++];
  item->name = owned_name;
  item->name_length = name_length;
  item->replacement = owned_replacement;
  item->replacement_length = replacement_length;
  item->public_id = NULL;
  item->system_id = NULL;
  item->external = 0;
  item->identity = owned_name;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_add_external_parameter_entity(xml_ctx * p,
    const unsigned char * name, size_t name_length, const char * public_id,
    const char * system_id) {
  if (dtd_find_parameter_entity(p, name, name_length)) return GTEXT_XML_OK;
  GTEXT_XML_Status status = xml_grow(p,
      (void **)&p->parameter_entities, &p->parameter_entity_capacity,
      sizeof(*p->parameter_entities), p->parameter_entity_count + 1);
  if (status != GTEXT_XML_OK) return status;
  char * owned_name = xml_alloc(p, name_length + 1);
  const size_t public_length = public_id ? strlen(public_id) : 0;
  const size_t system_length = strlen(system_id);
  char * owned_public = public_id ? xml_alloc(p, public_length + 1) : NULL;
  char * owned_system = xml_alloc(p, system_length + 1);
  if (!owned_name || !owned_system || (public_id && !owned_public)) {
    xml_release(p, owned_name); xml_release(p, owned_public);
    xml_release(p, owned_system);
    return xml_fail(p, GTEXT_XML_E_OOM,
        "could not retain external parameter entity identifiers");
  }
  memcpy(owned_name, name, name_length);
  owned_name[name_length] = '\0';
  if (owned_public) {
    memcpy(owned_public, public_id, public_length);
    owned_public[public_length] = '\0';
  }
  memcpy(owned_system, system_id, system_length);
  owned_system[system_length] = '\0';
  xml_parameter_entity * item =
      &p->parameter_entities[p->parameter_entity_count++];
  memset(item, 0, sizeof(*item));
  item->name = owned_name;
  item->name_length = name_length;
  item->public_id = owned_public;
  item->system_id = owned_system;
  item->external = 1;
  item->identity = owned_name;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_expand_parameter_entity(xml_ctx * p,
    const unsigned char * name, size_t name_length,
    const GTEXT_Markup_Location * reference_at,
    GTEXT_Markup_Node * doctype) {
  (void)doctype; /* Parsing resumes in the caller's iterative DTD loop. */
  xml_parameter_entity * entity = dtd_find_parameter_entity(p,
      name, name_length);
  if (!entity) return xml_fail_at(p, GTEXT_XML_E_DOCTYPE,
      "parameter entity reference has no preceding declaration", reference_at);
  if (entity->external) {
    xml_resolved_resource resource;
    GTEXT_XML_Status status = xml_resolve_external_resource(p,
        GTEXT_XML_RESOURCE_PARAMETER_ENTITY, XML_SOURCE_PARAMETER_ENTITY,
        entity->name, entity->public_id, entity->system_id, &resource);
    if (status == GTEXT_XML_OK && !resource.opened) {
      status = xml_fail_at(p, GTEXT_XML_E_RESOLVER,
          "external parameter entity was not resolved by the caller",
          reference_at);
    }
    if (status == GTEXT_XML_OK)
      status = xml_activate_external_parameter_entity(p, &resource);
    if (resource.opened)
      return xml_finish_external_resource(p, &resource, status);
    markup_buf_free(&resource.bytes);
    return status;
  }
  return xml_source_push(p, XML_SOURCE_INTERNAL_PARAMETER_ENTITY, entity->identity,
      entity->name, entity->name_length, entity->replacement,
      entity->replacement_length);
}

/* EntityValue has reference syntax of its own. */
static GTEXT_XML_Status dtd_entity_value(xml_ctx * p,
    unsigned char ** output, size_t * output_length) {
  if (p->pos >= p->length || (p->in[p->pos] != '\'' && p->in[p->pos] != '"'))
    return dtd_bad(p, "an entity value was expected");
  markup_buf expanded;
  markup_buf_init(&expanded, p->allocator);
#define DTD_ENTITY_CONSUME(count) do { \
  GTEXT_XML_Status consume_status = xml_consume(p, (count)); \
  if (consume_status != GTEXT_XML_OK) { \
    markup_buf_free(&expanded); \
    return consume_status; \
  } \
} while (0)
  unsigned char quote = p->in[p->pos];
  const void * base_identity = p->sources[p->source_count - 1].identity;
  DTD_ENTITY_CONSUME(1);
  for (;;) {
    if (p->pos >= p->length && p->source_count > 1
        && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
      GTEXT_XML_Status popped = xml_source_pop(p);
      if (popped != GTEXT_XML_OK) { markup_buf_free(&expanded); return popped; }
      continue;
    }
    if (p->pos >= p->length) {
      markup_buf_free(&expanded);
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated entity value");
    }
    const int at_base = p->sources[p->source_count - 1].identity == base_identity;
    if (at_base && p->in[p->pos] == quote) break;
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) { markup_buf_free(&expanded); return work; }
    if (p->in[p->pos] == '%') {
      const GTEXT_Markup_Location reference_at = xml_here(p);
      GTEXT_XML_Status reference = dtd_charge_reference(p);
      if (reference != GTEXT_XML_OK) { markup_buf_free(&expanded); return reference; }
      DTD_ENTITY_CONSUME(1);
      const size_t name_at = p->pos;
      size_t name_length = 0;
      reference = xml_scan_name(p, &name_length);
      if (reference != GTEXT_XML_OK) { markup_buf_free(&expanded); return reference; }
      xml_parameter_entity * entity = dtd_find_parameter_entity(p,
          p->in + name_at, name_length);
      if (!entity) {
        markup_buf_free(&expanded);
        return xml_fail_at(p, GTEXT_XML_E_DOCTYPE,
            "parameter entity reference has no preceding declaration", &reference_at);
      }
      DTD_ENTITY_CONSUME(name_length);
      reference = xml_expect(p, ";", 1,
          "a reference in an entity value ends with ';'");
      if (reference != GTEXT_XML_OK) { markup_buf_free(&expanded); return reference; }
      p->dtd_generated_parameter_entity = entity->name;
      reference = dtd_expand_parameter_entity(p,
          (const unsigned char *)entity->name, entity->name_length,
          &reference_at, NULL);
      if (reference != GTEXT_XML_OK) { markup_buf_free(&expanded); return reference; }
      continue;
    }
    if (p->in[p->pos] == '&') {
      const size_t reference_start = p->pos;
      GTEXT_XML_Status reference = dtd_charge_reference(p);
      if (reference != GTEXT_XML_OK) {
        markup_buf_free(&expanded);
        return reference;
      }
      DTD_ENTITY_CONSUME(1);
      int character_reference = 0;
      uint32_t character_value = 0;
      if (p->pos < p->length && p->in[p->pos] == '#') {
        character_reference = 1;
        DTD_ENTITY_CONSUME(1);
        int hex = p->pos < p->length && (p->in[p->pos] == 'x' || p->in[p->pos] == 'X');
        if (hex) DTD_ENTITY_CONSUME(1);
        size_t digits = 0;
        while (p->pos < p->length) {
          unsigned char c = p->in[p->pos];
          unsigned int digit = c >= '0' && c <= '9' ? (unsigned)(c - '0')
              : hex && c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
              : hex && c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : UINT_MAX;
          if (digit == UINT_MAX) break;
          if (character_value > (UINT32_MAX - digit) / (hex ? 16 : 10)) {
            markup_buf_free(&expanded);
            return dtd_bad(p, "character reference is out of range");
          }
          character_value = character_value * (hex ? 16 : 10) + digit;
          DTD_ENTITY_CONSUME(1); ++digits;
        }
        if (!digits) {
          markup_buf_free(&expanded);
          return dtd_bad(p, "a character reference needs digits");
        }
        if (!dtd_xml_char(p, character_value)) {
          markup_buf_free(&expanded);
          return xml_fail(p, GTEXT_XML_E_CHARACTER,
              "character reference is not allowed in this XML version");
        }
      }
      else {
        GTEXT_XML_Status s = dtd_plain_name(p);
        if (s != GTEXT_XML_OK) { markup_buf_free(&expanded); return s; }
      }
      if (p->pos >= p->length || p->in[p->pos] != ';') {
        markup_buf_free(&expanded);
        return dtd_bad(p, "a reference in an entity value ends with ';'");
      }
      DTD_ENTITY_CONSUME(1);
      if (character_reference) {
        unsigned char encoded[4];
        size_t encoded_length;
        if (character_value < 0x80) {
          encoded[0] = (unsigned char)character_value; encoded_length = 1;
        }
        else if (character_value < 0x800) {
          encoded[0] = (unsigned char)(0xC0 | (character_value >> 6));
          encoded[1] = (unsigned char)(0x80 | (character_value & 0x3F)); encoded_length = 2;
        }
        else if (character_value < 0x10000) {
          encoded[0] = (unsigned char)(0xE0 | (character_value >> 12));
          encoded[1] = (unsigned char)(0x80 | ((character_value >> 6) & 0x3F));
          encoded[2] = (unsigned char)(0x80 | (character_value & 0x3F)); encoded_length = 3;
        }
        else {
          encoded[0] = (unsigned char)(0xF0 | (character_value >> 18));
          encoded[1] = (unsigned char)(0x80 | ((character_value >> 12) & 0x3F));
          encoded[2] = (unsigned char)(0x80 | ((character_value >> 6) & 0x3F));
          encoded[3] = (unsigned char)(0x80 | (character_value & 0x3F)); encoded_length = 4;
        }
        if (markup_buf_append(&expanded, (const char *)encoded,
                encoded_length) != GTEXT_MARKUP_OK) {
          markup_buf_free(&expanded);
          return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
        }
      }
      else if (markup_buf_append(&expanded,
              (const char *)p->in + reference_start,
              p->pos - reference_start) != GTEXT_MARKUP_OK) {
        markup_buf_free(&expanded);
        return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
      }
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK) {
      markup_buf_free(&expanded);
      return xml_unexpected(p, "invalid character in entity value");
    }
    if (markup_buf_append(&expanded, (const char *)p->in + p->pos, n)
        != GTEXT_MARKUP_OK) {
      markup_buf_free(&expanded);
      return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
    }
    DTD_ENTITY_CONSUME(n);
  }
  DTD_ENTITY_CONSUME(1);
  *output = (unsigned char *)expanded.data;
  *output_length = expanded.length;
  expanded.data = NULL;
  markup_buf_free(&expanded);
#undef DTD_ENTITY_CONSUME
  return GTEXT_XML_OK;
}

static int dtd_xml_char(const xml_ctx * p, uint32_t c) {
  if (p->version == XML_VERSION_11)
    return (c >= 1 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD)
        || (c >= 0x10000 && c <= 0x10FFFF);
  return c == 0x9 || c == 0xA || c == 0xD
      || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD)
      || (c >= 0x10000 && c <= 0x10FFFF);
}

static GTEXT_XML_Status dtd_attribute_value(xml_ctx * p) {
  if (p->pos >= p->length || (p->in[p->pos] != '\'' && p->in[p->pos] != '"'))
    return dtd_bad(p, "an attribute default value was expected");
  unsigned char quote = p->in[p->pos];
  XML_CONSUME_OR_RETURN(p, 1);
  while (p->pos < p->length && p->in[p->pos] != quote) {
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) return work;
    unsigned char c = p->in[p->pos];
    if (c == '<') return dtd_bad(p, "'<' is not allowed in an attribute default value");
    if (c == '&') {
      GTEXT_XML_Status reference = dtd_charge_reference(p);
      if (reference != GTEXT_XML_OK) return reference;
      XML_CONSUME_OR_RETURN(p, 1);
      if (p->pos < p->length && p->in[p->pos] == '#') {
        XML_CONSUME_OR_RETURN(p, 1);
        int hex = p->pos < p->length && (p->in[p->pos] == 'x' || p->in[p->pos] == 'X');
        if (hex) XML_CONSUME_OR_RETURN(p, 1);
        size_t digits = 0;
        uint32_t value = 0;
        while (p->pos < p->length) {
          unsigned char d = p->in[p->pos];
          unsigned int n = d >= '0' && d <= '9' ? (unsigned)(d - '0')
              : hex && d >= 'a' && d <= 'f' ? (unsigned)(d - 'a' + 10)
              : hex && d >= 'A' && d <= 'F' ? (unsigned)(d - 'A' + 10) : UINT_MAX;
          if (n == UINT_MAX) break;
          if (value > (UINT32_MAX - n) / (hex ? 16 : 10))
            return dtd_bad(p, "character reference is out of range");
          value = value * (hex ? 16 : 10) + n;
          XML_CONSUME_OR_RETURN(p, 1); ++digits;
        }
        if (!digits) return dtd_bad(p, "a character reference needs digits");
        if (!dtd_xml_char(p, value)) return xml_fail(p, GTEXT_XML_E_CHARACTER,
            "character reference is not allowed in this XML version");
      }
      else {
        GTEXT_XML_Status s = dtd_plain_name(p);
        if (s != GTEXT_XML_OK) return s;
      }
      if (p->pos >= p->length || p->in[p->pos] != ';')
        return dtd_bad(p, "a reference in an attribute default ends with ';'");
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    uint32_t cp = 0; size_t n = 0;
    if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK)
      return xml_unexpected(p, "invalid character in attribute default value");
    XML_CONSUME_OR_RETURN(p, n);
  }
  if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
      "unterminated attribute default value");
  return xml_consume(p, 1);
}

static GTEXT_XML_Status dtd_nmtoken_impl(xml_ctx * p) {
  size_t start = p->pos;
  size_t total = 0;
  for (;;) {
    if (p->pos >= p->length && p->source_count > 1
        && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
      GTEXT_XML_Status s = xml_source_pop(p);
      if (s != GTEXT_XML_OK) return s;
      continue;
    }
    if (p->pos >= p->length) break;
    if (p->in[p->pos] == '%' && dtd_pe_references_allowed(p)) {
      GTEXT_XML_Status s = dtd_expand_inline_reference(p);
      if (s != GTEXT_XML_OK) return s;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK || !markup_xml_name_char(cp)) break;
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) return work;
    if (n > p->max_name_bytes || total > p->max_name_bytes - n)
      return xml_fail(p, GTEXT_XML_E_LIMIT_NAME, "DTD Nmtoken exceeds the name byte limit");
    total += n;
    XML_CONSUME_OR_RETURN(p, n);
  }
  if (total == 0 && p->pos == start) return dtd_bad(p, "an Nmtoken was expected");
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_nmtoken(xml_ctx * p) {
  while (p->pos < p->length && p->in[p->pos] == '%') {
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return status;
  }
  return dtd_nmtoken_impl(p);
}

/* XML content particles are nested, but XML's grammar bounds their nesting by
   the parser's input/depth limits. This iterative form avoids C recursion. */
static GTEXT_XML_Status dtd_content_model_impl(xml_ctx * p,
    unsigned char * separators, size_t separator_capacity) {
  if (p->pos < p->length && p->in[p->pos] == 'E')
    return dtd_consume_logical_keyword(p, "EMPTY", 5);
  if (p->pos < p->length && p->in[p->pos] == 'A')
    return dtd_consume_logical_keyword(p, "ANY", 3);
  if (p->pos >= p->length || p->in[p->pos] != '(')
    return dtd_bad(p, "an element content specification was expected");
  XML_CONSUME_OR_RETURN(p, 1);
  DTD_SKIP_SPACE_OR_RETURN(p);
  if (p->pos < p->length && p->in[p->pos] == '#') {
    GTEXT_XML_Status keyword = dtd_consume_logical_keyword(p, "#PCDATA", 7);
    if (keyword != GTEXT_XML_OK) return keyword;
    DTD_SKIP_SPACE_OR_RETURN(p);
    size_t names = 0;
    while (p->pos < p->length && p->in[p->pos] == '|') {
      XML_CONSUME_OR_RETURN(p, 1);
      DTD_SKIP_SPACE_OR_RETURN(p);
      GTEXT_XML_Status s = dtd_name(p);
      if (s != GTEXT_XML_OK) return s;
      ++names;
      DTD_SKIP_SPACE_OR_RETURN(p);
    }
    if (p->pos >= p->length || p->in[p->pos] != ')')
      return dtd_bad(p, "')' was expected to end a mixed content model");
    XML_CONSUME_OR_RETURN(p, 1);
    GTEXT_XML_Status expand = dtd_expand_current_references(p);
    if (expand != GTEXT_XML_OK) return expand;
    if (names != 0) {
      if (p->pos >= p->length || p->in[p->pos] != '*')
        return dtd_bad(p, "a mixed content choice must end with '*'");
      XML_CONSUME_OR_RETURN(p, 1);
    }
    else if (p->pos < p->length && p->in[p->pos] == '*') {
      XML_CONSUME_OR_RETURN(p, 1);
    }
    return GTEXT_XML_OK;
  }
  size_t depth = 1;
  int need_particle = 1;
  int separator = 0;
  int has_indicator = 0;
  for (;;) {
    size_t before_space = p->pos;
    DTD_SKIP_SPACE_OR_RETURN(p);
    size_t skipped_space = p->pos - before_space;
    if (p->pos >= p->length && p->source_count > 1
        && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
      GTEXT_XML_Status popped = xml_source_pop(p);
      if (popped != GTEXT_XML_OK) return popped;
      continue;
    }
    if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated content model");
    unsigned char c = p->in[p->pos];
    if (!need_particle && !has_indicator && skipped_space != 0
        && (c == '?' || c == '*' || c == '+'))
      return dtd_bad(p, "an occurrence indicator must immediately follow its particle");
    if (need_particle) {
      if (c == '(') {
        XML_CONSUME_OR_RETURN(p, 1);
        if (depth >= p->max_depth || depth >= separator_capacity)
          return xml_fail(p, GTEXT_XML_E_LIMIT_DEPTH, "DTD content model depth limit exceeded");
        separators[depth] = 0;
        ++depth;
        need_particle = 1;
        separator = 0;
        has_indicator = 0;
        continue;
      }
      if (xml_looking_at(p, "#PCDATA", 7)) return dtd_bad(p, "#PCDATA is only legal in mixed content");
      GTEXT_XML_Status s = dtd_name(p);
      if (s != GTEXT_XML_OK) return s;
      need_particle = 0;
      has_indicator = 0;
      continue;
    }
    if (c == '?' || c == '*' || c == '+') {
      if (has_indicator) return dtd_bad(p, "a content particle may have only one occurrence indicator");
      has_indicator = 1;
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    if (c == ',' || c == '|') {
      if (separator && separator != c) return dtd_bad(p, "a content group cannot mix ',' and '|' separators");
      separator = c;
      separators[depth - 1] = (unsigned char)c;
      XML_CONSUME_OR_RETURN(p, 1);
      need_particle = 1;
      has_indicator = 0;
      continue;
    }
    if (c == ')') {
      if (need_particle) return dtd_bad(p, "a content particle was expected before ')'");
      XML_CONSUME_OR_RETURN(p, 1);
      if (--depth == 0) break;
      separator = separators[depth - 1];
      need_particle = 0;
      has_indicator = 0;
      continue;
    }
    return dtd_bad(p, "unexpected token in an element content model");
  }
  GTEXT_XML_Status expand = dtd_expand_current_references(p);
  if (expand != GTEXT_XML_OK) return expand;
  if (xml_looking_at(p, "?", 1) || xml_looking_at(p, "*", 1) || xml_looking_at(p, "+", 1))
    XML_CONSUME_OR_RETURN(p, 1);
  return GTEXT_XML_OK;
}

static int dtd_pe_references_allowed(const xml_ctx * p) {
  for (size_t i = 1; i < p->source_count; ++i)
    if (p->sources[i].external_source) return 1;
  return 0;
}

static GTEXT_XML_Status dtd_expand_inline_reference(xml_ctx * p) {
  if (!dtd_pe_references_allowed(p))
    return dtd_bad(p,
        "parameter entity references are not allowed inside internal-subset declarations");
  const GTEXT_Markup_Location at = xml_here(p);
  GTEXT_XML_Status status = dtd_charge_reference(p);
  if (status != GTEXT_XML_OK) return status;
  XML_CONSUME_OR_RETURN(p, 1);
  const size_t name_at = p->pos;
  size_t name_length = 0;
  status = xml_scan_name(p, &name_length);
  if (status != GTEXT_XML_OK) return status;
  xml_parameter_entity * entity = dtd_find_parameter_entity(p,
      p->in + name_at, name_length);
  if (!entity) return xml_fail_at(p, GTEXT_XML_E_DOCTYPE,
      "parameter entity reference has no preceding declaration", &at);
  XML_CONSUME_OR_RETURN(p, name_length);
  status = xml_expect(p, ";", 1,
      "a parameter entity reference ends with ';'");
  if (status != GTEXT_XML_OK) return status;
  p->dtd_generated_parameter_entity = entity->name;
  return dtd_expand_parameter_entity(p, (const unsigned char *)entity->name,
      entity->name_length, &at, NULL);
}

static GTEXT_XML_Status dtd_consume_logical_keyword(xml_ctx * p,
    const char * keyword, size_t length) {
  for (size_t i = 0; i < length;) {
    if (p->pos >= p->length && p->source_count > 1
        && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
      GTEXT_XML_Status status = xml_source_pop(p);
      if (status != GTEXT_XML_OK) return status;
      continue;
    }
    if (p->pos >= p->length)
      return dtd_bad(p, "a DTD keyword was split before it was complete");
    if (p->in[p->pos] == '%' && dtd_pe_references_allowed(p)) {
      GTEXT_XML_Status status = dtd_expand_inline_reference(p);
      if (status != GTEXT_XML_OK) return status;
      continue;
    }
    if (p->in[p->pos] != (unsigned char)keyword[i])
      return dtd_bad(p, "an unexpected token was found in the DTD");
    XML_CONSUME_OR_RETURN(p, 1);
    ++i;
  }
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_expand_current_references(xml_ctx * p) {
  for (;;) {
    while (p->pos >= p->length && p->source_count > 1
        && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
      GTEXT_XML_Status status = xml_source_pop(p);
      if (status != GTEXT_XML_OK) return status;
    }
    if (p->pos >= p->length || p->in[p->pos] != '%')
      return GTEXT_XML_OK;
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return status;
  }
}

static GTEXT_XML_Status dtd_pop_finished_parameter_sources(xml_ctx * p) {
  while (p->source_count > 1
      && XML_SOURCE_IS_PARAMETER_ENTITY(p->sources[p->source_count - 1].kind)
      && p->pos >= p->length) {
    GTEXT_XML_Status status = xml_source_pop(p);
    if (status != GTEXT_XML_OK) return status;
  }
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_content_model(xml_ctx * p) {
  while (p->pos < p->length && p->in[p->pos] == '%') {
    GTEXT_XML_Status expand = dtd_expand_inline_reference(p);
    if (expand != GTEXT_XML_OK) return expand;
  }
  size_t capacity = p->length / 2 + 1;
  if (capacity > p->max_depth) capacity = p->max_depth;
  if (capacity == 0) return xml_fail(p, GTEXT_XML_E_LIMIT_DEPTH,
      "DTD content model depth limit exceeded");
  if (capacity > SIZE_MAX / sizeof(unsigned char))
    return xml_fail(p, GTEXT_XML_E_OOM, "DTD content model stack is too large");
  unsigned char * separators = xml_alloc(p, capacity * sizeof(*separators));
  if (!separators) return xml_fail(p, GTEXT_XML_E_OOM,
      "could not allocate DTD content model stack");
  memset(separators, 0, capacity * sizeof(*separators));
  GTEXT_XML_Status status = dtd_content_model_impl(p, separators, capacity);
  xml_release(p, separators);
  if (status == GTEXT_XML_OK) {
    GTEXT_XML_Status popped = dtd_pop_finished_parameter_sources(p);
    if (popped != GTEXT_XML_OK) return popped;
  }
  return status;
}

static GTEXT_XML_Status dtd_attribute_type_impl(xml_ctx * p) {
  static const char * const words[] = {"CDATA", "IDREFS", "IDREF", "ID", "ENTITIES", "ENTITY", "NMTOKENS", "NMTOKEN"};
  int notation = 0;
  if (p->pos < p->length && p->in[p->pos] != '(') {
    GTEXT_XML_Status s = dtd_name(p);
    if (s != GTEXT_XML_OK) return s;
    int recognized = 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
      if (p->dtd_name_token_length == strlen(words[i])
          && memcmp(p->dtd_name_token, words[i], p->dtd_name_token_length) == 0) {
        recognized = 1;
        break;
      }
    }
    if (recognized) return GTEXT_XML_OK;
    if (p->dtd_name_token_length == 8
        && memcmp(p->dtd_name_token, "NOTATION", 8) == 0) {
      notation = 1;
      s = dtd_need_space(p);
      if (s != GTEXT_XML_OK) return s;
    }
    else return dtd_bad(p, "an attribute type was expected");
  }
  if (p->pos >= p->length || p->in[p->pos] != '(') return dtd_bad(p, "an attribute type was expected");
  XML_CONSUME_OR_RETURN(p, 1);
  DTD_SKIP_SPACE_OR_RETURN(p);
  GTEXT_XML_Status s = notation ? dtd_name(p) : dtd_nmtoken(p);
  if (s != GTEXT_XML_OK) return s;
  DTD_SKIP_SPACE_OR_RETURN(p);
  while (p->pos < p->length && p->in[p->pos] == '|') {
    XML_CONSUME_OR_RETURN(p, 1); DTD_SKIP_SPACE_OR_RETURN(p);
    s = notation ? dtd_name(p) : dtd_nmtoken(p); if (s != GTEXT_XML_OK) return s;
    DTD_SKIP_SPACE_OR_RETURN(p);
  }
  if (p->pos >= p->length || p->in[p->pos] != ')') return dtd_bad(p, "')' was expected to end an attribute type");
  return xml_consume(p, 1);
}

static GTEXT_XML_Status dtd_attribute_type(xml_ctx * p) {
  while (p->pos < p->length && p->in[p->pos] == '%') {
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return status;
  }
  GTEXT_XML_Status status = dtd_attribute_type_impl(p);
  if (status == GTEXT_XML_OK)
    status = dtd_pop_finished_parameter_sources(p);
  return status;
}

static GTEXT_XML_Status dtd_default_decl_impl(xml_ctx * p) {
  if (p->pos < p->length && p->in[p->pos] == '#') {
    XML_CONSUME_OR_RETURN(p, 1);
    GTEXT_XML_Status s = dtd_name(p);
    if (s != GTEXT_XML_OK) return s;
    int required = p->dtd_name_token_length == 8
        && memcmp(p->dtd_name_token, "REQUIRED", 8) == 0;
    int implied = p->dtd_name_token_length == 7
        && memcmp(p->dtd_name_token, "IMPLIED", 7) == 0;
    int fixed = p->dtd_name_token_length == 5
        && memcmp(p->dtd_name_token, "FIXED", 5) == 0;
    if (!required && !implied && !fixed)
      return dtd_bad(p, "#REQUIRED, #IMPLIED, or #FIXED was expected");
    if (!fixed) return GTEXT_XML_OK;
    s = dtd_need_space(p);
    if (s != GTEXT_XML_OK) return s;
    return dtd_attribute_value(p);
  }
  return dtd_attribute_value(p);
}

static GTEXT_XML_Status dtd_default_decl(xml_ctx * p) {
  while (p->pos < p->length && p->in[p->pos] == '%') {
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return status;
  }
  GTEXT_XML_Status status = dtd_default_decl_impl(p);
  if (status == GTEXT_XML_OK)
    status = dtd_pop_finished_parameter_sources(p);
  return status;
}

static GTEXT_XML_Status dtd_external_id_capture_impl(xml_ctx * p,
    int allow_public_only, markup_buf * public_id, markup_buf * system_id) {
  GTEXT_XML_Status s;
  if (p->pos < p->length && p->in[p->pos] == 'S') {
    s = dtd_consume_logical_keyword(p, "SYSTEM", 6);
    if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    return literal(p, system_id, 0);
  }
  if (p->pos >= p->length || p->in[p->pos] != 'P')
    return dtd_bad(p, "SYSTEM or PUBLIC was expected");
  s = dtd_consume_logical_keyword(p, "PUBLIC", 6);
  if (s != GTEXT_XML_OK) return s;
  s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
  s = literal(p, public_id, 1); if (s != GTEXT_XML_OK) return s;
  size_t space = xml_skip_space(p);
  if (p->err.code != GTEXT_XML_OK) return p->err.code;
  if (allow_public_only && (p->pos >= p->length || p->in[p->pos] == '>')) return GTEXT_XML_OK;
  if (space == 0) return dtd_bad(p, "white space and a system literal were expected after a public identifier");
  return literal(p, system_id, 0);
}

static GTEXT_XML_Status dtd_external_id_capture(xml_ctx * p,
    int allow_public_only, markup_buf * public_id, markup_buf * system_id) {
  while (p->pos < p->length && p->in[p->pos] == '%') {
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return status;
  }
  GTEXT_XML_Status status = dtd_external_id_capture_impl(p,
      allow_public_only, public_id, system_id);
  if (status == GTEXT_XML_OK)
    status = dtd_pop_finished_parameter_sources(p);
  return status;
}

static GTEXT_XML_Status dtd_external_id(xml_ctx * p, int allow_public_only) {
  markup_buf public_id, system_id;
  markup_buf_init(&public_id, p->allocator);
  markup_buf_init(&system_id, p->allocator);
  GTEXT_XML_Status status = dtd_external_id_capture(p, allow_public_only,
      &public_id, &system_id);
  markup_buf_free(&public_id);
  markup_buf_free(&system_id);
  return status;
}

/* Parse one complete declaration body (cursor is immediately after keyword). */
static GTEXT_XML_Status validate_declaration(xml_ctx * p, int kind) {
  GTEXT_XML_Status s;
  const unsigned char * pe_name = NULL;
  size_t pe_name_length = 0;
  const unsigned char * pe_replacement = NULL;
  size_t pe_replacement_length = 0;
  unsigned char * pe_owned_replacement = NULL;
  markup_buf pe_public, pe_system;
  int pe_external = 0;
  /* The caller has checked that S follows the keyword; consume it here. */
  DTD_SKIP_SPACE_OR_RETURN(p);
  if (kind == 0) { /* ELEMENT */
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_content_model(p); if (s != GTEXT_XML_OK) return s;
  }
  else if (kind == 1) { /* ATTLIST */
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    size_t initial_space = xml_skip_space(p);
    if (p->err.code != GTEXT_XML_OK) return p->err.code;
    if ((p->pos >= p->length || p->in[p->pos] != '>') && initial_space == 0)
      return dtd_bad(p, "white space is required before an attribute definition");
    size_t attrs = 0;
    for (;;) {
      size_t space = xml_skip_space(p);
      if (p->err.code != GTEXT_XML_OK) return p->err.code;
      if (p->pos < p->length && p->in[p->pos] == '>') {
        return xml_consume(p, 1);
      }
      if (attrs != 0 && !space) return dtd_bad(p, "white space is required between attribute definitions");
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_attribute_type(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_default_decl(p); if (s != GTEXT_XML_OK) return s;
      attrs++;
    }
  }
  else if (kind == 2) { /* ENTITY */
    int parameter_entity = 0;
    if (p->pos < p->length && p->in[p->pos] == '%') {
      parameter_entity = 1;
      XML_CONSUME_OR_RETURN(p, 1);
      s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    }
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    if (parameter_entity) {
      pe_name = p->dtd_name_token;
      pe_name_length = p->dtd_name_token_length;
    }
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    if (p->pos < p->length && (p->in[p->pos] == '\'' || p->in[p->pos] == '"')) {
      s = dtd_entity_value(p, &pe_owned_replacement,
          &pe_replacement_length);
      if (s == GTEXT_XML_OK && parameter_entity)
        pe_replacement = pe_owned_replacement;
    }
    else {
      if (parameter_entity) {
        markup_buf_init(&pe_public, p->allocator);
        markup_buf_init(&pe_system, p->allocator);
        pe_external = 1;
        s = dtd_external_id_capture(p, 0, &pe_public, &pe_system);
      }
      else s = dtd_external_id(p, 0);
      size_t separator_space = xml_skip_space(p);
      if (p->err.code != GTEXT_XML_OK) {
        if (pe_external) { markup_buf_free(&pe_public); markup_buf_free(&pe_system); }
        return p->err.code;
      }
      if (s == GTEXT_XML_OK && xml_looking_at(p, "NDATA", 5)) {
        if (parameter_entity) {
          if (pe_external) { markup_buf_free(&pe_public); markup_buf_free(&pe_system); }
          return dtd_bad(p, "a parameter entity cannot have NDATA");
        }
        if (separator_space == 0)
          return dtd_bad(p, "white space is required before NDATA");
        s = xml_consume(p, 5);
        if (s == GTEXT_XML_OK) s = dtd_need_space(p);
        if (s == GTEXT_XML_OK) s = dtd_name(p);
      }
    }
    if (s != GTEXT_XML_OK) {
      if (pe_external) { markup_buf_free(&pe_public); markup_buf_free(&pe_system); }
      xml_release(p, pe_owned_replacement);
      return s;
    }
  }
  else { /* NOTATION */
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_external_id(p, 1); if (s != GTEXT_XML_OK) return s;
  }
  xml_skip_space(p);
  if (p->err.code != GTEXT_XML_OK) {
    if (pe_external) { markup_buf_free(&pe_public); markup_buf_free(&pe_system); }
    xml_release(p, pe_owned_replacement);
    return p->err.code;
  }
  if (p->pos >= p->length || p->in[p->pos] != '>') {
    if (pe_external) { markup_buf_free(&pe_public); markup_buf_free(&pe_system); }
    xml_release(p, pe_owned_replacement);
    return dtd_bad(p, "'>' was expected to end the DTD declaration");
  }
  s = xml_consume(p, 1);
  if (s == GTEXT_XML_OK && pe_name) {
    if (pe_external) {
      s = dtd_add_external_parameter_entity(p, pe_name, pe_name_length,
          pe_public.length ? pe_public.data : NULL,
          pe_system.data ? pe_system.data : "");
    }
    else s = dtd_add_parameter_entity(p, pe_name, pe_name_length,
        pe_replacement, pe_replacement_length);
  }
  if (pe_external) { markup_buf_free(&pe_public); markup_buf_free(&pe_system); }
  xml_release(p, pe_owned_replacement);
  return s;
}

/* Build the parse's effective lookup keys from each successfully parsed
   declaration. The doctype keeps every syntax item separately, including
   declarations that lose here. */
static GTEXT_XML_Status bind_key(xml_ctx *p, int kind,
    const unsigned char *element, size_t element_len,
    const unsigned char *name, size_t name_len, int external,
    GTEXT_Markup_Node *doctype, const unsigned char *decl, size_t decl_len,
    const GTEXT_Markup_Location *where) {
  for (size_t i = 0; i < p->dtd_binding_count; ++i) {
    GTEXT_XML_Status charged = xml_charge_work(p, 1);
    if (charged != GTEXT_XML_OK) return charged;
    xml_dtd_binding *b = &p->dtd_bindings[i];
    if (b->kind != kind || b->name_length != name_len
        || memcmp(b->name, name, name_len) != 0) continue;
    if (kind == GTEXT_MARKUP_DTD_ATTLIST
        && (b->element_name_length != element_len
            || memcmp(b->element_name, element, element_len) != 0)) continue;
    /* Internal declarations outrank external declarations even when an
       external PE was encountered earlier while reading the internal subset. */
    if (!external && b->external) {
      char *copy = xml_alloc(p, name_len + 1);
      char *element_copy = element_len ? xml_alloc(p, element_len + 1) : NULL;
      if (!copy || (element_len && !element_copy)) {
        xml_release(p, copy); xml_release(p, element_copy);
        return xml_fail(p, GTEXT_XML_E_OOM, "out of memory compiling DTD bindings");
      }
      memcpy(copy, name, name_len); copy[name_len] = 0;
      if (element_len) { memcpy(element_copy, element, element_len); element_copy[element_len] = 0; }
      xml_release(p, b->name); xml_release(p, b->element_name);
      b->name = copy; b->element_name = element_copy;
      b->name_length = name_len; b->element_name_length = element_len;
      b->external = 0;
      if (doctype) {
        GTEXT_Markup_Status ms = markup_doctype_set_binding(doctype, kind,
            (const char *)element, element_len, (const char *)name, name_len,
            (const char *)decl, decl_len, where, 0);
        if (ms != GTEXT_MARKUP_OK) return xml_from_markup(p, ms);
      }
    }
    return GTEXT_XML_OK;
  }
  GTEXT_XML_Status s = xml_grow(p, (void **)&p->dtd_bindings,
      &p->dtd_binding_capacity, sizeof(*p->dtd_bindings), p->dtd_binding_count + 1);
  if (s != GTEXT_XML_OK) return s;
  char *copy = xml_alloc(p, name_len + 1);
  char *element_copy = element_len ? xml_alloc(p, element_len + 1) : NULL;
  if (!copy || (element_len && !element_copy)) {
    xml_release(p, copy); xml_release(p, element_copy);
    return xml_fail(p, GTEXT_XML_E_OOM, "out of memory compiling DTD bindings");
  }
  memcpy(copy, name, name_len); copy[name_len] = 0;
  if (element_len) { memcpy(element_copy, element, element_len); element_copy[element_len] = 0; }
  const size_t binding_index = p->dtd_binding_count;
  p->dtd_bindings[binding_index] = (xml_dtd_binding){
      .kind = kind, .element_name = element_copy, .element_name_length = element_len,
      .name = copy, .name_length = name_len,
      .syntax_index = binding_index, .external = (unsigned char)external};
  p->dtd_binding_count = binding_index + 1;
  if (doctype) {
    GTEXT_Markup_Status ms = markup_doctype_set_binding(doctype, kind,
        (const char *)element, element_len, (const char *)name, name_len,
        (const char *)decl, decl_len, where, external);
    if (ms != GTEXT_MARKUP_OK) return xml_from_markup(p, ms);
  }
  return GTEXT_XML_OK;
}

static size_t decl_name(const unsigned char *s, size_t n, size_t *at) {
  while (*at < n && xml_is_space(s[*at])) ++*at;
  size_t begin = *at;
  while (*at < n && !xml_is_space(s[*at]) && s[*at] != '>'
      && s[*at] != '(' && s[*at] != ')' && s[*at] != '\'' && s[*at] != '"') ++*at;
  return *at - begin;
}

static GTEXT_XML_Status compile_decl_bindings(xml_ctx *p, int kind,
    const unsigned char *s, size_t n, int external,
    GTEXT_Markup_Node *doctype, const GTEXT_Markup_Location *where) {
  GTEXT_XML_Status charged = xml_charge_work(p, n);
  if (charged != GTEXT_XML_OK) return charged;
  const char *keyword = kind == 0 ? "<!ELEMENT" : kind == 1 ? "<!ATTLIST"
      : kind == 2 ? "<!ENTITY" : "<!NOTATION";
  size_t at = strlen(keyword);
  int key_kind = kind;
  while (at < n && xml_is_space(s[at])) ++at;
  if (kind == 2 && at < n && s[at] == '%') {
    ++at;
    while (at < n && xml_is_space(s[at])) ++at;
    key_kind = 4;
  }
  size_t name_at = at, name_len = decl_name(s, n, &at);
  name_at = at - name_len;
  if (!name_len) return GTEXT_XML_OK; /* grammar parser already diagnosed it */
  if (kind != 1) return bind_key(p, key_kind, NULL, 0, s + name_at,
      name_len, external, doctype, s, n, where);
  const unsigned char *element = s + name_at;
  size_t element_len = name_len;
  while (at < n) {
    while (at < n && xml_is_space(s[at])) ++at;
    if (at >= n || s[at] == '>') break;
    name_at = at; name_len = decl_name(s, n, &at);
    if (!name_len) break;
    GTEXT_XML_Status status = bind_key(p, kind, element, element_len,
        s + name_at, name_len, external, doctype, s, n, where);
    if (status != GTEXT_XML_OK) return status;
    /* Skip the type (including a parenthesized enumeration) and default
       declaration, respecting quoted literals. */
    while (at < n && xml_is_space(s[at])) ++at;
    int parens = 0; unsigned char quote = 0;
    while (at < n) {
      unsigned char c = s[at++];
      if (quote) { if (c == quote) quote = 0; continue; }
      if (c == '\'' || c == '"') quote = c;
      else if (c == '(') ++parens;
      else if (c == ')' && parens) --parens;
      else if (xml_is_space(c) && parens == 0) break;
    }
    while (at < n && xml_is_space(s[at])) ++at;
    if (at < n && s[at] == '#') {
      while (at < n && !xml_is_space(s[at]) && s[at] != '>' && s[at] != '\'' && s[at] != '"') ++at;
      while (at < n && xml_is_space(s[at])) ++at;
    }
    if (at < n && (s[at] == '\'' || s[at] == '"')) {
      quote = s[at++]; while (at < n && s[at] != quote) ++at;
      if (at < n) ++at;
    }
  }
  return GTEXT_XML_OK;
}

typedef struct {
  const markup_dtd_syntax *syntax;
  int external;
} ordered_dtd_item;

static int compare_dtd_order(const void *a, const void *b) {
  const ordered_dtd_item *x = a, *y = b;
  return x->syntax->sequence < y->syntax->sequence ? -1
      : x->syntax->sequence > y->syntax->sequence ? 1 : 0;
}

GTEXT_XML_Status xml_doctype_rebuild_indexes(xml_ctx *p,
    GTEXT_Markup_Node *node) {
  if (!p) return GTEXT_XML_E_INVALID;
  if (!node || node->kind != GTEXT_MARKUP_KIND_DOCTYPE)
    return xml_fail(p, GTEXT_XML_E_INVALID, "a doctype is required to rebuild DTD indexes");
  markup_doctype *d = (markup_doctype *)node;
  size_t count = d->syntax_count + d->external_syntax_count
      + d->generated_syntax_count;
  if (count > SIZE_MAX / sizeof(ordered_dtd_item))
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK, "DTD index rebuild size overflow");
  ordered_dtd_item *items = count
      ? markup_alloc(node->doc, count * sizeof(*items)) : NULL;
  if (count && !items)
    return xml_fail(p, GTEXT_XML_E_OOM, "could not allocate DTD index rebuild order");
  size_t at = 0;
  for (size_t i = 0; i < d->syntax_count; ++i)
    items[at++] = (ordered_dtd_item){&d->syntax[i], 0};
  for (size_t i = 0; i < d->external_syntax_count; ++i)
    items[at++] = (ordered_dtd_item){&d->external_syntax[i], 1};
  for (size_t i = 0; i < d->generated_syntax_count; ++i)
    items[at++] = (ordered_dtd_item){&d->generated_syntax[i],
        d->generated_syntax[i].external_context};
  size_t sort_work = 0;
  for (size_t n = count; n > 1; n = (n + 1) / 2) ++sort_work;
  if (count && sort_work > SIZE_MAX / count) {
    markup_free(node->doc, items);
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK, "DTD index rebuild work overflow");
  }
  GTEXT_XML_Status status = xml_charge_work(p, count * sort_work);
  if (status == GTEXT_XML_OK && count > 1)
    qsort(items, count, sizeof(*items), compare_dtd_order);
  if (status == GTEXT_XML_OK) {
    markup_dtd_binding *old_model = d->bindings;
    size_t old_model_count = d->binding_count;
    size_t old_model_capacity = d->binding_capacity;
    xml_dtd_binding *old_parse = p->dtd_bindings;
    size_t old_parse_count = p->dtd_binding_count;
    size_t old_parse_capacity = p->dtd_binding_capacity;
    d->bindings = NULL; d->binding_count = d->binding_capacity = 0;
    p->dtd_bindings = NULL;
    p->dtd_binding_count = p->dtd_binding_capacity = 0;
    for (size_t i = 0; status == GTEXT_XML_OK && i < count; ++i) {
      const markup_dtd_syntax *item = items[i].syntax;
      if (item->kind > GTEXT_MARKUP_DTD_NOTATION) continue;
      status = compile_decl_bindings(p, item->kind,
          (const unsigned char *)item->source, item->length, items[i].external,
          node, &item->location);
    }
    if (status != GTEXT_XML_OK) {
      markup_doctype_clear_bindings(node);
      markup_free(node->doc, d->bindings);
      d->bindings = old_model; d->binding_count = old_model_count;
      d->binding_capacity = old_model_capacity;
      for (size_t i = 0; i < p->dtd_binding_count; ++i) {
        xml_release(p, p->dtd_bindings[i].element_name);
        xml_release(p, p->dtd_bindings[i].name);
      }
      xml_release(p, p->dtd_bindings);
      p->dtd_bindings = old_parse;
      p->dtd_binding_count = old_parse_count;
      p->dtd_binding_capacity = old_parse_capacity;
    }
    else {
    for (size_t i = 0; i < old_model_count; ++i) {
      markup_free(node->doc, old_model[i].element_name);
      markup_free(node->doc, old_model[i].name);
    }
    markup_free(node->doc, old_model);
    for (size_t i = 0; i < old_parse_count; ++i) {
      xml_release(p, old_parse[i].element_name);
      xml_release(p, old_parse[i].name);
    }
    xml_release(p, old_parse);
    }
  }
  markup_free(node->doc, items);
  return status;
}

/* DTD-off preserves the original skip-only behavior. DTD processing validates
   productions and compiles derived lookup keys while retaining syntax. */
static GTEXT_XML_Status skip_declaration(xml_ctx * p, int is_entity) {
  for (;;) {
    size_t i = p->pos;
    while (i < p->length) {
      GTEXT_XML_Status work = xml_charge_work(p, 1);
      if (work != GTEXT_XML_OK) return work;
      unsigned char b = p->in[i];
      if (b == '"' || b == '\'' || b == '>' || b == '<' || b == ']' || b == '%') break;
      if ((b >= 0x20 && b < 0x80 && !(b == 0x7F && p->version == XML_VERSION_11))
          || b == '\t' || b == '\n' || b == '\r') { ++i; continue; }
      uint32_t cp = 0; size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "unterminated declaration in the internal subset");
    unsigned char b = p->in[p->pos];
    if (b == '>') return xml_consume(p, 1);
    if (b == '<' || b == ']') return dtd_bad(p,
        "declaration in the internal subset is not terminated by '>'");
    if (b == '%' && p->partial && p->pos + 1 >= p->length)
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    if (b == '%' && is_entity && p->pos + 1 < p->length
        && xml_space_bytes(p, p->pos + 1) > 0) {
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    if (b == '%') return dtd_bad(p,
        "a parameter entity reference cannot occur inside a declaration in the internal subset");
    unsigned char quote = b;
    XML_CONSUME_OR_RETURN(p, 1);
    i = p->pos;
    while (i < p->length && p->in[i] != quote) {
      GTEXT_XML_Status work = xml_charge_work(p, 1);
      if (work != GTEXT_XML_OK) return work;
      unsigned char c = p->in[i];
      if ((c >= 0x20 && c < 0x80 && !(c == 0x7F && p->version == XML_VERSION_11))
          || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
      uint32_t cp = 0; size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "unterminated literal in the internal subset");
    XML_CONSUME_OR_RETURN(p, 1);
  }
}

#undef DTD_SKIP_SPACE_OR_RETURN

/* External subsets use the same declaration validators as the internal
   subset. Conditional sections share one heap-backed depth stack, so both
   INCLUDE and IGNORE nesting is bounded without consuming the C stack. */
static GTEXT_XML_Status external_conditional_header(xml_ctx * p,
    int * include) {
  XML_CONSUME_OR_RETURN(p, 3); /* <![ */
  xml_skip_space(p);
  if (p->err.code != GTEXT_XML_OK) return p->err.code;
  while (p->pos < p->length && p->in[p->pos] == '%') {
    GTEXT_XML_Status status = dtd_expand_inline_reference(p);
    if (status != GTEXT_XML_OK) return status;
  }
  GTEXT_XML_Status status = dtd_name(p);
  if (status != GTEXT_XML_OK) return status;
  if (p->dtd_name_token_length == 7
      && memcmp(p->dtd_name_token, "INCLUDE", 7) == 0) *include = 1;
  else if (p->dtd_name_token_length == 6
      && memcmp(p->dtd_name_token, "IGNORE", 6) == 0) *include = 0;
  else return dtd_bad(p, "INCLUDE or IGNORE was expected in a conditional section");
  xml_skip_space(p);
  if (p->err.code != GTEXT_XML_OK) return p->err.code;
  GTEXT_XML_Status pop_status = dtd_pop_finished_parameter_sources(p);
  if (pop_status != GTEXT_XML_OK) return pop_status;
  if (p->pos >= p->length || p->in[p->pos] != '[')
    return dtd_bad(p, "'[' was expected in a conditional section");
  XML_CONSUME_OR_RETURN(p, 1);
  return GTEXT_XML_OK;
}

/* Conditional-section delimiters are physical grammar boundaries. Do not
   assemble one by expanding a parameter entity between its characters. */
static GTEXT_XML_Status external_conditional_end(xml_ctx * p) {
  if (p->length - p->pos < 3)
    return dtd_bad(p,
        "a conditional-section delimiter crosses a parameter-entity boundary");
  if (memcmp(p->in + p->pos, "]]>", 3) != 0)
    return dtd_bad(p, "a conditional section must end with ']]>'");
  return xml_consume(p, 3);
}

static GTEXT_XML_Status external_subset_body(xml_ctx * p,
    GTEXT_Markup_Node * doctype) {
  static const char * const keywords[] = {
      "<!ELEMENT", "<!ATTLIST", "<!ENTITY", "<!NOTATION"};
  unsigned char * modes = NULL; /* one means INCLUDE; zero means IGNORE */
  size_t depth = 0, capacity = 0;
  const size_t entry_source_count = p->source_count;
  GTEXT_XML_Status result = GTEXT_XML_OK;
  for (;;) {
    const int ignoring = depth != 0 && !modes[depth - 1];
    if (!ignoring) {
      xml_skip_space(p);
      if (p->err.code != GTEXT_XML_OK) { result = p->err.code; break; }
    }
    if (p->pos >= p->length && p->source_count > entry_source_count
        && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
      result = xml_source_pop(p);
      if (result != GTEXT_XML_OK) break;
      continue;
    }
    if (p->pos >= p->length) {
      result = depth ? xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated conditional section") : GTEXT_XML_OK;
      break;
    }
    uint32_t codepoint = 0;
    size_t codepoint_length = 0;
    if (xml_decode(p, p->pos, &codepoint, &codepoint_length) != XML_CHAR_OK) {
      result = xml_unexpected(p, "invalid UTF-8 in external subset");
      break;
    }
    (void)codepoint;
    (void)codepoint_length;
    if (depth && p->in[p->pos] == ']') {
      result = external_conditional_end(p);
      if (result != GTEXT_XML_OK) break;
      --depth;
      continue;
    }
    if (ignoring && !xml_looking_at(p, "<![", 3)) {
      uint32_t cp = 0;
      size_t n = 0;
      if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK) {
        result = xml_unexpected(p, "invalid character in ignored conditional section");
        break;
      }
      result = xml_charge_work(p, 1);
      if (result != GTEXT_XML_OK) break;
      result = xml_consume(p, n);
      if (result != GTEXT_XML_OK) break;
      continue;
    }
    if (!ignoring && p->in[p->pos] == '%') {
      const GTEXT_Markup_Location at = xml_here(p);
      result = dtd_charge_reference(p);
      if (result != GTEXT_XML_OK) break;
      result = xml_consume(p, 1);
      if (result != GTEXT_XML_OK) break;
      const size_t name_at = p->pos;
      size_t name_length = 0;
      result = xml_scan_name(p, &name_length);
      if (result != GTEXT_XML_OK) break;
      result = xml_consume(p, name_length);
      if (result != GTEXT_XML_OK) break;
      result = xml_expect(p, ";", 1,
          "a parameter entity reference ends with ';'");
      if (result != GTEXT_XML_OK) break;
      result = dtd_expand_parameter_entity(p, p->in + name_at,
          name_length, &at, doctype);
      if (result != GTEXT_XML_OK) break;
      continue;
    }
    if (xml_looking_at(p, "<![", 3)) {
      if (depth >= p->max_depth) {
        result = xml_fail(p, GTEXT_XML_E_LIMIT_DEPTH,
            "conditional section nesting limit exceeded");
        break;
      }
      int include = 0;
      if (ignoring) {
        /* ignoreSectContents nests delimiters directly: bytes after this
           opener are opaque, and no INCLUDE/IGNORE header is parsed here. */
        result = xml_consume(p, 3);
        if (result != GTEXT_XML_OK) break;
      }
      else {
        result = external_conditional_header(p, &include);
        if (result != GTEXT_XML_OK) break;
      }
      result = xml_grow(p, (void **)&modes, &capacity, sizeof(*modes), depth + 1);
      if (result != GTEXT_XML_OK) break;
      modes[depth++] = (unsigned char)include;
      continue;
    }
    if (xml_looking_at(p, "<!--", 4) || xml_looking_at(p, "<?", 2)) {
      const size_t start = p->pos;
      const GTEXT_Markup_Location at = xml_here(p);
      const GTEXT_Markup_DTD_Syntax_Kind kind =
          xml_looking_at(p, "<!--", 4) ? GTEXT_MARKUP_DTD_COMMENT
              : GTEXT_MARKUP_DTD_PROCESSING_INSTRUCTION;
      GTEXT_XML_Status status = kind == GTEXT_MARKUP_DTD_COMMENT
          ? xml_parse_comment(p, 0) : xml_parse_pi(p, 0);
      if (status != GTEXT_XML_OK) { result = status; break; }
      if (doctype && !p->stream) {
        if (p->source_count > 1 && XML_SOURCE_IS_PARAMETER_ENTITY(
            p->sources[p->source_count - 1].kind)) {
          status = xml_from_markup(p, markup_doctype_append_generated_syntax(
              doctype, kind, (const char *)p->in + start, p->pos - start, &at,
              p->sources[p->source_count - 1].owned_name,
              p->sources[p->source_count - 1].external_source));
        }
        else status = xml_from_markup(p, markup_doctype_append_external_syntax(
            doctype, kind, (const char *)p->in + start, p->pos - start, &at));
        if (status != GTEXT_XML_OK) { result = status; break; }
      }
      continue;
    }
    if (p->in[p->pos] != '<') {
      result = dtd_bad(p, "unexpected content in external subset");
      break;
    }
    size_t k = 0;
    for (; k < sizeof(keywords) / sizeof(keywords[0]); ++k)
      if (xml_looking_at(p, keywords[k], strlen(keywords[k]))) break;
    if (k == sizeof(keywords) / sizeof(keywords[0])) {
      result = dtd_bad(p, "not a declaration, comment, PI or conditional section");
      break;
    }
    const size_t start = p->pos;
    const GTEXT_Markup_Location at = xml_here(p);
    const void * declaration_source_identity = p->sources[p->source_count - 1].identity;
    p->dtd_generated_parameter_entity = NULL;
    GTEXT_XML_Status status = xml_charge(p, &p->declarations,
        p->max_declarations, 1, GTEXT_XML_E_LIMIT_DECLARATIONS,
        "DTD declaration limit exceeded");
    if (status != GTEXT_XML_OK) { result = status; break; }
    result = xml_consume(p, strlen(keywords[k]));
    if (result != GTEXT_XML_OK) break;
    if (dtd_skip_space(p) == 0) {
      if (p->err.code != GTEXT_XML_OK) { result = p->err.code; break; }
      result = bad(p, "white space is required after the declaration keyword");
      break;
    }
    status = validate_declaration(p, (int)k);
    if (status != GTEXT_XML_OK) { result = status; break; }
    if (p->source_count == 0
        || p->sources[p->source_count - 1].identity
            != declaration_source_identity) {
      result = xml_fail_at(p, GTEXT_XML_E_DOCTYPE,
          "a markup declaration crosses a parameter entity boundary", &at);
      break;
    }
    if (doctype && !p->stream) {
      if (p->dtd_generated_parameter_entity) {
        status = xml_from_markup(p, markup_doctype_append_generated_syntax(
            doctype, (int)k, (const char *)p->in + start, p->pos - start, &at,
            p->dtd_generated_parameter_entity,
            p->sources[p->source_count - 1].external_source));
      }
      else if (p->source_count > 1 && XML_SOURCE_IS_PARAMETER_ENTITY(
          p->sources[p->source_count - 1].kind)) {
        status = xml_from_markup(p, markup_doctype_append_generated_syntax(
            doctype, (int)k, (const char *)p->in + start, p->pos - start, &at,
            p->sources[p->source_count - 1].owned_name,
            p->sources[p->source_count - 1].external_source));
      }
      else status = xml_from_markup(p, markup_doctype_append_external_syntax(
          doctype, (int)k, (const char *)p->in + start, p->pos - start, &at));
      if (status != GTEXT_XML_OK) { result = status; break; }
    }
    status = compile_decl_bindings(p, (int)k, p->in + start, p->pos - start,
        p->source_count != 0 && p->sources[p->source_count - 1].external_source,
        doctype && !p->stream ? doctype : NULL, &at);
    if (status != GTEXT_XML_OK) { result = status; break; }
  }
  xml_release(p, modes);
  return result;
}

static GTEXT_XML_Status text_decl_value(xml_ctx * p, size_t * at,
    size_t * length) {
  if (p->pos >= p->length || (p->in[p->pos] != '\'' && p->in[p->pos] != '"'))
    return dtd_bad(p, "a quoted text declaration value was expected");
  const unsigned char quote = p->in[p->pos];
  XML_CONSUME_OR_RETURN(p, 1);
  *at = p->pos;
  while (p->pos < p->length && p->in[p->pos] != quote) {
    if (p->in[p->pos] < 0x20 || p->in[p->pos] >= 0x80)
      return xml_unexpected(p, "non-ASCII character in text declaration");
    XML_CONSUME_OR_RETURN(p, 1);
  }
  if (p->pos >= p->length)
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "unterminated text declaration value");
  *length = p->pos - *at;
  return xml_consume(p, 1);
}

static GTEXT_XML_Status external_text_declaration(xml_ctx * p) {
  if (!xml_looking_at(p, "<?xml", 5) || p->pos + 5 >= p->length
      || xml_space_bytes(p, p->pos + 5) == 0)
    return GTEXT_XML_OK;
  XML_CONSUME_OR_RETURN(p, 5);
  if (xml_skip_space(p) == 0) return dtd_bad(p,
      "white space is required in a text declaration");
  size_t version_at = 0, version_length = 0;
  if (xml_looking_at(p, "version", 7)) {
    XML_CONSUME_OR_RETURN(p, 7);
    xml_skip_space(p);
    if (p->pos >= p->length || p->in[p->pos] != '=')
      return dtd_bad(p, "'=' was expected after version");
    XML_CONSUME_OR_RETURN(p, 1); xml_skip_space(p);
    GTEXT_Markup_Location where = xml_here(p);
    GTEXT_XML_Status s = text_decl_value(p, &version_at, &version_length);
    if (s != GTEXT_XML_OK) return s;
    if (!((version_length == 3 && p->in[version_at] == '1'
            && p->in[version_at + 1] == '.'
            && (p->in[version_at + 2] == '0' || p->in[version_at + 2] == '1'))))
      return xml_fail_at(p, GTEXT_XML_E_UNSUPPORTED_VERSION,
          "only XML 1.0 and XML 1.1 text declarations are supported", &where);
    const xml_version declared = p->in[version_at + 2] == '1'
        ? XML_VERSION_11 : XML_VERSION_10;
    /* XML 1.1 permits an external entity labelled 1.0; the document's
       selected profile still governs processing. XML 1.0 cannot consume an
       entity labelled 1.1. */
    if (declared == XML_VERSION_11 && p->version == XML_VERSION_10)
      return xml_fail_at(p, GTEXT_XML_E_UNSUPPORTED_VERSION,
          "an XML 1.0 document cannot use an XML 1.1 external entity", &where);
    if (xml_skip_space(p) == 0)
      return dtd_bad(p, "encoding is required after the text declaration version");
  }
  if (!xml_looking_at(p, "encoding", 8))
    return dtd_bad(p, "encoding is required in a text declaration");
  XML_CONSUME_OR_RETURN(p, 8); xml_skip_space(p);
  if (p->pos >= p->length || p->in[p->pos] != '=')
    return dtd_bad(p, "'=' was expected after encoding");
  XML_CONSUME_OR_RETURN(p, 1); xml_skip_space(p);
  size_t encoding_at = 0, encoding_length = 0;
  GTEXT_Markup_Location where = xml_here(p);
  GTEXT_XML_Status s = text_decl_value(p, &encoding_at, &encoding_length);
  if (s != GTEXT_XML_OK) return s;
  int valid = encoding_length > 0
      && ((p->in[encoding_at] | 0x20) >= 'a'
          && (p->in[encoding_at] | 0x20) <= 'z');
  for (size_t i = 1; valid && i < encoding_length; ++i) {
    unsigned char c = p->in[encoding_at + i];
    valid = ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
        || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  }
  if (!valid) return xml_fail_at(p, GTEXT_XML_E_SYNTAX,
      "not an encoding name", &where);
  s = xml_check_label(p, p->in + encoding_at, encoding_length, &where);
  if (s != GTEXT_XML_OK) return s;
  xml_skip_space(p);
  return xml_expect(p, "?>", 2,
      "'?>' was expected to end the text declaration");
}

GTEXT_XML_Status xml_parse_external_subset(xml_ctx * p,
    GTEXT_Markup_Node * doctype, const unsigned char * bytes, size_t length,
    const char * base_uri) {
  xml_ctx ext = *p;
  ext.in = bytes;
  ext.length = length;
  ext.pos = 0;
  ext.line = 1;
  ext.column = 1;
  ext.cursor_previous = 0;
  ext.err = (GTEXT_XML_Error){0};
  ext.base_uri = base_uri;
  markup_buf_init(&ext.decoded, ext.allocator);
  GTEXT_XML_Status status = xml_detect_encoding(&ext);
  if (status == GTEXT_XML_OK) {
    /* The source frame records the active decoded coordinates for location
       queries while the resolver-owned source is still open. */
    if (ext.source_count != 0) {
      xml_source_frame * f = &ext.sources[ext.source_count - 1];
      f->bytes = ext.in; f->length = ext.length; f->pos = ext.pos;
      f->line = 1; f->column = 1; f->cursor_previous = 0;
    }
    status = external_text_declaration(&ext);
  }
  if (status == GTEXT_XML_OK)
    status = external_subset_body(&ext, doctype);
  p->references = ext.references;
  p->declarations = ext.declarations;
  p->total_work = ext.total_work;
  p->sources = ext.sources;
  p->source_count = ext.source_count;
  p->source_capacity = ext.source_capacity;
  p->source_names = ext.source_names;
  p->source_name_count = ext.source_name_count;
  p->source_name_capacity = ext.source_name_capacity;
  p->next_source_id = ext.next_source_id;
  p->parameter_entities = ext.parameter_entities;
  p->parameter_entity_count = ext.parameter_entity_count;
  p->parameter_entity_capacity = ext.parameter_entity_capacity;
  p->dtd_bindings = ext.dtd_bindings;
  p->dtd_binding_count = ext.dtd_binding_count;
  p->dtd_binding_capacity = ext.dtd_binding_capacity;
  if (status != GTEXT_XML_OK) p->err = ext.err;
  markup_buf_free(&ext.decoded);
  return status;
}

/* Turn an already pushed resolver frame into the active DTD input. The
   external_subset_body loop consumes it and pops it, so nested PEs use heap
   source frames rather than the C call stack. */
GTEXT_XML_Status xml_activate_external_parameter_entity(xml_ctx * p,
    xml_resolved_resource * resource) {
  if (!p || !resource || !resource->opened || !resource->frame_pushed
      || p->source_count <= 1
      || p->sources[p->source_count - 1].identity != resource->source.identity)
    return xml_fail(p, GTEXT_XML_E_STATE,
        "external parameter entity source frame is not active");
  static const unsigned char empty[1] = {0};
  xml_ctx ext = *p;
  ext.in = resource->bytes.data
      ? (const unsigned char *)resource->bytes.data : empty;
  ext.length = resource->bytes.length;
  ext.pos = 0; ext.line = 1; ext.column = 1; ext.cursor_previous = 0;
  ext.err = (GTEXT_XML_Error){0};
  ext.base_uri = resource->source.base_uri;
  markup_buf_init(&ext.decoded, ext.allocator);
  GTEXT_XML_Status status = xml_detect_encoding(&ext);
  if (status == GTEXT_XML_OK) status = external_text_declaration(&ext);
  if (status == GTEXT_XML_OK) {
    xml_source_frame * f = &ext.sources[ext.source_count - 1];
    f->bytes = ext.in; f->length = ext.length; f->pos = ext.pos;
    f->line = ext.line; f->column = ext.column;
    f->cursor_previous = ext.cursor_previous;
    f->owned_bytes = resource->bytes.data;
    resource->bytes.data = NULL;
    resource->bytes.length = resource->bytes.capacity = 0;
    f->owned_decoded = ext.decoded.data;
    ext.decoded.data = NULL; ext.decoded.length = ext.decoded.capacity = 0;
    f->external_close = resource->source.close;
    f->external_handle = resource->source.handle;
    resource->source.close = NULL;
    resource->opened = 0;
    resource->frame_pushed = 0;
    p->in = ext.in; p->length = ext.length; p->pos = ext.pos;
    p->line = ext.line; p->column = ext.column;
    p->cursor_previous = ext.cursor_previous; p->base = ext.base;
    p->references = ext.references; p->declarations = ext.declarations;
    p->total_work = ext.total_work;
    p->sources = ext.sources; p->source_count = ext.source_count;
    p->source_capacity = ext.source_capacity;
    p->source_names = ext.source_names;
    p->source_name_count = ext.source_name_count;
    p->source_name_capacity = ext.source_name_capacity;
    p->next_source_id = ext.next_source_id;
    p->parameter_entities = ext.parameter_entities;
    p->parameter_entity_count = ext.parameter_entity_count;
    p->parameter_entity_capacity = ext.parameter_entity_capacity;
    p->dtd_bindings = ext.dtd_bindings;
    p->dtd_binding_count = ext.dtd_binding_count;
    p->dtd_binding_capacity = ext.dtd_binding_capacity;
    p->base_uri = f->base_uri;
  }
  else p->err = ext.err;
  markup_buf_free(&ext.decoded);
  return status;
}

static GTEXT_XML_Status retain_syntax(xml_ctx * p, GTEXT_Markup_Node * doctype,
    GTEXT_Markup_DTD_Syntax_Kind kind, size_t start,
    const GTEXT_Markup_Location * at) {
  if (!doctype || p->stream) return GTEXT_XML_OK;
  GTEXT_Markup_Status status;
  if (p->dtd_generated_parameter_entity) {
    status = markup_doctype_append_generated_syntax(doctype, kind,
        (const char *)p->in + start, p->pos - start, at,
        p->dtd_generated_parameter_entity,
        p->source_count && p->sources[p->source_count - 1].external_source);
  }
  else if (p->source_count > 1
      && XML_SOURCE_IS_PARAMETER_ENTITY(p->sources[p->source_count - 1].kind)) {
    status = markup_doctype_append_generated_syntax(doctype, kind,
        (const char *)p->in + start, p->pos - start, at,
        p->sources[p->source_count - 1].owned_name,
        p->sources[p->source_count - 1].external_source);
  }
  else {
    status = markup_doctype_append_syntax(doctype, kind,
        (const char *)p->in + start, p->pos - start, at);
  }
  return xml_from_markup(p, status);
}

/* Past the '[' to past the ']'. */
static GTEXT_XML_Status skip_internal_subset(xml_ctx * p,
    GTEXT_Markup_Node * doctype) {
  static const char * const keywords[] = {
      "<!ELEMENT", "<!ATTLIST", "<!ENTITY", "<!NOTATION"};
  for (;;) {
    GTEXT_XML_Status status;
    if (p->err.code != GTEXT_XML_OK) {
      return p->err.code;
    }
    xml_skip_space(p);
    if (p->err.code != GTEXT_XML_OK) {
      return p->err.code;
    }
    if (p->pos >= p->length && p->source_count > 1
        && XML_SOURCE_IS_PARAMETER_ENTITY(p->sources[p->source_count - 1].kind)) {
      status = xml_source_pop(p);
      if (status != GTEXT_XML_OK) return status;
      continue;
    }
    if (p->pos >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated internal subset");
    }
    const unsigned char b = p->in[p->pos];
    if (b == ']') {
      XML_CONSUME_OR_RETURN(p, 1);
      return GTEXT_XML_OK;
    }
    if (b == '%') {
      const size_t start = p->pos;
      const GTEXT_Markup_Location at = xml_here(p);
      status = dtd_charge_reference(p);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, 1);
      size_t length = 0;
      status = xml_scan_name(p, &length);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, length);
      status = xml_expect(p, ";", 1, "a parameter entity reference ends with ';'");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      status = retain_syntax(p, doctype,
          GTEXT_MARKUP_DTD_PARAMETER_REFERENCE, start, &at);
      if (status != GTEXT_XML_OK) return status;
      if (p->dtd_mode != GTEXT_XML_DTD_PROCESS) continue;
      const unsigned char * reference_name = p->in + start + 1;
      const size_t reference_length = p->pos - start - 2;
      status = dtd_expand_parameter_entity(p, reference_name,
          reference_length, &at, doctype);
      if (status != GTEXT_XML_OK) return status;
      if (p->source_count > 1
          && p->sources[p->source_count - 1].external_source) {
        status = external_subset_body(p, doctype);
        if (status != GTEXT_XML_OK) return status;
      }
      continue;
    }
    if (b != '<') {
      return bad(p, "unexpected content in the internal subset");
    }
    if (xml_cut_literal(p, "<!--", 4) || xml_cut_literal(p, "<?", 2)) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    }
    const size_t start = p->pos;
    const GTEXT_Markup_Location at = xml_here(p);
    GTEXT_Markup_DTD_Syntax_Kind kind;
    if (xml_looking_at(p, "<!--", 4)) {
      kind = GTEXT_MARKUP_DTD_COMMENT;
      status = xml_parse_comment(p, 0);
    }
    else if (xml_looking_at(p, "<?", 2)) {
      kind = GTEXT_MARKUP_DTD_PROCESSING_INSTRUCTION;
      status = xml_parse_pi(p, 0);
    }
    else {
      size_t k = 0;
      for (; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        if (xml_looking_at(p, keywords[k], strlen(keywords[k]))) {
          break;
        }
        if (xml_cut_literal(p, keywords[k], strlen(keywords[k]))) {
          return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
        }
      }
      if (k == sizeof(keywords) / sizeof(keywords[0])) {
        return xml_fail(p, GTEXT_XML_E_DOCTYPE,
            "not a declaration, comment or processing instruction");
      }
      kind = (GTEXT_Markup_DTD_Syntax_Kind)k;
      const void * declaration_source_identity =
          p->sources[p->source_count - 1].identity;
      p->dtd_generated_parameter_entity = NULL;
      status = xml_charge(p, &p->declarations, p->max_declarations, 1,
          GTEXT_XML_E_LIMIT_DECLARATIONS, "DTD declaration limit exceeded");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, strlen(keywords[k]));
      if (dtd_skip_space(p) == 0) {
        if (p->err.code != GTEXT_XML_OK) return p->err.code;
        return bad(p, "white space is required after the declaration keyword");
      }
      status = p->dtd_mode == GTEXT_XML_DTD_PROCESS
          ? validate_declaration(p, (int)k)
          : skip_declaration(p, k == 2);
      if (status == GTEXT_XML_OK && p->dtd_mode == GTEXT_XML_DTD_PROCESS
          && (p->source_count == 0
              || p->sources[p->source_count - 1].identity
                  != declaration_source_identity)) {
        status = xml_fail_at(p, GTEXT_XML_E_DOCTYPE,
            "a markup declaration crosses a parameter entity boundary", &at);
      }
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
    status = retain_syntax(p, doctype, kind, start, &at);
    if (status != GTEXT_XML_OK) return status;
    if (kind <= GTEXT_MARKUP_DTD_NOTATION && p->dtd_mode == GTEXT_XML_DTD_PROCESS) {
      status = compile_decl_bindings(p, (int)kind, p->in + start,
          p->pos - start,
          p->source_count != 0 && p->sources[p->source_count - 1].external_source,
          doctype && !p->stream ? doctype : NULL, &at);
      if (status != GTEXT_XML_OK) return status;
    }
  }
}

GTEXT_XML_Status xml_parse_doctype(xml_ctx * p) {
  const GTEXT_Markup_Location start = xml_here(p);
  markup_buf name;
  markup_buf pub;
  markup_buf sys;
  markup_buf_init(&name, p->allocator);
  markup_buf_init(&pub, p->allocator);
  markup_buf_init(&sys, p->allocator);
  int has_public = 0;
  int has_system = 0;
  GTEXT_Markup_Node * doctype = NULL;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  size_t length = 0;

  status = xml_consume(p, 9); /* "<!DOCTYPE" */
  if (status != GTEXT_XML_OK) goto done;
  if (xml_skip_space(p) == 0) {
    status = bad(p, "white space is required after <!DOCTYPE");
    goto done;
  }
  status = xml_scan_name(p, &length);
  if (status != GTEXT_XML_OK) {
    goto done;
  }
  status = xml_append(p, &name, (const char *)p->in + p->pos, length);
  if (status != GTEXT_XML_OK) {
    goto done;
  }
  status = xml_consume(p, length);
  if (status != GTEXT_XML_OK) goto done;
  size_t space = xml_skip_space(p);

  if (xml_cut_literal(p, "SYSTEM", 6) || xml_cut_literal(p, "PUBLIC", 6)) {
    status = xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    goto done;
  }
  const int is_system = xml_looking_at(p, "SYSTEM", 6);
  const int is_public = !is_system && xml_looking_at(p, "PUBLIC", 6);
  if (is_system || is_public) {
    if (space == 0) {
      status = bad(p, "white space is required before the external identifier");
      goto done;
    }
    status = xml_consume(p, 6);
    if (status != GTEXT_XML_OK) goto done;
    if (xml_skip_space(p) == 0) {
      status = bad(p, "white space is required after SYSTEM or PUBLIC");
      goto done;
    }
    if (is_public) {
      status = literal(p, &pub, 1);
      if (status != GTEXT_XML_OK) {
        goto done;
      }
      has_public = 1;
      if (xml_skip_space(p) == 0) {
        status = bad(p, "white space is required before the system identifier");
        goto done;
      }
    }
    status = literal(p, &sys, 0);
    if (status != GTEXT_XML_OK) {
      goto done;
    }
    has_system = 1;
    space = xml_skip_space(p);
  }
  (void)space;

  if (p->dtd_mode == GTEXT_XML_DTD_PROCESS && !p->stream) {
    status = xml_from_markup(p, gtext_markup_create_doctype(p->doc, name.data,
        has_public ? pub.data : NULL, has_system ? sys.data : NULL, &doctype));
    if (status != GTEXT_XML_OK) goto done;
  }

  if (p->pos < p->length && p->in[p->pos] == '[') {
    status = xml_consume(p, 1);
    if (status != GTEXT_XML_OK) goto done;
    status = skip_internal_subset(p, doctype);
    if (status != GTEXT_XML_OK) {
      goto done;
    }
    xml_skip_space(p);
  }
  if (p->pos >= p->length || p->in[p->pos] != '>') {
    status = bad(p, "'>' was expected to end the document type declaration");
    goto done;
  }
  status = xml_consume(p, 1);
  if (status != GTEXT_XML_OK) goto done;

  /* External bytes are obtained only from a caller resolver after explicit
     per-parse authorization. Story 2.7 feeds the acquired source into the DTD
     grammar; this slice establishes the bounded read and ownership contract. */
  if (has_system) {
    status = xml_resolve_external_subset(p, doctype,
        has_public ? pub.data : NULL, sys.data);
    if (status != GTEXT_XML_OK) goto done;
  }

  if (doctype && p->dtd_mode == GTEXT_XML_DTD_PROCESS) {
    status = xml_doctype_rebuild_indexes(p, doctype);
    if (status != GTEXT_XML_OK) goto done;
  }

  if (p->stream) {
    status = xml_emit_doctype(p, &start, name.data,
        has_public ? pub.data : NULL, has_system ? sys.data : NULL);
  }
  else {
    GTEXT_Markup_Node * node = doctype;
    if (!node) status = xml_from_markup(p,
        gtext_markup_create_doctype(p->doc, name.data,
            has_public ? pub.data : NULL, has_system ? sys.data : NULL, &node));
    if (status == GTEXT_XML_OK) {
      xml_attach(p->doc->root, node, &start);
    }
  }

done:
  if (status != GTEXT_XML_OK && doctype) {
    gtext_markup_node_free(doctype);
  }
  markup_buf_free(&name);
  markup_buf_free(&pub);
  markup_buf_free(&sys);
  return status;
}
