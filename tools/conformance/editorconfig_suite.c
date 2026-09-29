/**
 * @file
 *
 * Print what the EditorConfig dialect made of one document, for
 * tools/conformance/editorconfig_suite.py to score against
 * `editorconfig-core-test`.
 *
 * Usage: `editorconfig_suite <file>`. Output, in document order:
 *
 *     E <status>                        the parse was refused; nothing follows
 *     S <hex section name>              a group header
 *     P <hex canonical key> <hex value> an entry; before any S it is a preamble key
 *
 * Every byte string is hex, because a section name may hold any byte and a value
 * may hold a `#`, a space or a NUL.
 *
 * **This prints the parse and resolves nothing**, and the split is the point.
 * Answering an assertion means matching a filepath against each section's glob
 * and then merging the matching sections' pairs, and a filepath glob matcher is
 * not a text library's job - it is the subject of 130 of the suite's 202
 * assertions, which are out of scope and said to be out of scope. So the glob and
 * the merge live in the Python harness, where they are visibly the harness's own,
 * and a **control run** of that harness against a throwaway parser written in
 * Python establishes that the glob layer scores 34 of 34 before the library is
 * asked anything. A failure under the library is then the library's.
 *
 * The key printed is the *canonical* one, because that is what the dialect says a
 * key is: "pair keys are case-insensitive; all keys are lowercased after
 * parsing". The value is the raw value, which for this dialect is also the
 * decoded one - it defines no escapes.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/ini.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/** Write bytes as lowercase hex; `.` for empty, which is not the same as absent. */
static void put_hex(const char * bytes, size_t len) {
  if (!bytes) {
    fputs("-", stdout);
    return;
  }
  if (!len) {
    fputs(".", stdout);
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char) bytes[i]);
  }
}

int main(int argc, char ** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: editorconfig_suite <file>\n");
    return 2;
  }
  size_t len = 0;
  char * data = slurp(argv[1], &len);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }

  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_editorconfig();
  GTEXT_INI_Error err;
  GTEXT_INI_Document * doc = gtext_ini_parse(data, len, &opts, &err);
  if (!doc) {
    printf("E %d\n", (int) err.code);
    gtext_ini_error_free(&err);
    free(data);
    return 0;
  }

  size_t groups = gtext_ini_document_group_count(doc);
  for (size_t g = 0; g < groups; g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    if (!gtext_ini_group_is_preamble(group)) {
      size_t name_len = 0;
      const char * name = gtext_ini_group_name(group, &name_len);
      fputs("S ", stdout);
      put_hex(name, name_len);
      fputs("\n", stdout);
    }
    size_t entries = gtext_ini_group_entry_count(group);
    for (size_t e = 0; e < entries; e++) {
      size_t key_len = 0;
      const char * key = gtext_ini_group_canonical_key_at(group, e, &key_len);
      size_t value_len = 0;
      const char * value = gtext_ini_group_value_at(group, e, &value_len);
      fputs("P ", stdout);
      put_hex(key, key_len);
      fputs(" ", stdout);
      put_hex(value, value_len);
      fputs("\n", stdout);
    }
  }
  gtext_ini_free(doc);
  free(data);
  return 0;
}
