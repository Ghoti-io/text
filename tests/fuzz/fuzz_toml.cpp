/**
 * @file
 *
 * libFuzzer harness for the TOML *reader*.
 *
 * The contract being fuzzed: for any byte string at all, the parser either
 * returns a document or reports an error. It must not crash, read out of
 * bounds, or leak - and if it does return a document, walking, writing and
 * converting that document must be safe too.
 *
 * Four properties beyond "it did not crash", each of which a single-call
 * harness cannot see:
 *
 *   - **1.0.0 is a subset of 1.1.0.** Every document this module accepts at
 *     GTEXT_TOML_VERSION_1_0_0 must be accepted at _1_1_0. That is not a
 *     property of the specifications - 1.1.0 *tightens* two things 1.0.0's prose
 *     allowed - but it is a property of this module, which takes the strict
 *     ABNF reading of both wherever they disagree. The corpus cannot state it:
 *     each manifest only asks an arm the cases its own list decides.
 *   - **The event walk agrees with the parse.** gtext_toml_read_events() is the
 *     same parser with a callback, so it must accept exactly the same documents,
 *     with the same status and at the same line and column. Anything else means
 *     an emission point broke a good document, which is the failure mode of
 *     adding events to a working parser.
 *   - **What the writer writes, the parser reads.** Any accepted document must
 *     write, its output must parse - at 1.0.0 as well as at the version it was
 *     read under, since the writer emits 1.0.0 spellings - and the values must
 *     survive. This is the channel that phase 2 found a one-byte overread with
 *     and that no sanitizer reported.
 *   - **A trip through JSON settles after one pass.** JSON cannot hold a
 *     date-time or tell 1.0 from 1, so the first conversion loses those; a
 *     second must lose nothing more. Converting twice and comparing is free on
 *     every input and asserts the shape of the loss rather than its absence.
 *
 * The comparable form of "the same values" is the JSON text of the document,
 * which is what gtext_toml_to_json() is for here - the non-finite policy is set
 * to strings so that every document converts rather than the interesting ones
 * being skipped, and **the keys are sorted**. That last part is not tidiness:
 * the writer's documented rule is that plain keys come before sub-tables,
 * whatever order they were defined in, because every bare key after a
 * `[header]` belongs to that header's table. So `[[a.b]]` before `y = 2`
 * *must* come back in the other order, and the first minute of the first run
 * of this harness reported it - which was the harness comparing something
 * stricter than the contract. Sorting is what makes the comparison the one the
 * corpus makes, where a tagged-JSON tree is compared key by key.
 *
 * Build with: make fuzz-toml     Run: make fuzz-run-toml
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/toml.h>
}

namespace {

/* Bytes as something legible: a crash artifact is a handful of bytes that says
   nothing on its own, and what makes one triageable is seeing the document. */
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

/** Walk a document so the accessors run, not only the parser. */
void walk(const GTEXT_TOML_Value * v, int depth) {
  if (!v || depth > 400) return;
  switch (gtext_toml_value_type(v)) {
    case GTEXT_TOML_TABLE: {
      const size_t n = gtext_toml_table_size(v);
      for (size_t i = 0; i < n; i++) {
        size_t key_len = 0;
        const char * key = gtext_toml_table_key_at(v, i, &key_len);
        /* Found by the same lookup, which is the pair of paths a table has: by
           position and by key. */
        (void) gtext_toml_table_get(v, key, key_len);
        walk(gtext_toml_table_value_at(v, i), depth + 1);
      }
      (void) gtext_toml_value_leading_comment(v);
      (void) gtext_toml_value_inline_comment(v);
      (void) gtext_toml_value_trailing_comment(v);
      break;
    }
    case GTEXT_TOML_ARRAY: {
      const size_t n = gtext_toml_array_size(v);
      for (size_t i = 0; i < n; i++) walk(gtext_toml_array_get(v, i), depth + 1);
      break;
    }
    case GTEXT_TOML_STRING: {
      size_t len = 0;
      (void) gtext_toml_value_string(v, &len);
      break;
    }
    case GTEXT_TOML_INTEGER: {
      int64_t i = 0;
      (void) gtext_toml_value_integer(v, &i);
      break;
    }
    case GTEXT_TOML_FLOAT: {
      double d = 0.0;
      (void) gtext_toml_value_float(v, &d);
      break;
    }
    case GTEXT_TOML_DATETIME: {
      GCHRON_TomlValue dt;
      std::memset(&dt, 0, sizeof(dt));
      (void) gtext_toml_value_datetime(v, &dt);
      break;
    }
    default: break;
  }
}

/** The JSON text of a document, as the comparable form of its values. */
bool values_of(const GTEXT_TOML_Value * root, std::string * out) {
  GTEXT_TOML_To_JSON_Options opts = gtext_toml_to_json_options_default();
  /* Strings rather than the refusing default, so that a document holding `nan`
     is compared rather than skipped. */
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
  GTEXT_JSON_Error jerr;
  std::memset(&jerr, 0, sizeof(jerr));
  GTEXT_JSON_Write_Options wopts = gtext_json_write_options_default();
  /* The whole reason this goes through a writer rather than a comparison
     function: a stable text needs a stable key order, and a TOML round trip is
     allowed to change it. */
  wopts.sort_object_keys = true;
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

/** Everything the writer produced, or false if it refused. */
bool write_document(const GTEXT_TOML_Value * root,
    GTEXT_TOML_Table_Style style, std::string * out) {
  GTEXT_TOML_Write_Options wopts = gtext_toml_write_options_default();
  wopts.table_style = style;
  GTEXT_TOML_Sink sink;
  if (gtext_toml_sink_buffer(&sink) != GTEXT_TOML_OK) return false;
  const bool ok = gtext_toml_write(root, &sink, &wopts) == GTEXT_TOML_OK;
  if (ok) {
    out->assign(
        gtext_toml_sink_buffer_data(&sink), gtext_toml_sink_buffer_size(&sink));
  }
  gtext_toml_sink_buffer_free(&sink);
  return ok;
}

struct Outcome {
  bool accepted = false;
  GTEXT_TOML_Status status = GTEXT_TOML_OK;
  int line = 0;
  int col = 0;
};

/** What the event walk makes of the same bytes under the same options. */
Outcome walk_events(
    const char * text, size_t len, const GTEXT_TOML_Parse_Options * opts) {
  auto sink = [](void *, const GTEXT_TOML_Event * e,
                  GTEXT_TOML_Error *) -> GTEXT_TOML_Status {
    /* Reading the event's own fields, so that a key path or a borrowed scalar
       pointing at freed memory is a finding rather than a value nobody looks
       at. */
    for (size_t i = 0; i < e->key.count; i++) {
      volatile char first = e->key.parts[i].len ? e->key.parts[i].data[0] : 0;
      (void) first;
    }
    if (e->value) (void) gtext_toml_value_type(e->value);
    if (e->comment && e->comment_len) {
      volatile char first = e->comment[0];
      (void) first;
    }
    return GTEXT_TOML_OK;
  };
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  Outcome out;
  const GTEXT_TOML_Status status =
      gtext_toml_read_events(text, len, opts, sink, nullptr, &err);
  out.accepted = status == GTEXT_TOML_OK;
  out.status = status;
  out.line = err.line;
  out.col = err.col;
  gtext_toml_error_free(&err);
  return out;
}

Outcome parse_document(const char * text, size_t len,
    const GTEXT_TOML_Parse_Options * opts, GTEXT_TOML_Value ** out_root) {
  GTEXT_TOML_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root = gtext_toml_parse(text, len, opts, &err);
  Outcome out;
  out.accepted = root != nullptr;
  out.status = err.code;
  out.line = err.line;
  out.col = err.col;
  gtext_toml_error_free(&err);
  if (out_root) *out_root = root;
  else gtext_toml_free(root);
  return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) return 0;

  /* One byte of options, drawn from the input so that the version arm and the
     comment machinery are reached at all - a fixed-options harness would fuzz
     one quarter of this module. */
  const uint8_t knobs = data[0];
  const char * text = reinterpret_cast<const char *>(data + 1);
  const size_t len = size - 1;

  GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
  opts.version = (knobs & 0x01) ? GTEXT_TOML_VERSION_1_1_0
                                : GTEXT_TOML_VERSION_1_0_0;
  opts.retain_comments = (knobs & 0x02) != 0;
  /* Zero is in range on purpose: it is the setting with no limit, which the
     explicit stacks are supposed to make safe. */
  opts.max_depth = (knobs & 0x04) ? 0 : (size_t) ((knobs >> 3) & 0x1F);

  GTEXT_TOML_Value * root = nullptr;
  const Outcome parsed = parse_document(text, len, &opts, &root);

  /* The event walk is the same parser, so it must reach the same verdict in the
     same place. */
  const Outcome walked = walk_events(text, len, &opts);
  if (walked.accepted != parsed.accepted || walked.status != parsed.status
      || walked.line != parsed.line || walked.col != parsed.col) {
    fprintf(stderr,
        "the event walk disagrees with the parse:\n"
        "  parse  accepted=%d status=%d at %d:%d\n"
        "  events accepted=%d status=%d at %d:%d\n  %s\n",
        (int) parsed.accepted, (int) parsed.status, parsed.line, parsed.col,
        (int) walked.accepted, (int) walked.status, walked.line, walked.col,
        legible(text, len).c_str());
    __builtin_trap();
  }

  /* 1.0.0's documents are a subset of 1.1.0's, in this module. Asserted from
     the strict side only: a document 1.1.0 accepts may well be refused at
     1.0.0, which is what the option is for. */
  if (opts.version == GTEXT_TOML_VERSION_1_0_0 && parsed.accepted) {
    GTEXT_TOML_Parse_Options next = opts;
    next.version = GTEXT_TOML_VERSION_1_1_0;
    if (!parse_document(text, len, &next, nullptr).accepted) {
      fprintf(stderr, "accepted at 1.0.0 and refused at 1.1.0:\n  %s\n",
          legible(text, len).c_str());
      __builtin_trap();
    }
  }

  if (!root) return 0;
  walk(root, 0);

  std::string before;
  const bool comparable = values_of(root, &before);

  /* What the writer writes, the parser reads - at 1.0.0 whatever the document
     was read under, because the writer emits 1.0.0 spellings and that is the
     sharper claim. The style is an axis: two of the three rearrange the
     document, and the inline style legitimately refuses a document whose
     comments cannot be placed inside braces. */
  static const GTEXT_TOML_Table_Style styles[] = {
      GTEXT_TOML_TABLE_STYLE_AS_READ,
      GTEXT_TOML_TABLE_STYLE_HEADERS,
      GTEXT_TOML_TABLE_STYLE_INLINE,
  };
  for (const GTEXT_TOML_Table_Style style : styles) {
    std::string written;
    if (!write_document(root, style, &written)) continue;
    GTEXT_TOML_Parse_Options strict = gtext_toml_parse_options_default();
    strict.max_depth = 0;
    strict.retain_comments = opts.retain_comments;
    GTEXT_TOML_Value * again = nullptr;
    if (!parse_document(written.data(), written.size(), &strict, &again)
             .accepted) {
      fprintf(stderr,
          "the writer wrote what the parser refuses (style %d):\n  %s\n",
          (int) style, legible(written.data(), written.size()).c_str());
      __builtin_trap();
    }
    std::string after;
    if (comparable && values_of(again, &after) && before != after) {
      fprintf(stderr,
          "the writer wrote a different document (style %d):\n"
          "  wrote  %s\n  before %s\n  after  %s\n",
          (int) style, legible(written.data(), written.size()).c_str(),
          before.c_str(), after.c_str());
      __builtin_trap();
    }
    gtext_toml_free(again);
  }

  /* A trip through JSON loses a date-time's type and a whole float's, and
     nothing else - so doing it twice must equal doing it once. */
  if (comparable) {
    GTEXT_TOML_To_JSON_Options jopts = gtext_toml_to_json_options_default();
    jopts.nonfinite = GTEXT_TOML_JSON_NONFINITE_STRING;
    jopts.max_depth = 0;
    GTEXT_JSON_Value * json = nullptr;
    if (gtext_toml_to_json(root, &jopts, &json, nullptr) == GTEXT_TOML_OK) {
      GTEXT_TOML_From_JSON_Options bopts =
          gtext_toml_from_json_options_default();
      bopts.max_depth = 0;
      GTEXT_TOML_Value * back = nullptr;
      if (gtext_json_to_toml(json, &bopts, &back, nullptr) == GTEXT_TOML_OK) {
        std::string once;
        std::string twice;
        if (values_of(back, &once)) {
          GTEXT_JSON_Value * second = nullptr;
          if (gtext_toml_to_json(back, &jopts, &second, nullptr)
              == GTEXT_TOML_OK) {
            GTEXT_TOML_Value * again = nullptr;
            if (gtext_json_to_toml(second, &bopts, &again, nullptr)
                == GTEXT_TOML_OK) {
              if (values_of(again, &twice) && once != twice) {
                fprintf(stderr,
                    "a second trip through JSON changed the document:\n"
                    "  once  %s\n  twice %s\n  from  %s\n",
                    once.c_str(), twice.c_str(), legible(text, len).c_str());
                __builtin_trap();
              }
              gtext_toml_free(again);
            }
            gtext_json_free(second);
          }
        }
        gtext_toml_free(back);
      }
      gtext_json_free(json);
    }
  }

  gtext_toml_free(root);
  return 0;
}
