/*
 * Our side of the systemd differential.
 *
 * Per document: `<len>\n` then the document's bytes. `-1\n` ends the stream. Output:
 *
 *     BEGIN
 *     sd ok | sd err <status>
 *     Q <hex word> ...            the words of one `Environment=` value
 *     Qerr <status>               ...or why it has none
 *     W <hex rewrite>
 *     END
 *
 * A `Q` or `Qerr` line per `Environment` entry in section order, because the
 * reference echoes every one of them and a differential that looked at only the
 * first would stop seeing a repeated key.
 *
 * **`Environment=` is the channel and not a special case here.** systemd has no verb
 * that prints a parsed setting; what it does have is `Environment=`'s per-word
 * complaint, which reports each word after unquoting, unescaping and word splitting.
 * So this asks gtext_ini_value_words() the same question and the diff lines the two
 * lists up.
 *
 * The `W` line is the rewrite, which needs no reference: a document this module
 * parses must write back byte for byte.
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
  if (len && fread(data, 1, (size_t) len, stdin) != (size_t) len) exit(3);
  data[len] = '\0';
  *out_len = (size_t) len;
  return data;
}

int main(void) {
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_systemd();
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
      printf("sd err %d\n", (int) err.code);
      gtext_ini_error_free(&err);
    }
    else {
      printf("sd ok\n");
      size_t groups = gtext_ini_document_group_count(doc);
      for (size_t g = 0; g < groups; g++) {
        const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
        size_t entries = gtext_ini_group_entry_count(group);
        for (size_t e = 0; e < entries; e++) {
          size_t key_len = 0;
          const char * key = gtext_ini_group_canonical_key_at(group, e, &key_len);
          if (key_len != 11 || memcmp(key, "Environment", 11) != 0) continue;
          size_t value_len = 0;
          const char * value = gtext_ini_group_value_at(group, e, &value_len);
          GTEXT_INI_List * words = NULL;
          GTEXT_INI_Status status = gtext_ini_value_words(&dialect, value,
              value_len, NULL, &words);
          if (status != GTEXT_INI_OK) {
            printf("Qerr %d\n", (int) status);
            continue;
          }
          fputs("Q", stdout);
          for (size_t i = 0; i < gtext_ini_list_count(words); i++) {
            size_t item_len = 0;
            const char * item = gtext_ini_list_at(words, i, &item_len);
            fputs(" ", stdout);
            put_hex(item, item_len);
          }
          fputs("\n", stdout);
          gtext_ini_list_free(words);
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
    }
    printf("END\n");
    free(data);
  }
  fflush(stdout);
  return 0;
}
