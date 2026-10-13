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
 * What the Encoding tests share beyond encoding_model.h: the Encoding Standard's data read from the pinned JSON files with
 * this library's JSON parser (so the expectations do not come from the generated tables).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_ENCODING_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_ENCODING_TEST_SUPPORT_H

#include <ghoti.io/text/json.h>

#include <gtest/gtest.h>

#include "encoding_model.h"

namespace enctest {

// ---- the standard's data, from the pinned JSON ---------------------------------------------------------

inline GTEXT_JSON_Value * load_json(const char * path) {
  GTEXT_JSON_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_JSON_Value * v = gtext_json_parse_file(path, nullptr, &err);
  if (!v) {
    ADD_FAILURE() << path << " is not here or does not parse (run tools/encoding/fetch.sh)";
  }
  return v;
}

struct EncodingRow {
  std::string name;
  std::vector<std::string> labels;
};

// encodings.json: every encoding in the standard's order.
inline std::vector<EncodingRow> load_encodings() {
  std::vector<EncodingRow> rows;
  GTEXT_JSON_Value * root = load_json("third_party/encoding/encodings.json");
  if (!root) {
    return rows;
  }
  for (size_t g = 0; g < gtext_json_array_size(root); g++) {
    const GTEXT_JSON_Value * group = gtext_json_array_get(root, g);
    const GTEXT_JSON_Value * encs = gtext_json_object_get(group, "encodings", 9);
    for (size_t e = 0; e < gtext_json_array_size(encs); e++) {
      const GTEXT_JSON_Value * enc = gtext_json_array_get(encs, e);
      EncodingRow row;
      const char * s = nullptr;
      size_t n = 0;
      gtext_json_get_string(gtext_json_object_get(enc, "name", 4), &s, &n);
      row.name.assign(s, n);
      const GTEXT_JSON_Value * labels = gtext_json_object_get(enc, "labels", 6);
      for (size_t l = 0; l < gtext_json_array_size(labels); l++) {
        gtext_json_get_string(gtext_json_array_get(labels, l), &s, &n);
        row.labels.emplace_back(s, n);
      }
      rows.push_back(row);
    }
  }
  gtext_json_free(root);
  return rows;
}

// indexes.json: the single-byte indexes, name (lowercase) to 128 code points, -1 for null.
inline std::map<std::string, std::vector<int>> load_single_byte_indexes() {
  std::map<std::string, std::vector<int>> out;
  GTEXT_JSON_Value * root = load_json("third_party/encoding/indexes.json");
  if (!root) {
    return out;
  }
  for (size_t k = 0; k < gtext_json_object_size(root); k++) {
    const GTEXT_JSON_Value * arr = gtext_json_object_value(root, k);
    if (gtext_json_array_size(arr) != 128) {
      continue;
    }
    std::vector<int> cps;
    for (size_t i = 0; i < 128; i++) {
      const GTEXT_JSON_Value * v = gtext_json_array_get(arr, i);
      int64_t x = -1;
      if (gtext_json_typeof(v) != GTEXT_JSON_NULL) {
        gtext_json_get_i64(v, &x);
      }
      cps.push_back(int(x));
    }
    out[gtext_json_object_key(root, k, nullptr)] = cps;
  }
  gtext_json_free(root);
  return out;
}

} // namespace enctest

#endif // GHOTI_IO_GTEXT_TESTS_ENCODING_TEST_SUPPORT_H
