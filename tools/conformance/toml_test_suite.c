/*
 * A toml-test decoder for this library.
 *
 * Reads a TOML document on stdin. On success writes toml-test's tagged JSON
 * on stdout and exits 0; on refusal writes the status code and message on
 * stderr and exits 1. toml_test_suite.py compares the JSON against the case's
 * expectation, and for an invalid case wants only the non-zero exit.
 *
 * The tagged encoding is the suite's: every scalar becomes
 * {"type": <tag>, "value": "<text>"}, an array becomes a JSON array and a
 * table a JSON object. The tags are string, integer, float, bool, datetime,
 * datetime-local, date-local and time-local - which is where TOML's four
 * date-time types are told apart, so a decoder that collapsed them would fail
 * here even though its values were right.
 *
 * The JSON is written by hand rather than through this library's own JSON
 * writer. Not for independence - the values have already been read by then -
 * but so that a defect in one module cannot make the other module's score
 * move, which is a thing that costs an afternoon to work out when it happens.
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

int main(void) {
  size_t capacity = 65536;
  size_t len = 0;
  char * data = malloc(capacity);
  if (!data) return 2;
  for (;;) {
    if (len == capacity) {
      capacity *= 2;
      char * grown = realloc(data, capacity);
      if (!grown) {
        free(data);
        return 2;
      }
      data = grown;
    }
    size_t got = fread(data + len, 1, capacity - len, stdin);
    len += got;
    if (got == 0) break;
  }

  GTEXT_TOML_Error err;
  memset(&err, 0, sizeof(err));
  GTEXT_TOML_Value * root = gtext_toml_parse(data, len, NULL, &err);
  if (!root) {
    fprintf(stderr, "code %d at line %d column %d: %s\n", (int) err.code,
        err.line, err.col, err.message ? err.message : "(no message)");
    gtext_toml_error_free(&err);
    free(data);
    return 1;
  }
  emit_value(root);
  putchar('\n');
  gtext_toml_free(root);
  free(data);
  return 0;
}
