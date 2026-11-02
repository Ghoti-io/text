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
 * This library's answers to the documents of the rule-operations differential (tools/oracle/css_ruleops_diff.py): the same operations the browsers' driver runs
 * (containers/cssprop/rules_driver.mjs), one document after another.
 *
 *   css_ruleops_ours < ops.tsv
 *
 * Input: for each document a line `D`, then one line for each operation (tab separated; backslash, line feed and tab escaped as `\\`, `\n`, `\t`): `S css`, `C`,
 * `I path index text` (index `-`: omitted), `D path index`, `A path text`, `X path select`, `F path select`. Output: one line for each document, a JSON object
 * {"r": [one answer for each operation], "t": [the text of each rule of the current sheet]}, the answers being "ok", the name of the exception, or for `F` the text or null.
 *
 * The browsers' answers for a call a JavaScript caller makes with a value of another type or on an object that has no such method are reproduced here as the caller's
 * language gives them: a path that names no rule, a method the rule does not have, is a TypeError; `deleteRule("50%")` on a rule that is not @keyframes converts the string to
 * an unsigned long (0), and `deleteRule(n)` on @keyframes is a selector that no keyframe has.
 */

#include <ghoti.io/text/css.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "cssom_rule_internal.h"

namespace {

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

}  // namespace

int main() {
  GTEXT_CSS_Registry * registry = nullptr;
  if (gtext_css_registry_new(nullptr, &registry) != GTEXT_CSS_OK) {
    return 2;
  }
  std::string line;
  bool in_doc = false;
  GTEXT_CSSOM_Document * doc = nullptr;
  GTEXT_CSSOM_Sheet * sheet = nullptr;
  std::vector<std::string> answers;
  auto finish = [&]() {
    if (!in_doc) {
      return;
    }
    std::string out = "{\"r\":[";
    for (size_t i = 0; i < answers.size(); i++) {
      out += (i ? "," : "") + (answers[i] == "\x01null" ? std::string("null") : json(answers[i]));
    }
    out += "],\"t\":[";
    const GTEXT_CSSOM_Rule_List * list = nullptr;
    bool first = true;
    if (sheet && gtext_cssom_sheet_rules(sheet, &list) == GTEXT_CSS_OK) {
      for (GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_first(list); r;) {
        out += (first ? "" : ",") + json(css_text(r));
        first = false;
        GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(list, r);
        gtext_cssom_rule_release(r);
        r = next;
      }
    }
    out += "]}";
    puts(out.c_str());
    gtext_cssom_sheet_release(sheet);
    sheet = nullptr;
    gtext_cssom_document_release(doc);
    doc = nullptr;
    answers.clear();
  };
  while (std::getline(std::cin, line)) {
    if (line == "D") {
      finish();
      in_doc = true;
      GTEXT_CSSOM_Config c = gtext_cssom_config_default();
      c.registry = registry;
      gtext_cssom_document_new(&c, &doc);
      continue;
    }
    if (line.empty() || !in_doc) {
      continue;
    }
    const std::vector<std::string> f = split(line);
    const std::string & op = f[0];
    std::string answer = "ok";
    if (op == "S" || op == "C") {
      gtext_cssom_sheet_release(sheet);
      sheet = nullptr;
      if (op == "S") {
        gtext_cssom_sheet_new_parsed(doc, f[1].data(), f[1].size(), nullptr, &sheet);
      }
      else {
        gtext_cssom_sheet_new_parsed(doc, "", 0, nullptr, &sheet);
        sheet->constructed = true;
      }
    }
    else {
      Target t = find(sheet, f[1]);
      GTEXT_CSSOM_Outcome o;
      if (!t.ok) {
        answer = "TypeError";
      }
      else if (op == "I") {
        const size_t index = f[2] == "-" ? 0 : std::strtoul(f[2].c_str(), nullptr, 10);
        GTEXT_CSS_Status st;
        if (t.rule) {
          st = gtext_cssom_rule_insert_rule(t.rule, f[3].data(), f[3].size(), index, nullptr, &o);
          answer = st == GTEXT_CSS_E_UNSUPPORTED ? "TypeError" : name(st);
        }
        else {
          answer = name(gtext_cssom_sheet_insert_rule(t.sheet, f[3].data(), f[3].size(), index, nullptr, &o));
        }
      }
      else if (op == "D" || op == "X") {
        const bool keyframes = t.rule && gtext_cssom_rule_kind(t.rule) == GTEXT_CSSOM_RULE_KEYFRAMES;
        if (keyframes) {
          /* deleteRule(select): a number is converted to a string, which no keyframe selector is */
          const std::string select = op == "X" ? f[2] : f[2];
          answer = name(gtext_cssom_keyframes_delete_rule(t.rule, select.data(), select.size(), &o));
        }
        else {
          const size_t index = op == "D" ? std::strtoul(f[2].c_str(), nullptr, 10) : 0;   /* a string is NaN as a number: 0 */
          GTEXT_CSS_Status st = t.rule ? gtext_cssom_rule_delete_rule(t.rule, index, &o) : gtext_cssom_sheet_delete_rule(t.sheet, index, &o);
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
          answer = found ? css_text(found) : "\x01null";
          gtext_cssom_rule_release(found);
        }
      }
      gtext_cssom_rule_release(t.rule);
    }
    answers.push_back(answer);
  }
  finish();
  gtext_css_registry_free(registry);
  return 0;
}
