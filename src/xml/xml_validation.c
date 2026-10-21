/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 */

#include <ghoti.io/text/macros.h>
#include "xml_internal.h"

#include <string.h>

typedef struct { size_t start, end; } fragment;
typedef struct {
  fragment value;
  unsigned char have_value;
  unsigned char separator;
  unsigned char connector;
  size_t items;
} group_frame;

static size_t state_bit_bytes(size_t state_count) {
  return state_count / 8 + (state_count % 8 != 0);
}

static GTEXT_XML_Status validation_charge(xml_ctx *p, size_t amount) {
  if (amount > p->max_validation_work
      || p->validation_work > p->max_validation_work - amount)
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation work limit exceeded");
  p->validation_work += amount;
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_validation_memory_reserve(xml_ctx *p, size_t amount) {
  if (amount > p->max_validation_memory
      || p->validation_memory > p->max_validation_memory - amount)
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation memory limit exceeded");
  p->validation_memory += amount;
  return GTEXT_XML_OK;
}

void xml_validation_memory_release(xml_ctx *p, size_t amount) {
  if (p && amount <= p->validation_memory) p->validation_memory -= amount;
}

static size_t validation_array_capacity(size_t capacity, size_t needed) {
  size_t result = capacity ? capacity : 8;
  while (result < needed) {
    if (result > SIZE_MAX / 2) return 0;
    result *= 2;
  }
  return result;
}

static int next_utf8_value(const unsigned char *s, size_t n, size_t *at,
    uint32_t *out) {
  if (*at >= n) return 0;
  const unsigned char lead = s[(*at)++];
  if (lead < 0x80) { *out = lead; return 1; }
  unsigned int more;
  uint32_t value;
  if (lead >= 0xc2 && lead <= 0xdf) { more = 1; value = lead & 0x1f; }
  else if (lead >= 0xe0 && lead <= 0xef) { more = 2; value = lead & 0x0f; }
  else if (lead >= 0xf0 && lead <= 0xf4) { more = 3; value = lead & 0x07; }
  else return 0;
  if (n - *at < more) return 0;
  for (unsigned int i = 0; i < more; ++i) {
    unsigned char byte = s[(*at)++];
    if ((byte & 0xc0) != 0x80) return 0;
    if (i == 0 && ((lead == 0xe0 && byte < 0xa0)
        || (lead == 0xed && byte >= 0xa0)
        || (lead == 0xf0 && byte < 0x90)
        || (lead == 0xf4 && byte >= 0x90))) return 0;
    value = (value << 6) | (byte & 0x3f);
  }
  *out = value;
  return 1;
}

static int xml_name_value(const char *s, size_t n) {
  size_t i = 0; int first = 1;
  if (!n) return 0;
  while (i < n) {
    uint32_t c;
    if (!next_utf8_value((const unsigned char *)s, n, &i, &c)
        || (first ? !markup_xml_name_start_char(c) : !markup_xml_name_char(c))) return 0;
    first = 0;
  }
  return 1;
}

static int xml_nmtoken_value(const char *s, size_t n) {
  size_t i = 0;
  if (!n) return 0;
  while (i < n) {
    uint32_t c;
    if (!next_utf8_value((const unsigned char *)s, n, &i, &c)
        || !markup_xml_name_char(c)) return 0;
  }
  return 1;
}

/* The DTD lookups below are index probes (xml_doctype.c), charged one unit
   each; they used to walk every binding and were charged for it. */
static const xml_dtd_binding * attribute_binding(const xml_ctx *p,
    const unsigned char *element, size_t element_length,
    const unsigned char *attribute, size_t attribute_length) {
  return xml_find_dtd_binding(p, GTEXT_MARKUP_DTD_ATTLIST, element,
      element_length, attribute, attribute_length);
}

static int value_in_enumeration(const xml_dtd_binding *b,
    const char *value, size_t length) {
  size_t at = 0;
  while (at < b->attribute_enumeration_length) {
    while (at < b->attribute_enumeration_length
        && (xml_is_space((unsigned char)b->attribute_enumeration[at])
            || b->attribute_enumeration[at] == '|')) ++at;
    size_t begin = at;
    while (at < b->attribute_enumeration_length
        && !xml_is_space((unsigned char)b->attribute_enumeration[at])
        && b->attribute_enumeration[at] != '|') ++at;
    if (at - begin == length && memcmp(b->attribute_enumeration + begin, value, length) == 0) return 1;
  }
  return 0;
}

static int declared_notation(const xml_ctx *p, const char *name, size_t n) {
  return xml_find_dtd_binding(p, GTEXT_MARKUP_DTD_NOTATION, NULL, 0,
      (const unsigned char *)name, n) != NULL;
}

static int declared_unparsed_entity(const xml_ctx *p, const char *name, size_t n) {
  const xml_general_entity *entity = xml_find_general_entity(p,
      (const unsigned char *)name, n);
  return entity && entity->unparsed;
}

static GTEXT_XML_Status retain_identity(xml_ctx *p, int reference,
    const char *value, size_t length, const GTEXT_Markup_Location *where) {
  char ***items = reference ? &p->validation_idrefs : &p->validation_ids;
  size_t **lengths = reference ? &p->validation_idref_lengths : &p->validation_id_lengths;
  size_t *count = reference ? &p->validation_idref_count : &p->validation_id_count;
  size_t *capacity = reference ? &p->validation_idref_capacity : &p->validation_id_capacity;
  if (validation_charge(p, length + 1) != GTEXT_XML_OK) return p->err.code;
  if (xml_validation_memory_reserve(p, length + 1) != GTEXT_XML_OK) return p->err.code;
  size_t next = validation_array_capacity(*capacity, *count + 1);
  if (!next || next > SIZE_MAX / sizeof(**items) || next > SIZE_MAX / sizeof(**lengths))
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK, "DTD identity table is too large");
  if (next != *capacity) {
    size_t per_entry = sizeof(**items) + sizeof(**lengths)
        + (reference ? sizeof(*p->validation_idref_locations) : 0);
    if (next > SIZE_MAX / per_entry)
      return xml_fail(p, GTEXT_XML_E_LIMIT_WORK, "DTD identity table is too large");
    size_t old_bytes = *capacity * per_entry;
    size_t new_bytes = next * per_entry;
    if (xml_validation_memory_reserve(p, new_bytes - old_bytes) != GTEXT_XML_OK) return p->err.code;
    void *a = xml_alloc(p, next * sizeof(**items));
    void *b = xml_alloc(p, next * sizeof(**lengths));
    if (!a || !b) { xml_release(p, a); xml_release(p, b); return xml_fail(p, GTEXT_XML_E_OOM, "could not grow DTD identity table"); }
    if (*count) { memcpy(a, *items, *count * sizeof(**items)); memcpy(b, *lengths, *count * sizeof(**lengths)); }
    xml_release(p, *items); xml_release(p, *lengths);
    *items = a; *lengths = b;
    if (reference) {
      GTEXT_Markup_Location *loc = xml_alloc(p, next * sizeof(*loc));
      if (!loc) return xml_fail(p, GTEXT_XML_E_OOM, "could not grow DTD IDREF locations");
      if (*count) memcpy(loc, p->validation_idref_locations, *count * sizeof(*loc));
      xml_release(p, p->validation_idref_locations);
      p->validation_idref_locations = loc;
    }
    *capacity = next;
  }
  char *copy = xml_alloc(p, length + 1);
  if (!copy) return xml_fail(p, GTEXT_XML_E_OOM, "could not retain DTD identity value");
  memcpy(copy, value, length); copy[length] = 0;
  (*items)[*count] = copy; (*lengths)[*count] = length;
  if (reference) p->validation_idref_locations[*count] = where ? *where : (GTEXT_Markup_Location){0};
  ++*count;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status validation_report(xml_ctx *p,
    GTEXT_XML_Validation_Code code, const GTEXT_Markup_Location *where,
    const char *message) {
  p->validation_has_violation = 1;
  if (p->doc) p->doc->xml_validation_has_violation = 1;
  if (p->validation_diagnostic_count >= p->max_validation_diagnostics) {
    if (!p->validation_diagnostics_truncated) {
      p->validation_diagnostics_truncated = 1;
      if (p->doc) p->doc->xml_validation_diagnostics_truncated = 1;
      if (p->stream) return xml_emit_validation_diagnostics_truncated(p, where);
    }
    return GTEXT_XML_OK;
  }
  ++p->validation_diagnostic_count;
  if (p->doc) {
    GTEXT_Markup_Document *doc = p->doc;
    size_t count = doc->xml_validation_diagnostic_count;
    if (count == SIZE_MAX / sizeof(*doc->xml_validation_diagnostics))
      return xml_fail(p, GTEXT_XML_E_OOM,
          "too many DTD validation diagnostics");
    void *grown = markup_realloc(doc, doc->xml_validation_diagnostics,
        (count + 1) * sizeof(*doc->xml_validation_diagnostics));
    if (!grown)
      return xml_fail(p, GTEXT_XML_E_OOM,
          "could not retain a DTD validation diagnostic");
    doc->xml_validation_diagnostics = grown;
    doc->xml_validation_diagnostics[count].code = (uint32_t)code;
    doc->xml_validation_diagnostics[count].location = where
        ? *where : (GTEXT_Markup_Location){0};
    doc->xml_validation_diagnostics[count].message = message;
    doc->xml_validation_diagnostic_count = count + 1;
    doc->xml_validation_diagnostics_truncated =
        p->validation_diagnostics_truncated;
  }
  if (p->stream)
    return xml_emit_validation_diagnostic(p, code, where, message);
  return GTEXT_XML_OK;
}

static void free_model(xml_ctx *p, xml_content_decl *m) {
  if (!m) return;
  xml_release(p, m->element_name);
  xml_release(p, m->declaration);
  xml_release(p, m->states);
  xml_release(p, m->mixed_names);
  if (m->validation_budget_bytes <= p->validation_memory)
    p->validation_memory -= m->validation_budget_bytes;
  memset(m, 0, sizeof(*m));
}

static GTEXT_XML_Status new_state(xml_ctx *p, xml_content_decl *m,
    size_t *out) {
  if (m->state_count >= UINT32_MAX)
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD content model has too many states");
  GTEXT_XML_Status status = xml_grow(p, (void **)&m->states,
      &m->state_capacity, sizeof(*m->states), m->state_count + 1);
  if (status != GTEXT_XML_OK) return status;
  *out = m->state_count++;
  memset(&m->states[*out], 0, sizeof(m->states[*out]));
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status edge(xml_ctx *p, xml_content_decl *m, size_t from,
    size_t to, size_t name_offset, size_t name_length) {
  if (from >= m->state_count || to >= m->state_count
      || m->states[from].edge_count >= 2)
    return xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "invalid compiled DTD content model");
  m->states[from].edges[m->states[from].edge_count++] =
      (xml_nfa_edge){to, name_offset, name_length};
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status epsilon(xml_ctx *p, xml_content_decl *m,
    size_t from, size_t to) {
  return edge(p, m, from, to, SIZE_MAX, 0);
}

static GTEXT_XML_Status named_fragment(xml_ctx *p, xml_content_decl *m,
    size_t offset, size_t length, fragment *out) {
  GTEXT_XML_Status status = new_state(p, m, &out->start);
  if (status == GTEXT_XML_OK) status = new_state(p, m, &out->end);
  if (status == GTEXT_XML_OK)
    status = edge(p, m, out->start, out->end, offset, length);
  return status;
}

static GTEXT_XML_Status combine(xml_ctx *p, xml_content_decl *m,
    fragment left, fragment right, unsigned char separator, fragment *out) {
  GTEXT_XML_Status status;
  if (separator == ',') {
    status = epsilon(p, m, left.end, right.start);
    *out = (fragment){left.start, right.end};
    return status;
  }
  size_t start = 0, end = 0;
  status = new_state(p, m, &start);
  if (status == GTEXT_XML_OK) status = new_state(p, m, &end);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, start, left.start);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, start, right.start);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, left.end, end);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, right.end, end);
  *out = (fragment){start, end};
  return status;
}

static GTEXT_XML_Status repeat_fragment(xml_ctx *p, xml_content_decl *m,
    fragment value, unsigned char operation, fragment *out) {
  if (!operation) { *out = value; return GTEXT_XML_OK; }
  size_t start = 0, end = 0;
  GTEXT_XML_Status status = new_state(p, m, &start);
  if (status == GTEXT_XML_OK) status = new_state(p, m, &end);
  if (status == GTEXT_XML_OK && operation != '+')
    status = epsilon(p, m, start, end);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, start, value.start);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, value.end, end);
  if (status == GTEXT_XML_OK && operation != '?')
    status = epsilon(p, m, value.end, value.start);
  *out = (fragment){start, end};
  return status;
}

static size_t skip_space(const unsigned char *s, size_t n, size_t at) {
  while (at < n && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n'))
    ++at;
  return at;
}

static GTEXT_XML_Status parse_children_model(xml_ctx *p, xml_content_decl *m,
    size_t at, size_t n) {
  group_frame *groups = NULL;
  size_t group_count = 0, group_capacity = 0;
  int expect_particle = 1;
  fragment completed = {0, 0};
  GTEXT_XML_Status status = GTEXT_XML_OK;
  at = skip_space((const unsigned char *)m->declaration, n, at);
  if (at >= n || m->declaration[at] != '(')
    return xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "a children content model must begin with a group");

  while (at < n && status == GTEXT_XML_OK) {
    const unsigned char *s = (const unsigned char *)m->declaration;
    at = skip_space(s, n, at);
    if (expect_particle) {
      if (at < n && s[at] == '(') {
        status = xml_grow(p, (void **)&groups, &group_capacity,
            sizeof(*groups), group_count + 1);
        if (status != GTEXT_XML_OK) break;
        if (group_count >= p->max_depth) {
          status = xml_fail(p, GTEXT_XML_E_LIMIT_DEPTH,
              "DTD content model nesting limit exceeded");
          break;
        }
        groups[group_count++] = (group_frame){0};
        ++at;
        continue;
      }
      size_t begin = at;
      while (at < n && !xml_is_space(s[at]) && s[at] != '(' && s[at] != ')'
          && s[at] != ',' && s[at] != '|' && s[at] != '?' && s[at] != '*'
          && s[at] != '+') ++at;
      if (at == begin) {
        status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
            "an element name was expected in a children content model");
        break;
      }
      fragment item;
      status = named_fragment(p, m, begin, at - begin, &item);
      if (status != GTEXT_XML_OK) break;
      if (at < n && (s[at] == '?' || s[at] == '*' || s[at] == '+')) {
        const unsigned char op = s[at++];
        status = repeat_fragment(p, m, item, op, &item);
        if (status != GTEXT_XML_OK) break;
      }
      group_frame *g = &groups[group_count - 1];
      if (!g->have_value) {
        g->value = item;
        g->have_value = 1;
      }
      else {
        if (!g->separator) {
          status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
              "a content group requires a connector");
          break;
        }
        fragment joined;
        status = combine(p, m, g->value, item, g->separator, &joined);
        if (status != GTEXT_XML_OK) break;
        g->value = joined;
        g->separator = 0;
      }
      ++g->items;
      expect_particle = 0;
      continue;
    }

    if (at >= n) break;
    if (s[at] == ',' || s[at] == '|') {
      group_frame *g = &groups[group_count - 1];
      if (g->connector && g->connector != s[at]) {
        status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
            "a content group cannot mix choice and sequence connectors");
        break;
      }
      g->connector = s[at];
      g->separator = s[at++];
      expect_particle = 1;
      continue;
    }
    if (s[at] == ')') {
      if (group_count == 0) {
        status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
            "unmatched content-model group close");
        break;
      }
      group_frame g = groups[--group_count];
      if (!g.have_value || g.separator
          || (g.connector == '|' && g.items < 2)) {
        status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
            "a children content group requires at least two particles");
        break;
      }
      ++at;
      if (at < n && (s[at] == '?' || s[at] == '*' || s[at] == '+')) {
        const unsigned char op = s[at++];
        status = repeat_fragment(p, m, g.value, op, &g.value);
        if (status != GTEXT_XML_OK) break;
      }
      if (group_count == 0) {
        completed = g.value;
        break;
      }
      group_frame *parent = &groups[group_count - 1];
      if (!parent->have_value) {
        parent->value = g.value;
        parent->have_value = 1;
      }
      else {
        if (!parent->separator) {
          status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
              "a content group requires a connector");
          break;
        }
        fragment joined;
        status = combine(p, m, parent->value, g.value,
            parent->separator, &joined);
        if (status != GTEXT_XML_OK) break;
        parent->value = joined;
        parent->separator = 0;
      }
      ++parent->items;
      expect_particle = 0;
      continue;
    }
    status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "unexpected token in children content model");
  }
  xml_release(p, groups);
  if (status != GTEXT_XML_OK) return status;
  if (group_count != 0)
    return xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "unterminated children content-model group");
  m->start_state = completed.start;
  m->accept_state = completed.end;
  m->kind = XML_CONTENT_CHILDREN;
  size_t start = 0, accept = 0;
  status = new_state(p, m, &start);
  if (status == GTEXT_XML_OK) status = new_state(p, m, &accept);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, start, completed.start);
  if (status == GTEXT_XML_OK) status = epsilon(p, m, completed.end, accept);
  if (status == GTEXT_XML_OK && at < n &&
      (m->declaration[at] == '?' || m->declaration[at] == '*'
       || m->declaration[at] == '+')) {
    const unsigned char op = (unsigned char)m->declaration[at];
    /* The outer group's occurrence indicator was already consumed by the
       close path; reaching one here means malformed retained syntax. */
    status = xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "unexpected occurrence indicator after content model");
    (void)op;
  }
  m->start_state = start;
  m->accept_state = accept;
  return status;
}

static GTEXT_XML_Status parse_mixed_model(xml_ctx *p, xml_content_decl *m,
    size_t at, size_t n) {
  const unsigned char *s = (const unsigned char *)m->declaration;
  at = skip_space(s, n, at);
  if (at + 7 > n || memcmp(s + at, "#PCDATA", 7) != 0)
    return xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "#PCDATA was expected in a mixed content model");
  at += 7;
  size_t capacity = 0;
  m->kind = XML_CONTENT_MIXED;
  for (;;) {
    at = skip_space(s, n, at);
    if (at >= n || s[at] == ')') break;
    if (s[at++] != '|')
      return xml_fail(p, GTEXT_XML_E_DOCTYPE,
          "'|' was expected in a mixed content model");
    at = skip_space(s, n, at);
    const size_t begin = at;
    while (at < n && !xml_is_space(s[at]) && s[at] != '|' && s[at] != ')')
      ++at;
    if (at == begin)
      return xml_fail(p, GTEXT_XML_E_DOCTYPE,
          "an element name was expected in a mixed content model");
    GTEXT_XML_Status status = xml_grow(p, (void **)&m->mixed_names,
        &capacity, sizeof(*m->mixed_names), m->mixed_name_count + 1);
    if (status != GTEXT_XML_OK) return status;
    m->mixed_names[m->mixed_name_count++] =
        (xml_content_name){begin, at - begin};
  }
  if (at >= n || s[at++] != ')')
    return xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "')' was expected to end a mixed content model");
  if (m->mixed_name_count && (at >= n || s[at++] != '*'))
    return xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "a mixed content model with element names must end with '*'");
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status check_determinism(xml_ctx *p, xml_content_decl *m);
static GTEXT_XML_Status ensure_scratch(xml_ctx *p, size_t state_count);

static GTEXT_XML_Status compile_content_decl(xml_ctx *p,
    const unsigned char *source, size_t length,
    const GTEXT_Markup_Location *location, int external,
    xml_content_decl *out) {
  memset(out, 0, sizeof(*out));
  out->location = location ? *location : (GTEXT_Markup_Location){0};
  out->external = (unsigned char)external;
  out->declaration = xml_alloc(p, length + 1);
  if (!out->declaration)
    return xml_fail(p, GTEXT_XML_E_OOM,
        "could not retain element content declaration");
  memcpy(out->declaration, source, length);
  out->declaration[length] = 0;
  out->declaration_length = length;
  size_t at = 9; /* <!ELEMENT */
  while (at < length && xml_is_space(source[at])) ++at;
  const size_t name_at = at;
  while (at < length && !xml_is_space(source[at]) && source[at] != '>') ++at;
  const size_t name_length = at - name_at;
  if (!name_length) return xml_fail(p, GTEXT_XML_E_DOCTYPE,
      "an element type name was expected in its declaration");
  out->element_name = xml_alloc(p, name_length + 1);
  if (!out->element_name)
    return xml_fail(p, GTEXT_XML_E_OOM,
        "could not retain declared element name");
  memcpy(out->element_name, source + name_at, name_length);
  out->element_name[name_length] = 0;
  out->element_name_length = name_length;
  at = skip_space(source, length, at);
  const size_t remaining = length - at;
  if (remaining >= 5 && memcmp(source + at, "EMPTY", 5) == 0) {
    out->kind = XML_CONTENT_EMPTY;
    return GTEXT_XML_OK;
  }
  if (remaining >= 3 && memcmp(source + at, "ANY", 3) == 0) {
    out->kind = XML_CONTENT_ANY;
    return GTEXT_XML_OK;
  }
  if (at >= length || source[at] != '(')
    return xml_fail(p, GTEXT_XML_E_DOCTYPE,
        "an element content specification was expected");
  size_t after_open = skip_space(source, length, at + 1);
  if (after_open + 7 <= length
      && memcmp(source + after_open, "#PCDATA", 7) == 0)
    return parse_mixed_model(p, out, after_open, length);
  return parse_children_model(p, out, at, length);
}

static xml_content_decl *find_model(xml_ctx *p,
    const unsigned char *name, size_t length) {
  for (size_t i = 0; i < p->content_decl_count; ++i) {
    xml_content_decl *m = &p->content_decls[i];
    if (m->element_name_length == length
        && memcmp(m->element_name, name, length) == 0) return m;
  }
  return NULL;
}

GTEXT_XML_Status xml_validation_add_element_decl(xml_ctx *p,
    const unsigned char *source, size_t length,
    const GTEXT_Markup_Location *location, int external) {
  if (p->dtd_mode != GTEXT_XML_DTD_VALIDATE || p->dtd_suppress_declaration_effects)
    return GTEXT_XML_OK;
  GTEXT_XML_Status status = validation_charge(p, length);
  if (status != GTEXT_XML_OK) return status;
  /* Reserve before compilation: the NFA and mixed-name arrays grow with the
   * declaration length, and the factor covers geometric capacity growth. */
  const size_t per_byte = 2 * sizeof(xml_nfa_state)
      + sizeof(xml_content_name) + 2;
  if (length > (SIZE_MAX - sizeof(xml_content_decl)) / per_byte)
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation model memory estimate overflow");
  const size_t model_budget = sizeof(xml_content_decl) + length * per_byte;
  status = xml_validation_memory_reserve(p, model_budget);
  if (status != GTEXT_XML_OK) return status;
  xml_content_decl model;
  status = compile_content_decl(p, source, length,
      location, external, &model);
  if (status != GTEXT_XML_OK) {
    free_model(p, &model);
    p->validation_memory -= model_budget;
    return status;
  }
  model.validation_budget_bytes = model_budget;
  xml_content_decl *old = find_model(p,
      (const unsigned char *)model.element_name, model.element_name_length);
  if (old) {
    if (old->declaration_length == model.declaration_length
        && memcmp(old->declaration, model.declaration,
            model.declaration_length) == 0
        && old->location.source_id == model.location.source_id
        && old->location.offset == model.location.offset) {
      free_model(p, &model); /* The final index rebuild saw this same record. */
      return GTEXT_XML_OK;
    }
    if (!old->duplicate_reported) {
      status = validation_report(p,
          GTEXT_XML_VALIDATION_DUPLICATE_ELEMENT_DECLARATION, location,
          "an element type is declared more than once");
      if (status != GTEXT_XML_OK) { free_model(p, &model); return status; }
      old->duplicate_reported = 1;
    }
    if (old->external && !external) {
      free_model(p, old);
      model.duplicate_reported = 1;
      *old = model;
    }
    else free_model(p, &model);
    return GTEXT_XML_OK;
  }
  size_t new_capacity = validation_array_capacity(p->content_decl_capacity,
      p->content_decl_count + 1);
  if (!new_capacity || new_capacity > SIZE_MAX / sizeof(*p->content_decls)) {
    free_model(p, &model);
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation declaration index overflow");
  }
  const size_t new_bytes = new_capacity * sizeof(*p->content_decls);
  const size_t extra_bytes = new_bytes > p->validation_content_decls_bytes
      ? new_bytes - p->validation_content_decls_bytes : 0;
  status = xml_validation_memory_reserve(p, extra_bytes);
  if (status != GTEXT_XML_OK) { free_model(p, &model); return status; }
  status = xml_grow(p, (void **)&p->content_decls,
      &p->content_decl_capacity, sizeof(*p->content_decls),
      p->content_decl_count + 1);
  if (status != GTEXT_XML_OK) {
    p->validation_memory -= extra_bytes;
    free_model(p, &model);
    return status;
  }
  p->validation_content_decls_bytes = new_bytes;
  p->content_decls[p->content_decl_count++] = model;
  return check_determinism(p, &p->content_decls[p->content_decl_count - 1]);
}

static int bit_is_set(const unsigned char *bits, size_t n) {
  return (bits[n >> 3] & (unsigned char)(1u << (n & 7))) != 0;
}

static void bit_set(unsigned char *bits, size_t n) {
  bits[n >> 3] |= (unsigned char)(1u << (n & 7));
}

static GTEXT_XML_Status ensure_scratch(xml_ctx *p, size_t state_count) {
  const size_t bytes = state_bit_bytes(state_count);
  if (state_count > SIZE_MAX / sizeof(size_t))
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation matcher size overflow");
  const size_t list_bytes = state_count * sizeof(size_t);
  if (bytes <= p->validation_scratch_bytes
      && list_bytes <= p->validation_worklist_bytes) return GTEXT_XML_OK;
  const size_t old = p->validation_scratch_bytes
      + p->validation_worklist_bytes;
  const size_t new_bits = bytes > p->validation_scratch_bytes
      ? bytes : p->validation_scratch_bytes;
  const size_t new_list = list_bytes > p->validation_worklist_bytes
      ? list_bytes : p->validation_worklist_bytes;
  if (new_bits > SIZE_MAX - new_list)
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation matcher size overflow");
  const size_t total = new_bits + new_list;
  if (total > p->max_validation_memory
      || p->validation_memory > p->max_validation_memory - (total - old))
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation memory limit exceeded");
  unsigned char *scratch = xml_alloc(p, new_bits);
  size_t *worklist = xml_alloc(p, new_list);
  if (!scratch || !worklist) {
    xml_release(p, scratch); xml_release(p, worklist);
    return xml_fail(p, GTEXT_XML_E_OOM,
        "could not allocate DTD validation matcher state");
  }
  memset(scratch, 0, new_bits);
  xml_release(p, p->validation_scratch);
  xml_release(p, p->validation_worklist);
  p->validation_scratch = scratch;
  p->validation_worklist = worklist;
  p->validation_worklist_capacity = state_count;
  p->validation_memory += total - old;
  p->validation_scratch_bytes = new_bits;
  p->validation_worklist_bytes = new_list;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status epsilon_close(xml_ctx *p, const xml_content_decl *m,
    unsigned char *bits) {
  size_t count = 0;
  for (size_t i = 0; i < m->state_count; ++i) {
    GTEXT_XML_Status charge = validation_charge(p, 1);
    if (charge != GTEXT_XML_OK) return charge;
    if (bit_is_set(bits, i)) p->validation_worklist[count++] = i;
  }
  size_t cursor = 0;
  while (cursor < count) {
    const size_t current = p->validation_worklist[cursor++];
    GTEXT_XML_Status status = validation_charge(p, 1);
    if (status != GTEXT_XML_OK) return status;
    const xml_nfa_state *state = &m->states[current];
    for (size_t e = 0; e < state->edge_count; ++e) {
      const xml_nfa_edge *edge = &state->edges[e];
      if (edge->name_offset == SIZE_MAX && !bit_is_set(bits, edge->target)) {
        bit_set(bits, edge->target);
        p->validation_worklist[count++] = edge->target;
      }
    }
  }
  return GTEXT_XML_OK;
}

/* Check the XML determinism constraint over every reachable prefix, not only
 * prefixes present in the current document. The subset exploration is finite;
 * both its comparisons and retained subset storage are charged to the caller's
 * validation budgets. */
static GTEXT_XML_Status check_determinism(xml_ctx *p, xml_content_decl *m) {
  const size_t bytes = state_bit_bytes(m->state_count);
  unsigned char *sets = NULL;
  size_t count = 0, capacity = 0;
  GTEXT_XML_Status status = ensure_scratch(p, m->state_count);
  if (status != GTEXT_XML_OK) return status;
  if (bytes > p->max_validation_memory
      || p->validation_memory > p->max_validation_memory - bytes)
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD determinism memory limit exceeded");
  sets = xml_alloc(p, bytes);
  if (!sets) return xml_fail(p, GTEXT_XML_E_OOM,
      "could not allocate DTD determinism state");
  capacity = 1;
  p->validation_memory += bytes;
  memset(sets, 0, bytes);
  bit_set(sets, m->start_state);
  status = epsilon_close(p, m, sets);
  if (status != GTEXT_XML_OK) goto done;
  count = 1;

  for (size_t cursor = 0; cursor < count && status == GTEXT_XML_OK; ++cursor) {
    unsigned char *current = sets + cursor * bytes;
    for (size_t i = 0; i < m->state_count && status == GTEXT_XML_OK; ++i) {
      status = validation_charge(p, 1);
      if (status != GTEXT_XML_OK) break;
      if (!bit_is_set(current, i)) continue;
      const xml_nfa_state *state = &m->states[i];
      for (size_t e = 0; e < state->edge_count; ++e) {
        const xml_nfa_edge *a = &state->edges[e];
        if (a->name_offset == SIZE_MAX) continue;
        for (size_t j = i; j < m->state_count; ++j) {
          status = validation_charge(p, 1);
          if (status != GTEXT_XML_OK) break;
          if (!bit_is_set(current, j)) continue;
          const xml_nfa_state *other = &m->states[j];
          for (size_t f = (j == i ? e + 1 : 0); f < other->edge_count; ++f) {
            const xml_nfa_edge *b = &other->edges[f];
            if (b->name_offset == SIZE_MAX) continue;
            status = validation_charge(p, 1);
            if (status != GTEXT_XML_OK) break;
            if (a->name_length == b->name_length
                && memcmp(m->declaration + a->name_offset,
                    m->declaration + b->name_offset, a->name_length) == 0) {
              if (!m->ambiguity_reported) {
                m->ambiguity_reported = 1;
                status = validation_report(p,
                    GTEXT_XML_VALIDATION_NONDETERMINISTIC_MODEL, &m->location,
                    "content model is nondeterministic");
              }
              break;
            }
          }
          if (status != GTEXT_XML_OK) break;
        }
        if (status != GTEXT_XML_OK) break;
      }
    }
    if (status != GTEXT_XML_OK) break;

    /* Each unique symbol in this subset produces one successor subset. */
    for (size_t i = 0; i < m->state_count && status == GTEXT_XML_OK; ++i) {
      status = validation_charge(p, 1);
      if (status != GTEXT_XML_OK) break;
      if (!bit_is_set(current, i)) continue;
      const xml_nfa_state *state = &m->states[i];
      for (size_t e = 0; e < state->edge_count && status == GTEXT_XML_OK; ++e) {
        const xml_nfa_edge *edge = &state->edges[e];
        if (edge->name_offset == SIZE_MAX) continue;
        int earlier = 0;
        for (size_t j = 0; j <= i && !earlier; ++j) {
          status = validation_charge(p, 1);
          if (status != GTEXT_XML_OK) break;
          if (!bit_is_set(current, j)) continue;
          const xml_nfa_state *prior = &m->states[j];
          const size_t edge_limit = j == i ? e : prior->edge_count;
          for (size_t f = 0; f < edge_limit; ++f) {
            const xml_nfa_edge *candidate = &prior->edges[f];
            if (candidate->name_offset != SIZE_MAX
                && candidate->name_length == edge->name_length
                && memcmp(m->declaration + candidate->name_offset,
                    m->declaration + edge->name_offset,
                    edge->name_length) == 0) { earlier = 1; break; }
          }
        }
        if (earlier) continue;
        if (bytes > p->max_validation_memory
            || p->validation_memory > p->max_validation_memory - bytes) {
          status = xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
              "DTD determinism memory limit exceeded");
          break;
        }
        unsigned char *next = xml_alloc(p, bytes);
        if (!next) { status = xml_fail(p, GTEXT_XML_E_OOM,
            "could not allocate DTD determinism state"); break; }
        p->validation_memory += bytes;
        memset(next, 0, bytes);
        for (size_t j = 0; j < m->state_count; ++j) {
          status = validation_charge(p, 1);
          if (status != GTEXT_XML_OK) break;
          if (!bit_is_set(current, j)) continue;
          const xml_nfa_state *from = &m->states[j];
          for (size_t f = 0; f < from->edge_count; ++f) {
            status = validation_charge(p, 1);
            if (status != GTEXT_XML_OK) break;
            const xml_nfa_edge *candidate = &from->edges[f];
            if (candidate->name_offset != SIZE_MAX
                && candidate->name_length == edge->name_length
                && memcmp(m->declaration + candidate->name_offset,
                    m->declaration + edge->name_offset,
                    edge->name_length) == 0)
              bit_set(next, candidate->target);
          }
          if (status != GTEXT_XML_OK) break;
        }
        if (status == GTEXT_XML_OK) status = epsilon_close(p, m, next);
        if (status != GTEXT_XML_OK) { xml_release(p, next); break; }
        int seen = 0;
        for (size_t k = 0; k < count; ++k) {
          status = validation_charge(p, bytes);
          if (status != GTEXT_XML_OK) break;
          if (memcmp(sets + k * bytes, next, bytes) == 0) { seen = 1; break; }
        }
        if (status == GTEXT_XML_OK && !seen) {
          if (count == SIZE_MAX / bytes) {
            status = xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
                "DTD determinism state count overflow");
          }
          else {
            if (count == capacity) {
              size_t new_capacity = capacity > SIZE_MAX / 2
                  ? SIZE_MAX : capacity * 2;
              if (new_capacity < count + 1) new_capacity = count + 1;
              if (new_capacity > SIZE_MAX / bytes) {
                status = xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
                    "DTD determinism state count overflow");
              }
              else {
                const size_t new_bytes = new_capacity * bytes;
                if (new_bytes > p->max_validation_memory
                    || p->validation_memory > p->max_validation_memory - new_bytes)
                  status = xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
                      "DTD determinism memory limit exceeded");
                else {
                  unsigned char *grown = xml_alloc(p, new_bytes);
                  if (!grown) status = xml_fail(p, GTEXT_XML_E_OOM,
                      "could not grow DTD determinism states");
                  else {
                    memcpy(grown, sets, count * bytes);
                    xml_release(p, sets);
                    p->validation_memory -= capacity * bytes;
                    p->validation_memory += new_bytes;
                    sets = grown;
                    capacity = new_capacity;
                    current = sets + cursor * bytes;
                  }
                }
              }
            }
            if (status == GTEXT_XML_OK) {
              memcpy(sets + count * bytes, next, bytes);
              ++count;
            }
          }
        }
        xml_release(p, next);
        p->validation_memory -= bytes;
      }
    }
  }
done:
  xml_release(p, sets);
  p->validation_memory -= capacity * bytes;
  return status;
}

static GTEXT_XML_Status frame_push(xml_ctx *p, const xml_content_decl *m,
    const GTEXT_Markup_Location *where) {
  const size_t new_capacity = validation_array_capacity(
      p->validation_frame_capacity, p->validation_frame_count + 1);
  if (!new_capacity || new_capacity > SIZE_MAX / sizeof(*p->validation_frames))
    return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
        "DTD validation frame stack overflow");
  const size_t new_bytes = new_capacity * sizeof(*p->validation_frames);
  const size_t extra_bytes = new_bytes > p->validation_frames_bytes
      ? new_bytes - p->validation_frames_bytes : 0;
  GTEXT_XML_Status status = xml_validation_memory_reserve(p, extra_bytes);
  if (status != GTEXT_XML_OK) return status;
  status = xml_grow(p, (void **)&p->validation_frames,
      &p->validation_frame_capacity, sizeof(*p->validation_frames),
      p->validation_frame_count + 1);
  if (status != GTEXT_XML_OK) {
    p->validation_memory -= extra_bytes;
    return status;
  }
  p->validation_frames_bytes = new_bytes;
  xml_validation_frame frame = {0};
  frame.model = m;
  frame.location = where ? *where : (GTEXT_Markup_Location){0};
  if (m && m->kind == XML_CONTENT_CHILDREN) {
    frame.active_bytes = state_bit_bytes(m->state_count);
    if (frame.active_bytes > p->max_validation_memory
        || p->validation_memory > p->max_validation_memory - frame.active_bytes)
      return xml_fail(p, GTEXT_XML_E_LIMIT_WORK,
          "DTD validation memory limit exceeded");
    frame.active = xml_alloc(p, frame.active_bytes);
    if (!frame.active)
      return xml_fail(p, GTEXT_XML_E_OOM,
          "could not allocate DTD validation element state");
    memset(frame.active, 0, frame.active_bytes);
    p->validation_memory += frame.active_bytes;
    bit_set(frame.active, m->start_state);
    status = ensure_scratch(p, m->state_count);
    if (status == GTEXT_XML_OK) status = epsilon_close(p, m, frame.active);
    if (status != GTEXT_XML_OK) {
      p->validation_memory -= frame.active_bytes;
      xml_release(p, frame.active);
      return status;
    }
  }
  p->validation_frames[p->validation_frame_count++] = frame;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status finish_frame(xml_ctx *p,
    const GTEXT_Markup_Location *where) {
  if (p->validation_frame_count == 0) return GTEXT_XML_OK;
  xml_validation_frame *frame =
      &p->validation_frames[p->validation_frame_count - 1];
  const xml_content_decl *m = frame->model;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  if (m && m->kind == XML_CONTENT_CHILDREN
      && !bit_is_set(frame->active, m->accept_state))
    status = validation_report(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT,
        where ? where : &frame->location,
        "element content does not match its DTD declaration");
  p->validation_memory -= frame->active_bytes;
  xml_release(p, frame->active);
  --p->validation_frame_count;
  return status;
}

static GTEXT_XML_Status match_child(xml_ctx *p, xml_validation_frame *frame,
    const unsigned char *name, size_t length,
    const GTEXT_Markup_Location *where) {
  const xml_content_decl *m = frame->model;
  if (!m || m->kind == XML_CONTENT_ANY) return GTEXT_XML_OK;
  if (m->kind == XML_CONTENT_EMPTY)
    return validation_report(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT, where,
        "an EMPTY element contains a child element");
  if (m->kind == XML_CONTENT_MIXED) {
    for (size_t i = 0; i < m->mixed_name_count; ++i) {
      const xml_content_name *entry = &m->mixed_names[i];
      if (entry->name_length == length
          && memcmp(m->declaration + entry->name_offset, name, length) == 0)
        return GTEXT_XML_OK;
    }
    return validation_report(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT, where,
        "child element is not allowed by the mixed content declaration");
  }
  memset(p->validation_scratch, 0, frame->active_bytes);
  size_t matches = 0;
  for (size_t i = 0; i < m->state_count; ++i) {
    GTEXT_XML_Status status = validation_charge(p, 1);
    if (status != GTEXT_XML_OK) return status;
    if (!bit_is_set(frame->active, i)) continue;
    const xml_nfa_state *state = &m->states[i];
    for (size_t e = 0; e < state->edge_count; ++e) {
      const xml_nfa_edge *edge = &state->edges[e];
      if (edge->name_offset != SIZE_MAX && edge->name_length == length
          && memcmp(m->declaration + edge->name_offset, name, length) == 0) {
        bit_set(p->validation_scratch, edge->target);
        ++matches;
        status = validation_charge(p, 1);
        if (status != GTEXT_XML_OK) return status;
      }
    }
  }
  if (matches > 1 && !((xml_content_decl *)m)->ambiguity_reported) {
    if (!((xml_content_decl *)m)->ambiguity_reported) {
      ((xml_content_decl *)m)->ambiguity_reported = 1;
      GTEXT_XML_Status status = validation_report(p,
          GTEXT_XML_VALIDATION_NONDETERMINISTIC_MODEL, &m->location,
          "content model is nondeterministic");
      if (status != GTEXT_XML_OK) return status;
    }
  }
  if (!matches) {
    GTEXT_XML_Status status = validation_report(p,
        GTEXT_XML_VALIDATION_ELEMENT_CONTENT, where,
        "child element does not match its parent's content model");
    if (status != GTEXT_XML_OK) return status;
  }
  GTEXT_XML_Status status = epsilon_close(p, m, p->validation_scratch);
  if (status != GTEXT_XML_OK) return status;
  memcpy(frame->active, p->validation_scratch, frame->active_bytes);
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_validation_start_element(xml_ctx *p,
    const unsigned char *name, size_t name_length,
    const GTEXT_Markup_Location *location, int is_empty,
    const xml_attr *attrs, size_t attr_count, const markup_buf *attr_pool) {
  if (p->dtd_mode != GTEXT_XML_DTD_VALIDATE) return GTEXT_XML_OK;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  const xml_dtd_binding *first = xml_first_attribute_binding(p, name,
      name_length);
  for (size_t bi = first ? (size_t)(first - p->dtd_bindings) : SIZE_MAX;
       bi != SIZE_MAX; bi = p->dtd_bindings[bi].next_attribute) {
    status = validation_charge(p, 1);
    if (status != GTEXT_XML_OK) return status;
    const xml_dtd_binding *b = &p->dtd_bindings[bi];
    if (!b->attribute_required) continue;
    int found = 0;
    for (size_t ai = 0; ai < attr_count; ++ai) {
      status = validation_charge(p, 1);
      if (status != GTEXT_XML_OK) return status;
      if (attrs[ai].name_length == b->name_length
          && memcmp(attrs[ai].name_ptr, b->name, b->name_length) == 0) { found = 1; break; }
    }
    if (!found) {
      status = validation_report(p, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE,
          location, "a required DTD attribute is missing");
      if (status != GTEXT_XML_OK) return status;
    }
  }
  for (size_t ai = 0; ai < attr_count; ++ai) {
    status = validation_charge(p, 1);
    if (status != GTEXT_XML_OK) return status;
    status = validation_charge(p, attrs[ai].value_length);
    if (status != GTEXT_XML_OK) return status;
    const xml_attr *a = &attrs[ai];
    const xml_dtd_binding *b = attribute_binding(p, name, name_length,
        a->name_ptr, a->name_length);
    if (!b) continue;
    const char *value = attr_pool->data + a->value_offset;
    size_t length = a->value_length;
    int valid = 1;
    if (b->attribute_default_kind == 2 && !a->defaulted
        && !a->fixed_value_matches) {
      status = validation_report(p, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE,
          &a->where, "an attribute value differs from its DTD #FIXED value");
      if (status != GTEXT_XML_OK) return status;
    }
    if (b->attribute_type == XML_ATYPE_ENUMERATION) {
      status = validation_charge(p, b->attribute_enumeration_length);
      if (status != GTEXT_XML_OK) return status;
      if (!value_in_enumeration(b, value, length)) valid = 0;
    }
    if (b->attribute_type == XML_ATYPE_ID || b->attribute_type == XML_ATYPE_IDREF
        || b->attribute_type == XML_ATYPE_ENTITY || b->attribute_type == XML_ATYPE_NOTATION)
      valid = xml_name_value(value, length);
    if (b->attribute_type == XML_ATYPE_NMTOKEN) {
      valid = xml_nmtoken_value(value, length);
    }
    if (b->attribute_type == XML_ATYPE_IDREFS || b->attribute_type == XML_ATYPE_ENTITIES
        || b->attribute_type == XML_ATYPE_NMTOKENS) {
      size_t at = 0, tokens = 0;
      while (at < length) {
        while (at < length && xml_is_space((unsigned char)value[at])) ++at;
        if (at == length) break;
        size_t begin = at;
        while (at < length && !xml_is_space((unsigned char)value[at])) ++at;
        ++tokens;
        if (b->attribute_type == XML_ATYPE_ENTITIES) {
          status = validation_charge(p, 1);
          if (status != GTEXT_XML_OK) return status;
        }
        if (b->attribute_type == XML_ATYPE_NMTOKENS) {
          if (!xml_nmtoken_value(value + begin, at - begin)) valid = 0;
        }
        else if (!xml_name_value(value + begin, at - begin)) valid = 0;
        if (valid && b->attribute_type == XML_ATYPE_ENTITIES
            && !declared_unparsed_entity(p, value + begin, at - begin)) valid = 0;
        if (valid && b->attribute_type == XML_ATYPE_IDREFS)
          status = retain_identity(p, 1, value + begin, at - begin, &a->where);
        if (status != GTEXT_XML_OK) return status;
      }
      if (!tokens) valid = 0;
    }
    if (b->attribute_type == XML_ATYPE_IDREF && valid) {
      status = retain_identity(p, 1, value, length, &a->where);
      if (status != GTEXT_XML_OK) return status;
    }
    if (valid && b->attribute_type == XML_ATYPE_ENTITY
        && (validation_charge(p, 1) != GTEXT_XML_OK
            || !declared_unparsed_entity(p, value, length))) {
      if (p->err.code != GTEXT_XML_OK) return p->err.code;
      valid = 0;
    }
    if (valid && b->attribute_type == XML_ATYPE_NOTATION
        && (validation_charge(p, 1) != GTEXT_XML_OK
            || !declared_notation(p, value, length))) {
      if (p->err.code != GTEXT_XML_OK) return p->err.code;
      valid = 0;
    }
    if (valid && b->attribute_type == XML_ATYPE_NOTATION) {
      status = validation_charge(p, b->attribute_enumeration_length);
      if (status != GTEXT_XML_OK) return status;
      if (!value_in_enumeration(b, value, length)) valid = 0;
    }
    if (b->attribute_type == XML_ATYPE_ID && valid) {
      for (size_t i = 0; i < p->validation_id_count; ++i) {
        status = validation_charge(p, length + 1);
        if (status != GTEXT_XML_OK) return status;
        if (p->validation_id_lengths[i] == length
            && memcmp(p->validation_ids[i], value, length) == 0) {
          status = validation_report(p, GTEXT_XML_VALIDATION_DUPLICATE_ID,
              &a->where, "an ID attribute value is not unique");
          if (status != GTEXT_XML_OK) return status;
          break;
        }
      }
      status = retain_identity(p, 0, value, length, &a->where);
      if (status != GTEXT_XML_OK) return status;
    }
    if (!valid) {
      status = validation_report(p, GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE,
          &a->where, "an attribute value does not satisfy its DTD type");
      if (status != GTEXT_XML_OK) return status;
    }
    if (p->standalone == GTEXT_MARKUP_STANDALONE_YES && a->defaulted && b->external) {
      status = validation_report(p, GTEXT_XML_VALIDATION_STANDALONE,
          &a->where, "an external DTD declaration supplied an attribute default in a standalone document");
      if (status != GTEXT_XML_OK) return status;
    }
    if (p->standalone == GTEXT_MARKUP_STANDALONE_YES
        && !a->defaulted && a->normalization_changed && b->external) {
      status = validation_report(p, GTEXT_XML_VALIDATION_STANDALONE,
          &a->where, "an external DTD declaration changed attribute normalization in a standalone document");
      if (status != GTEXT_XML_OK) return status;
    }
  }
  if (p->validation_frame_count) {
    xml_validation_frame *parent =
        &p->validation_frames[p->validation_frame_count - 1];
    parent->child_location = location ? *location : (GTEXT_Markup_Location){0};
    status = match_child(p, parent, name, name_length, location);
    if (status != GTEXT_XML_OK) return status;
  }
  else if (p->validation_doctype_name
      && (p->validation_doctype_name_length != name_length
          || memcmp(p->validation_doctype_name, name, name_length) != 0)) {
    status = validation_report(p, GTEXT_XML_VALIDATION_ROOT_TYPE, location,
        "document element does not match the document type name");
    if (status != GTEXT_XML_OK) return status;
  }
  xml_content_decl *model = find_model(p, name, name_length);
  if (!model && p->validation_doctype_name
      && !p->dtd_knowledge_incomplete) {
    status = validation_report(p, GTEXT_XML_VALIDATION_UNDECLARED_ELEMENT,
        location, "element type has no DTD declaration");
    if (status != GTEXT_XML_OK) return status;
  }
  const size_t prior_count = p->validation_frame_count;
  status = frame_push(p, model, location);
  if (status != GTEXT_XML_OK) return status;
  if (is_empty) return finish_frame(p, location);
  if (prior_count < p->validation_frame_count)
    p->validation_frames[prior_count].location = location
        ? *location : (GTEXT_Markup_Location){0};
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_validation_end_element(xml_ctx *p,
    const unsigned char *name, size_t name_length,
    const GTEXT_Markup_Location *location) {
  (void)name; (void)name_length;
  if (p->dtd_mode != GTEXT_XML_DTD_VALIDATE) return GTEXT_XML_OK;
  return finish_frame(p, location);
}

GTEXT_XML_Status xml_validation_text(xml_ctx *p, const char *text,
    size_t length, const GTEXT_Markup_Location *location, int cdata) {
  (void)cdata; /* Validity depends on the characters, not their lexical form. */
  if (p->dtd_mode != GTEXT_XML_DTD_VALIDATE || !p->validation_frame_count)
    return GTEXT_XML_OK;
  xml_validation_frame *frame =
      &p->validation_frames[p->validation_frame_count - 1];
  const xml_content_decl *m = frame->model;
  if (!m || m->kind == XML_CONTENT_ANY || m->kind == XML_CONTENT_MIXED)
    return GTEXT_XML_OK;
  if (m->kind == XML_CONTENT_EMPTY)
    return validation_report(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT, location,
        "an EMPTY element contains character data");
  int non_space = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
      non_space = 1;
  }
  if (non_space)
    return validation_report(p, GTEXT_XML_VALIDATION_ELEMENT_CONTENT, location,
        "character data is not allowed by an element-only content model");
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_validation_finish(xml_ctx *p) {
  if (p->dtd_mode != GTEXT_XML_DTD_VALIDATE) return GTEXT_XML_OK;
  /* A well-formed document without a DOCTYPE has no required DTD evidence
     against which its validity can be established. Keep that knowledge gap
     distinct from operation success. */
  if (!p->validation_doctype_name) {
    p->dtd_knowledge_incomplete = 1;
    if (p->doc) p->doc->dtd_knowledge_incomplete = 1;
  }
  for (size_t i = 0; i < p->general_entity_count; ++i) {
    GTEXT_XML_Status charged = validation_charge(p, 1);
    if (charged != GTEXT_XML_OK) return charged;
    const xml_general_entity *entity = &p->general_entities[i];
    if (!entity->unparsed) continue;
    charged = validation_charge(p, 1);
    if (charged != GTEXT_XML_OK) return charged;
    if (!declared_notation(p, entity->notation_name,
        entity->notation_name_length)) {
      GTEXT_XML_Status s = validation_report(p,
          GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE,
          &entity->declaration_location,
          "an unparsed entity names an undeclared notation");
      if (s != GTEXT_XML_OK) return s;
    }
  }
  for (size_t i = 0; i < p->dtd_binding_count; ++i) {
    GTEXT_XML_Status charged = validation_charge(p, 1);
    if (charged != GTEXT_XML_OK) return charged;
    const xml_dtd_binding *b = &p->dtd_bindings[i];
    if (b->kind != GTEXT_MARKUP_DTD_ATTLIST) continue;
    if (b->attribute_type == XML_ATYPE_ID) {
      if (b->attribute_default_kind != 0) {
        GTEXT_XML_Status s = validation_report(p,
            GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE,
            &b->declaration_location,
            "an ID attribute declaration may only be #IMPLIED or #REQUIRED");
        if (s != GTEXT_XML_OK) return s;
      }
      /* The element's earlier attribute bindings, from its chain. */
      const xml_dtd_binding *head = xml_first_attribute_binding(p,
          (const unsigned char *)b->element_name, b->element_name_length);
      for (size_t j = head ? (size_t)(head - p->dtd_bindings) : SIZE_MAX;
           j != SIZE_MAX && j < i; j = p->dtd_bindings[j].next_attribute) {
        charged = validation_charge(p, 1);
        if (charged != GTEXT_XML_OK) return charged;
        const xml_dtd_binding *prior = &p->dtd_bindings[j];
        if (prior->attribute_type == XML_ATYPE_ID) {
          GTEXT_XML_Status s = validation_report(p,
              GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE,
              &b->declaration_location,
              "an element type has more than one ID attribute declaration");
          if (s != GTEXT_XML_OK) return s;
          break;
        }
      }
    }
    if (b->attribute_type == XML_ATYPE_NOTATION) {
      /* The element's earlier attribute bindings, from its chain. */
      const xml_dtd_binding *head = xml_first_attribute_binding(p,
          (const unsigned char *)b->element_name, b->element_name_length);
      for (size_t j = head ? (size_t)(head - p->dtd_bindings) : SIZE_MAX;
           j != SIZE_MAX && j < i; j = p->dtd_bindings[j].next_attribute) {
        charged = validation_charge(p, 1);
        if (charged != GTEXT_XML_OK) return charged;
        const xml_dtd_binding *prior = &p->dtd_bindings[j];
        if (prior->attribute_type == XML_ATYPE_NOTATION) {
          GTEXT_XML_Status s = validation_report(p,
              GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE,
              &b->declaration_location,
              "an element type has more than one NOTATION attribute declaration");
          if (s != GTEXT_XML_OK) return s;
          break;
        }
      }
      size_t at = 0;
      charged = validation_charge(p, b->attribute_enumeration_length);
      if (charged != GTEXT_XML_OK) return charged;
      while (at < b->attribute_enumeration_length) {
        while (at < b->attribute_enumeration_length
            && (xml_is_space((unsigned char)b->attribute_enumeration[at])
                || b->attribute_enumeration[at] == '|')) ++at;
        size_t begin = at;
        while (at < b->attribute_enumeration_length
            && !xml_is_space((unsigned char)b->attribute_enumeration[at])
            && b->attribute_enumeration[at] != '|') ++at;
        if (at > begin && !declared_notation(p,
            b->attribute_enumeration + begin, at - begin)) {
          GTEXT_XML_Status s = validation_report(p,
              GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE,
              &b->declaration_location,
              "a NOTATION attribute names an undeclared notation");
          if (s != GTEXT_XML_OK) return s;
        }
      }
    }
  }
  for (size_t i = 0; i < p->validation_idref_count; ++i) {
    int found = 0;
    for (size_t j = 0; j < p->validation_id_count; ++j) {
      GTEXT_XML_Status s = validation_charge(p, p->validation_idref_lengths[i] + 1);
      if (s != GTEXT_XML_OK) return s;
      if (p->validation_idref_lengths[i] == p->validation_id_lengths[j]
          && memcmp(p->validation_idrefs[i], p->validation_ids[j],
              p->validation_id_lengths[j]) == 0) { found = 1; break; }
    }
    if (!found) {
      GTEXT_XML_Status s = validation_report(p,
          GTEXT_XML_VALIDATION_UNRESOLVED_IDREF,
          &p->validation_idref_locations[i],
          "an IDREF value has no matching ID");
      if (s != GTEXT_XML_OK) return s;
    }
  }
  if (p->doc) {
    p->doc->xml_validation_ran = 1;
    p->doc->xml_validation_stale = 0;
  }
  return GTEXT_XML_OK;
}

void xml_validation_result_from_context(const xml_ctx *p,
    GTEXT_XML_Validation_Result *out) {
  if (!out) return;
  *out = (GTEXT_XML_Validation_Result){
      .verdict = GTEXT_XML_VALIDATION_NOT_RUN,
      .knowledge_complete = !p->dtd_knowledge_incomplete,
      .diagnostics_truncated = p->validation_diagnostics_truncated,
      .diagnostic_count = p->validation_diagnostic_count};
  if (p->dtd_mode != GTEXT_XML_DTD_VALIDATE) return;
  if (p->validation_has_violation)
    out->verdict = GTEXT_XML_VALIDATION_INVALID;
  else if (p->dtd_knowledge_incomplete)
    out->verdict = GTEXT_XML_VALIDATION_INCOMPLETE;
  else
    out->verdict = GTEXT_XML_VALIDATION_VALID;
}

GTEXT_XML_Status xml_validation_external_entity_reference(xml_ctx *p,
    const xml_general_entity *entity, const GTEXT_Markup_Location *where) {
  if (p->dtd_mode != GTEXT_XML_DTD_VALIDATE
      || p->standalone != GTEXT_MARKUP_STANDALONE_YES
      || !entity || !entity->external_context) return GTEXT_XML_OK;
  return validation_report(p, GTEXT_XML_VALIDATION_STANDALONE, where,
      "a document declared standalone references an entity declared by an external markup declaration");
}

void xml_validation_cleanup(xml_ctx *p) {
  for (size_t i = 0; i < p->content_decl_count; ++i)
    free_model(p, &p->content_decls[i]);
  xml_release(p, p->content_decls);
  p->validation_memory -= p->validation_content_decls_bytes;
  p->validation_content_decls_bytes = 0;
  p->content_decls = NULL;
  p->content_decl_count = p->content_decl_capacity = 0;
  for (size_t i = 0; i < p->validation_frame_count; ++i)
    xml_release(p, p->validation_frames[i].active);
  xml_release(p, p->validation_frames);
  p->validation_memory -= p->validation_frames_bytes;
  p->validation_frames_bytes = 0;
  xml_release(p, p->validation_scratch);
  xml_release(p, p->validation_worklist);
  xml_release(p, p->validation_doctype_name);
  for (size_t i = 0; i < p->validation_id_count; ++i) xml_release(p, p->validation_ids[i]);
  for (size_t i = 0; i < p->validation_idref_count; ++i) xml_release(p, p->validation_idrefs[i]);
  xml_release(p, p->validation_ids); xml_release(p, p->validation_id_lengths);
  xml_release(p, p->validation_idrefs); xml_release(p, p->validation_idref_lengths);
  xml_release(p, p->validation_idref_locations);
  p->validation_frames = NULL;
  p->validation_scratch = NULL;
  p->validation_worklist = NULL;
  p->validation_doctype_name = NULL;
  p->validation_ids = p->validation_idrefs = NULL;
  p->validation_id_lengths = p->validation_idref_lengths = NULL;
  p->validation_idref_locations = NULL;
}

size_t gtext_xml_validation_diagnostic_count(
    const GTEXT_Markup_Document *document) {
  return document && !document->xml_validation_stale
      ? document->xml_validation_diagnostic_count : 0;
}

bool gtext_xml_validation_diagnostic(const GTEXT_Markup_Document *document,
    size_t index, GTEXT_XML_Validation_Diagnostic *out) {
  if (!document || document->xml_validation_stale || !out
      || index >= document->xml_validation_diagnostic_count)
    return false;
  const markup_xml_validation_diagnostic *source =
      &document->xml_validation_diagnostics[index];
  *out = (GTEXT_XML_Validation_Diagnostic){
      (GTEXT_XML_Validation_Code)source->code, source->location,
      source->message};
  return true;
}

bool gtext_xml_validation_diagnostics_truncated(
    const GTEXT_Markup_Document *document) {
  return document && !document->xml_validation_stale
      && document->xml_validation_diagnostics_truncated;
}

bool gtext_xml_validation_result(const GTEXT_Markup_Document *document,
    GTEXT_XML_Validation_Result *out) {
  if (!document || !out || !document->xml_validation_ran
      || document->xml_validation_stale) return false;
  GTEXT_XML_Validation_Result result = {
      .verdict = GTEXT_XML_VALIDATION_NOT_RUN,
      .knowledge_complete = !(document->dtd_knowledge_incomplete
          || document->dtd_knowledge_stale),
      .diagnostics_truncated = document->xml_validation_diagnostics_truncated,
      .diagnostic_count = document->xml_validation_diagnostic_count};
  if (document->xml_validation_has_violation)
    result.verdict = GTEXT_XML_VALIDATION_INVALID;
  else if (document->dtd_knowledge_incomplete || document->dtd_knowledge_stale)
    result.verdict = GTEXT_XML_VALIDATION_INCOMPLETE;
  else
    result.verdict = GTEXT_XML_VALIDATION_VALID;
  *out = result;
  return true;
}

typedef struct {
  const GTEXT_Markup_Node *node;
  char *name;
  size_t name_length;
  GTEXT_Markup_Location location;
} revalidation_open;

static char *revalidation_qname(xml_ctx *p, const markup_atom *prefix,
    const markup_atom *local, size_t *length) {
  if (!local) return NULL;
  size_t prefix_length = 0;
  if (prefix) {
    if (prefix->length == SIZE_MAX) return NULL;
    prefix_length = prefix->length + 1;
  }
  if (prefix_length >= SIZE_MAX
      || local->length > SIZE_MAX - prefix_length - 1) return NULL;
  *length = local->length + prefix_length;
  if (validation_charge(p, *length + 1) != GTEXT_XML_OK
      || xml_validation_memory_reserve(p, *length + 1) != GTEXT_XML_OK)
    return NULL;
  char *name = markup_alloc(p->doc, *length + 1);
  if (!name) { p->validation_memory -= *length + 1; return NULL; }
  size_t at = 0;
  if (prefix) {
    memcpy(name, prefix->text, prefix->length);
    at = prefix->length;
    name[at++] = ':';
  }
  memcpy(name + at, local->text, local->length);
  name[*length] = 0;
  return name;
}

static void revalidation_qname_free(xml_ctx *p, char *name, size_t length) {
  if (!name) return;
  markup_free(p->doc, name);
  p->validation_memory -= length + 1;
}

static int revalidation_is_ancestor(xml_ctx *p,
    const GTEXT_Markup_Node *ancestor, const GTEXT_Markup_Node *node) {
  for (const GTEXT_Markup_Node *at = node; at; at = at->parent) {
    if (validation_charge(p, 1) != GTEXT_XML_OK) return 0;
    if (at == ancestor) return 1;
  }
  return 0;
}

GTEXT_XML_Status gtext_xml_revalidate(GTEXT_Markup_Document *doc,
    const GTEXT_XML_Revalidation_Options *options,
    GTEXT_XML_Validation_Result *result, GTEXT_XML_Error *error) {
  if (error) memset(error, 0, sizeof(*error));
  if (!doc || !result) return GTEXT_XML_E_INVALID;
  GTEXT_XML_Revalidation_Options use = options ? *options
      : (GTEXT_XML_Revalidation_Options){0};
  xml_ctx p;
  memset(&p, 0, sizeof(p));
  p.doc = doc;
  p.allocator = &doc->allocator;
  p.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  p.standalone = doc->standalone;
  p.max_validation_work = use.max_work ? use.max_work
      : XML_DEFAULT_MAX_VALIDATION_WORK;
  p.max_total_work = p.max_validation_work;
  p.max_depth = XML_DEFAULT_MAX_DEPTH;
  p.max_name_bytes = XML_DEFAULT_MAX_NAME_BYTES;
  p.max_validation_memory = use.max_memory ? use.max_memory
      : XML_DEFAULT_MAX_VALIDATION_MEMORY;
  p.max_validation_diagnostics = use.max_diagnostics ? use.max_diagnostics
      : XML_DEFAULT_MAX_VALIDATION_DIAGNOSTICS;
  p.dtd_knowledge_incomplete = doc->dtd_knowledge_incomplete
      || doc->dtd_knowledge_stale;

  /* Stage all result writes in a shallow document view. The old successful
     result remains untouched if any checked work or allocation fails. */
  GTEXT_Markup_Document staged = *doc;
  staged.xml_validation_diagnostics = NULL;
  staged.xml_validation_diagnostic_count = 0;
  staged.xml_validation_diagnostics_truncated = 0;
  staged.xml_validation_ran = 0;
  staged.xml_validation_has_violation = 0;
  staged.xml_validation_stale = 0;
  p.doc = &staged;

  size_t doctype_name_budget = 0;
  GTEXT_Markup_Node *doctype = gtext_markup_document_doctype(doc);
  if (doctype) {
    markup_doctype replay_doctype = *(markup_doctype *)doctype;
    replay_doctype.bindings = NULL;
    replay_doctype.binding_count = replay_doctype.binding_capacity = 0;
    replay_doctype.validation_memory_used = &p.validation_memory;
    replay_doctype.validation_memory_limit = p.max_validation_memory;
    GTEXT_Markup_Node *replay_node = (GTEXT_Markup_Node *)&replay_doctype;
    const char *doctype_name = gtext_markup_doctype_name(doctype);
    if (doctype_name) {
      p.validation_doctype_name_length = strlen(doctype_name);
      if (p.validation_doctype_name_length == SIZE_MAX) {
        p.err.code = GTEXT_XML_E_LIMIT_WORK;
      }
      else if (xml_validation_memory_reserve(&p,
              p.validation_doctype_name_length + 1) != GTEXT_XML_OK) {
        p.err.code = GTEXT_XML_E_LIMIT_WORK;
      }
      else {
        doctype_name_budget = p.validation_doctype_name_length + 1;
        p.validation_doctype_name = xml_alloc(&p, doctype_name_budget);
      }
      if (!p.validation_doctype_name && p.err.code == GTEXT_XML_OK) {
        p.err.code = GTEXT_XML_E_OOM;
        p.err.message = "out of memory preparing revalidation";
      }
      else if (p.validation_doctype_name) memcpy(p.validation_doctype_name, doctype_name,
          p.validation_doctype_name_length + 1);
    }
    if (p.err.code == GTEXT_XML_OK) {
      const GTEXT_XML_Status rebuilt = xml_doctype_rebuild_indexes(&p, replay_node);
      if (rebuilt != GTEXT_XML_OK) p.err.code = rebuilt;
      else {
        p.validation_replay_entities_active = 1;
        const GTEXT_XML_Status entities =
            xml_doctype_rebuild_general_entities(&p, replay_node);
        p.validation_replay_entities_active = 0;
        if (entities != GTEXT_XML_OK) p.err.code = entities;
      }
    }
    markup_doctype_free_bindings(replay_node);
  }

  revalidation_open *stack = NULL;
  uint32_t *external_sources_seen = NULL;
  size_t external_sources_seen_count = 0;
  size_t external_sources_budget = 0;
  if (doc->standalone == GTEXT_MARKUP_STANDALONE_YES && doc->source_count) {
    if (doc->source_count > SIZE_MAX / sizeof(*external_sources_seen)) {
      p.err.code = GTEXT_XML_E_LIMIT_WORK;
    }
    else if ((external_sources_budget = doc->source_count
          * sizeof(*external_sources_seen),
        xml_validation_memory_reserve(&p, external_sources_budget)) != GTEXT_XML_OK) {
      p.err.code = GTEXT_XML_E_LIMIT_WORK;
    }
    else {
      if (validation_charge(&p, doc->source_count) != GTEXT_XML_OK)
        p.err.code = GTEXT_XML_E_LIMIT_WORK;
      if (p.err.code == GTEXT_XML_OK)
        external_sources_seen = xml_alloc(&p,
            doc->source_count * sizeof(*external_sources_seen));
      if (!external_sources_seen && p.err.code == GTEXT_XML_OK) {
        p.err.code = GTEXT_XML_E_OOM;
        p.err.message = "out of memory tracking external source provenance";
      }
    }
  }
  size_t depth = 0, capacity = 0;
  if (p.err.code == GTEXT_XML_OK) {
    GTEXT_Markup_Node *root = gtext_markup_document_root(doc);
    for (GTEXT_Markup_Node *node = gtext_markup_node_next_in_order(root, root,
            false); node; node = gtext_markup_node_next_in_order(root, node,
            false)) {
      if (p.validation_work >= p.max_validation_work) {
        p.err.code = GTEXT_XML_E_LIMIT_WORK;
        p.err.message = "tree revalidation work limit exceeded";
        break;
      }
      p.validation_work++;
      if (external_sources_seen && p.err.code == GTEXT_XML_OK) {
        GTEXT_Markup_Location source_location = {0};
        if (gtext_markup_node_location(node, &source_location)
            && source_location.source_id) {
          GTEXT_Markup_Source source_info;
          if (gtext_markup_document_source(doc, source_location.source_id,
                  &source_info)
              && source_info.kind == GTEXT_MARKUP_SOURCE_EXTERNAL_ENTITY) {
            int declared_externally = 0;
            if (source_info.name && validation_charge(&p, 1) == GTEXT_XML_OK) {
              size_t source_name_length = 0;
              while (source_info.name[source_name_length]) {
                if (validation_charge(&p, 1) != GTEXT_XML_OK) break;
                ++source_name_length;
              }
              const xml_general_entity *entity = p.err.code == GTEXT_XML_OK
                  ? xml_find_general_entity(&p,
                      (const unsigned char *)source_info.name,
                      source_name_length) : NULL;
              declared_externally = entity && entity->external_context;
            }
            int known = 0;
            for (size_t i = 0; i < external_sources_seen_count; ++i) {
              if (validation_charge(&p, 1) != GTEXT_XML_OK) break;
              if (external_sources_seen[i] == source_location.source_id) { known = 1; break; }
            }
            if (declared_externally && !known && p.err.code == GTEXT_XML_OK) {
              external_sources_seen[external_sources_seen_count++] =
                  source_location.source_id;
              GTEXT_XML_Status reported = validation_report(&p,
                  GTEXT_XML_VALIDATION_STANDALONE, &source_location,
                  "a document declared standalone references an entity declared by an external markup declaration");
              if (reported != GTEXT_XML_OK) p.err.code = reported;
            }
          }
        }
      }
      if (p.err.code != GTEXT_XML_OK) break;
      while (depth && !revalidation_is_ancestor(&p, stack[depth - 1].node, node)) {
        if (p.err.code != GTEXT_XML_OK) break;
        revalidation_open *open = &stack[--depth];
        GTEXT_XML_Status s = xml_validation_end_element(&p,
            (const unsigned char *)open->name, open->name_length,
            &open->location);
        revalidation_qname_free(&p, open->name, open->name_length);
        if (s != GTEXT_XML_OK) { p.err.code = s; break; }
      }
      if (p.err.code != GTEXT_XML_OK) break;
      const GTEXT_Markup_Kind kind = gtext_markup_node_kind(node);
      if (kind == GTEXT_MARKUP_KIND_ELEMENT) {
        markup_element *element = (markup_element *)node;
        size_t name_length = 0;
        char *name = revalidation_qname(&p, element->prefix, element->local,
            &name_length);
        if (!name) { if (p.err.code == GTEXT_XML_OK) p.err.code = GTEXT_XML_E_OOM; p.err.message = "out of memory walking tree"; break; }
        if (depth == capacity) {
          size_t next = capacity ? capacity * 2 : 16;
          if (next < capacity || next > SIZE_MAX / sizeof(*stack)
              || xml_validation_memory_reserve(&p,
                  (next - capacity) * sizeof(*stack)) != GTEXT_XML_OK) {
            revalidation_qname_free(&p, name, name_length); p.err.code = GTEXT_XML_E_LIMIT_WORK;
            p.err.message = "tree revalidation memory limit exceeded"; break;
          }
          revalidation_open *grown = markup_realloc(doc, stack,
              next * sizeof(*stack));
          if (!grown) { p.validation_memory -= (next - capacity) * sizeof(*stack); revalidation_qname_free(&p, name, name_length); p.err.code = GTEXT_XML_E_OOM; p.err.message = "out of memory walking tree"; break; }
          stack = grown; capacity = next;
        }
        GTEXT_Markup_Location where = {0};
        (void)gtext_markup_node_location(node, &where);
        if (element->attr_count > SIZE_MAX / sizeof(xml_attr)) {
          revalidation_qname_free(&p, name, name_length);
          p.err.code = GTEXT_XML_E_LIMIT_WORK; break;
        }
        size_t attr_budget = element->attr_count * sizeof(xml_attr);
        if (xml_validation_memory_reserve(&p, attr_budget) != GTEXT_XML_OK) {
          revalidation_qname_free(&p, name, name_length); p.err.code = GTEXT_XML_E_LIMIT_WORK; break;
        }
        xml_attr *attrs = element->attr_count ? xml_alloc(&p, attr_budget) : NULL;
        if (validation_charge(&p, element->attr_count) != GTEXT_XML_OK) {
          xml_release(&p, attrs); p.validation_memory -= attr_budget;
          revalidation_qname_free(&p, name, name_length); break;
        }
        size_t pool_size = 0;
        for (size_t i = 0; i < element->attr_count; ++i) {
          markup_attr *a = &element->attrs[i];
          if (!a->local || (a->prefix && a->prefix->length == SIZE_MAX)) {
            p.err.code = GTEXT_XML_E_LIMIT_WORK; break;
          }
          size_t prefix_length = a->prefix ? a->prefix->length + 1 : 0;
          if (prefix_length > SIZE_MAX - a->local->length) {
            p.err.code = GTEXT_XML_E_LIMIT_WORK; break;
          }
          size_t qn = a->local->length + prefix_length;
          if (pool_size == SIZE_MAX || qn > SIZE_MAX - pool_size - 1) { p.err.code = GTEXT_XML_E_LIMIT_WORK; break; }
          pool_size += qn + 1;
        }
        int pool_reserved = 0;
        if (p.err.code == GTEXT_XML_OK && pool_size) {
          if (xml_validation_memory_reserve(&p, pool_size) != GTEXT_XML_OK)
            p.err.code = GTEXT_XML_E_LIMIT_WORK;
          else pool_reserved = 1;
        }
        char *pool_data = p.err.code == GTEXT_XML_OK && pool_size
            ? xml_alloc(&p, pool_size) : NULL;
        if (p.err.code == GTEXT_XML_OK
            && ((element->attr_count && !attrs) || (pool_size && !pool_data))) {
          p.err.code = GTEXT_XML_E_OOM;
        }
        markup_buf pool = {.data = pool_data, .length = pool_size,
            .capacity = pool_size};
        size_t off = 0;
        if (p.err.code == GTEXT_XML_OK) for (size_t i = 0; i < element->attr_count; ++i) {
          markup_attr *a = &element->attrs[i];
          size_t qn = 0;
          char *qname = revalidation_qname(&p, a->prefix, a->local, &qn);
          if (!qname) { if (p.err.code == GTEXT_XML_OK) p.err.code = GTEXT_XML_E_OOM; break; }
          memcpy(pool_data + off, qname, qn + 1);
          attrs[i] = (xml_attr){.name_length = qn, .name_ptr = (unsigned char *)pool_data + off,
              .value_offset = 0, .value_length = a->value_length,
              .where = {0}, .defaulted = a->origin == GTEXT_MARKUP_ATTRIBUTE_DEFAULTED,
              .normalization_changed = a->normalization_changed};
          const xml_dtd_binding *binding = attribute_binding(&p,
              (const unsigned char *)name, name_length,
              attrs[i].name_ptr, qn);
          if (binding && binding->attribute_default_kind == 2
              && binding->attribute_default_length == a->value_length
              && memcmp(binding->attribute_default_value, a->value,
                  a->value_length) == 0)
            attrs[i].fixed_value_matches = 1;
          off += qn + 1;
          revalidation_qname_free(&p, qname, qn);
        }
        /* Validation consumes a shared value pool. Build it after lexical names. */
        if (p.err.code == GTEXT_XML_OK) {
          size_t values = 0;
          for (size_t i = 0; i < element->attr_count; ++i) {
            if (element->attrs[i].value_length > SIZE_MAX - values) { p.err.code = GTEXT_XML_E_LIMIT_WORK; break; }
            values += element->attrs[i].value_length;
          }
          int values_reserved = 0;
          if (values && p.err.code == GTEXT_XML_OK) {
            if (xml_validation_memory_reserve(&p, values) != GTEXT_XML_OK)
              p.err.code = GTEXT_XML_E_LIMIT_WORK;
            else values_reserved = 1;
          }
          char *value_pool = p.err.code == GTEXT_XML_OK && values
              ? xml_alloc(&p, values) : NULL;
          if (values && p.err.code == GTEXT_XML_OK && !value_pool) {
            p.err.code = GTEXT_XML_E_OOM;
          }
          if (values && !value_pool && p.err.code == GTEXT_XML_OK)
            p.err.code = GTEXT_XML_E_OOM;
          if (p.err.code == GTEXT_XML_OK) {
            size_t value_at = 0;
            for (size_t i = 0; i < element->attr_count; ++i) {
              attrs[i].value_offset = value_at;
              if (element->attrs[i].value_length)
                memcpy(value_pool + value_at, element->attrs[i].value,
                    element->attrs[i].value_length);
              value_at += element->attrs[i].value_length;
              (void)gtext_markup_element_attribute_location(node, i, &attrs[i].where);
            }
            pool.data = value_pool; pool.length = values; pool.capacity = values;
            GTEXT_XML_Status s = xml_validation_start_element(&p,
                (const unsigned char *)name, name_length, &where,
                node->first_child == NULL, attrs, element->attr_count, &pool);
            if (s != GTEXT_XML_OK) p.err.code = s;
          }
          xml_release(&p, value_pool);
          if (values_reserved) p.validation_memory -= values;
        }
        xml_release(&p, attrs); xml_release(&p, pool_data);
        if (pool_reserved) p.validation_memory -= pool_size;
        p.validation_memory -= attr_budget;
        if (p.err.code != GTEXT_XML_OK) { revalidation_qname_free(&p, name, name_length); break; }
        stack[depth++] = (revalidation_open){node, name, name_length, where};
      }
      else if (kind == GTEXT_MARKUP_KIND_TEXT || kind == GTEXT_MARKUP_KIND_CDATA) {
        size_t length = 0; const char *text = gtext_markup_node_data(node, &length);
        GTEXT_Markup_Location where = {0}; (void)gtext_markup_node_location(node, &where);
        GTEXT_XML_Status s = xml_validation_text(&p, text, length, &where,
            kind == GTEXT_MARKUP_KIND_CDATA);
        if (s != GTEXT_XML_OK) p.err.code = s;
      }
      else if (kind == GTEXT_MARKUP_KIND_ENTITY_REFERENCE) {
        p.dtd_knowledge_incomplete = 1;
      }
      if (p.err.code != GTEXT_XML_OK) break;
    }
  }
  while (depth) {
    revalidation_open *open = &stack[--depth];
    if (p.err.code == GTEXT_XML_OK) {
      GTEXT_XML_Status s = xml_validation_end_element(&p,
          (const unsigned char *)open->name, open->name_length, &open->location);
      if (s != GTEXT_XML_OK) p.err.code = s;
    }
    revalidation_qname_free(&p, open->name, open->name_length);
  }
  if (capacity <= SIZE_MAX / sizeof(*stack))
    p.validation_memory -= capacity * sizeof(*stack);
  markup_free(doc, stack);
  xml_release(&p, external_sources_seen);
  if (external_sources_budget) p.validation_memory -= external_sources_budget;
  if (p.err.code == GTEXT_XML_OK) p.err.code = xml_validation_finish(&p);

  if (p.err.code == GTEXT_XML_OK) {
    staged.xml_validation_stale = 0;
    staged.xml_validation_ran = 1;
    staged.xml_validation_has_violation = p.validation_has_violation;
    staged.xml_validation_diagnostics_truncated = p.validation_diagnostics_truncated;
    markup_free(doc, doc->xml_validation_diagnostics);
    doc->xml_validation_diagnostics = staged.xml_validation_diagnostics;
    doc->xml_validation_diagnostic_count = staged.xml_validation_diagnostic_count;
    doc->xml_validation_diagnostics_truncated = staged.xml_validation_diagnostics_truncated;
    doc->xml_validation_ran = staged.xml_validation_ran;
    doc->xml_validation_has_violation = staged.xml_validation_has_violation;
    doc->xml_validation_stale = 0;
    doc->dtd_knowledge_incomplete |= staged.dtd_knowledge_incomplete;
    xml_validation_result_from_context(&p, result);
    if (doc->xml_validation_has_violation) result->verdict = GTEXT_XML_VALIDATION_INVALID;
    else if (doc->dtd_knowledge_incomplete || doc->dtd_knowledge_stale) result->verdict = GTEXT_XML_VALIDATION_INCOMPLETE;
    else result->verdict = GTEXT_XML_VALIDATION_VALID;
    result->knowledge_complete = !(doc->dtd_knowledge_incomplete || doc->dtd_knowledge_stale);
    result->diagnostic_count = doc->xml_validation_diagnostic_count;
    result->diagnostics_truncated = doc->xml_validation_diagnostics_truncated;
    staged.xml_validation_diagnostics = NULL;
  }
  else {
    if (error) { error->code = p.err.code; error->message = p.err.message; }
    markup_free(doc, staged.xml_validation_diagnostics);
  }
  p.doc = &staged;
  xml_validation_cleanup(&p);
  xml_source_free(&p);
  xml_release(&p, p.validation_doctype_name);
  xml_validation_memory_release(&p, doctype_name_budget);
  xml_validation_memory_release(&p, p.validation_replay_entity_bytes);
  xml_validation_memory_release(&p, p.validation_replay_entity_array_bytes);
  return p.err.code;
}

static void refresh_free_syntax(GTEXT_Markup_Document *doc,
    markup_dtd_syntax *items, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    markup_free(doc, items[i].source);
    markup_free(doc, items[i].parameter_entity);
  }
  markup_free(doc, items);
}

static void refresh_free_bindings(GTEXT_Markup_Document *doc,
    markup_dtd_binding *items, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    markup_free(doc, items[i].element_name);
    markup_free(doc, items[i].name);
  }
  markup_free(doc, items);
}

static uint32_t refresh_remap_source(uint32_t id, uint32_t offset) {
  if (!id) return 0;
  if (id == 1) return 1;
  return id - 1 > UINT32_MAX - offset ? 0 : id - 1 + offset;
}

static void refresh_remap_location(GTEXT_Markup_Location *location,
    uint32_t offset) {
  location->source_id = refresh_remap_source(location->source_id, offset);
  location->reference_source_id = refresh_remap_source(
      location->reference_source_id, offset);
}

static uint32_t refresh_max_source_id(const GTEXT_Markup_Document *doc) {
  uint32_t max = 0;
  for (size_t i = 0; i < doc->source_count; ++i)
    if (doc->sources[i].view.id > max) max = doc->sources[i].view.id;
  return max;
}

static int refresh_append_literal(markup_buf *b, const char *s) {
  return s && markup_buf_append(b, s, strlen(s)) == GTEXT_MARKUP_OK;
}

static int refresh_append_quoted(markup_buf *b, const char *s) {
  if (!s) return 0;
  const char *quote = strchr(s, '\'') ? "\"" : "'";
  return markup_buf_append(b, quote, 1) == GTEXT_MARKUP_OK
      && markup_buf_append(b, s, strlen(s)) == GTEXT_MARKUP_OK
      && markup_buf_append(b, quote, 1) == GTEXT_MARKUP_OK;
}

/* Build a private document that sends the current internal subset through the
 * ordinary parser together with a fresh external subset. The live document is
 * not touched until parsing, index construction, and revalidation all finish. */
GTEXT_XML_Status gtext_xml_refresh_external_dtd(GTEXT_Markup_Document *doc,
    const GTEXT_XML_External_DTD_Refresh_Options *options,
    GTEXT_XML_Validation_Result *result, GTEXT_XML_Error *error) {
  if (error) memset(error, 0, sizeof(*error));
  if (!doc || !options || !options->resolver || !result)
    return GTEXT_XML_E_INVALID;
  GTEXT_Markup_Node *live_node = gtext_markup_document_doctype(doc);
  if (!live_node || !gtext_markup_doctype_system_id(live_node))
    return GTEXT_XML_E_INVALID;
  const char *name = gtext_markup_doctype_name(live_node);
  const char *public_id = gtext_markup_doctype_public_id(live_node);
  const char *system_id = gtext_markup_doctype_system_id(live_node);
  if (!name || !system_id) return GTEXT_XML_E_INVALID;

  markup_buf input;
  markup_buf_init(&input, &doc->allocator);
  int built = refresh_append_literal(&input, "<!DOCTYPE ")
      && refresh_append_literal(&input, name);
  if (built && public_id) {
    built = refresh_append_literal(&input, " PUBLIC ")
        && refresh_append_quoted(&input, public_id)
        && markup_buf_append(&input, " ", 1) == GTEXT_MARKUP_OK
        && refresh_append_quoted(&input, system_id);
  }
  else if (built) {
    built = refresh_append_literal(&input, " SYSTEM ")
        && refresh_append_quoted(&input, system_id);
  }
  if (built) built = markup_buf_append(&input, " [", 2) == GTEXT_MARKUP_OK;
  for (size_t i = 0; built && i < gtext_markup_doctype_syntax_count(live_node); ++i) {
    GTEXT_Markup_DTD_Syntax syntax;
    if (!gtext_markup_doctype_syntax(live_node, i, &syntax)) {
      built = 0;
      break;
    }
    built = markup_buf_append(&input, syntax.source, syntax.source_length)
        == GTEXT_MARKUP_OK;
  }
  if (built) built = markup_buf_append(&input, "]>", 2) == GTEXT_MARKUP_OK;
  if (built) built = refresh_append_literal(&input, "<")
      && refresh_append_literal(&input, name);
  /* Give a prefixed doctype name a namespace binding in the synthetic root. */
  const char *colon = strchr(name, ':');
  if (built && colon) {
    built = markup_buf_append(&input, " xmlns:", 7) == GTEXT_MARKUP_OK
        && markup_buf_append(&input, name, (size_t)(colon - name)) == GTEXT_MARKUP_OK
        && refresh_append_literal(&input, "='urn:ghoti:external-dtd-refresh'");
  }
  if (built) built = markup_buf_append(&input, "/>" , 2) == GTEXT_MARKUP_OK;
  if (!built) {
    markup_buf_free(&input);
    return GTEXT_XML_E_OOM;
  }

  GTEXT_XML_Parse_Options parse = gtext_xml_parse_options_default();
  parse.allocator = &doc->allocator;
  parse.dtd_mode = GTEXT_XML_DTD_VALIDATE;
  parse.external_access = GTEXT_XML_EXTERNAL_ACCESS_ALLOW;
  parse.resolver = options->resolver;
  parse.resolver_user_data = options->resolver_user_data;
#define REFRESH_LIMIT(field) do { if (options->field) parse.field = options->field; } while (0)
  REFRESH_LIMIT(max_external_source_bytes);
  REFRESH_LIMIT(max_external_bytes);
  REFRESH_LIMIT(max_expanded_bytes);
  REFRESH_LIMIT(max_amplification);
  REFRESH_LIMIT(amplification_threshold);
  REFRESH_LIMIT(max_entity_depth);
  REFRESH_LIMIT(max_references);
  REFRESH_LIMIT(max_declarations);
  REFRESH_LIMIT(max_total_work);
  REFRESH_LIMIT(max_validation_work);
  REFRESH_LIMIT(max_validation_memory);
#undef REFRESH_LIMIT
  GTEXT_Markup_Document *stage_doc = NULL;
  GTEXT_XML_Error stage_error;
  GTEXT_XML_Status status = gtext_xml_parse(input.data, input.length, &parse,
      &stage_doc, &stage_error);
  markup_buf_free(&input);
  if (status != GTEXT_XML_OK) {
    if (error) *error = stage_error;
    return status;
  }
  if (!stage_doc || stage_doc->dtd_knowledge_incomplete
      || stage_doc->dtd_knowledge_stale) {
    gtext_markup_document_free(stage_doc);
    if (error) {
      error->code = GTEXT_XML_E_UNAVAILABLE_RESOURCE;
      error->message = "external DTD evidence is incomplete after refresh";
    }
    return GTEXT_XML_E_UNAVAILABLE_RESOURCE;
  }
  GTEXT_Markup_Node *stage_node = gtext_markup_document_doctype(stage_doc);
  if (!stage_node) {
    gtext_markup_document_free(stage_doc);
    return GTEXT_XML_E_STATE;
  }

  /* Keep all old arrays alive until validation of the staged declarations over
     the caller's current tree succeeds. This gives the commit a rollback path. */
  markup_doctype *live = (markup_doctype *)live_node;
  markup_doctype *staged = (markup_doctype *)stage_node;
  markup_dtd_syntax *old_external = live->external_syntax;
  size_t old_external_count = live->external_syntax_count;
  size_t old_external_capacity = live->external_syntax_capacity;
  markup_dtd_syntax *old_generated = live->generated_syntax;
  size_t old_generated_count = live->generated_syntax_count;
  size_t old_generated_capacity = live->generated_syntax_capacity;
  markup_dtd_binding *old_bindings = live->bindings;
  size_t old_binding_count = live->binding_count;
  size_t old_binding_capacity = live->binding_capacity;
  markup_source *old_sources = doc->sources;
  size_t old_source_count = doc->source_count;
  size_t old_source_capacity = doc->source_capacity;
  markup_dtd_unread_resource *old_unread = doc->dtd_unread_resources;
  size_t old_unread_count = doc->dtd_unread_resource_count;
  size_t old_unread_capacity = doc->dtd_unread_resource_capacity;
  uint8_t old_incomplete = doc->dtd_knowledge_incomplete;
  uint8_t old_stale = doc->dtd_knowledge_stale;

  const uint32_t source_offset = refresh_max_source_id(doc);
  if (stage_doc->source_count > UINT32_MAX
      || stage_doc->source_count > SIZE_MAX - old_source_count
      || old_source_count + stage_doc->source_count
          > SIZE_MAX / sizeof(markup_source)
      || source_offset > UINT32_MAX - (uint32_t)(stage_doc->source_count - 1)) {
    gtext_markup_document_free(stage_doc);
    return GTEXT_XML_E_LIMIT_REFERENCES;
  }
  const size_t merged_source_count = old_source_count + stage_doc->source_count;
  markup_source *merged_sources = markup_alloc(doc,
      (merged_source_count ? merged_source_count : 1) * sizeof(markup_source));
  if (!merged_sources) {
    gtext_markup_document_free(stage_doc);
    return GTEXT_XML_E_OOM;
  }
  if (old_source_count)
    memcpy(merged_sources, old_sources, old_source_count * sizeof(markup_source));
  size_t merged_at = old_source_count;
  int source_copy_ok = 1;
  for (size_t i = 0; i < stage_doc->source_count; ++i) {
    const markup_source *src = &stage_doc->sources[i];
    if (src->view.id == 1) continue; /* The caller's document source stays ID 1. */
    if (src->view.id - 1 > UINT32_MAX - source_offset) {
      source_copy_ok = 0;
      break;
    }
    markup_source *dst = &merged_sources[merged_at];
    *dst = *src;
    dst->owned_name = src->owned_name
        ? markup_copy(doc, src->owned_name, strlen(src->owned_name)) : NULL;
    if (src->owned_name && !dst->owned_name) { source_copy_ok = 0; break; }
    dst->owned_base_uri = src->owned_base_uri
        ? markup_copy(doc, src->owned_base_uri, strlen(src->owned_base_uri)) : NULL;
    if (src->owned_base_uri && !dst->owned_base_uri) {
      markup_free(doc, dst->owned_name);
      dst->owned_name = NULL;
      source_copy_ok = 0;
      break;
    }
    dst->owned_encoding = src->owned_encoding
        ? markup_copy(doc, src->owned_encoding, strlen(src->owned_encoding)) : NULL;
    if (src->owned_encoding && !dst->owned_encoding) {
      markup_free(doc, dst->owned_name);
      markup_free(doc, dst->owned_base_uri);
      dst->owned_name = dst->owned_base_uri = NULL;
      source_copy_ok = 0;
      break;
    }
    dst->view.name = dst->owned_name;
    dst->view.base_uri = dst->owned_base_uri;
    dst->view.encoding = dst->owned_encoding;
    dst->view.id = src->view.id - 1 + source_offset;
    dst->view.reference_source_id = refresh_remap_source(
        src->view.reference_source_id, source_offset);
    dst->origin_doc = NULL;
    dst->origin_id = 0;
    ++merged_at;
  }
  if (!source_copy_ok) {
    for (size_t i = old_source_count; i < merged_at; ++i) {
      markup_free(doc, merged_sources[i].owned_name);
      markup_free(doc, merged_sources[i].owned_base_uri);
      markup_free(doc, merged_sources[i].owned_encoding);
    }
    markup_free(doc, merged_sources);
    gtext_markup_document_free(stage_doc);
    return GTEXT_XML_E_OOM;
  }
  /* A refresh publishes a new source table atomically. Keep source 1 from the
     original tree and append the fresh DTD resource ancestry with remapped IDs. */
  for (size_t i = 0; i < staged->syntax_count; ++i)
    refresh_remap_location(&staged->syntax[i].location, source_offset);
  for (size_t i = 0; i < staged->external_syntax_count; ++i)
    refresh_remap_location(&staged->external_syntax[i].location, source_offset);
  for (size_t i = 0; i < staged->generated_syntax_count; ++i)
    refresh_remap_location(&staged->generated_syntax[i].location, source_offset);
  for (size_t i = 0; i < staged->binding_count; ++i)
    refresh_remap_location(&staged->bindings[i].location, source_offset);
  for (size_t i = 0; i < stage_doc->dtd_unread_resource_count; ++i)
    refresh_remap_location(&stage_doc->dtd_unread_resources[i].view.location,
        source_offset);

  /* Transfer syntax and indexes from the isolated parse. Its allocator is the
     same caller allocator, so ownership can move without retaining callbacks. */
  live->external_syntax = staged->external_syntax;
  live->external_syntax_count = staged->external_syntax_count;
  live->external_syntax_capacity = staged->external_syntax_capacity;
  live->generated_syntax = staged->generated_syntax;
  live->generated_syntax_count = staged->generated_syntax_count;
  live->generated_syntax_capacity = staged->generated_syntax_capacity;
  live->next_syntax_sequence = staged->next_syntax_sequence;
  staged->external_syntax = staged->generated_syntax = NULL;
  staged->external_syntax_count = staged->external_syntax_capacity = 0;
  staged->generated_syntax_count = staged->generated_syntax_capacity = 0;
  doc->sources = merged_sources;
  doc->source_count = merged_at;
  doc->source_capacity = merged_source_count;
  doc->dtd_unread_resources = stage_doc->dtd_unread_resources;
  doc->dtd_unread_resource_count = stage_doc->dtd_unread_resource_count;
  doc->dtd_unread_resource_capacity = stage_doc->dtd_unread_resource_capacity;
  stage_doc->dtd_unread_resources = NULL;
  stage_doc->dtd_unread_resource_count = stage_doc->dtd_unread_resource_capacity = 0;
  doc->dtd_knowledge_incomplete = 0;
  doc->dtd_knowledge_stale = 0;

  /* Rebuild indexes against the live internal syntax. This matters because
     edits to internal parameter entities are authoritative and the staged raw
     internal syntax belongs to its synthetic input document. */
  markup_doctype rebuilt = *live;
  rebuilt.bindings = NULL;
  rebuilt.binding_count = rebuilt.binding_capacity = 0;
  xml_ctx rebuild_ctx;
  memset(&rebuild_ctx, 0, sizeof(rebuild_ctx));
  rebuild_ctx.doc = doc;
  rebuild_ctx.allocator = &doc->allocator;
  rebuild_ctx.max_total_work = options->max_total_work
      ? options->max_total_work : XML_DEFAULT_MAX_TOTAL_WORK;
  rebuild_ctx.max_validation_work = options->max_validation_work
      ? options->max_validation_work : XML_DEFAULT_MAX_VALIDATION_WORK;
  rebuild_ctx.max_validation_memory = options->max_validation_memory
      ? options->max_validation_memory : XML_DEFAULT_MAX_VALIDATION_MEMORY;
  GTEXT_XML_Status rebuild_status = xml_doctype_rebuild_indexes(&rebuild_ctx,
      (GTEXT_Markup_Node *)&rebuilt);
  if (rebuild_status == GTEXT_XML_OK) {
    live->bindings = rebuilt.bindings;
    live->binding_count = rebuilt.binding_count;
    live->binding_capacity = rebuilt.binding_capacity;
    /* live's own index sees a new array and refiles it on its next use. */
  }
  else {
    status = rebuild_status;
    if (error) *error = rebuild_ctx.err;
  }
  markup_doctype_release_binding_index((GTEXT_Markup_Node *)&rebuilt);
  /* The rebuilt markup bindings belong to the document, but the temporary
     parser indexes and validation model belong to rebuild_ctx. */
  xml_source_free(&rebuild_ctx);
  xml_validation_cleanup(&rebuild_ctx);

  if (status == GTEXT_XML_OK)
    status = gtext_xml_revalidate(doc, NULL, result, error);
  if (status != GTEXT_XML_OK) {
    refresh_free_bindings(doc, live->bindings, live->binding_count);
    refresh_free_syntax(doc, live->external_syntax, live->external_syntax_count);
    refresh_free_syntax(doc, live->generated_syntax, live->generated_syntax_count);
    for (size_t i = old_source_count; i < doc->source_count; ++i) {
      markup_free(doc, doc->sources[i].owned_name);
      markup_free(doc, doc->sources[i].owned_base_uri);
      markup_free(doc, doc->sources[i].owned_encoding);
    }
    markup_free(doc, doc->sources);
    for (size_t i = 0; i < doc->dtd_unread_resource_count; ++i) {
      markup_free(doc, doc->dtd_unread_resources[i].owned_name);
      markup_free(doc, doc->dtd_unread_resources[i].owned_system_id);
    }
    markup_free(doc, doc->dtd_unread_resources);
    live->external_syntax = old_external; live->external_syntax_count = old_external_count; live->external_syntax_capacity = old_external_capacity;
    live->generated_syntax = old_generated; live->generated_syntax_count = old_generated_count; live->generated_syntax_capacity = old_generated_capacity;
    live->bindings = old_bindings; live->binding_count = old_binding_count; live->binding_capacity = old_binding_capacity;
    doc->sources = old_sources; doc->source_count = old_source_count; doc->source_capacity = old_source_capacity;
    doc->dtd_unread_resources = old_unread; doc->dtd_unread_resource_count = old_unread_count; doc->dtd_unread_resource_capacity = old_unread_capacity;
    doc->dtd_knowledge_incomplete = old_incomplete;
    doc->dtd_knowledge_stale = old_stale;
  }
  else {
    refresh_free_syntax(doc, old_external, old_external_count);
    refresh_free_syntax(doc, old_generated, old_generated_count);
    refresh_free_bindings(doc, old_bindings, old_binding_count);
    markup_free(doc, old_sources);
    for (size_t i = 0; i < old_unread_count; ++i) {
      markup_free(doc, old_unread[i].owned_name);
      markup_free(doc, old_unread[i].owned_system_id);
    }
    markup_free(doc, old_unread);
  }
  gtext_markup_document_free(stage_doc);
  return status;
}
