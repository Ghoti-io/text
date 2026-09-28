/*
 * The Desktop Entry reference driver: GLib's GKeyFile and freedesktop's
 * desktop-file-validate, asked about the same document in one pass.
 *
 * **Two references, not one, because they answer different questions.**
 * GKeyFile says what a value *is* - it is the permissive reader, and it accepts
 * documents the specification forbids. desktop-file-validate says whether a
 * document is *legal* - it enforces rules GKeyFile ignores, and the
 * specification sits with the validator. A differential built on either alone
 * would agree with that one's blind spot and score clean.
 *
 * Protocol: a batch, because a container invocation per document would be tens
 * of thousands of them. Read `<len>\n` then that many bytes, repeated; `-1\n`
 * ends the stream. **Every byte string in the output is hex**, so a document
 * containing a newline, a NUL or invalid UTF-8 cannot corrupt the framing - the
 * failure this repository met once already, where a value containing U+2028 was
 * split into a short record by Python's splitlines().
 *
 * Copyright 2026 by Corey Pennycuff
 */

/*
 * popen() and pclose() are POSIX rather than C17, and this is compiled as C17.
 * Asking for the feature set by name is better than relaxing the standard to
 * gnu17: it says which functions are wanted and why they are not in C.
 */
#define _POSIX_C_SOURCE 200809L

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GTEXT_DRIVER_SHA
#define GTEXT_DRIVER_SHA "unknown"
#endif
#ifndef GTEXT_DFV_VERSION
#define GTEXT_DFV_VERSION "unknown"
#endif

/** Write bytes as lowercase hex, or `-` for an absent string. */
static void put_hex(const char * bytes, size_t len) {
  if (!bytes) {
    fputs("-", stdout);
    return;
  }
  if (!len) {
    fputs(".", stdout); /* Empty is not absent, and the two must not collide. */
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char) bytes[i]);
  }
}

/** Ask GKeyFile, printing one `G`/`K` line per group and entry. */
static void ask_gkeyfile(const char * data, size_t len) {
  GKeyFile * kf = g_key_file_new();
  GError * err = NULL;
  /*
   * KEEP_COMMENTS|KEEP_TRANSLATIONS so that nothing is dropped before it can be
   * compared: without KEEP_TRANSLATIONS a `Name[de]` key is filtered by the
   * current locale and the comparison would be against a subset of the document.
   */
  if (!g_key_file_load_from_data(kf, data, len,
          G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &err)) {
    printf("gk err %d ", err ? err->code : -1);
    put_hex(err && err->message ? err->message : NULL,
        err && err->message ? strlen(err->message) : 0);
    printf("\n");
    if (err) g_error_free(err);
    g_key_file_free(kf);
    return;
  }
  printf("gk ok\n");
  gsize ngroups = 0;
  gchar ** groups = g_key_file_get_groups(kf, &ngroups);
  for (gsize i = 0; i < ngroups; i++) {
    printf("G ");
    put_hex(groups[i], strlen(groups[i]));
    printf("\n");
    gsize nkeys = 0;
    gchar ** keys = g_key_file_get_keys(kf, groups[i], &nkeys, NULL);
    for (gsize j = 0; keys && j < nkeys; j++) {
      gchar * raw = g_key_file_get_value(kf, groups[i], keys[j], NULL);
      GError * serr = NULL;
      gchar * str = g_key_file_get_string(kf, groups[i], keys[j], &serr);
      printf("K ");
      put_hex(keys[j], strlen(keys[j]));
      printf(" ");
      put_hex(raw, raw ? strlen(raw) : 0);
      printf(" %s ", str ? "ok" : "err");
      put_hex(str, str ? strlen(str) : 0);
      printf("\n");
      g_free(raw);
      g_free(str);
      if (serr) g_error_free(serr);
    }
    g_strfreev(keys);
  }
  g_strfreev(groups);
  g_key_file_free(kf);
}

/**
 * Ask desktop-file-validate, printing its exit status and every diagnostic.
 *
 * The file has to be named `*.desktop` or the validator declines to look at it,
 * and it has to be a file at all - unlike GKeyFile there is no data entry point.
 */
static void ask_validator(const char * data, size_t len) {
  const char * path = "/tmp/gtext-ini-probe.desktop";
  FILE * f = fopen(path, "wb");
  if (!f) {
    printf("dfv -1 0\n");
    return;
  }
  if (len && fwrite(data, 1, len, f) != len) {
    fclose(f);
    printf("dfv -1 0\n");
    return;
  }
  fclose(f);

  char command[256];
  snprintf(command, sizeof(command),
      "desktop-file-validate %s 2>&1", path);
  FILE * pipe = popen(command, "r");
  if (!pipe) {
    printf("dfv -1 0\n");
    return;
  }
  char * lines[64];
  int count = 0;
  char buffer[4096];
  while (count < 64 && fgets(buffer, sizeof(buffer), pipe)) {
    size_t n = strlen(buffer);
    while (n && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r')) buffer[--n] = 0;
    lines[count] = g_strdup(buffer);
    count++;
  }
  int status = pclose(pipe);
  int code = status == -1 ? -1 : (status / 256);
  printf("dfv %d %d\n", code, count);
  for (int i = 0; i < count; i++) {
    printf("D ");
    put_hex(lines[i], strlen(lines[i]));
    printf("\n");
    g_free(lines[i]);
  }
}

int main(int argc, char ** argv) {
  if (argc > 1 && !strcmp(argv[1], "--version")) {
    /*
     * One line, no protocol prefix, because oracle_env.py reads the first line
     * of stdout as the version. glib's is read from the library actually linked;
     * the validator's is compiled in from the package at build time, since
     * `desktop-file-validate --version` prints nothing on this Debian.
     */
    printf("glib %d.%d.%d, desktop-file-utils %s, driver %s\n",
        glib_major_version, glib_minor_version, glib_micro_version,
        GTEXT_DFV_VERSION, GTEXT_DRIVER_SHA);
    return 0;
  }

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
    ask_gkeyfile(data, (size_t) len);
    ask_validator(data, (size_t) len);
    printf("END\n");
    free(data);
  }
  fflush(stdout);
  return 0;
}
