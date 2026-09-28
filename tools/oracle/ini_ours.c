/**
 * @file
 *
 * This library's answers to the same questions the Desktop Entry reference
 * driver answers, in the same framed, hex-encoded protocol, so that
 * tools/oracle/ini_diff.py compares two structures rather than two texts.
 *
 * Read `<len>\n` then that many bytes, repeated; `-1\n` ends the stream. Every
 * byte string out is hex, so a document containing a newline, a NUL or invalid
 * UTF-8 cannot corrupt the framing.
 *
 * The shape deliberately mirrors containers/inidesktop/driver.c line for line:
 * `ours ok` or `ours err <code>`, then a `G` line per group and a `K` line per
 * entry carrying the key, the raw value, and whether the value decodes as a
 * string plus the decoded bytes. The last two are what make the comparison with
 * `g_key_file_get_string()` possible at all - and the escape set is the one axis
 * where neither reference can be consulted, so they are reported and excluded
 * rather than silently trusted.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/ini.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_hex(const char * bytes, size_t len) {
  if (!bytes) {
    fputs("-", stdout);
    return;
  }
  if (!len) {
    fputs(".", stdout);
    return;
  }
  for (size_t i = 0; i < len; i++) printf("%02x", (unsigned char) bytes[i]);
}

int main(void) {
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_desktop_entry();
  char header[64];
  while (fgets(header, sizeof(header), stdin)) {
    long len = strtol(header, NULL, 10);
    if (len < 0) break;
    char * data = malloc((size_t) len + 1);
    if (!data) return 3;
    if (len && fread(data, 1, (size_t) len, stdin) != (size_t) len) {
      free(data);
      return 3;
    }
    data[len] = '\0';

    printf("BEGIN\n");
    GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
    GTEXT_INI_Error err;
    memset(&err, 0, sizeof(err));
    GTEXT_INI_Document * doc =
        gtext_ini_parse(data, (size_t) len, &opts, &err);
    if (!doc) {
      printf("ours err %d ", (int) err.code);
      put_hex(err.message, err.message ? strlen(err.message) : 0);
      printf("\n");
      gtext_ini_error_free(&err);
    }
    else {
      gtext_ini_error_free(&err);
      printf("ours ok\n");
      for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
        const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
        size_t n = 0;
        const char * name = gtext_ini_group_name(group, &n);
        printf("G ");
        put_hex(name, n);
        printf("\n");
        for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
          size_t klen = 0;
          size_t vlen = 0;
          const char * key = gtext_ini_group_key_at(group, e, &klen);
          const char * raw = gtext_ini_group_value_at(group, e, &vlen);
          char * decoded = NULL;
          size_t dlen = 0;
          GTEXT_INI_Status status = gtext_ini_unescape(&dialect, raw, vlen,
              NULL, &decoded, &dlen);
          printf("K ");
          put_hex(key, klen);
          printf(" ");
          put_hex(raw, vlen);
          printf(" %s ", status == GTEXT_INI_OK ? "ok" : "err");
          put_hex(decoded, dlen);
          printf("\n");
          gtext_ini_string_free(NULL, decoded);
        }
      }
      /* The rewrite, so the differential can assert section 3's preservation
       * requirement over the generated population as well as over the corpus. */
      GTEXT_INI_Sink sink;
      if (gtext_ini_sink_buffer(&sink) == GTEXT_INI_OK) {
        if (gtext_ini_write(doc, &sink, NULL) == GTEXT_INI_OK) {
          printf("W ");
          put_hex(gtext_ini_sink_buffer_data(&sink),
              gtext_ini_sink_buffer_size(&sink));
          printf("\n");
        }
        else {
          printf("W -\n");
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
