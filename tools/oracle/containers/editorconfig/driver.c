/*
 * The EditorConfig reference driver: editorconfig-core-c, asked about one
 * document at a time through its library rather than its command line.
 *
 * **Two references here, and they disagree** - the other is
 * editorconfig-core-py, which tools/oracle/ini_ec_ask.py reaches through a
 * virtualenv. Neither is normative. The normative artefact is
 * editorconfig-core-test, whose 34 `parser` assertions
 * tools/conformance/run-ini-editorconfig.sh scores on their own; these two are
 * here to be differed against, and a disagreement between them is a finding
 * rather than a failure - notes/text/INI-DIALECTS.md records which of them is
 * wrong about what.
 *
 * Protocol: a batch, because a container invocation per document would be tens
 * of thousands of them. Per document, `<len>\n` then that many bytes of the
 * document, then `<len>\n` then that many bytes of the **query filename** - the
 * name to resolve properties for, which is what makes a section matter at all.
 * `-1\n` ends the stream. Every byte string in the output is hex, so a document
 * containing a newline, a NUL or invalid UTF-8 cannot corrupt the framing.
 *
 * Output per document, mirroring tools/oracle/ini_ec_ours.c:
 *
 *     BEGIN
 *     ec ok | ec err <line> | ec fail <code>
 *     V <hex name> <hex value>
 *     END
 *
 * `ec err <line>` is a parse error at that 1-based line; core-c returns the line
 * number and keeps whatever it read before it, which is why the V lines are
 * still emitted after one. `ec fail` is a negative return - a usage or memory
 * error in the driver's own framing, never a verdict about the document.
 *
 * **The document is written into a private directory and the conf file is not
 * called `.editorconfig`.** core-c walks upward from the queried path looking for
 * conf files and merges every one it finds, so a stray `.editorconfig` in /tmp
 * or / would silently join every document in the run and the comparison would be
 * against the document plus whatever the image ships.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <editorconfig/editorconfig.h>
#include <editorconfig/editorconfig_handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GTEXT_DRIVER_SHA
#define GTEXT_DRIVER_SHA "unknown"
#endif
#ifndef GTEXT_EC_VERSION
#define GTEXT_EC_VERSION "unknown"
#endif
/* core-py's version, compiled in from the image. **Both references are named on
 * one line** because the gate checks that line against a single IMAGES field, the
 * way containers/inidesktop names glib and desktop-file-utils together. A driver
 * that named only its own reference would let the other one move unnoticed. */
#ifndef GTEXT_EC_PY_VERSION
#define GTEXT_EC_PY_VERSION "unknown"
#endif

/** The conf file name, deliberately not `.editorconfig`. See the note above. */
#define CONF_NAME ".gtext-ec"

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

/** Read `<len>\n<len bytes>` from stdin. Returns NULL on end of stream. */
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

int main(int argc, char ** argv) {
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    /* The gate checks this line against containers/IMAGES, so a rebuild that
     * moved core-c fails the check rather than quietly changing what the
     * comparison means. The suffix is part of the version: a release candidate
     * and its release answer as the same major.minor.patch. */
    printf("editorconfig-core-c %s%s, editorconfig-core-py %s, driver %s\n",
        GTEXT_EC_VERSION, editorconfig_get_version_suffix(),
        GTEXT_EC_PY_VERSION, GTEXT_DRIVER_SHA);
    return 0;
  }

  char dir[] = "/tmp/gtext-ec-XXXXXX";
  if (!mkdtemp(dir)) return 3;
  char conf[sizeof(dir) + sizeof(CONF_NAME) + 1];
  snprintf(conf, sizeof(conf), "%s/%s", dir, CONF_NAME);

  for (;;) {
    size_t doc_len = 0;
    char * doc = read_block(&doc_len);
    if (!doc) break;
    size_t name_len = 0;
    char * name = read_block(&name_len);
    if (!name) {
      free(doc);
      break;
    }

    FILE * f = fopen(conf, "wb");
    if (!f) return 3;
    if (doc_len && fwrite(doc, 1, doc_len, f) != doc_len) return 3;
    fclose(f);

    /* The queried path need not exist - core-c matches the name, it does not
     * open the file - but it must be absolute, which is what
     * EDITORCONFIG_PARSE_NOT_FULL_PATH complains about. */
    char * query = malloc(strlen(dir) + name_len + 2);
    if (!query) return 3;
    sprintf(query, "%s/%s", dir, name);

    printf("BEGIN\n");
    editorconfig_handle h = editorconfig_handle_init();
    if (!h) return 3;
    editorconfig_handle_set_conf_file_name(h, CONF_NAME);
    int err = editorconfig_parse(query, h);
    if (err < 0) {
      printf("ec fail %d\n", err);
    }
    else {
      printf(err > 0 ? "ec err %d\n" : "ec ok\n", err);
      int count = editorconfig_handle_get_name_value_count(h);
      for (int i = 0; i < count; i++) {
        const char * n = NULL;
        const char * v = NULL;
        editorconfig_handle_get_name_value(h, i, &n, &v);
        printf("V ");
        put_hex(n, n ? strlen(n) : 0);
        printf(" ");
        put_hex(v, v ? strlen(v) : 0);
        printf("\n");
      }
    }
    printf("END\n");
    editorconfig_handle_destroy(h);
    free(query);
    free(name);
    free(doc);
  }
  unlink(conf);
  rmdir(dir);
  fflush(stdout);
  return 0;
}
