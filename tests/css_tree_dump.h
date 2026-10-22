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
 * A complete dump of a CSS tree: every field of every node, spans, identities
 * and flags included, which the canonical form of the round trip
 * (tools/conformance/css_tree_canon.h) leaves out. For comparing two parses
 * that must agree about where things are. No test framework is needed, so the
 * unit tests and the fuzz harnesses both use it.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_TREE_DUMP_H
#define GHOTI_IO_GTEXT_TESTS_CSS_TREE_DUMP_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

namespace css_test {

inline void d_span(std::string & o, const GTEXT_CSS_Span & s) {
  char b[128];
  std::snprintf(b, sizeof b, "<%zu-%zu@%zu:%zu>", s.offset, s.end, s.line, s.column);
  o += b;
}

inline void d_str(std::string & o, const GTEXT_CSS_String & s) {
  o += s.data ? "\"" : "(null)\"";
  if (s.data) {
    o.append(s.data, s.length);
  }
  o += "\"";
}

inline void d_values(std::string & o, const GTEXT_CSS_Component_Value * v, size_t n);

inline void d_value(std::string & o, const GTEXT_CSS_Component_Value & v) {
  char b[256];
  std::snprintf(b, sizeof b, "{#%llu k%d t%d f%x ", (unsigned long long)v.id, (int)v.kind, (int)v.type, (unsigned)v.flags);
  o += b;
  d_span(o, v.span);
  d_str(o, v.value);
  d_str(o, v.unit);
  d_str(o, v.repr);
  uint64_t bits;
  std::memcpy(&bits, &v.number, sizeof bits);
  std::snprintf(b, sizeof b, " n%016llx d%u r%u-%u c%zu[", (unsigned long long)bits, (unsigned)v.delim,
      (unsigned)v.range_start, (unsigned)v.range_end, v.child_count);
  o += b;
  d_values(o, v.children, v.child_count);
  o += "]}";
}

inline void d_values(std::string & o, const GTEXT_CSS_Component_Value * v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    d_value(o, v[i]);
  }
}

inline void d_decl(std::string & o, const GTEXT_CSS_Declaration & d) {
  char b[128];
  std::snprintf(b, sizeof b, "(D#%llu ", (unsigned long long)d.id);
  o += b;
  d_span(o, d.span);
  d_str(o, d.name);
  d_span(o, d.name_span);
  o += d.important ? " !" : " .";
  d_span(o, d.value_span);
  d_str(o, d.original_text);
  std::snprintf(b, sizeof b, " v%zu[", d.value_count);
  o += b;
  d_values(o, d.value, d.value_count);
  o += "])";
}

inline void d_rules(std::string & o, const GTEXT_CSS_Rule * r, size_t n);

inline void d_rule(std::string & o, const GTEXT_CSS_Rule & r) {
  char b[160];
  std::snprintf(b, sizeof b, "(R#%llu k%d ", (unsigned long long)r.id, (int)r.kind);
  o += b;
  d_span(o, r.span);
  d_str(o, r.name);
  d_span(o, r.prelude_span);
  d_span(o, r.block_span);
  std::snprintf(b, sizeof b, " b%d u%d p%zu[", r.has_block ? 1 : 0, r.unclosed ? 1 : 0, r.prelude_count);
  o += b;
  d_values(o, r.prelude, r.prelude_count);
  std::snprintf(b, sizeof b, "] d%zu[", r.declaration_count);
  o += b;
  for (size_t i = 0; i < r.declaration_count; i++) {
    d_decl(o, r.declarations[i]);
  }
  std::snprintf(b, sizeof b, "] r%zu[", r.rule_count);
  o += b;
  d_rules(o, r.rules, r.rule_count);
  std::snprintf(b, sizeof b, "] c%zu[", r.content_count);
  o += b;
  for (size_t i = 0; i < r.content_count; i++) {
    const GTEXT_CSS_Block_Item & it = r.contents[i];
    if (it.kind == GTEXT_CSS_ITEM_RULE) {
      d_rule(o, *it.rule);
    }
    else {
      o += "[";
      for (size_t k = 0; k < it.declaration_count; k++) {
        d_decl(o, it.declarations[k]);
      }
      o += "]";
    }
  }
  o += "])";
}

inline void d_rules(std::string & o, const GTEXT_CSS_Rule * r, size_t n) {
  for (size_t i = 0; i < n; i++) {
    d_rule(o, r[i]);
  }
}

/// Every field of a tree.
inline std::string dump_tree(const GTEXT_CSS_Tree * t) {
  if (!t) {
    return "(no tree)";
  }
  std::string o;
  char b[160];
  std::snprintf(b, sizeof b, "tree k%d e%d nodes%zu loc=%s\n", (int)t->kind, (int)t->syntax_error, t->node_count,
      t->location ? t->location : "(null)");
  o += b;
  d_rules(o, t->rules, t->rule_count);
  o += "\n";
  for (size_t i = 0; i < t->declaration_count; i++) {
    d_decl(o, t->declarations[i]);
  }
  o += "\n";
  for (size_t i = 0; i < t->item_count; i++) {
    const GTEXT_CSS_Block_Item & it = t->items[i];
    if (it.kind == GTEXT_CSS_ITEM_RULE) {
      d_rule(o, *it.rule);
    }
    else {
      o += "[";
      for (size_t k = 0; k < it.declaration_count; k++) {
        d_decl(o, it.declarations[k]);
      }
      o += "]";
    }
  }
  o += "\n";
  d_values(o, t->values, t->value_count);
  o += "\n";
  for (size_t i = 0; i < t->list_count; i++) {
    o += "<list ";
    d_span(o, t->lists[i].span);
    d_values(o, t->lists[i].values, t->lists[i].count);
    o += ">";
  }
  return o;
}


// Every span of a tree, as (offset, end) pairs in document order (rules, their preludes, declarations, values and
// children), for comparing the places two parses of the same structure put things.
inline void spans_of_values(std::vector<size_t> & out, const GTEXT_CSS_Component_Value * v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    out.push_back(v[i].span.offset);
    out.push_back(v[i].span.end);
    spans_of_values(out, v[i].children, v[i].child_count);
  }
}

inline void spans_of_rules(std::vector<size_t> & out, const GTEXT_CSS_Rule * r, size_t n);

inline void spans_of_decls(std::vector<size_t> & out, const GTEXT_CSS_Declaration * d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    for (const GTEXT_CSS_Span * s : {&d[i].span, &d[i].name_span, &d[i].value_span}) {
      out.push_back(s->offset);
      out.push_back(s->end);
    }
    spans_of_values(out, d[i].value, d[i].value_count);
  }
}

inline void spans_of_rules(std::vector<size_t> & out, const GTEXT_CSS_Rule * r, size_t n) {
  for (size_t i = 0; i < n; i++) {
    for (const GTEXT_CSS_Span * s : {&r[i].span, &r[i].prelude_span, &r[i].block_span}) {
      out.push_back(s->offset);
      out.push_back(s->end);
    }
    spans_of_values(out, r[i].prelude, r[i].prelude_count);
    spans_of_decls(out, r[i].declarations, r[i].declaration_count);
    spans_of_rules(out, r[i].rules, r[i].rule_count);
    for (size_t k = 0; k < r[i].content_count; k++) {
      if (r[i].contents[k].kind == GTEXT_CSS_ITEM_RULE) {
        spans_of_rules(out, r[i].contents[k].rule, 1);
      }
      else {
        spans_of_decls(out, r[i].contents[k].declarations, r[i].contents[k].declaration_count);
      }
    }
  }
}


inline std::vector<size_t> collect_spans(const GTEXT_CSS_Tree * t) {
  std::vector<size_t> out;
  spans_of_rules(out, t->rules, t->rule_count);
  spans_of_decls(out, t->declarations, t->declaration_count);
  for (size_t i = 0; i < t->item_count; i++) {
    if (t->items[i].kind == GTEXT_CSS_ITEM_RULE) {
      spans_of_rules(out, t->items[i].rule, 1);
    }
    else {
      spans_of_decls(out, t->items[i].declarations, t->items[i].declaration_count);
    }
  }
  spans_of_values(out, t->values, t->value_count);
  for (size_t i = 0; i < t->list_count; i++) {
    out.push_back(t->lists[i].span.offset);
    out.push_back(t->lists[i].span.end);
    spans_of_values(out, t->lists[i].values, t->lists[i].count);
  }
  return out;
}

} // namespace css_test

#endif // GHOTI_IO_GTEXT_TESTS_CSS_TREE_DUMP_H
