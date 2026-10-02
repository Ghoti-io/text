/**
 * @file
 *
 * libFuzzer harness for the duplicate-name policy, across the two readers.
 *
 * GTEXT_JSON_Parse_Options::dupkeys has four values and the two readers are
 * not equally able to apply them, so the property is in two halves and only
 * the first is a plain agreement:
 *
 *     1. gtext_json_parse() and the streaming parser must agree about whether
 *        an input is a legal document, in every one of the four modes and at
 *        every chunk size;
 *
 *     2. and under LAST_WINS and COLLECT, the structure a consumer builds from
 *        the events - applying the policy itself, from
 *        GTEXT_JSON_Event::repeated_key - must be the structure
 *        gtext_json_parse() built from the same bytes under the same option.
 *
 * The second is the one that needed a harness. A stream cannot apply either of
 * those two modes for the caller: last-wins means replacing a value already
 * handed over, collect means wrapping one after the fact. So it delivers every
 * member and marks the repeats, and the claim that this is *enough* is exactly
 * a claim that the reconstruction above always matches. A flag set on every
 * key, or on none, satisfies half of it; only the comparison catches both.
 *
 * ## Why this is not in fuzz_json.cpp
 *
 * That harness's header has always said the duplicate-name policy was part of
 * its comparison. **It never set `dupkeys`**, so every one of its executions
 * ran in the default ERROR mode and the paragraph described a comparison the
 * code did not make. The honest repair is a harness of its own rather than a
 * fourth selector byte there: fuzz_json's corpus is 128,000 units deep and
 * every one of them is read as two selector bytes plus a document, so moving
 * the document by a byte reinterprets all of them and throws away the coverage
 * they encode.
 *
 * Reconstruction is limited to the members of the *top-level* object, because
 * a nested one would need a stack in the consumer and the question here is
 * about the flag rather than about how elaborate a consumer can be. Nesting is
 * covered by name, per object, in tests/test-json-stream-dupkeys.cpp.
 *
 * Build with: make fuzz-json-dupkeys   Run: make fuzz-run-json-dupkeys
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <ghoti.io/text/json.h>
}

namespace {

[[noreturn]] void fail(const char * what, const std::string & input) {
  std::fprintf(stderr, "%s\ninput was (%zu bytes): ", what, input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f) {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  __builtin_trap();
}

/* A member's value, spelled so that the two sides are comparable. Only the
   scalar types are spelled: a member whose value is an array or an object
   makes the input unusable for the reconstruction, which is reported as such
   rather than compared. */
struct Members {
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> values;
  bool usable = true;
};

std::string event_text(const GTEXT_JSON_Event * evt) {
  switch (evt->type) {
    case GTEXT_JSON_EVT_KEY:
    case GTEXT_JSON_EVT_STRING:
      return std::string(evt->as.str.s ? evt->as.str.s : "", evt->as.str.len);
    case GTEXT_JSON_EVT_NUMBER:
      return std::string(
          evt->as.number.s ? evt->as.number.s : "", evt->as.number.len);
    case GTEXT_JSON_EVT_BOOL:
      return evt->as.boolean ? "true" : "false";
    case GTEXT_JSON_EVT_NULL:
      return "null";
    default:
      return std::string();
  }
}

struct Consumer {
  Members out;
  std::string pending_key;
  bool pending_repeat = false;
  bool have_key = false;
  int depth = 0;
  GTEXT_JSON_Dupkey_Mode mode = GTEXT_JSON_DUPKEY_ERROR;
};

GTEXT_JSON_Status consume(
    void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err) {
  (void)err;
  Consumer * c = static_cast<Consumer *>(user);

  /* The flag may only ever be true on a key, and only in the two modes that
     deliver a repeated member. Checked here rather than in the comparison, so
     that an input which turns out to be unusable for the reconstruction still
     exercises it. */
  if (evt->repeated_key && evt->type != GTEXT_JSON_EVT_KEY) {
    __builtin_trap();
  }
  if (evt->repeated_key && c->mode != GTEXT_JSON_DUPKEY_LAST_WINS
      && c->mode != GTEXT_JSON_DUPKEY_COLLECT) {
    __builtin_trap();
  }

  switch (evt->type) {
    case GTEXT_JSON_EVT_OBJECT_BEGIN:
    case GTEXT_JSON_EVT_ARRAY_BEGIN:
      if (c->depth == 1 && c->have_key) {
        /* A non-scalar member of the top-level object. */
        c->out.usable = false;
        c->have_key = false;
      }
      if (c->depth == 0 && evt->type == GTEXT_JSON_EVT_ARRAY_BEGIN) {
        /* The document is not an object at all. */
        c->out.usable = false;
      }
      c->depth++;
      break;
    case GTEXT_JSON_EVT_OBJECT_END:
    case GTEXT_JSON_EVT_ARRAY_END:
      c->depth--;
      break;
    case GTEXT_JSON_EVT_KEY:
      if (c->depth == 1) {
        c->pending_key = event_text(evt);
        c->pending_repeat = evt->repeated_key;
        c->have_key = true;
      }
      break;
    default:
      if (c->depth == 0) {
        /* A bare scalar document. */
        c->out.usable = false;
      }
      if (c->depth == 1 && c->have_key) {
        const std::string value = event_text(evt);
        if (!c->pending_repeat) {
          c->out.order.push_back(c->pending_key);
          c->out.values[c->pending_key] = {value};
        }
        else if (c->mode == GTEXT_JSON_DUPKEY_LAST_WINS) {
          c->out.values[c->pending_key] = {value};
        }
        else {
          c->out.values[c->pending_key].push_back(value);
        }
        c->have_key = false;
      }
      break;
  }
  return GTEXT_JSON_OK;
}

/* Feed the stream in `chunk`-byte pieces. `out` is filled whether or not the
   parse succeeds; the caller looks at it only when it did. */
bool stream_accepts(const std::string & text,
    const GTEXT_JSON_Parse_Options * opts, size_t chunk, Consumer * out) {
  GTEXT_JSON_Stream * st = gtext_json_stream_new(opts, consume, out);
  if (!st) {
    return false;
  }
  GTEXT_JSON_Error err{};
  GTEXT_JSON_Status status = GTEXT_JSON_OK;
  for (size_t i = 0; i < text.size() && status == GTEXT_JSON_OK; i += chunk) {
    const size_t n = (chunk < text.size() - i) ? chunk : text.size() - i;
    status = gtext_json_stream_feed(st, text.data() + i, n, &err);
  }
  if (status == GTEXT_JSON_OK) {
    status = gtext_json_stream_finish(st, &err);
  }
  gtext_json_stream_free(st);
  gtext_json_error_free(&err);
  return status == GTEXT_JSON_OK;
}

std::string spell(const GTEXT_JSON_Value * v) {
  const char * s = nullptr;
  size_t len = 0;
  switch (gtext_json_typeof(v)) {
    case GTEXT_JSON_NUMBER:
      if (gtext_json_get_number_lexeme(v, &s, &len) == GTEXT_JSON_OK) {
        return std::string(s ? s : "", len);
      }
      return std::string();
    case GTEXT_JSON_STRING:
      if (gtext_json_get_string(v, &s, &len) == GTEXT_JSON_OK) {
        return std::string(s ? s : "", len);
      }
      return std::string();
    case GTEXT_JSON_BOOL: {
      bool b = false;
      gtext_json_get_bool(v, &b);
      return std::string(b ? "true" : "false");
    }
    case GTEXT_JSON_NULL:
      return "null";
    default:
      return std::string();
  }
}

/* The same shape, read out of the DOM. `usable` is false for exactly the
   documents the consumer above declines, so the two agree about what they are
   able to compare. */
Members dom_members(const GTEXT_JSON_Value * root, GTEXT_JSON_Dupkey_Mode mode) {
  Members m;
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT) {
    m.usable = false;
    return m;
  }
  for (size_t i = 0; i < gtext_json_object_size(root); i++) {
    size_t klen = 0;
    const char * k = gtext_json_object_key(root, i, &klen);
    const GTEXT_JSON_Value * v = gtext_json_object_value(root, i);
    const std::string key(k ? k : "", klen);
    m.order.push_back(key);

    const GTEXT_JSON_Type t = gtext_json_typeof(v);
    if (mode == GTEXT_JSON_DUPKEY_COLLECT && t == GTEXT_JSON_ARRAY) {
      /* Under COLLECT a repeated name holds an array of the values. It is not
         possible to tell that from an array the document itself wrote, so an
         array member makes the input unusable in this mode too - the same
         decision the consumer makes when it sees ARRAY_BEGIN under a key. */
      std::vector<std::string> flat;
      for (size_t j = 0; j < gtext_json_array_size(v); j++) {
        const GTEXT_JSON_Value * e = gtext_json_array_get(v, j);
        if (gtext_json_typeof(e) == GTEXT_JSON_ARRAY
            || gtext_json_typeof(e) == GTEXT_JSON_OBJECT) {
          m.usable = false;
          return m;
        }
        flat.push_back(spell(e));
      }
      m.values[key] = flat;
      continue;
    }
    if (t == GTEXT_JSON_ARRAY || t == GTEXT_JSON_OBJECT) {
      m.usable = false;
      return m;
    }
    m.values[key] = {spell(v)};
  }
  return m;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }

  /* One byte picks the mode, so one corpus unit explores one policy. All four
     are fuzzed, the two the parser applies itself included: those are where
     the acceptance half of the property lives. */
  static const GTEXT_JSON_Dupkey_Mode kModes[4] = {
      GTEXT_JSON_DUPKEY_ERROR,
      GTEXT_JSON_DUPKEY_FIRST_WINS,
      GTEXT_JSON_DUPKEY_LAST_WINS,
      GTEXT_JSON_DUPKEY_COLLECT,
  };
  const GTEXT_JSON_Dupkey_Mode mode = kModes[data[0] & 0x03];
  /* Normalisation changes which names are the same name, which is the one
     other option that bears on this property. */
  const bool normalize = (data[0] & 0x04) != 0;

  const std::string text(
      reinterpret_cast<const char *>(data + 1), size - 1);

  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
  opts.dupkeys = mode;
  opts.normalize_unicode = normalize;

  GTEXT_JSON_Error err{};
  GTEXT_JSON_Value * root =
      gtext_json_parse(text.data(), text.size(), &opts, &err);
  const bool dom_ok = root != nullptr;
  Members dom;
  if (root) {
    dom = dom_members(root, mode);
  }

  /* Property 1: the two readers agree about acceptance, at three chunk sizes.
     One byte at a time is quadratic in the length, so it is spent on the short
     inputs where a minimised corpus keeps the interesting ones. */
  const size_t chunks[3] = {text.size() ? text.size() : 1, 1, 7};
  for (int i = 0; i < 3; i++) {
    if (i == 1 && text.size() > 64) continue;
    if (i == 2 && text.size() <= 1) continue;
    Consumer c;
    c.mode = mode;
    const bool stream_ok = stream_accepts(text, &opts, chunks[i], &c);
    if (stream_ok != dom_ok) {
      fail("the stream and the DOM parser disagree about acceptance", text);
    }

    /* Property 2: and under the two modes the stream cannot apply, what a
       consumer builds from the events equals what the DOM parser built. */
    if (stream_ok && dom_ok && dom.usable && c.out.usable
        && (mode == GTEXT_JSON_DUPKEY_LAST_WINS
            || mode == GTEXT_JSON_DUPKEY_COLLECT)) {
      if (c.out.order != dom.order) {
        fail("reconstructed member order differs from the DOM's", text);
      }
      if (c.out.values != dom.values) {
        fail("reconstructed member values differ from the DOM's", text);
      }
    }
  }

  if (root) {
    gtext_json_free(root);
  }
  gtext_json_error_free(&err);
  return 0;
}
