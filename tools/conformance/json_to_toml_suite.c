/*
 * Put JSONTestSuite's documents through gtext_json_to_toml(), one per line of
 * output.
 *
 *   json-to-toml-runner [--wrap] [--null-skip] [--roundtrip] [--dupkeys=last]
 *       <file>...
 *
 * Prints one tab-separated record per file:
 *
 *   <path> <stage> <code> [<payload>]
 *
 * `stage` is where the case stopped - `json` for a document this library's own
 * JSON parser refuses, `to-toml`, `write`, `reparse`, `to-json`, or `ok` - and
 * `code` is the status name. With --roundtrip the payload of an `ok` record is
 * the JSON the document became after going out to TOML text and back, on one
 * line, which is what the scorer compares against the original by value.
 *
 * --wrap puts the document under a single table key, which is how the cases
 * whose root is not a table reach the rest of the conversion: v1.0.0 says a
 * TOML document is a table, so most of JSONTestSuite refuses at the first step
 * otherwise and the accepted population would be a handful of files.
 *
 * The wrapping is done to the *bytes* - `{"wrapped": ` in front and `}` behind -
 * rather than by building an object around the parsed value. A DOM node belongs
 * to the arena of the document it was parsed into, and putting one into an
 * object made by gtext_json_new_object() would be mixing two arenas to save a
 * reparse.
 *
 * --null-skip asks for GTEXT_TOML_JSON_NULL_SKIP instead of the default
 * refusal, which is the one option on this path whose effect a corpus can see.
 *
 * --dupkeys=last reads a duplicate name as its last occurrence rather than
 * refusing the document. That is not this conversion's option - it is the JSON
 * parser's - and it is here because two of JSONTestSuite's `y_` files carry a
 * duplicate name, so without it two documents the corpus says must be accepted
 * never reach the conversion at all. run-json.sh reports the same two under the
 * same policy.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/toml.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * toml_code(GTEXT_TOML_Status status) {
  switch (status) {
  case GTEXT_TOML_OK: return "OK";
  case GTEXT_TOML_E_INVALID: return "E_INVALID";
  case GTEXT_TOML_E_RANGE: return "E_RANGE";
  case GTEXT_TOML_E_DEPTH: return "E_DEPTH";
  case GTEXT_TOML_E_OOM: return "E_OOM";
  case GTEXT_TOML_E_UNREPRESENTABLE: return "E_UNREPRESENTABLE";
  case GTEXT_TOML_E_DATETIME: return "E_DATETIME";
  default: break;
  }
  return "E_OTHER";
}

/* The file's bytes, or NULL. */
static char * slurp(const char * path, size_t * out_len) {
  FILE * fh = fopen(path, "rb");
  if (!fh) {
    return NULL;
  }
  size_t cap = 4096, len = 0;
  char * buf = (char *) malloc(cap);
  if (!buf) {
    fclose(fh);
    return NULL;
  }
  for (;;) {
    if (len == cap) {
      char * grown = (char *) realloc(buf, cap * 2);
      if (!grown) {
        free(buf);
        fclose(fh);
        return NULL;
      }
      buf = grown;
      cap *= 2;
    }
    size_t got = fread(buf + len, 1, cap - len, fh);
    len += got;
    if (got == 0) {
      break;
    }
  }
  fclose(fh);
  *out_len = len;
  return buf;
}

/* One line of output, with the record's own tabs and newlines escaped out of
 * it: the scorer reads this as TSV and a payload holding either would move a
 * field boundary. */
static void put_escaped(const char * text, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char) text[i];
    if (c == '\t') {
      fputs("\\t", stdout);
    }
    else if (c == '\n') {
      fputs("\\n", stdout);
    }
    else if (c == '\r') {
      fputs("\\r", stdout);
    }
    else if (c == '\\') {
      fputs("\\\\", stdout);
    }
    else {
      putchar((int) c);
    }
  }
}

static void record(const char * path, const char * stage, const char * code,
    const char * payload, size_t payload_len) {
  printf("%s\t%s\t%s\t", path, stage, code);
  if (payload) {
    put_escaped(payload, payload_len);
  }
  putchar('\n');
}

int main(int argc, char ** argv) {
  bool wrap = false, null_skip = false, roundtrip = false, dup_last = false;
  int first = argc;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--wrap") == 0) {
      wrap = true;
    }
    else if (strcmp(argv[i], "--null-skip") == 0) {
      null_skip = true;
    }
    else if (strcmp(argv[i], "--roundtrip") == 0) {
      roundtrip = true;
    }
    else if (strcmp(argv[i], "--dupkeys=last") == 0) {
      dup_last = true;
    }
    else {
      first = i;
      break;
    }
  }

  for (int i = first; i < argc; i++) {
    const char * path = argv[i];
    size_t len = 0;
    char * bytes = slurp(path, &len);
    if (!bytes) {
      record(path, "read", "E_IO", NULL, 0);
      continue;
    }

    if (wrap) {
      static const char head[] = "{\"wrapped\":";
      size_t head_len = sizeof(head) - 1;
      char * wrapped = (char *) malloc(head_len + len + 2);
      if (!wrapped) {
        record(path, "wrap", "E_OOM", NULL, 0);
        free(bytes);
        continue;
      }
      memcpy(wrapped, head, head_len);
      memcpy(wrapped + head_len, bytes, len);
      wrapped[head_len + len] = '}';
      free(bytes);
      bytes = wrapped;
      len = head_len + len + 1;
    }

    /* The lexeme is what decides a JSON number's TOML type and value, so a
     * parse that threw it away would make every number E_INVALID - which is
     * documented, and is not what this corpus is measuring. */
    GTEXT_JSON_Parse_Options jopts = gtext_json_parse_options_default();
    jopts.preserve_number_lexeme = true;
    if (dup_last) {
      jopts.dupkeys = GTEXT_JSON_DUPKEY_LAST_WINS;
    }
    GTEXT_JSON_Value * json = gtext_json_parse(bytes, len, &jopts, NULL);
    free(bytes);
    if (!json) {
      record(path, "json", "REFUSED", NULL, 0);
      continue;
    }
    const GTEXT_JSON_Value * subject = json;

    GTEXT_TOML_From_JSON_Options fopts = gtext_toml_from_json_options_default();
    if (null_skip) {
      fopts.null_values = GTEXT_TOML_JSON_NULL_SKIP;
    }
    GTEXT_TOML_Value * toml = NULL;
    GTEXT_TOML_Error terr;
    memset(&terr, 0, sizeof(terr));
    GTEXT_TOML_Status status =
        gtext_json_to_toml(subject, &fopts, &toml, &terr);
    if (status != GTEXT_TOML_OK) {
      record(path, "to-toml", toml_code(status), NULL, 0);
      gtext_toml_error_free(&terr);
      gtext_json_free(json);
      continue;
    }

    if (!roundtrip) {
      record(path, "ok", "OK", NULL, 0);
      gtext_toml_free(toml);
      gtext_json_free(json);
      continue;
    }

    GTEXT_TOML_Sink sink;
    if (gtext_toml_sink_buffer(&sink) != GTEXT_TOML_OK) {
      record(path, "write", "E_OOM", NULL, 0);
      gtext_toml_free(toml);
      gtext_json_free(json);
      continue;
    }
    GTEXT_TOML_Write_Options wopts = gtext_toml_write_options_default();
    status = gtext_toml_write(toml, &sink, &wopts);
    gtext_toml_free(toml);
    if (status != GTEXT_TOML_OK) {
      record(path, "write", toml_code(status), NULL, 0);
      gtext_toml_sink_buffer_free(&sink);
      gtext_json_free(json);
      continue;
    }

    GTEXT_TOML_Parse_Options popts = gtext_toml_parse_options_default();
    GTEXT_TOML_Value * again = gtext_toml_parse(gtext_toml_sink_buffer_data(
                                                   &sink),
        gtext_toml_sink_buffer_size(&sink), &popts, &terr);
    gtext_toml_sink_buffer_free(&sink);
    if (!again) {
      record(path, "reparse", toml_code(terr.code), NULL, 0);
      gtext_toml_error_free(&terr);
      gtext_json_free(json);
      continue;
    }

    GTEXT_JSON_Value * back = NULL;
    GTEXT_TOML_To_JSON_Options jto = gtext_toml_to_json_options_default();
    status = gtext_toml_to_json(again, &jto, &back, &terr);
    gtext_toml_free(again);
    if (status != GTEXT_TOML_OK) {
      record(path, "to-json", toml_code(status), NULL, 0);
      gtext_toml_error_free(&terr);
      gtext_json_free(json);
      continue;
    }

    GTEXT_JSON_Sink jsink;
    if (gtext_json_sink_buffer(&jsink) != GTEXT_JSON_OK) {
      record(path, "write-json", "E_OOM", NULL, 0);
    }
    else {
      GTEXT_JSON_Write_Options jw = gtext_json_write_options_default();
      if (gtext_json_write_value(&jsink, &jw, back, NULL) != GTEXT_JSON_OK) {
        record(path, "write-json", "E_OTHER", NULL, 0);
      }
      else {
        record(path, "ok", "OK", gtext_json_sink_buffer_data(&jsink),
            gtext_json_sink_buffer_size(&jsink));
      }
      gtext_json_sink_buffer_free(&jsink);
    }
    gtext_json_free(back);
    gtext_json_free(json);
  }
  return 0;
}
