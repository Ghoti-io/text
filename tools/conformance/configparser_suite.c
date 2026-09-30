/**
 * @file
 *
 * Read one file under the configparser dialect and say what happened.
 *
 * Usage: `configparser_suite <file>`. Prints one line per record, hex-encoded:
 *
 *     cp ok | cp err <status>
 *     G <hex name>
 *     E <hex canonical key> <hex joined value>
 *     W same | W differs | W refused
 *
 * **Hex and not text**, unlike the Desktop Entry and systemd runners beside it. The
 * values here can contain a newline - that is what this dialect's continuation
 * produces - and a whitespace-delimited format cannot carry one. A comparison that
 * collapsed whitespace to make its own output readable was the first version of
 * this and it reported 82 disagreements that did not exist, and hid the one that
 * did.
 *
 * The **joined** value, not the raw span: the raw span holds the line terminators
 * and the indentation the join removes, so comparing it against the reference's
 * value would be comparing two different things. gtext_ini_unescape() performs the
 * join, which is where the dialect puts it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/ini.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Write bytes as lowercase hex; `.` for empty. */
static void put_hex(const char * bytes, size_t len) {
  if (!len) {
    fputs(".", stdout);
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char) bytes[i]);
  }
}

/** Read a whole file. Returns NULL on failure. */
static char * slurp(const char * path, size_t * out_len) {
  FILE * f = fopen(path, "rb");
  if (!f) return NULL;
  size_t capacity = 8192;
  size_t len = 0;
  char * data = malloc(capacity);
  if (!data) {
    fclose(f);
    return NULL;
  }
  for (;;) {
    if (len == capacity) {
      capacity *= 2;
      char * grown = realloc(data, capacity);
      if (!grown) {
        free(data);
        fclose(f);
        return NULL;
      }
      data = grown;
    }
    size_t got = fread(data + len, 1, capacity - len, f);
    len += got;
    if (got == 0) break;
  }
  fclose(f);
  *out_len = len;
  return data;
}

int main(int argc, char ** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: configparser_suite <file>\n");
    return 2;
  }
  size_t len = 0;
  char * data = slurp(argv[1], &len);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }

  GTEXT_INI_Dialect dialect = gtext_ini_dialect_configparser();
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  GTEXT_INI_Error err;
  GTEXT_INI_Document * doc = gtext_ini_parse(data, len, &opts, &err);
  if (!doc) {
    printf("cp err %d\n", (int) err.code);
    gtext_ini_error_free(&err);
    free(data);
    return 0;
  }
  printf("cp ok\n");
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    size_t name_len = 0;
    const char * name = gtext_ini_group_name(group, &name_len);
    fputs("G ", stdout);
    put_hex(name, name_len);
    fputs("\n", stdout);
    for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
      size_t key_len = 0;
      const char * key = gtext_ini_group_canonical_key_at(group, e, &key_len);
      size_t value_len = 0;
      const char * value = gtext_ini_group_value_at(group, e, &value_len);
      char * joined = NULL;
      size_t joined_len = 0;
      GTEXT_INI_Status status = gtext_ini_unescape(&dialect, value, value_len,
          NULL, &joined, &joined_len);
      fputs("E ", stdout);
      put_hex(key, key_len);
      fputs(" ", stdout);
      if (status == GTEXT_INI_OK) {
        put_hex(joined, joined_len);
      }
      else {
        printf("<join-failed-%d>", (int) status);
      }
      fputs("\n", stdout);
      gtext_ini_string_free(NULL, joined);
    }
  }

  const char * verdict = "refused";
  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) == GTEXT_INI_OK) {
    if (gtext_ini_write(doc, &sink, NULL) == GTEXT_INI_OK) {
      size_t written = gtext_ini_sink_buffer_size(&sink);
      const char * bytes = gtext_ini_sink_buffer_data(&sink);
      verdict = (written == len && memcmp(bytes, data, len) == 0) ? "same"
                                                                 : "differs";
    }
    gtext_ini_sink_buffer_free(&sink);
  }
  printf("W %s\n", verdict);
  gtext_ini_free(doc);
  free(data);
  return 0;
}
