/*
 * A toml-test runner for this library, in three modes.
 *
 *   (default)     decoder: TOML on stdin, tagged JSON on stdout.
 *   --roundtrip   TOML on stdin, written back out, read again, tagged JSON of
 *                 the *second* read on stdout. Scores the writer against the
 *                 suite's own expectations.
 *   --encode      encoder: tagged JSON on stdin, TOML on stdout. This is the
 *                 direction toml-test's own runner calls encoder mode, and
 *                 what makes the write side measurable against something
 *                 other than this library: the TOML that comes out is handed
 *                 to tomllib by toml_test_suite.py.
 *
 * --style=as-read|headers|inline picks GTEXT_TOML_Table_Style, so the option
 * is measured rather than asserted: each setting is run over the whole corpus.
 *
 * --version=1.0.0|1.1.0 picks GTEXT_TOML_Parse_Options::version, and the two
 * manifests are the two sides of it: toml_test_suite.py scores the 1.0.0 list
 * with the 1.0.0 arm and the 1.1.0 list with the 1.1.0 arm, and the eleven
 * cases where the lists disagree are cases each arm must get wrong when run
 * with the other's setting. Note that this reaches BOTH parses in --roundtrip:
 * re-reading at the same version is the honest reading of read-write-read, and
 * the sharper claim - that a 1.1.0 document writes as TOML a 1.0.0 parser
 * accepts - is asserted in tests/test-toml.cpp where it can be stated exactly.
 *
 * Exit status: 0 with output, or 1 with a reason on stderr for a document this
 * library refuses - which is what an invalid case wants. **3 is different**:
 * it means this runner's own output could not be written or could not be read
 * back, and no case of any kind may produce it. Without that separation a
 * writer that emitted garbage would score as a correct refusal on all 501
 * invalid cases.
 *
 * The JSON on both sides is hand-rolled rather than run through this library's
 * JSON module. Not for independence of the values - but so that a defect in
 * one module cannot make the other module's score move, which is a thing that
 * costs an afternoon to work out when it happens.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/chron/chron.h>
#include <ghoti.io/text/toml.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*==========================================================================*
 * Writing tagged JSON
 *==========================================================================*/

static void emit_json_string(const char * bytes, size_t len) {
  putchar('"');
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char) bytes[i];
    switch (c) {
      case '"': fputs("\\\"", stdout); break;
      case '\\': fputs("\\\\", stdout); break;
      case '\b': fputs("\\b", stdout); break;
      case '\f': fputs("\\f", stdout); break;
      case '\n': fputs("\\n", stdout); break;
      case '\r': fputs("\\r", stdout); break;
      case '\t': fputs("\\t", stdout); break;
      default:
        if (c < 0x20 || c == 0x7F) {
          printf("\\u%04x", c);
        }
        else {
          putchar((char) c);
        }
    }
  }
  putchar('"');
}

static void emit_tagged(const char * tag, const char * text, size_t len) {
  fputs("{\"type\": \"", stdout);
  fputs(tag, stdout);
  fputs("\", \"value\": ", stdout);
  emit_json_string(text, len);
  putchar('}');
}

/** The suite's tag for a date-time, which is where the four kinds separate. */
static const char * datetime_tag(GCHRON_TomlKind kind) {
  switch (kind) {
    case GCHRON_TOML_OFFSET_DATE_TIME: return "datetime";
    case GCHRON_TOML_LOCAL_DATE_TIME: return "datetime-local";
    case GCHRON_TOML_LOCAL_DATE: return "date-local";
    default: return "time-local";
  }
}

static void emit_value(const GTEXT_TOML_Value * value);

static void emit_float(double d) {
  char text[64];
  if (isnan(d)) {
    /* The suite accepts nan and -nan; signbit rather than a comparison,
     * because every comparison with a NaN is false. */
    snprintf(text, sizeof(text), "%snan", signbit(d) ? "-" : "");
  }
  else if (isinf(d)) {
    snprintf(text, sizeof(text), "%sinf", d < 0 ? "-" : "");
  }
  else {
    /* 17 significant digits round-trips every double. The comparison in the
     * Python half is numeric, so the spelling only has to be lossless. */
    snprintf(text, sizeof(text), "%.17g", d);
  }
  emit_tagged("float", text, strlen(text));
}

static void emit_value(const GTEXT_TOML_Value * value) {
  size_t len = 0;
  switch (gtext_toml_value_type(value)) {
    case GTEXT_TOML_TABLE: {
      putchar('{');
      size_t n = gtext_toml_table_size(value);
      for (size_t i = 0; i < n; ++i) {
        if (i) fputs(", ", stdout);
        size_t key_len = 0;
        const char * key = gtext_toml_table_key_at(value, i, &key_len);
        emit_json_string(key, key_len);
        fputs(": ", stdout);
        emit_value(gtext_toml_table_value_at(value, i));
      }
      putchar('}');
      break;
    }
    case GTEXT_TOML_ARRAY: {
      putchar('[');
      size_t n = gtext_toml_array_size(value);
      for (size_t i = 0; i < n; ++i) {
        if (i) fputs(", ", stdout);
        emit_value(gtext_toml_array_get(value, i));
      }
      putchar(']');
      break;
    }
    case GTEXT_TOML_STRING: {
      const char * text = gtext_toml_value_string(value, &len);
      emit_tagged("string", text, len);
      break;
    }
    case GTEXT_TOML_INTEGER: {
      int64_t i = 0;
      gtext_toml_value_integer(value, &i);
      char text[32];
      snprintf(text, sizeof(text), "%" PRId64, i);
      emit_tagged("integer", text, strlen(text));
      break;
    }
    case GTEXT_TOML_FLOAT: {
      double d = 0;
      gtext_toml_value_float(value, &d);
      emit_float(d);
      break;
    }
    case GTEXT_TOML_BOOLEAN: {
      bool b = false;
      gtext_toml_value_boolean(value, &b);
      emit_tagged("bool", b ? "true" : "false", b ? 4 : 5);
      break;
    }
    case GTEXT_TOML_DATETIME: {
      GCHRON_TomlValue dt;
      memset(&dt, 0, sizeof(dt));
      gtext_toml_value_datetime(value, &dt);
      char text[64];
      size_t written = 0;
      if (gchron_write_toml(&dt, NULL, text, sizeof(text), &written)
          != GCHRON_OK) {
        /* A value chron read and cannot write back would be a defect in chron
         * or in what this stored; say so rather than printing something that
         * would be compared as a string. */
        fputs("\nchron refused to write back a date-time it parsed\n", stderr);
        exit(2);
      }
      emit_tagged(datetime_tag(dt.kind), text, written);
      break;
    }
  }
}

/*==========================================================================*
 * Reading tagged JSON, for the encoder direction
 *
 * A reader for the suite's encoding only, which is a small language: objects,
 * arrays and strings, where every scalar is {"type": t, "value": v} and every
 * v is a string. Numbers, true, false and null never appear, so they are
 * refused rather than guessed at - a runner that quietly accepted a shape the
 * suite does not use would be measuring its own tolerance.
 *==========================================================================*/

typedef struct {
  const char * s;
  size_t len;
  size_t pos;
  const char * error;
} jreader;

/** A growable byte buffer, for one decoded JSON string at a time. */
typedef struct {
  char * data;
  size_t len;
  size_t cap;
} jbuf;

static bool jbuf_put(jbuf * b, const char * bytes, size_t len) {
  if (b->len + len + 1 > b->cap) {
    size_t want = b->cap ? b->cap : 64;
    while (want < b->len + len + 1) want *= 2;
    char * grown = realloc(b->data, want);
    if (!grown) return false;
    b->data = grown;
    b->cap = want;
  }
  memcpy(b->data + b->len, bytes, len);
  b->len += len;
  b->data[b->len] = '\0';
  return true;
}

static void jskip(jreader * r) {
  while (r->pos < r->len) {
    char c = r->s[r->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++r->pos;
    }
    else {
      break;
    }
  }
}

static bool jtake(jreader * r, char c) {
  jskip(r);
  if (r->pos < r->len && r->s[r->pos] == c) {
    ++r->pos;
    return true;
  }
  r->error = "unexpected character";
  return false;
}

static bool jutf8(jbuf * out, unsigned long scalar) {
  char bytes[4];
  size_t n;
  if (scalar < 0x80) {
    bytes[0] = (char) scalar;
    n = 1;
  }
  else if (scalar < 0x800) {
    bytes[0] = (char) (0xC0 | (scalar >> 6));
    bytes[1] = (char) (0x80 | (scalar & 0x3F));
    n = 2;
  }
  else if (scalar < 0x10000) {
    bytes[0] = (char) (0xE0 | (scalar >> 12));
    bytes[1] = (char) (0x80 | ((scalar >> 6) & 0x3F));
    bytes[2] = (char) (0x80 | (scalar & 0x3F));
    n = 3;
  }
  else {
    bytes[0] = (char) (0xF0 | (scalar >> 18));
    bytes[1] = (char) (0x80 | ((scalar >> 12) & 0x3F));
    bytes[2] = (char) (0x80 | ((scalar >> 6) & 0x3F));
    bytes[3] = (char) (0x80 | (scalar & 0x3F));
    n = 4;
  }
  return jbuf_put(out, bytes, n);
}

static bool jhex4(jreader * r, unsigned long * out) {
  if (r->pos + 4 > r->len) return false;
  unsigned long v = 0;
  for (int i = 0; i < 4; ++i) {
    char c = r->s[r->pos + i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned long) (c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned long) (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned long) (c - 'A' + 10);
    else return false;
  }
  r->pos += 4;
  *out = v;
  return true;
}

/** Read one JSON string into @p out, which is reset first. */
static bool jstring(jreader * r, jbuf * out) {
  out->len = 0;
  if (!jtake(r, '"')) return false;
  while (r->pos < r->len) {
    char c = r->s[r->pos++];
    if (c == '"') {
      if (!out->data && !jbuf_put(out, "", 0)) {
        r->error = "out of memory";
        return false;
      }
      return true;
    }
    if (c != '\\') {
      if (!jbuf_put(out, &c, 1)) {
        r->error = "out of memory";
        return false;
      }
      continue;
    }
    if (r->pos >= r->len) break;
    char e = r->s[r->pos++];
    char literal = 0;
    switch (e) {
      case '"': literal = '"'; break;
      case '\\': literal = '\\'; break;
      case '/': literal = '/'; break;
      case 'b': literal = '\b'; break;
      case 'f': literal = '\f'; break;
      case 'n': literal = '\n'; break;
      case 'r': literal = '\r'; break;
      case 't': literal = '\t'; break;
      case 'u': {
        unsigned long hi = 0;
        if (!jhex4(r, &hi)) {
          r->error = "bad hex escape";
          return false;
        }
        if (hi >= 0xD800 && hi <= 0xDBFF && r->pos + 6 <= r->len
            && r->s[r->pos] == '\\' && r->s[r->pos + 1] == 'u') {
          size_t save = r->pos;
          r->pos += 2;
          unsigned long lo = 0;
          if (jhex4(r, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            hi = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
          }
          else {
            r->pos = save;
          }
        }
        if (!jutf8(out, hi)) {
          r->error = "out of memory";
          return false;
        }
        continue;
      }
      default:
        r->error = "unknown escape";
        return false;
    }
    if (!jbuf_put(out, &literal, 1)) {
      r->error = "out of memory";
      return false;
    }
  }
  r->error = "unterminated string";
  return false;
}

static GTEXT_TOML_Value * jvalue(jreader * r);

/**
 * Turn one tagged scalar into a value.
 *
 * `strtod` and `strtoll` here rather than this library's locale-independent
 * pair: those are internal, and this program never calls setlocale, so it is
 * in the "C" locale from start to finish and the decimal point is a dot. The
 * library's own conversions are what is being measured, not these.
 */
static GTEXT_TOML_Value * tagged_value(
    const char * type, const char * text, size_t len, const char ** error) {
  if (strcmp(type, "string") == 0) {
    return gtext_toml_new_string(NULL, text, len);
  }
  if (strcmp(type, "integer") == 0) {
    char * end = NULL;
    long long v = strtoll(text, &end, 10);
    if (!end || *end != '\0') {
      *error = "tagged integer is not an integer";
      return NULL;
    }
    return gtext_toml_new_integer(NULL, (int64_t) v);
  }
  if (strcmp(type, "float") == 0) {
    char * end = NULL;
    double v = strtod(text, &end);
    if (!end || *end != '\0') {
      *error = "tagged float is not a float";
      return NULL;
    }
    return gtext_toml_new_float(NULL, v);
  }
  if (strcmp(type, "bool") == 0) {
    if (strcmp(text, "true") == 0) return gtext_toml_new_boolean(NULL, true);
    if (strcmp(text, "false") == 0) return gtext_toml_new_boolean(NULL, false);
    *error = "tagged bool is neither true nor false";
    return NULL;
  }
  if (strcmp(type, "datetime") == 0 || strcmp(type, "datetime-local") == 0
      || strcmp(type, "date-local") == 0
      || strcmp(type, "time-local") == 0) {
    GCHRON_TomlValue dt;
    memset(&dt, 0, sizeof(dt));
    GCHRON_ParseOptions popts;
    gchron_parse_options_toml(&popts);
    if (gchron_parse_toml(text, len, &popts, &dt, NULL, NULL) != GCHRON_OK) {
      *error = "chron refused a tagged date-time";
      return NULL;
    }
    return gtext_toml_new_datetime(NULL, &dt);
  }
  *error = "unknown tag";
  return NULL;
}

/**
 * Read an object, which is either a tagged scalar or a table.
 *
 * The two are told apart by what follows the first key: in a tagged scalar the
 * value of `type` or `value` is a bare string, and in a table every value is
 * an object or an array. So a table with a key literally called "type" - which
 * the corpus has - is not mistaken for a scalar.
 */
static GTEXT_TOML_Value * jobject(jreader * r) {
  if (!jtake(r, '{')) return NULL;
  jskip(r);
  if (r->pos < r->len && r->s[r->pos] == '}') {
    ++r->pos;
    return gtext_toml_new_table(NULL);
  }

  jbuf key;
  memset(&key, 0, sizeof(key));
  if (!jstring(r, &key)) {
    free(key.data);
    return NULL;
  }
  if (!jtake(r, ':')) {
    free(key.data);
    return NULL;
  }
  jskip(r);
  bool scalar = r->pos < r->len && r->s[r->pos] == '"'
      && ((key.len == 4 && memcmp(key.data, "type", 4) == 0)
          || (key.len == 5 && memcmp(key.data, "value", 5) == 0));

  if (scalar) {
    jbuf first;
    jbuf second;
    jbuf second_key;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&second_key, 0, sizeof(second_key));
    GTEXT_TOML_Value * out = NULL;
    if (jstring(r, &first) && jtake(r, ',') && jstring(r, &second_key)
        && jtake(r, ':') && jstring(r, &second) && jtake(r, '}')) {
      const char * type;
      const char * text;
      size_t text_len;
      if (strcmp(key.data, "type") == 0) {
        type = first.data;
        text = second.data;
        text_len = second.len;
      }
      else {
        type = second.data;
        text = first.data;
        text_len = first.len;
      }
      out = tagged_value(type, text, text_len, &r->error);
      if (!out && !r->error) r->error = "out of memory";
    }
    free(key.data);
    free(first.data);
    free(second.data);
    free(second_key.data);
    return out;
  }

  GTEXT_TOML_Value * table = gtext_toml_new_table(NULL);
  if (!table) {
    free(key.data);
    r->error = "out of memory";
    return NULL;
  }
  for (;;) {
    GTEXT_TOML_Value * child = jvalue(r);
    if (!child) break;
    GTEXT_TOML_Status s =
        gtext_toml_table_set(table, key.data, key.len, child);
    if (s != GTEXT_TOML_OK) {
      gtext_toml_free(child);
      r->error = "the tagged JSON names one key twice";
      break;
    }
    jskip(r);
    if (r->pos < r->len && r->s[r->pos] == ',') {
      ++r->pos;
      if (!jstring(r, &key) || !jtake(r, ':')) break;
      continue;
    }
    if (jtake(r, '}')) {
      free(key.data);
      return table;
    }
    break;
  }
  free(key.data);
  gtext_toml_free(table);
  return NULL;
}

static GTEXT_TOML_Value * jarray(jreader * r) {
  if (!jtake(r, '[')) return NULL;
  GTEXT_TOML_Value * array = gtext_toml_new_array(NULL);
  if (!array) {
    r->error = "out of memory";
    return NULL;
  }
  jskip(r);
  if (r->pos < r->len && r->s[r->pos] == ']') {
    ++r->pos;
    return array;
  }
  for (;;) {
    GTEXT_TOML_Value * child = jvalue(r);
    if (!child) break;
    if (gtext_toml_array_append(array, child) != GTEXT_TOML_OK) {
      gtext_toml_free(child);
      r->error = "out of memory";
      break;
    }
    jskip(r);
    if (r->pos < r->len && r->s[r->pos] == ',') {
      ++r->pos;
      continue;
    }
    if (jtake(r, ']')) return array;
    break;
  }
  gtext_toml_free(array);
  return NULL;
}

static GTEXT_TOML_Value * jvalue(jreader * r) {
  jskip(r);
  if (r->pos >= r->len) {
    r->error = "the input ended early";
    return NULL;
  }
  char c = r->s[r->pos];
  if (c == '{') return jobject(r);
  if (c == '[') return jarray(r);
  r->error = "the suite's encoding has no bare scalars, and this is one";
  return NULL;
}

/*==========================================================================*
 * Driving it
 *==========================================================================*/

static char * read_stdin(size_t * out_len) {
  size_t capacity = 65536;
  size_t len = 0;
  char * data = malloc(capacity);
  if (!data) return NULL;
  for (;;) {
    if (len == capacity) {
      capacity *= 2;
      char * grown = realloc(data, capacity);
      if (!grown) {
        free(data);
        return NULL;
      }
      data = grown;
    }
    size_t got = fread(data + len, 1, capacity - len, stdin);
    len += got;
    if (got == 0) break;
  }
  *out_len = len;
  return data;
}

static void report(const GTEXT_TOML_Error * err) {
  fprintf(stderr, "code %d at line %d column %d: %s\n", (int) err->code,
      err->line, err->col, err->message ? err->message : "(no message)");
}

int main(int argc, char ** argv) {
  bool roundtrip = false;
  bool encode = false;
  GTEXT_TOML_Write_Options wopts = gtext_toml_write_options_default();
  GTEXT_TOML_Parse_Options popts = gtext_toml_parse_options_default();

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--roundtrip") == 0) roundtrip = true;
    else if (strcmp(argv[i], "--encode") == 0) encode = true;
    else if (strcmp(argv[i], "--version=1.0.0") == 0) {
      popts.version = GTEXT_TOML_VERSION_1_0_0;
    }
    else if (strcmp(argv[i], "--version=1.1.0") == 0) {
      popts.version = GTEXT_TOML_VERSION_1_1_0;
    }
    else if (strcmp(argv[i], "--style=as-read") == 0) {
      wopts.table_style = GTEXT_TOML_TABLE_STYLE_AS_READ;
    }
    else if (strcmp(argv[i], "--style=headers") == 0) {
      wopts.table_style = GTEXT_TOML_TABLE_STYLE_HEADERS;
    }
    else if (strcmp(argv[i], "--style=inline") == 0) {
      wopts.table_style = GTEXT_TOML_TABLE_STYLE_INLINE;
    }
    else {
      fprintf(stderr, "unknown option %s\n", argv[i]);
      return 3;
    }
  }

  size_t len = 0;
  char * data = read_stdin(&len);
  if (!data) return 3;

  if (encode) {
    jreader r;
    r.s = data;
    r.len = len;
    r.pos = 0;
    r.error = NULL;
    GTEXT_TOML_Value * root = jvalue(&r);
    if (root) {
      jskip(&r);
      if (r.pos != r.len) {
        r.error = "trailing bytes after the tagged JSON";
        gtext_toml_free(root);
        root = NULL;
      }
    }
    if (!root) {
      /* The suite's own expectation file did not read: this runner's problem,
       * never a verdict about a case. */
      fprintf(stderr, "tagged JSON at byte %zu: %s\n", r.pos,
          r.error ? r.error : "(no reason)");
      free(data);
      return 3;
    }
    if (gtext_toml_value_type(root) != GTEXT_TOML_TABLE) {
      fputs("the tagged JSON's root is not a table\n", stderr);
      gtext_toml_free(root);
      free(data);
      return 3;
    }
    GTEXT_TOML_Sink sink;
    if (gtext_toml_sink_buffer(&sink) != GTEXT_TOML_OK) {
      gtext_toml_free(root);
      free(data);
      return 3;
    }
    GTEXT_TOML_Status s = gtext_toml_write(root, &sink, &wopts);
    if (s != GTEXT_TOML_OK) {
      fprintf(stderr, "the writer refused, code %d\n", (int) s);
      gtext_toml_sink_buffer_free(&sink);
      gtext_toml_free(root);
      free(data);
      return 3;
    }
    fwrite(gtext_toml_sink_buffer_data(&sink), 1,
        gtext_toml_sink_buffer_size(&sink), stdout);
    gtext_toml_sink_buffer_free(&sink);
    gtext_toml_free(root);
    free(data);
    return 0;
  }

  GTEXT_TOML_Error err;
  memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root = gtext_toml_parse(data, len, &popts, &err);
  if (!root) {
    report(&err);
    gtext_toml_error_free(&err);
    free(data);
    return 1;
  }

  if (roundtrip) {
    GTEXT_TOML_Sink sink;
    if (gtext_toml_sink_buffer(&sink) != GTEXT_TOML_OK) {
      gtext_toml_free(root);
      free(data);
      return 3;
    }
    GTEXT_TOML_Status s = gtext_toml_write(root, &sink, &wopts);
    if (s != GTEXT_TOML_OK) {
      fprintf(stderr, "the writer refused a parsed document, code %d\n",
          (int) s);
      gtext_toml_sink_buffer_free(&sink);
      gtext_toml_free(root);
      free(data);
      return 3;
    }
    GTEXT_TOML_Error again;
    memset(&again, 0, sizeof(again));
    GTEXT_TOML_Value * reread =
        gtext_toml_parse(gtext_toml_sink_buffer_data(&sink),
            gtext_toml_sink_buffer_size(&sink), &popts, &again);
    if (!reread) {
      /* Exit 3, not 1: this is the writer producing a document this parser
       * refuses, and it must not be mistaken for a correct refusal of the
       * case. */
      fputs("the writer's own output would not parse: ", stderr);
      report(&again);
      fputs("--- what it wrote ---\n", stderr);
      fwrite(gtext_toml_sink_buffer_data(&sink), 1,
          gtext_toml_sink_buffer_size(&sink), stderr);
      gtext_toml_error_free(&again);
      gtext_toml_sink_buffer_free(&sink);
      gtext_toml_free(root);
      free(data);
      return 3;
    }
    gtext_toml_free(root);
    root = reread;
    gtext_toml_sink_buffer_free(&sink);
  }

  emit_value(root);
  putchar('\n');
  gtext_toml_free(root);
  free(data);
  return 0;
}
