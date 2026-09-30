/**
 * @file
 *
 * Read one systemd unit file under the systemd dialect and say what happened.
 *
 * Usage: `systemd_unit_suite <file>`. Prints one line:
 *
 *     ok same        parsed, and wrote back byte for byte
 *     ok differs     parsed, and the rewrite is not the input
 *     err <status>   refused
 *
 * Two questions and not three, unlike the Desktop Entry runner beside it, and the
 * missing one is the point: there is no third question to ask, because **this machine
 * has no systemd**. The Desktop Entry corpus comes with `desktop-file-validate`, so a
 * file can be asked whether it is *valid*; these files are shipped by packages on a
 * machine whose PID 1 is `init`, and nothing here can say whether accepting one is
 * right. Acceptance and preservation are all a local corpus can offer, and
 * tools/conformance/systemd_unit_suite.py prints which constructs the corpus does not
 * contain so that the gap is visible rather than assumed away.
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

int main(int argc, char ** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: systemd_unit_suite <file>\n");
    return 2;
  }
  size_t len = 0;
  char * data = slurp(argv[1], &len);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }

  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_systemd();
  GTEXT_INI_Error err;
  GTEXT_INI_Document * doc = gtext_ini_parse(data, len, &opts, &err);
  if (!doc) {
    printf("err %d %s\n", (int) err.code, err.message ? err.message : "");
    gtext_ini_error_free(&err);
    free(data);
    return 0;
  }

  const char * verdict = "differs";
  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) == GTEXT_INI_OK) {
    if (gtext_ini_write(doc, &sink, NULL) == GTEXT_INI_OK) {
      size_t written = gtext_ini_sink_buffer_size(&sink);
      const char * bytes = gtext_ini_sink_buffer_data(&sink);
      if (written == len && memcmp(bytes, data, len) == 0) verdict = "same";
    }
    gtext_ini_sink_buffer_free(&sink);
  }
  printf("ok %s\n", verdict);
  gtext_ini_free(doc);
  free(data);
  return 0;
}
