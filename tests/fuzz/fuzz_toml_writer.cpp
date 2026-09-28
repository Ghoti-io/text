/**
 * @file
 *
 * libFuzzer harness for the TOML *writer*, and for the comments.
 *
 * fuzz_toml.cpp writes every document it parsed, so the writer is not unreached
 * - but a corpus of TOML text can only carry values the parser accepts, and
 * that is the bound that has mattered most in this repository. The interesting
 * refusals here are all reachable only through the DOM API:
 *
 *   - a comment holding a control character, a line break, or bytes that are
 *     not UTF-8. A parse cannot produce one: it has already refused those.
 *   - a comment on a value that is going inside `{ }`, where there is no line
 *     to put it on. A parse never attaches one there.
 *   - a string or key that is not valid UTF-8, and a date-time `chron` will not
 *     spell. Both refusals exist and no document can reach either.
 *   - `gtext_toml_value_set_inline()` in combinations a parse cannot make: an
 *     array of tables marked static, a table marked inline holding a table
 *     marked as a header.
 *
 * The property is the same one in both families:
 *
 *     if the writer says OK, the bytes it wrote must parse,
 *     and must hold the same values.
 *
 * and for the documents that came from parsing, with comments retained, one
 * more: **the same comments must come back**. That is the corpus's `comments`
 * mode over inputs nobody chose. It is asserted only on the parsed family,
 * because a caller *can* set a comment that legitimately moves - a trailing
 * comment on a sub-table is written after that table's block, where the next
 * parse reads it as the following header's leading comment, which is what it
 * then is.
 *
 * "The same values" is the sorted JSON text of the document, for the reason
 * fuzz_toml.cpp gives: a round trip is allowed to reorder keys, because every
 * bare key after a `[header]` belongs to that header's table.
 *
 * Build with: make fuzz-toml-writer     Run: make fuzz-run-toml-writer
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

extern "C" {
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/toml.h>
}

namespace {

std::string legible(const char * p, size_t n) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char) p[i];
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c >= 0x20 && c < 0x7F) out.push_back((char) c);
    else {
      out += "\\x";
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

/** A cursor over the fuzzer's bytes, driving the DOM builder. */
struct Bytes {
  const uint8_t * p;
  size_t n;
  size_t i = 0;
  uint8_t next() { return i < n ? p[i++] : 0; }
  bool done() const { return i >= n; }
  /* A run of bytes, used raw: that is the point of building through the API,
     since a value put there is under no obligation to be text the parser would
     have accepted. */
  std::string chunk(uint8_t mask = 0x0F) {
    size_t len = next() & mask;
    std::string s;
    for (size_t k = 0; k < len && !done(); k++) s.push_back((char) next());
    return s;
  }
};

/** The sorted JSON text of a document: the comparable form of its values. */
bool values_of(const GTEXT_TOML_Value * root, std::string * out) {
  GTEXT_TOML_To_JSON_Options opts = gtext_toml_to_json_options_default();
  opts.nonfinite = GTEXT_TOML_JSON_NONFINITE_STRING;
  opts.max_depth = 0;
  GTEXT_JSON_Value * json = nullptr;
  if (gtext_toml_to_json(root, &opts, &json, nullptr) != GTEXT_TOML_OK) {
    return false;
  }
  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    gtext_json_free(json);
    return false;
  }
  GTEXT_JSON_Write_Options wopts = gtext_json_write_options_default();
  wopts.sort_object_keys = true;
  GTEXT_JSON_Error jerr;
  std::memset(&jerr, 0, sizeof(jerr));
  bool ok = false;
  if (gtext_json_write_value(&sink, &wopts, json, &jerr) == GTEXT_JSON_OK) {
    out->assign(
        gtext_json_sink_buffer_data(&sink), gtext_json_sink_buffer_size(&sink));
    ok = true;
  }
  gtext_json_sink_buffer_free(&sink);
  gtext_json_error_free(&jerr);
  gtext_json_free(json);
  return ok;
}

/**
 * Every comment in a document, as a multiset keyed by kind and text.
 *
 * A multiset and not a list: the writer is allowed to move a statement - plain
 * keys come before sub-tables - so the comment on it moves too, and the order
 * of the walk is not part of the promise. What is promised is that the same
 * comments, of the same kinds, are still there.
 */
void collect_comments(
    const GTEXT_TOML_Value * v, std::map<std::string, int> * into) {
  if (!v) return;
  const char * kinds[3] = {gtext_toml_value_leading_comment(v),
      gtext_toml_value_inline_comment(v), gtext_toml_value_trailing_comment(v)};
  static const char letters[3] = {'L', 'I', 'T'};
  for (int k = 0; k < 3; k++) {
    if (kinds[k]) {
      (*into)[std::string(1, letters[k]) + "|" + kinds[k]]++;
    }
  }
  if (gtext_toml_value_type(v) == GTEXT_TOML_TABLE) {
    const size_t n = gtext_toml_table_size(v);
    for (size_t i = 0; i < n; i++) {
      collect_comments(gtext_toml_table_value_at(v, i), into);
    }
  }
  else if (gtext_toml_value_type(v) == GTEXT_TOML_ARRAY) {
    const size_t n = gtext_toml_array_size(v);
    for (size_t i = 0; i < n; i++) {
      collect_comments(gtext_toml_array_get(v, i), into);
    }
  }
}

std::string spell_comments(const std::map<std::string, int> & counts) {
  std::string out;
  for (const auto & entry : counts) {
    out += entry.first + " x" + std::to_string(entry.second) + "; ";
  }
  return out;
}

bool write_document(const GTEXT_TOML_Value * root,
    GTEXT_TOML_Table_Style style, std::string * out,
    GTEXT_TOML_Status * status) {
  GTEXT_TOML_Write_Options wopts = gtext_toml_write_options_default();
  wopts.table_style = style;
  GTEXT_TOML_Sink sink;
  if (gtext_toml_sink_buffer(&sink) != GTEXT_TOML_OK) return false;
  *status = gtext_toml_write(root, &sink, &wopts);
  const bool ok = *status == GTEXT_TOML_OK;
  if (ok) {
    out->assign(
        gtext_toml_sink_buffer_data(&sink), gtext_toml_sink_buffer_size(&sink));
  }
  gtext_toml_sink_buffer_free(&sink);
  return ok;
}

/** Write, read back, and require the values to survive. */
void must_round_trip(const GTEXT_TOML_Value * root,
    GTEXT_TOML_Table_Style style, bool compare_comments) {
  std::string written;
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  if (!write_document(root, style, &written, &status)) return;

  GTEXT_TOML_Parse_Options popts = gtext_toml_parse_options_default();
  popts.max_depth = 0;
  popts.retain_comments = true;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * back =
      gtext_toml_parse(written.data(), written.size(), &popts, &err);
  if (!back) {
    fprintf(stderr,
        "the writer wrote what the parser refuses (style %d): %s\n  %s\n",
        (int) style, err.message ? err.message : "",
        legible(written.data(), written.size()).c_str());
    gtext_toml_error_free(&err);
    __builtin_trap();
  }
  gtext_toml_error_free(&err);

  std::string before;
  std::string after;
  if (values_of(root, &before) && values_of(back, &after) && before != after) {
    fprintf(stderr,
        "the writer wrote a different document (style %d):\n"
        "  wrote  %s\n  before %s\n  after  %s\n",
        (int) style, legible(written.data(), written.size()).c_str(),
        before.c_str(), after.c_str());
    __builtin_trap();
  }

  if (compare_comments) {
    std::map<std::string, int> was;
    std::map<std::string, int> is;
    collect_comments(root, &was);
    collect_comments(back, &is);
    if (was != is) {
      fprintf(stderr,
          "the comments changed (style %d):\n  wrote  %s\n"
          "  before %s\n  after  %s\n",
          (int) style, legible(written.data(), written.size()).c_str(),
          spell_comments(was).c_str(), spell_comments(is).c_str());
      __builtin_trap();
    }
  }
  gtext_toml_free(back);
}

/** One scalar from the fuzzer's bytes, including the kinds only the API makes. */
GTEXT_TOML_Value * build_scalar(Bytes & b) {
  switch (b.next() % 6) {
    case 0: {
      const std::string s = b.chunk(0x1F);
      return gtext_toml_new_string(nullptr, s.data(), s.size());
    }
    case 1: {
      /* The bounds and their neighbours are where a writer's two spellings of
         one limit disagree, so they are worth reaching often. */
      int64_t v = 0;
      for (int k = 0; k < 8; k++) v = (v << 8) | b.next();
      return gtext_toml_new_integer(nullptr, v);
    }
    case 2: {
      uint64_t bits = 0;
      for (int k = 0; k < 8; k++) bits = (bits << 8) | b.next();
      double d = 0.0;
      std::memcpy(&d, &bits, sizeof(d));
      /* Any double at all, infinities and NaNs included - which is the half of
         the float writer a corpus of text reaches only through `inf` and
         `nan`. */
      return gtext_toml_new_float(nullptr, d);
    }
    case 3: return gtext_toml_new_boolean(nullptr, (b.next() & 1) != 0);
    case 4: {
      /* A date-time built rather than parsed, so the fields may be ones no TOML
         text could have said. gtext_toml_write() answers E_DATETIME for those,
         which is a refusal and not a defect. */
      GCHRON_TomlValue dt;
      std::memset(&dt, 0, sizeof(dt));
      dt.kind = (GCHRON_TomlKind) (1 + (b.next() % 4));
      dt.civil.date.year = (int32_t) ((b.next() << 8) | b.next()) - 4000;
      dt.civil.date.month = (uint8_t) (b.next() % 14);
      dt.civil.date.day = (uint8_t) (b.next() % 34);
      dt.civil.time.hour = (uint8_t) (b.next() % 26);
      dt.civil.time.minute = (uint8_t) (b.next() % 62);
      dt.civil.time.second = (uint8_t) (b.next() % 62);
      dt.civil.time.nsec = (int32_t) (((uint32_t) b.next() << 22) | b.next());
      dt.offset_sec = (int32_t) (((int) b.next() - 128) * 900);
      dt.offset_unknown = (b.next() & 1) != 0;
      return gtext_toml_new_datetime(nullptr, &dt);
    }
    default: {
      /* A string whose bytes are not UTF-8 at all, which the writer must refuse
         rather than emit: this is the one shape no parsed document can be. */
      std::string s = b.chunk(0x07);
      s.push_back((char) 0xC3);
      s.push_back((char) 0x28);
      return gtext_toml_new_string(nullptr, s.data(), s.size());
    }
  }
}

/** Hang a comment or two on a node, from the bytes. */
void decorate(GTEXT_TOML_Value * v, Bytes & b) {
  const uint8_t how = b.next();
  if (how & 0x01) {
    const std::string c = b.chunk(0x0F);
    gtext_toml_value_set_leading_comment(v, c.c_str());
  }
  if (how & 0x02) {
    const std::string c = b.chunk(0x0F);
    gtext_toml_value_set_inline_comment(v, c.c_str());
  }
  if (how & 0x04) {
    const std::string c = b.chunk(0x0F);
    gtext_toml_value_set_trailing_comment(v, c.c_str());
  }
  if (how & 0x08) {
    /* Both directions, including the ones a parse never produces: an array of
       tables that says it is static, and a table that says it is inline while
       holding one that says it is a header. */
    gtext_toml_value_set_inline(v, (how & 0x10) != 0);
  }
}

GTEXT_TOML_Value * build(Bytes & b, int depth) {
  if (depth > 12 || b.done()) return build_scalar(b);
  const uint8_t what = b.next();
  if ((what % 5) < 3) {
    GTEXT_TOML_Value * scalar = build_scalar(b);
    if (scalar) decorate(scalar, b);
    return scalar;
  }
  const bool table = (what % 5) == 3;
  GTEXT_TOML_Value * container =
      table ? gtext_toml_new_table(nullptr) : gtext_toml_new_array(nullptr);
  if (!container) return nullptr;
  const size_t count = b.next() & 0x07;
  for (size_t i = 0; i < count && !b.done(); i++) {
    GTEXT_TOML_Value * child = build(b, depth + 1);
    if (!child) break;
    GTEXT_TOML_Status placed;
    if (table) {
      const std::string key = b.chunk(0x0F);
      placed = gtext_toml_table_set(container, key.data(), key.size(), child);
    }
    else {
      placed = gtext_toml_array_append(container, child);
    }
    if (placed != GTEXT_TOML_OK) {
      /* Refused for a reason the API states - a duplicate key, a cycle, a node
         already stored - so the child is still this function's to release. */
      gtext_toml_free(child);
    }
  }
  decorate(container, b);
  return container;
}

/** A document built through the API, written, and read back. */
void fuzz_built(const uint8_t * data, size_t size, uint8_t sel) {
  Bytes b{data, size};
  GTEXT_TOML_Value * root = gtext_toml_new_table(nullptr);
  if (!root) return;
  const size_t count = 1 + (b.next() & 0x03);
  for (size_t i = 0; i < count && !b.done(); i++) {
    GTEXT_TOML_Value * child = build(b, 0);
    if (!child) break;
    const std::string key = b.chunk(0x0F);
    if (gtext_toml_table_set(root, key.data(), key.size(), child)
        != GTEXT_TOML_OK) {
      gtext_toml_free(child);
    }
  }
  decorate(root, b);
  /* No comment comparison here: a caller can set a trailing comment on a
     sub-table, which is written after that table's block and read back as the
     next header's leading comment - the same comment, in the place it now
     occupies. The parsed family below is where comment fidelity is a promise. */
  must_round_trip(root, (GTEXT_TOML_Table_Style) (sel % 3), false);
  gtext_toml_free(root);
}

/** A document that came from parsing, with its comments, written and read. */
void fuzz_parsed(const uint8_t * data, size_t size, uint8_t sel) {
  GTEXT_TOML_Parse_Options popts = gtext_toml_parse_options_default();
  popts.retain_comments = true;
  popts.max_depth = 0;
  popts.version = (sel & 0x08) ? GTEXT_TOML_VERSION_1_1_0
                               : GTEXT_TOML_VERSION_1_0_0;
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root = gtext_toml_parse(
      reinterpret_cast<const char *>(data), size, &popts, &err);
  gtext_toml_error_free(&err);
  if (!root) return;
  /* The comments are a promise on this family: every one the tree holds came
     from a statement, so every one has a statement to go back on. */
  must_round_trip(root, (GTEXT_TOML_Table_Style) (sel % 3), true);
  gtext_toml_free(root);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) return 0;
  const uint8_t mode = data[0];
  const uint8_t sel = data[1];
  const uint8_t * body = data + 2;
  const size_t len = size - 2;
  if (mode & 1) fuzz_built(body, len, sel);
  else fuzz_parsed(body, len, sel);
  return 0;
}
