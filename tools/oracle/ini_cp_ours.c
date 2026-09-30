/*
 * Our side of the configparser differential.
 *
 * Per document: `<len>\n` then the document's bytes. `-1\n` ends the stream. Output:
 *
 *     BEGIN
 *     cp ok | cp err <status>
 *     G <hex name>
 *     E <hex canonical key> <hex joined value>
 *     W <hex rewrite>
 *     END
 *
 * **The joined value, not the raw span**, because those are two different things
 * under this dialect: the raw span holds the line terminators and the indentation a
 * continuation spans, and the reference's value is what remains after the join. Two
 * layers, and comparing the wrong one would fail on every multi-line value while
 * saying nothing about either.
 *
 * The canonical key, for the same reason: `configparser` lower-cases a key through
 * `optionxform` and the tree keeps both forms, so the folded one is what lines up.
 *
 * The `W` line needs no reference: a document this module parses must write back byte
 * for byte, which is a property of this module alone.
 *
 * Hex throughout, and shared with the reference driver's `hexed()` - including `.`
 * for the empty string. A value here can contain a newline, so a whitespace-delimited
 * format cannot carry one, and the two sides must encode by one rule: the first
 * version of this comparison had them disagree about the empty value and reported 368
 * differences that were entirely its own.
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

/** Read `<len>\n<len bytes>`. Returns NULL at end of stream. */
static char * read_block(size_t * out_len) {
  char header[64];
  if (!fgets(header, sizeof(header), stdin)) return NULL;
  long len = strtol(header, NULL, 10);
  if (len < 0) return NULL;
  char * data = malloc((size_t) len + 1);
  if (!data) exit(3);
  if (len && fread(data, 1, (size_t) len, stdin) != (size_t) len) exit(3);
  data[len] = '\0';
  *out_len = (size_t) len;
  return data;
}

int main(void) {
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_configparser();
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;

  for (;;) {
    size_t len = 0;
    char * data = read_block(&len);
    if (!data) break;

    printf("BEGIN\n");
    GTEXT_INI_Error err;
    GTEXT_INI_Document * doc = gtext_ini_parse(data, len, &opts, &err);
    if (!doc) {
      printf("cp err %d\n", (int) err.code);
      gtext_ini_error_free(&err);
      printf("END\n");
      free(data);
      continue;
    }
    printf("cp ok\n");
    size_t groups = gtext_ini_document_group_count(doc);
    for (size_t g = 0; g < groups; g++) {
      const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
      size_t name_len = 0;
      const char * name = gtext_ini_group_name(group, &name_len);
      fputs("G ", stdout);
      put_hex(name, name_len);
      fputs("\n", stdout);
      size_t entries = gtext_ini_group_entry_count(group);
      for (size_t e = 0; e < entries; e++) {
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
    GTEXT_INI_Sink sink;
    if (gtext_ini_sink_buffer(&sink) == GTEXT_INI_OK) {
      if (gtext_ini_write(doc, &sink, NULL) == GTEXT_INI_OK) {
        fputs("W ", stdout);
        put_hex(gtext_ini_sink_buffer_data(&sink),
            gtext_ini_sink_buffer_size(&sink));
        fputs("\n", stdout);
      }
      gtext_ini_sink_buffer_free(&sink);
    }
    gtext_ini_free(doc);
    printf("END\n");
    free(data);
  }
  fflush(stdout);
  return 0;
}
