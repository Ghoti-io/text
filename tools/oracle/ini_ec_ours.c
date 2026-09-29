/*
 * Our side of the EditorConfig differential, over the same batch protocol as
 * tools/oracle/containers/editorconfig/driver.c and py_driver.py, so that
 * tools/oracle/ini_ec_diff.py can read all three the same way.
 *
 * Per document: `<len>\n` then the document's bytes, then `<len>\n` then the query
 * filename. `-1\n` ends the stream. Output:
 *
 *     BEGIN
 *     ec ok | ec err <status>
 *     V <hex canonical key> <hex value>
 *     W <hex rewrite>
 *     END
 *
 * **Matching a section is string equality here, not globbing**, and the generator
 * is what makes that sound: every section name it emits is a literal filename with
 * no glob metacharacter in it, and the query is one of those names. A filepath
 * glob matcher is not this library's job - it is the subject of 130 of the
 * conformance suite's assertions, which are out of scope - so the differential is
 * built to need none, and the glob-bearing documents are the conformance gate's.
 *
 * The `W` line is the rewrite, which needs no reference at all: a document this
 * module parses must write back byte for byte. It is scored separately for that
 * reason - a property, not an agreement.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/ini.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Write bytes as lowercase hex; `.` for empty, `-` for absent. */
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

/** Read `<len>\n<len bytes>`. Returns NULL at end of stream. */
static char * read_block(size_t * out_len) {
  char header[64];
  if (!fgets(header, sizeof(header), stdin)) return NULL;
  long len = strtol(header, NULL, 10);
  if (len < 0) return NULL;
  char * data = malloc((size_t) len + 1);
  if (!data) exit(3);
  if (len && fread(data, 1, (size_t) len, stdin) != (size_t) len) {
    free(data);
    exit(3);
  }
  data[len] = '\0';
  *out_len = (size_t) len;
  return data;
}

/**
 * One resolved property. The list is in first-appearance order with the last
 * assigned value, which is what both cores print: each keeps a list and replaces
 * a repeated key's value in place rather than moving it to the end.
 */
typedef struct {
  const char * key;
  size_t key_len;
  const char * value;
  size_t value_len;
} resolved;

/** Resolve the query name's properties and print them. */
static void emit_resolved(const GTEXT_INI_Document * doc, const char * name,
    size_t name_len) {
  resolved * list = NULL;
  size_t count = 0;
  size_t capacity = 0;
  size_t groups = gtext_ini_document_group_count(doc);
  for (size_t g = 0; g < groups; g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    /*
     * A preamble key is never resolved. `root` steers the walk up the directory
     * tree, which a single document has none of, and no other preamble key is in
     * any section - both cores agree, and the conformance suite's expectations
     * never contain one.
     */
    if (gtext_ini_group_is_preamble(group)) continue;
    size_t gname_len = 0;
    const char * gname = gtext_ini_group_name(group, &gname_len);
    /*
     * String equality, not globbing - see the file comment. The generator emits
     * only literal section names, so this is the whole matching rule.
     */
    if (gname_len != name_len || memcmp(gname, name, name_len) != 0) continue;
    size_t entries = gtext_ini_group_entry_count(group);
    for (size_t e = 0; e < entries; e++) {
      size_t key_len = 0;
      const char * key = gtext_ini_group_canonical_key_at(group, e, &key_len);
      size_t value_len = 0;
      const char * value = gtext_ini_group_value_at(group, e, &value_len);
      size_t at = count;
      for (size_t i = 0; i < count; i++) {
        if (list[i].key_len == key_len &&
            memcmp(list[i].key, key, key_len) == 0) {
          at = i;
          break;
        }
      }
      if (at == count) {
        if (count == capacity) {
          size_t want = capacity ? capacity * 2 : 16;
          resolved * grown = realloc(list, want * sizeof(*grown));
          if (!grown) exit(3);
          list = grown;
          capacity = want;
        }
        count++;
      }
      list[at].key = key;
      list[at].key_len = key_len;
      list[at].value = value;
      list[at].value_len = value_len;
    }
  }
  for (size_t i = 0; i < count; i++) {
    fputs("V ", stdout);
    put_hex(list[i].key, list[i].key_len);
    fputs(" ", stdout);
    put_hex(list[i].value, list[i].value_len);
    fputs("\n", stdout);
  }
  free(list);
}

int main(void) {
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_editorconfig();

  for (;;) {
    size_t doc_len = 0;
    char * data = read_block(&doc_len);
    if (!data) break;
    size_t name_len = 0;
    char * name = read_block(&name_len);
    if (!name) {
      free(data);
      break;
    }

    printf("BEGIN\n");
    GTEXT_INI_Error err;
    GTEXT_INI_Document * doc = gtext_ini_parse(data, doc_len, &opts, &err);
    if (!doc) {
      printf("ec err %d\n", (int) err.code);
      gtext_ini_error_free(&err);
    }
    else {
      printf("ec ok\n");
      emit_resolved(doc, name, name_len);
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
    }
    printf("END\n");
    free(name);
    free(data);
  }
  fflush(stdout);
  return 0;
}
