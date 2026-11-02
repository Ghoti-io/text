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
 * This library's side of the CSSOM differential (tools/oracle/css_cssom_diff.py): the sequences of operations of tools/oracle/css_cssom_gen.py, run on a document of its own.
 *
 *   css-cssom-ours      documents on standard input, one JSON answer a line on standard output
 *
 * Input: a line `D` starts a document, then one tab-separated line for each operation (a field escapes backslash, line feed and tab as \\, \n, \t):
 *   S css | C | U n | I path index text | D path index | A path text | X path select | F path select      (the rule operations of css_ruleops_ours.cpp)
 *   R text                       replaceSync(text) on the current sheet
 *   T path text                  rule.selectorText = text            K path text   rule.keyText = text
 *   N path text                  rule.name = text                    E path attr text   rule[attr] = text
 *   P path prop value prio       rule.style.setProperty()            Q path prop   rule.style.removeProperty(), answering the old value
 *   Y path text                  rule.style.cssText = text           Z path text   rule.cssText = text (which does nothing)
 *   M target text                media.mediaText = text              MA target text  appendMedium         MD target text  deleteMedium   (target: a rule's path, "" the sheet)
 *   W path map name values       rule[map].set(name, values), values a comma list of integers
 *   V path attr                  String(rule[attr])                  VS path prop  style.getPropertyValue   VL path  style.length   VP path prop  style.getPropertyPriority
 *   DIS 0|1                      sheet.disabled = ...                AD list   document.adoptedStyleSheets = [the sheets at these creation indexes]
 *   MM text                      matchMedia(text): "media|matches"
 * Output: {"r": [one answer for each operation], "t": [{"r": [cssText of each rule], "m": mediaText, "d": disabled} for each sheet made]}. An answer is "ok", the name of an exception, a
 * string, or null (`\x01null` inside). What a JavaScript caller gets for a member the object does not have (a TypeError on a call, `undefined` on a read, nothing on a write) is given
 * here as the language gives it.
 */

#include <ghoti.io/text/css.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char * const kNull = "\x01null";

std::string unesc(const std::string & s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      i++;
      out += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
    }
    else {
      out += s[i];
    }
  }
  return out;
}

std::vector<std::string> split(const std::string & line) {
  std::vector<std::string> f;
  size_t at = 0;
  for (;;) {
    const size_t tab = line.find('\t', at);
    f.push_back(unesc(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at)));
    if (tab == std::string::npos) {
      break;
    }
    at = tab + 1;
  }
  return f;
}

std::string json(const std::string & s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '\r': out += "\\r"; break;
    default:
      if (c < 0x20) {
        char b[8];
        snprintf(b, sizeof b, "\\u%04x", c);
        out += b;
      }
      else {
        out += static_cast<char>(c);
      }
    }
  }
  return out + "\"";
}

std::string name(GTEXT_CSS_Status st) {
  if (st == GTEXT_CSS_OK) {
    return "ok";
  }
  const char * n = gtext_cssom_exception_name(st);
  return n ? n : std::string("status ") + gtext_css_status_string(st);
}

std::string css_text(GTEXT_CSSOM_Rule * r) {
  char * out = nullptr;
  size_t n = 0;
  if (gtext_cssom_rule_css_text(r, &out, &n) != GTEXT_CSS_OK) {
    return "<error>";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(nullptr, out);
  return s;
}

struct Target {
  GTEXT_CSSOM_Sheet * sheet = nullptr;
  GTEXT_CSSOM_Rule * rule = nullptr;
  bool ok = false;
};

/// The object at a path of the current sheet: "" is the sheet; "0.1" the second rule of the first. A rule with no `cssRules` makes the path a TypeError, as in script.
Target find(GTEXT_CSSOM_Sheet * sheet, const std::string & path) {
  Target t;
  if (!sheet) {
    return t;
  }
  if (path.empty()) {
    t.sheet = sheet;
    t.ok = true;
    return t;
  }
  const GTEXT_CSSOM_Rule_List * list = nullptr;
  if (gtext_cssom_sheet_rules(sheet, &list) != GTEXT_CSS_OK) {
    return t;
  }
  size_t at = 0;
  GTEXT_CSSOM_Rule * cur = nullptr;
  while (at <= path.size()) {
    const size_t dot = path.find('.', at);
    const size_t index = std::strtoul(path.substr(at, dot == std::string::npos ? std::string::npos : dot - at).c_str(), nullptr, 10);
    if (!list) {
      gtext_cssom_rule_release(cur);
      return t;
    }
    GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_at(list, index);
    gtext_cssom_rule_release(cur);
    cur = next;
    if (!cur) {
      return t;
    }
    list = gtext_cssom_rule_css_rules(cur);
    if (dot == std::string::npos) {
      break;
    }
    at = dot + 1;
  }
  t.rule = cur;
  t.ok = cur != nullptr;
  return t;
}


std::string take(GTEXT_CSSOM_Document * doc, char * buf, size_t n) {
  std::string s(buf ? buf : "", n);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(doc), buf);
  return s;
}

/// A style handle of the rule, or NULL when the rule has none (a TypeError in script).
GTEXT_CSSOM_Decl * style_of(GTEXT_CSSOM_Rule * rule) {
  GTEXT_CSSOM_Decl * d = nullptr;
  return gtext_cssom_rule_style(rule, &d) == GTEXT_CSS_OK ? d : nullptr;
}

GTEXT_CSSOM_Media_List * media_of(const Target & t) {
  GTEXT_CSSOM_Media_List * m = nullptr;
  const GTEXT_CSS_Status st = t.rule ? gtext_cssom_rule_media(t.rule, &m) : gtext_cssom_sheet_media(t.sheet, &m);
  return st == GTEXT_CSS_OK ? m : nullptr;
}

struct World {
  GTEXT_CSSOM_Document * doc = nullptr;
  GTEXT_CSS_Conditions * conditions = nullptr;
  std::vector<GTEXT_CSSOM_Sheet *> sheets;
  GTEXT_CSSOM_Sheet * current = nullptr;
};

bool open_world(World & w, GTEXT_CSS_Registry * registry) {
  GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(nullptr);
  env.viewport.width = 1280;
  env.viewport.height = 800;
  env.device.width = 1280;
  env.device.height = 800;
  GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
  o.media = &env;
  if (gtext_css_conditions_new(registry, &o, &w.conditions) != GTEXT_CSS_OK) {
    return false;
  }
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = registry;
  c.conditions = w.conditions;
  return gtext_cssom_document_new(&c, &w.doc) == GTEXT_CSS_OK;
}

std::string run_op(World & w, const std::vector<std::string> & f) {
  const std::string & op = f[0];
  GTEXT_CSSOM_Outcome o;
  std::memset(&o, 0, sizeof o);
  if (op == "S" || op == "C") {
    GTEXT_CSSOM_Sheet * s = nullptr;
    if (op == "S") {
      gtext_cssom_sheet_new_parsed(w.doc, f[1].data(), f[1].size(), nullptr, &s);
    }
    else {
      gtext_cssom_sheet_new_constructed(w.doc, nullptr, &s);
    }
    w.sheets.push_back(s);
    w.current = s;
    return "ok";
  }
  if (op == "U") {
    const size_t n = std::strtoul(f[1].c_str(), nullptr, 10);
    if (n < w.sheets.size()) {
      w.current = w.sheets[n];
    }
    return "ok";
  }
  if (op == "R") {
    return w.current ? name(gtext_cssom_sheet_replace_sync(w.current, f[1].data(), f[1].size(), &o)) : "TypeError";
  }
  if (op == "DIS") {
    return w.current ? name(gtext_cssom_sheet_set_disabled(w.current, f[1] == "1", &o)) : "TypeError";
  }
  if (op == "AD") {
    std::vector<GTEXT_CSSOM_Sheet *> list;
    size_t at = 0;
    while (at < f[1].size()) {
      const size_t comma = f[1].find(',', at);
      const size_t i = std::strtoul(f[1].substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str(), nullptr, 10);
      if (i < w.sheets.size()) {
        list.push_back(w.sheets[i]);
      }
      if (comma == std::string::npos) {
        break;
      }
      at = comma + 1;
    }
    return name(gtext_cssom_scope_adopted_set(gtext_cssom_document_scope(w.doc), list.data(), list.size(), &o));
  }
  if (op == "MM") {
    GTEXT_CSSOM_Media_Query_List * l = nullptr;
    if (gtext_cssom_match_media(w.doc, f[1].data(), f[1].size(), &l) != GTEXT_CSS_OK) {
      return "error";
    }
    char * m = nullptr;
    size_t n = 0;
    gtext_cssom_media_query_list_media(l, &m, &n);
    bool matches = false;
    gtext_cssom_media_query_list_matches(l, &matches);
    const std::string s = take(w.doc, m, n) + "|" + (matches ? "true" : "false");
    gtext_cssom_media_query_list_release(l);
    return s;
  }
  Target t = find(w.current, f[1]);
  if (!t.ok) {
    return "TypeError";
  }
  std::string answer = "ok";
  if (op == "I") {
    const size_t index = f[2] == "-" ? 0 : std::strtoul(f[2].c_str(), nullptr, 10);
    if (t.rule) {
      const GTEXT_CSS_Status st = gtext_cssom_rule_insert_rule(t.rule, f[3].data(), f[3].size(), index, nullptr, &o);
      answer = st == GTEXT_CSS_E_UNSUPPORTED ? "TypeError" : name(st);
    }
    else {
      answer = name(gtext_cssom_sheet_insert_rule(t.sheet, f[3].data(), f[3].size(), index, nullptr, &o));
    }
  }
  else if (op == "D" || op == "X") {
    const bool keyframes = t.rule && gtext_cssom_rule_kind(t.rule) == GTEXT_CSSOM_RULE_KEYFRAMES;
    if (keyframes) {
      answer = name(gtext_cssom_keyframes_delete_rule(t.rule, f[2].data(), f[2].size(), &o));
    }
    else {
      const size_t index = op == "D" ? std::strtoul(f[2].c_str(), nullptr, 10) : 0;
      const GTEXT_CSS_Status st = t.rule ? gtext_cssom_rule_delete_rule(t.rule, index, &o) : gtext_cssom_sheet_delete_rule(t.sheet, index, &o);
      answer = st == GTEXT_CSS_E_UNSUPPORTED ? "TypeError" : name(st);
    }
  }
  else if (op == "A") {
    answer = t.rule && gtext_cssom_rule_kind(t.rule) == GTEXT_CSSOM_RULE_KEYFRAMES ? name(gtext_cssom_keyframes_append_rule(t.rule, f[2].data(), f[2].size(), nullptr, &o)) : "TypeError";
  }
  else if (op == "F") {
    if (!t.rule || gtext_cssom_rule_kind(t.rule) != GTEXT_CSSOM_RULE_KEYFRAMES) {
      answer = "TypeError";
    }
    else {
      GTEXT_CSSOM_Rule * found = nullptr;
      gtext_cssom_keyframes_find_rule(t.rule, f[2].data(), f[2].size(), &found);
      answer = found ? css_text(found) : kNull;
      gtext_cssom_rule_release(found);
    }
  }
  else if (op == "T") {
    if (t.rule) {
      const GTEXT_CSS_Status st = gtext_cssom_rule_set_selector_text(t.rule, f[2].data(), f[2].size(), &o);
      answer = st == GTEXT_CSS_E_UNSUPPORTED ? "ok" : name(st);
    }
  }
  else if (op == "K") {
    if (t.rule) {
      const GTEXT_CSS_Status st = gtext_cssom_rule_set_key_text(t.rule, f[2].data(), f[2].size(), &o);
      answer = st == GTEXT_CSS_E_UNSUPPORTED ? "ok" : name(st);
    }
  }
  else if (op == "N" || op == "E") {
    if (t.rule) {
      const std::string attr = op == "N" ? "name" : f[2];
      const std::string & value = op == "N" ? f[2] : f[3];
      const GTEXT_CSS_Status st = gtext_cssom_rule_set_attribute(t.rule, attr.data(), attr.size(), value.data(), value.size(), &o);
      answer = st == GTEXT_CSS_E_UNSUPPORTED ? "ok" : name(st);
    }
  }
  else if (op == "Z") {
    answer = "ok";   /* CSSRule.cssText: "On setting, the cssText attribute must do nothing." */
  }
  else if (op == "P" || op == "Q" || op == "Y" || op == "VS" || op == "VL" || op == "VP") {
    GTEXT_CSSOM_Decl * d = t.rule ? style_of(t.rule) : nullptr;
    if (!d) {
      answer = "TypeError";
    }
    else {
      if (op == "P") {
        answer = name(gtext_cssom_decl_set_property(d, f[2].data(), f[2].size(), f[3].data(), f[3].size(), f[4].data(), f[4].size(), &o));
      }
      else if (op == "Q") {
        char * old = nullptr;
        size_t n = 0;
        const GTEXT_CSS_Status st = gtext_cssom_decl_remove_property(d, f[2].data(), f[2].size(), &old, &n, &o);
        answer = st == GTEXT_CSS_OK ? take(w.doc, old, n) : name(st);
      }
      else if (op == "Y") {
        answer = name(gtext_cssom_decl_set_css_text(d, f[2].data(), f[2].size(), &o));
      }
      else if (op == "VS") {
        char * v = nullptr;
        size_t n = 0;
        answer = gtext_cssom_decl_get_property_value(d, f[2].data(), f[2].size(), &v, &n) == GTEXT_CSS_OK ? take(w.doc, v, n) : "error";
      }
      else if (op == "VP") {
        const char * p = "";
        gtext_cssom_decl_get_property_priority(d, f[2].data(), f[2].size(), &p);
        answer = p;
      }
      else {
        size_t n = 0;
        gtext_cssom_decl_length(d, &n);
        answer = std::to_string(n);
      }
      gtext_cssom_decl_release(d);
    }
  }
  else if (op == "M" || op == "MA" || op == "MD") {
    GTEXT_CSSOM_Media_List * m = media_of(t);
    if (!m) {
      answer = "TypeError";
    }
    else {
      answer = name(op == "M" ? gtext_cssom_media_set_text(m, f[2].data(), f[2].size(), &o)
                              : op == "MA" ? gtext_cssom_media_append(m, f[2].data(), f[2].size(), &o) : gtext_cssom_media_delete(m, f[2].data(), f[2].size(), &o));
      gtext_cssom_media_release(m);
    }
  }
  else if (op == "W") {
    GTEXT_CSS_Feature_Type type;
    if (!t.rule || !gtext_cssom_feature_map_type(f[2].data(), f[2].size(), &type) || gtext_cssom_rule_kind(t.rule) != GTEXT_CSSOM_RULE_FONT_FEATURE_VALUES) {
      answer = "TypeError";
    }
    else {
      std::vector<uint32_t> v;
      size_t at = 0;
      while (at <= f[4].size() && !f[4].empty()) {
        const size_t comma = f[4].find(',', at);
        v.push_back(static_cast<uint32_t>(std::strtoul(f[4].substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str(), nullptr, 10)));
        if (comma == std::string::npos) {
          break;
        }
        at = comma + 1;
      }
      answer = name(gtext_cssom_feature_map_set(t.rule, type, f[3].data(), f[3].size(), v.data(), v.size(), &o));
    }
  }
  else if (op == "V") {
    if (!t.rule) {
      answer = "undefined";
    }
    else if (f[2] == "cssText") {
      answer = css_text(t.rule);
    }
    else {
      char * out = nullptr;
      size_t n = 0;
      bool is_null = false;
      GTEXT_CSS_Status st = GTEXT_CSS_E_UNSUPPORTED;
      if (f[2] == "selectorText") {
        st = gtext_cssom_rule_selector_text(t.rule, &out, &n);
      }
      else if (f[2] == "keyText") {
        st = gtext_cssom_rule_key_text(t.rule, &out, &n);
      }
      else {
        st = gtext_cssom_rule_get_attribute(t.rule, f[2].data(), f[2].size(), &out, &n, &is_null);
      }
      answer = st != GTEXT_CSS_OK ? "undefined" : is_null ? kNull : take(w.doc, out, n);
    }
  }
  else {
    answer = "?";
  }
  gtext_cssom_rule_release(t.rule);
  return answer;
}

std::string state(World & w) {
  std::string out = "[";
  for (size_t i = 0; i < w.sheets.size(); i++) {
    GTEXT_CSSOM_Sheet * s = w.sheets[i];
    out += i ? ",{\"r\":[" : "{\"r\":[";
    const GTEXT_CSSOM_Rule_List * list = nullptr;
    bool first = true;
    if (gtext_cssom_sheet_rules(s, &list) == GTEXT_CSS_OK) {
      for (GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_first(list); r;) {
        out += (first ? "" : ",") + json(css_text(r));
        first = false;
        GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(list, r);
        gtext_cssom_rule_release(r);
        r = next;
      }
    }
    GTEXT_CSSOM_Media_List * m = nullptr;
    std::string mt;
    if (gtext_cssom_sheet_media(s, &m) == GTEXT_CSS_OK) {
      char * b = nullptr;
      size_t n = 0;
      if (gtext_cssom_media_text(m, &b, &n) == GTEXT_CSS_OK) {
        mt = take(w.doc, b, n);
      }
      gtext_cssom_media_release(m);
    }
    out += "],\"m\":" + json(mt) + ",\"d\":" + (gtext_cssom_sheet_disabled(s) ? "true" : "false") + "}";
  }
  return out + "]";
}

}  // namespace

int main() {
  GTEXT_CSS_Registry * registry = nullptr;
  if (gtext_css_registry_new(nullptr, &registry) != GTEXT_CSS_OK) {
    return 2;
  }
  std::string line;
  bool in_doc = false;
  World w;
  std::vector<std::string> answers;
  auto finish = [&]() {
    if (!in_doc) {
      return;
    }
    std::string out = "{\"r\":[";
    for (size_t i = 0; i < answers.size(); i++) {
      out += (i ? "," : "") + (answers[i] == kNull ? std::string("null") : json(answers[i]));
    }
    out += "],\"t\":" + state(w) + "}";
    puts(out.c_str());
    for (GTEXT_CSSOM_Sheet * s : w.sheets) {
      gtext_cssom_sheet_release(s);
    }
    gtext_cssom_document_release(w.doc);
    gtext_css_conditions_free(w.conditions);
    w = World();
    answers.clear();
  };
  while (std::getline(std::cin, line)) {
    if (line == "D") {
      finish();
      in_doc = true;
      if (!open_world(w, registry)) {
        return 2;
      }
      continue;
    }
    if (line.empty() || !in_doc) {
      continue;
    }
    std::vector<std::string> f = split(line);
    while (f.size() < 6) {
      f.push_back("");
    }
    answers.push_back(run_op(w, f));
  }
  finish();
  gtext_css_registry_free(registry);
  return 0;
}
