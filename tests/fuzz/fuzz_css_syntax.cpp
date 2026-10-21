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
 * libFuzzer harness for CSS Syntax (src/css/): the tokenizer, every parser
 * entry point, the serializer and An+B, through the public API.
 *
 * The input is three option bytes and then the text:
 *
 *   o0  bits 0-3 the entry point (of nine, modulo), bit 4 WTF-8 input, bit 5
 *       count_errors_only, bit 6 a validity callback that refuses some
 *       declarations and rules, bit 7 max_errors 1;
 *   o1  a limit made tiny: bits 0-2 which (none, depth, tokens, rules,
 *       declarations, parse errors, input bytes, none), bits 3-7 its value
 *       (1 to 32);
 *   o2  the allocation to fail (0 for none).
 *
 * The properties, each a requirement of css_core.h, css_parse.h or CSS
 * Syntax:
 *
 *   the parse with the default limits succeeds (only the depth limit is
 *   within reach of an input this size, and it takes more than 512 openings),
 *   and its tree is well formed: ids 1 to node_count in
 *   document order, every span inside the input and in order, every string
 *   NUL-terminated well-formed UTF-8 with no NUL, every kind, type and count
 *   consistent (a qualified rule has a block, a nested declarations rule only
 *   declarations, an at-rule's block only contents), a custom property and
 *   only it with original text;
 *   the parse is deterministic, and the stylesheet entry point and the
 *   contents one give the same rules;
 *   the parse errors are in order of position, inside the input, at most
 *   max_errors kept, kept plus dropped the same whatever max_errors is, none
 *   kept when only counted;
 *   **the round trip** (section 9): serialized and parsed again by the same
 *   entry point, the tree is the same (css_tree_canon.h), and serializing
 *   again gives the same text (without the validity callback, which can split
 *   a run of declarations that then reads back as one, and for a tree that is
 *   a syntax error, which writes nothing);
 *   **each limit is exact**: what the parse counted (bytes, tokens, rules,
 *   declarations, parse errors) is accepted as a limit and one less is that
 *   limit's status, with no tree; the depth of the tree is a lower bound of
 *   the depth, so one less than it fails; and under the tiny limit of o1 the
 *   parse is that limit's status or the same tree;
 *   an allocation that fails is E_OOM with no tree, or the same answer, and
 *   never a leak or a free of a block the allocator did not make;
 *   the work is linear: at most 64 units per byte of input, plus 4096;
 *   the comma-separated list entry point and gtext_css_match_grammar_list()
 *   split a list into the same number of groups (two implementations of
 *   5.4.10);
 *   An+B read from the values, serialized (9.1) and read again, is the same.
 *
 * Seeds: tests/fuzz/corpus/css-syntax, made by tools/css/make-fuzz-seeds.py.
 * Build with: make fuzz-css-syntax      Run: make fuzz-run-css-syntax
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../../tools/conformance/css_tree_canon.h"
#include "../markup_test_support.h"

extern "C" GTEXT_CSS_Status gtext_css_parse_work(GTEXT_CSS_Tree_Kind entry, const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, size_t * work);

namespace css_fuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_syntax: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

using ParseFn = GTEXT_CSS_Status (*)(const char *, size_t, const GTEXT_CSS_Parse_Options *, GTEXT_CSS_Tree **,
    GTEXT_CSS_Result *);

const ParseFn kEntries[9] = {gtext_css_parse_stylesheet, gtext_css_parse_stylesheet_contents,
    gtext_css_parse_declaration_list, gtext_css_parse_block_contents, gtext_css_parse_rule,
    gtext_css_parse_declaration, gtext_css_parse_component_value, gtext_css_parse_component_values,
    gtext_css_parse_comma_list};
const GTEXT_CSS_Tree_Kind kKinds[9] = {GTEXT_CSS_TREE_STYLESHEET, GTEXT_CSS_TREE_STYLESHEET_CONTENTS,
    GTEXT_CSS_TREE_DECLARATION_LIST, GTEXT_CSS_TREE_BLOCK_CONTENTS, GTEXT_CSS_TREE_RULE, GTEXT_CSS_TREE_DECLARATION,
    GTEXT_CSS_TREE_COMPONENT_VALUE, GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, GTEXT_CSS_TREE_COMMA_LIST};

bool refuse_decl(void *, const GTEXT_CSS_Parent *, const GTEXT_CSS_Declaration * d) {
  return !(d->name.length > 0 && d->name.data[0] == 'x');
}

bool refuse_rule(void *, const GTEXT_CSS_Parent *, const GTEXT_CSS_Rule * r) {
  for (size_t i = 0; i < r->prelude_count; i++) {
    if (r->prelude[i].type == GTEXT_CSS_TOKEN_DELIM && r->prelude[i].delim == '<') {
      return false;
    }
  }
  return true;
}

struct Run {
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Result result{};
  Run() = default;
  Run(const Run &) = delete;
  Run & operator=(const Run &) = delete;
  ~Run() {
    gtext_css_tree_free(tree);
    gtext_css_result_free(&result);
  }
};

void parse(Run & r, ParseFn fn, const std::string & text, const GTEXT_CSS_Parse_Options & o, bool with_result = true) {
  r.status = fn(text.data(), text.size(), &o, &r.tree, with_result ? &r.result : nullptr);
}

std::string canon(const GTEXT_CSS_Tree * t) {
  char * c = css_tree_canon(t);
  std::string s(c);
  std::free(c);
  return s;
}

GTEXT_Markup_Status string_sink(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

std::string serialize(const GTEXT_CSS_Tree * t, const std::string & input) {
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  if (gtext_css_serialize_tree(t, nullptr, &sink) != GTEXT_CSS_OK) {
    fail("the serializer failed on a tree a parse made", "", input);
  }
  return out;
}

// ---- the tree is well formed --------------------------------------------------------------------

struct Checker {
  const std::string & input;
  GTEXT_CSS_Id next = 1;
  size_t depth = 0;
  size_t max_depth = 0;

  void span(const GTEXT_CSS_Span & s, const char * what) {
    if (s.offset > s.end || s.end > input.size() || s.line == 0 || s.column == 0) {
      fail("a span is outside the input or out of order", what, input);
    }
  }

  void string(const GTEXT_CSS_String & s, bool required, const char * what) {
    if (!s.data) {
      if (required || s.length) {
        fail("a string that must be there is not", what, input);
      }
      return;
    }
    if (s.data[s.length] != '\0' || std::memchr(s.data, 0, s.length)) {
      fail("a string is not terminated, or holds a NUL", what, input);
    }
    // Well-formed UTF-8, strictly (no surrogates, no overlongs).
    const unsigned char * p = (const unsigned char *)s.data;
    for (size_t i = 0; i < s.length;) {
      const unsigned char b = p[i];
      size_t n = b < 0x80 ? 1 : (b >= 0xC2 && b <= 0xDF) ? 2 : (b >= 0xE0 && b <= 0xEF) ? 3 : (b >= 0xF0 && b <= 0xF4) ? 4 : 0;
      if (n == 0 || i + n > s.length) {
        fail("a string is not UTF-8", what, input);
      }
      for (size_t k = 1; k < n; k++) {
        if ((p[i + k] & 0xC0) != 0x80) {
          fail("a string is not UTF-8", what, input);
        }
      }
      if ((b == 0xE0 && p[i + 1] < 0xA0) || (b == 0xED && p[i + 1] > 0x9F) || (b == 0xF0 && p[i + 1] < 0x90)
          || (b == 0xF4 && p[i + 1] > 0x8F)) {
        fail("a string is not UTF-8 (a surrogate or an overlong form)", what, input);
      }
      i += n;
    }
  }

  void id(GTEXT_CSS_Id got, const char * what) {
    if (got != next++) {
      fail("ids are not 1 to node_count in document order", what, input);
    }
  }

  void values(const GTEXT_CSS_Component_Value * v, size_t n) {
    if (n && !v) {
      fail("a count with no array", "values", input);
    }
    for (size_t i = 0; i < n; i++) {
      id(v[i].id, "value");
      span(v[i].span, "value");
      if (v[i].kind == GTEXT_CSS_VALUE_TOKEN) {
        if (v[i].type >= GTEXT_CSS_TOKEN_EOF || v[i].type == GTEXT_CSS_TOKEN_FUNCTION || v[i].type == GTEXT_CSS_TOKEN_OPEN_CURLY
            || v[i].type == GTEXT_CSS_TOKEN_OPEN_SQUARE || v[i].type == GTEXT_CSS_TOKEN_OPEN_PAREN || v[i].child_count) {
          fail("a preserved token of a type that is never preserved", gtext_css_token_type_name(v[i].type), input);
        }
        const bool named = v[i].type == GTEXT_CSS_TOKEN_IDENT || v[i].type == GTEXT_CSS_TOKEN_AT_KEYWORD
            || v[i].type == GTEXT_CSS_TOKEN_HASH || v[i].type == GTEXT_CSS_TOKEN_STRING || v[i].type == GTEXT_CSS_TOKEN_URL
            || v[i].type == GTEXT_CSS_TOKEN_DELIM;
        string(v[i].value, named, "value");
        if ((v[i].type == GTEXT_CSS_TOKEN_IDENT || v[i].type == GTEXT_CSS_TOKEN_AT_KEYWORD || v[i].type == GTEXT_CSS_TOKEN_HASH)
            && v[i].value.length == 0) {
          fail("an empty name", gtext_css_token_type_name(v[i].type), input);
        }
        const bool numeric = v[i].type == GTEXT_CSS_TOKEN_NUMBER || v[i].type == GTEXT_CSS_TOKEN_PERCENTAGE
            || v[i].type == GTEXT_CSS_TOKEN_DIMENSION;
        string(v[i].repr, numeric, "repr");
        string(v[i].unit, v[i].type == GTEXT_CSS_TOKEN_DIMENSION, "unit");
        if (v[i].type == GTEXT_CSS_TOKEN_DIMENSION && v[i].unit.length == 0) {
          fail("a dimension with no unit", "", input);
        }
        if (v[i].type == GTEXT_CSS_TOKEN_PERCENTAGE && (v[i].flags & GTEXT_CSS_FLAG_INTEGER)) {
          fail("a percentage with a type flag", "", input);
        }
      }
      else {
        if (v[i].kind != GTEXT_CSS_VALUE_FUNCTION && v[i].kind != GTEXT_CSS_VALUE_BLOCK) {
          fail("a value of no kind", "", input);
        }
        if (v[i].kind == GTEXT_CSS_VALUE_FUNCTION ? v[i].type != GTEXT_CSS_TOKEN_FUNCTION
                                                   : (v[i].type != GTEXT_CSS_TOKEN_OPEN_CURLY && v[i].type != GTEXT_CSS_TOKEN_OPEN_SQUARE
                                                         && v[i].type != GTEXT_CSS_TOKEN_OPEN_PAREN)) {
          fail("a function or block of the wrong type", "", input);
        }
        string(v[i].value, v[i].kind == GTEXT_CSS_VALUE_FUNCTION, "function name");
        depth++;
        max_depth = depth > max_depth ? depth : max_depth;
        values(v[i].children, v[i].child_count);
        depth--;
      }
    }
  }

  void decls(const GTEXT_CSS_Declaration * d, size_t n) {
    if (n && !d) {
      fail("a count with no array", "declarations", input);
    }
    for (size_t i = 0; i < n; i++) {
      id(d[i].id, "declaration");
      span(d[i].span, "declaration");
      span(d[i].name_span, "declaration name");
      span(d[i].value_span, "declaration value");
      string(d[i].name, true, "declaration name");
      const bool custom = d[i].name.length >= 2 && d[i].name.data[0] == '-' && d[i].name.data[1] == '-';
      string(d[i].original_text, custom, "original text");
      if (!custom && d[i].original_text.data) {
        fail("original text on a declaration that is not a custom property", "", input);
      }
      values(d[i].value, d[i].value_count);
    }
  }

  void rule(const GTEXT_CSS_Rule & r) {
    id(r.id, "rule");
    span(r.span, "rule");
    switch (r.kind) {
    case GTEXT_CSS_RULE_QUALIFIED:
      if (!r.has_block || r.contents || r.name.data) {
        fail("a qualified rule with no block, with contents or with a name", "", input);
      }
      break;
    case GTEXT_CSS_RULE_AT:
      string(r.name, true, "at-rule name");
      if (r.declarations || r.rules || (!r.has_block && r.contents)) {
        fail("an at-rule with materialized declarations or rules", "", input);
      }
      break;
    case GTEXT_CSS_RULE_NESTED_DECLARATIONS:
      if (r.prelude_count || r.rules || r.contents || r.has_block || r.declaration_count == 0) {
        fail("a nested declarations rule with more than declarations, or none", "", input);
      }
      break;
    default:
      fail("a rule of no kind", "", input);
    }
    values(r.prelude, r.prelude_count);
    if (r.has_block) {
      depth++;
      max_depth = depth > max_depth ? depth : max_depth;
    }
    decls(r.declarations, r.declaration_count);
    rules(r.rules, r.rule_count);
    for (size_t i = 0; i < r.content_count; i++) {
      if (r.contents[i].kind == GTEXT_CSS_ITEM_RULE) {
        rule(*r.contents[i].rule);
      }
      else {
        decls(r.contents[i].declarations, r.contents[i].declaration_count);
      }
    }
    if (r.has_block) {
      depth--;
    }
  }

  void rules(const GTEXT_CSS_Rule * r, size_t n) {
    if (n && !r) {
      fail("a count with no array", "rules", input);
    }
    for (size_t i = 0; i < n; i++) {
      rule(r[i]);
    }
  }

  void tree(const GTEXT_CSS_Tree * t) {
    rules(t->rules, t->rule_count);
    decls(t->declarations, t->declaration_count);
    for (size_t i = 0; i < t->item_count; i++) {
      if (t->items[i].kind == GTEXT_CSS_ITEM_RULE) {
        rule(*t->items[i].rule);
      }
      else {
        decls(t->items[i].declarations, t->items[i].declaration_count);
      }
    }
    values(t->values, t->value_count);
    for (size_t i = 0; i < t->list_count; i++) {
      values(t->lists[i].values, t->lists[i].count);
    }
    if (next - 1 != t->node_count) {
      fail("node_count is not the number of nodes", "", input);
    }
  }
};

void check_errors(const GTEXT_CSS_Result & r, const std::string & input, size_t max_kept) {
  size_t last = 0;
  const size_t n = gtext_css_result_error_count(&r);
  if (n > max_kept) {
    fail("more errors kept than max_errors", "", input);
  }
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Error e;
    if (!gtext_css_result_error_at(&r, i, &e)) {
      fail("an error inside the count cannot be read", "", input);
    }
    if (e.byte_offset > input.size() || e.byte_offset < last || e.line == 0 || e.column == 0
        || std::strcmp(e.name, gtext_css_error_name(e.code)) != 0 || std::strcmp(e.name, "unknown") == 0) {
      fail("an error out of order, outside the input, or misnamed", e.name, input);
    }
    last = e.byte_offset;
  }
}

// Under a limit set to `value`, the parse is OK (and the same tree) or the limit's status with no tree.
void limit_run(ParseFn fn, const std::string & text, GTEXT_CSS_Parse_Options o, size_t which, size_t value,
    GTEXT_CSS_Status want, const std::string & good, bool must_fail, const std::string & input) {
  switch (which) {
  case 1: o.max_depth = value; break;
  case 2: o.max_tokens = value; break;
  case 3: o.max_rules = value; break;
  case 4: o.max_declarations = value; break;
  case 5: o.max_total_errors = value; break;
  case 6: o.max_input_bytes = value; break;
  default: return;
  }
  Run r;
  parse(r, fn, text, o);
  if (r.status == GTEXT_CSS_OK) {
    if (must_fail) {
      fail("a limit one below what the parse counted did not stop it", std::to_string(which) + " " + std::to_string(value), input);
    }
    if (canon(r.tree) != good) {
      fail("a parse under a limit that it did not reach is not the same tree", std::to_string(which), input);
    }
    return;
  }
  if (r.status != want || r.tree || r.result.status != want) {
    fail("a limit that was reached is not its own status, or left a tree", gtext_css_status_string(r.status), input);
  }
}

int entry(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2];
  const std::string input(reinterpret_cast<const char *>(data) + 3, size - 3);
  const size_t e = (o0 & 0x0F) % 9;
  const ParseFn fn = kEntries[e];

  GTEXT_CSS_Parse_Options base = gtext_css_parse_options_default();
  base.input_wtf8 = (o0 & 0x10) != 0;
  const bool validity = (o0 & 0x40) != 0;
  if (validity) {
    base.validity.declaration = refuse_decl;
    base.validity.rule = refuse_rule;
  }

  // The parse with the default limits, and what it must be. Of the default limits only the depth (512) is
  // within reach of an input this size; reaching it is its status, with no tree, and needs that many openings.
  Run good;
  parse(good, fn, input, base);
  if (good.status == GTEXT_CSS_E_LIMIT_DEPTH) {
    size_t openings = 0;
    for (char c : input) {
      openings += c == '(' || c == '[' || c == '{';
    }
    if (good.tree || openings <= 512) {
      fail("the default depth limit was reached by an input that cannot reach it, or left a tree", "", input);
    }
    return 0;
  }
  if (good.status != GTEXT_CSS_OK || !good.tree || good.result.status != GTEXT_CSS_OK) {
    fail("a parse under the default limits failed", gtext_css_status_string(good.status), input);
  }
  if (good.tree->kind != kKinds[e]) {
    fail("the tree is not of its entry point's kind", "", input);
  }
  Checker ck{input};
  ck.tree(good.tree);
  check_errors(good.result, input, 1024);
  const std::string good_canon = canon(good.tree);
  {
    Run again;
    parse(again, fn, input, base);
    if (canon(again.tree) != good_canon) {
      fail("the parse is not deterministic", "", input);
    }
  }
  if (e == 0) {
    Run contents;
    parse(contents, gtext_css_parse_stylesheet_contents, input, base);
    css_canon_buf a = {nullptr, 0, 0}, b = {nullptr, 0, 0};
    css_canon_rules(&a, good.tree->rules, good.tree->rule_count);
    css_canon_rules(&b, contents.tree->rules, contents.tree->rule_count);
    const bool same = (a.len == b.len) && (a.len == 0 || std::memcmp(a.data, b.data, a.len) == 0);
    std::free(a.data);
    std::free(b.data);
    if (!same) {
      fail("a stylesheet and a stylesheet's contents have different rules", "", input);
    }
  }

  // The parse errors: the cap and the count.
  const size_t total = gtext_css_result_error_count(&good.result) + gtext_css_result_errors_dropped(&good.result);
  {
    GTEXT_CSS_Parse_Options o = base;
    o.max_errors = (o0 & 0x80) ? 1 : 2;
    o.count_errors_only = (o0 & 0x20) != 0;
    Run r;
    parse(r, fn, input, o);
    check_errors(r.result, input, o.count_errors_only ? 0 : o.max_errors);
    if (gtext_css_result_error_count(&r.result) + gtext_css_result_errors_dropped(&r.result) != total
        || canon(r.tree) != good_canon) {
      fail("the error cap changed the count or the tree", "", input);
    }
    for (size_t i = 0; i < gtext_css_result_error_count(&r.result); i++) {
      GTEXT_CSS_Error a, b;
      gtext_css_result_error_at(&r.result, i, &a);
      gtext_css_result_error_at(&good.result, i, &b);
      if (a.code != b.code || a.byte_offset != b.byte_offset) {
        fail("the cap did not keep the first errors", "", input);
      }
    }
  }

  // The round trip.
  const std::string text = serialize(good.tree, input);
  if (!validity && good.tree->syntax_error == GTEXT_CSS_SYNTAX_NONE) {
    Run back;
    parse(back, fn, text, base);
    if (back.status != GTEXT_CSS_OK || canon(back.tree) != good_canon) {
      fail("parse, serialize, parse is not the same tree",
          "text: " + text + "\nbefore: " + good_canon + "\nafter:  " + (back.tree ? canon(back.tree) : std::string("(none)")), input);
    }
    if (serialize(back.tree, input) != text) {
      fail("serializing again is not the same text", text, input);
    }
  }

  // Every limit at what the parse counted, and one below.
  const size_t counted[7] = {0, ck.max_depth, good.result.token_count, good.result.rule_count,
      good.result.declaration_count, total, input.size()};
  const GTEXT_CSS_Status limit_status[7] = {GTEXT_CSS_OK, GTEXT_CSS_E_LIMIT_DEPTH, GTEXT_CSS_E_LIMIT_TOKENS,
      GTEXT_CSS_E_LIMIT_RULES, GTEXT_CSS_E_LIMIT_DECLARATIONS, GTEXT_CSS_E_LIMIT_ERRORS, GTEXT_CSS_E_LIMIT_INPUT};
  for (size_t which = 1; which <= 6; which++) {
    if (counted[which] == 0) {
      continue;
    }
    if (which != 1) {
      limit_run(fn, input, base, which, counted[which], limit_status[which], good_canon, false, input);
    }
    if (counted[which] > 1) {
      limit_run(fn, input, base, which, counted[which] - 1, limit_status[which], good_canon, true, input);
    }
  }
  if ((o1 & 7) != 0 && (o1 & 7) != 7) {
    limit_run(fn, input, base, o1 & 7, (size_t)(o1 >> 3) + 1, limit_status[o1 & 7], good_canon, false, input);
  }

  // An allocation that fails.
  if (o2) {
    FaultAllocator fa;
    GTEXT_CSS_Parse_Options o = base;
    o.allocator = &fa.alloc;
    {
      Run r;
      fa.arm(o2);
      parse(r, fn, input, o, (o0 & 0x80) == 0);
      fa.disarm();
      if (r.status == GTEXT_CSS_E_OOM) {
        if (!fa.failed || r.tree) {
          fail("E_OOM with no allocation failed, or with a tree", "", input);
        }
      }
      else if (r.status != GTEXT_CSS_OK || canon(r.tree) != good_canon) {
        fail("an allocation that failed changed the answer", gtext_css_status_string(r.status), input);
      }
    }
    if (!fa.live.empty() || fa.bad_frees) {
      fail("an allocation that failed leaked a block or freed one it did not make", "", input);
    }
  }

  // The work is linear.
  size_t work = 0;
  if (gtext_css_parse_work(kKinds[e], input.data(), input.size(), &base, &work) != GTEXT_CSS_OK || work > 64 * input.size() + 4096) {
    fail("the work is not linear in the input", std::to_string(work), input);
  }

  // Two splittings of a comma-separated list agree.
  {
    Run list, values;
    parse(list, gtext_css_parse_comma_list, input, base);
    parse(values, gtext_css_parse_component_values, input, base);
    size_t groups = 0;
    GTEXT_CSS_Grammar g = {[](void *, size_t, const GTEXT_CSS_Component_Value *, size_t) { return true; }, nullptr};
    gtext_css_match_grammar_list(values.tree->values, values.tree->value_count, &g, &groups);
    bool only_ws = true;
    for (size_t i = 0; i < values.tree->value_count; i++) {
      only_ws = only_ws && values.tree->values[i].kind == GTEXT_CSS_VALUE_TOKEN
          && values.tree->values[i].type == GTEXT_CSS_TOKEN_WHITESPACE;
    }
    if (!only_ws && groups != list.tree->list_count) {
      fail("the comma-separated list and the grammar list split differently",
          std::to_string(groups) + " and " + std::to_string(list.tree->list_count), input);
    }
    // An+B, read, written and read again.
    int64_t a, b;
    if (gtext_css_anb_parse(values.tree->values, values.tree->value_count, &a, &b) && a > INT64_MIN / 2 && a < INT64_MAX / 2
        && b > INT64_MIN / 2 && b < INT64_MAX / 2) {
      char buf[64];
      const size_t n = gtext_css_anb_serialize(a, b, buf, sizeof buf);
      Run r;
      parse(r, gtext_css_parse_component_values, std::string(buf, n), gtext_css_parse_options_default());
      int64_t a2 = 0, b2 = 0;
      if (!gtext_css_anb_parse(r.tree->values, r.tree->value_count, &a2, &b2) || a2 != a || b2 != b) {
        fail("An+B serialized does not read back as itself", std::string(buf, n), input);
      }
    }
  }
  return 0;
}

} // namespace css_fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_fuzz::entry(data, size);
}
