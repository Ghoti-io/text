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
 * What the fuzzers of the cssText share: the model of the text given back to the parser.
 */

#ifndef GHOTI_TEXT_FUZZ_PENDING_TEXT_H
#define GHOTI_TEXT_FUZZ_PENDING_TEXT_H

#include <string>

namespace cssfuzz {

/// The cssText without its empty declarations. A longhand a shorthand with a var() left pending is written `name: ;` ("Pending-substitution values must be serialized as the empty string, if
/// an API allows them to be observed", Values 5; the WPT page css-variables/variable-cssText.html expects it and Chromium 156 and Firefox 158 write it), and that is no declaration to
/// read again: the text given back to the parser has lost them, so the property that holds is that it is the text with them taken out, not the text itself. A custom property with no
/// value is a declaration, and stays, and so is `copy-into` with none (its grammar, "none | [ ... ]?", takes none).
inline std::string without_pending(const std::string & text) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    // one declaration: up to the `;` that is not in a string or a function
    size_t j = i;
    int depth = 0;
    char quote = 0;
    for (; j < text.size(); j++) {
      const char c = text[j];
      if (quote) {
        if (c == '\\') {
          j++;
        }
        else if (c == quote) {
          quote = 0;
        }
      }
      else if (c == '"' || c == '\'') {
        quote = c;
      }
      else if (c == '(') {
        depth++;
      }
      else if (c == ')' && depth > 0) {
        depth--;
      }
      else if (c == ';' && depth == 0) {
        break;
      }
    }
    const size_t end = j < text.size() ? j + 1 : j;
    const std::string decl = text.substr(i, end - i);
    const size_t colon = decl.find(':');
    bool pending = false;
    if (colon != std::string::npos && decl.compare(0, 2, "--") != 0 && decl.compare(0, 10, "copy-into:") != 0) {   // the grammar of copy-into takes no value
      std::string value = decl.substr(colon + 1);
      while (!value.empty() && (value.back() == ';' || value.back() == ' ')) {
        value.pop_back();
      }
      while (!value.empty() && value.front() == ' ') {
        value.erase(0, 1);
      }
      pending = value.empty() || value == "!important";
    }
    if (!pending) {
      if (!out.empty()) {
        out += ' ';
      }
      out += decl;
    }
    i = end;
    while (i < text.size() && text[i] == ' ') {
      i++;
    }
  }
  return out;
}

}  // namespace cssfuzz

#endif
