/**
 * @file
 *
 * libFuzzer harness for the JSON parser.
 *
 * The contract being fuzzed: for any byte string at all, the parser either
 * returns a document or reports an error. It must not crash, read out of
 * bounds, or leak - and if it does return a document, walking that document
 * must be safe.
 *
 * The parse options are drawn from the first two input bytes so that the
 * relaxed extensions are reached as well as strict mode, which is what a
 * fixed-options harness would miss: the first byte carries the JSONC ones
 * (comments, trailing commas, single quotes, non-finite numbers) and the
 * number representations, the second the JSON5 ones. A corpus entry can also
 * ask for the JSON5 dialect as a whole, because that is how a caller asks for
 * it and because it turns on combinations the individual bits reach only by
 * chance.
 *
 * There is a second contract here, and it is the one a single-parser harness
 * cannot see: **the two parsers must agree**. gtext_json_parse() and
 * gtext_json_stream_feed() share the lexer and not the grammar, so a document
 * either is JSON or is not, whichever reads it, and at whatever chunk sizes it
 * arrives in. Six divergences between them were found by hand before this
 * property was here, and a seventh - a comment cut in half by a chunk boundary
 * being read as code - was found by writing a test that happened to feed one
 * byte at a time. This asserts it on every input.
 *
 * The duplicate-name policy is part of the comparison. The default is ERROR,
 * and both parsers refuse a repeated name. LAST_WINS and COLLECT still deliver
 * every member on the stream - a value already handed to the callback cannot
 * be replaced - and both of those accept, as the DOM parser does.
 *
 * Build with: make fuzz-json     Run: make fuzz-run-json
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>

extern "C" {
#include <ghoti.io/text/json.h>
}

/** Walk a value so the accessors are exercised, not just the parser. */
static void walk(const GTEXT_JSON_Value * v, int depth) {
  if (!v || depth > 300) {
    return;
  }
  switch (gtext_json_typeof(v)) {
    case GTEXT_JSON_ARRAY: {
      size_t n = gtext_json_array_size(v);
      for (size_t i = 0; i < n; i++) {
        walk(gtext_json_array_get(v, i), depth + 1);
      }
      break;
    }
    case GTEXT_JSON_OBJECT: {
      size_t n = gtext_json_object_size(v);
      for (size_t i = 0; i < n; i++) {
        size_t key_len = 0;
        (void)gtext_json_object_key(v, i, &key_len);
        walk(gtext_json_object_value(v, i), depth + 1);
      }
      break;
    }
    case GTEXT_JSON_STRING: {
      const char * s = nullptr;
      size_t len = 0;
      (void)gtext_json_get_string(v, &s, &len);
      break;
    }
    case GTEXT_JSON_NUMBER: {
      double d = 0.0;
      int64_t i = 0;
      (void)gtext_json_get_double(v, &d);
      (void)gtext_json_get_i64(v, &i);
      break;
    }
    default:
      break;
  }
}

/** Whether the streaming parser accepts the whole document, fed in chunks. */
static bool stream_accepts(const char * text, size_t len,
    const GTEXT_JSON_Parse_Options * opts, size_t chunk) {
  GTEXT_JSON_Event_cb cb = [](void *, const GTEXT_JSON_Event *,
                               GTEXT_JSON_Error *) { return GTEXT_JSON_OK; };
  GTEXT_JSON_Stream * st = gtext_json_stream_new(opts, cb, nullptr);
  if (!st) {
    return false;
  }
  GTEXT_JSON_Error err{};
  GTEXT_JSON_Status status = GTEXT_JSON_OK;
  for (size_t i = 0; i < len; i += chunk) {
    const size_t n = (chunk < len - i) ? chunk : len - i;
    status = gtext_json_stream_feed(st, text + i, n, &err);
    if (status != GTEXT_JSON_OK) {
      break;
    }
  }
  if (status == GTEXT_JSON_OK) {
    status = gtext_json_stream_finish(st, &err);
  }
  gtext_json_stream_free(st);
  gtext_json_error_free(&err);
  return status == GTEXT_JSON_OK;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }

  // Two bytes select the dialect; the rest is the document.
  const uint8_t flags = data[0];
  const uint8_t flags5 = data[1];
  const char * text = reinterpret_cast<const char *>(data + 2);
  const size_t len = size - 2;

  GTEXT_JSON_Parse_Options opts = (flags5 & 0x80) != 0
      ? gtext_json_parse_options_json5()
      : gtext_json_parse_options_default();
  opts.allow_comments = (flags & 0x01) != 0;
  opts.allow_trailing_commas = (flags & 0x02) != 0;
  opts.allow_nonfinite_numbers = (flags & 0x04) != 0;
  opts.allow_single_quotes = (flags & 0x08) != 0;
  opts.allow_unescaped_controls = (flags & 0x10) != 0;
  opts.parse_int64 = (flags & 0x20) != 0;
  opts.parse_double = (flags & 0x40) != 0;
  opts.preserve_number_lexeme = (flags & 0x80) != 0;
  if ((flags5 & 0x80) == 0) {
    opts.allow_hex_numbers = (flags5 & 0x01) != 0;
    opts.allow_leading_plus = (flags5 & 0x02) != 0;
    opts.allow_bare_decimal_point = (flags5 & 0x04) != 0;
    opts.allow_ecma_escapes = (flags5 & 0x08) != 0;
    opts.allow_line_continuations = (flags5 & 0x10) != 0;
    opts.allow_ecma_whitespace = (flags5 & 0x20) != 0;
    opts.allow_unquoted_keys = (flags5 & 0x40) != 0;
  }

  // Value-initialized: the parser frees any snippet already in the struct
  // before writing a new one, so an indeterminate pointer here would be
  // passed to free(). And the snippet it allocates belongs to the caller.
  GTEXT_JSON_Error err{};
  GTEXT_JSON_Value * root = gtext_json_parse(text, len, &opts, &err);
  const bool dom_ok = root != nullptr;
  if (root) {
    walk(root, 0);
    gtext_json_free(root);
  }
  gtext_json_error_free(&err);

  /* The two parsers must agree about whether this is a document, and the
   * streaming one must agree with itself at every chunk size. */

  /* One byte at a time is the chunk size that finds boundary bugs, and it is
   * quadratic in the input length, so it is spent on the short inputs where
   * the corpus minimiser leaves the interesting ones anyway. */
  if (stream_accepts(text, len, &opts, len ? len : 1) != dom_ok) {
    __builtin_trap();
  }
  if (len <= 64 && stream_accepts(text, len, &opts, 1) != dom_ok) {
    __builtin_trap();
  }
  if (len > 1 && stream_accepts(text, len, &opts, 7) != dom_ok) {
    __builtin_trap();
  }
  return 0;
}
