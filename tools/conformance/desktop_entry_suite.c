/**
 * @file
 *
 * Score the INI reader and writer over a corpus of real `.desktop` files.
 *
 * Reads one path per line on stdin and prints one tab-separated record per path:
 *
 *     <path>\t<strict>\t<roundtrip>\t<generic>\t<parity>\t<detail>
 *
 * where `strict` is `ok` or `err:<code>`, `roundtrip` is `same` or `differs`,
 * `generic` is `ok` or `err:<code>`, and `parity` is `same`, `differs`,
 * `skip:cr` or `n/a`.
 *
 * The three questions are separate on purpose. "It parsed" is the weakest of
 * them; "it wrote back byte for byte" is what Desktop Entry §3 actually
 * requires of an implementation that rewrites a file; and "the generic dialect
 * agrees" is the generic dialect's whole correctness claim, which is inherited
 * from this corpus rather than asserted on its own.
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

/** Write a document to a fresh buffer sink. Caller frees the sink. */
static int render(const GTEXT_INI_Document * doc, char ** out, size_t * out_len) {
  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) != GTEXT_INI_OK) return 0;
  if (gtext_ini_write(doc, &sink, NULL) != GTEXT_INI_OK) {
    gtext_ini_sink_buffer_free(&sink);
    return 0;
  }
  size_t len = gtext_ini_sink_buffer_size(&sink);
  char * copy = malloc(len + 1);
  if (!copy) {
    gtext_ini_sink_buffer_free(&sink);
    return 0;
  }
  memcpy(copy, gtext_ini_sink_buffer_data(&sink), len);
  copy[len] = '\0';
  gtext_ini_sink_buffer_free(&sink);
  *out = copy;
  *out_len = len;
  return 1;
}

/** Whether two documents hold the same groups, keys and raw values. */
static int trees_equal(const GTEXT_INI_Document * a,
    const GTEXT_INI_Document * b) {
  if (gtext_ini_document_group_count(a) != gtext_ini_document_group_count(b)) {
    return 0;
  }
  for (size_t g = 0; g < gtext_ini_document_group_count(a); g++) {
    const GTEXT_INI_Group * ga = gtext_ini_document_group_at(a, g);
    const GTEXT_INI_Group * gb = gtext_ini_document_group_at(b, g);
    size_t alen = 0;
    size_t blen = 0;
    const char * an = gtext_ini_group_name(ga, &alen);
    const char * bn = gtext_ini_group_name(gb, &blen);
    if (alen != blen || memcmp(an, bn, alen) != 0) return 0;
    if (gtext_ini_group_entry_count(ga) != gtext_ini_group_entry_count(gb)) {
      return 0;
    }
    for (size_t e = 0; e < gtext_ini_group_entry_count(ga); e++) {
      const char * ak = gtext_ini_group_key_at(ga, e, &alen);
      const char * bk = gtext_ini_group_key_at(gb, e, &blen);
      if (alen != blen || memcmp(ak, bk, alen) != 0) return 0;
      const char * av = gtext_ini_group_value_at(ga, e, &alen);
      const char * bv = gtext_ini_group_value_at(gb, e, &blen);
      if (alen != blen || memcmp(av, bv, alen) != 0) return 0;
    }
  }
  return 1;
}

int main(void) {
  char line[4096];
  while (fgets(line, sizeof(line), stdin)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (!n) continue;

    size_t len = 0;
    char * data = slurp(line, &len);
    if (!data) {
      printf("%s\tunreadable\tn/a\tn/a\tn/a\t-\n", line);
      continue;
    }

    GTEXT_INI_Parse_Options strict = gtext_ini_parse_options_default();
    GTEXT_INI_Error err;
    memset(&err, 0, sizeof(err));
    GTEXT_INI_Document * a = gtext_ini_parse(data, len, &strict, &err);
    if (!a) {
      printf("%s\terr:%d\tn/a\tn/a\tn/a\tline %d: %s\n", line, (int) err.code,
          err.line, err.message ? err.message : "-");
      gtext_ini_error_free(&err);
      free(data);
      continue;
    }
    gtext_ini_error_free(&err);

    /* §3: a rewrite must preserve what it did not understand, and comments
     * "should be preserved". Byte equality is the only check that says so. */
    char * again = NULL;
    size_t again_len = 0;
    const char * roundtrip = "differs";
    if (render(a, &again, &again_len)) {
      if (again_len == len && memcmp(again, data, len) == 0) roundtrip = "same";
      free(again);
    }

    GTEXT_INI_Parse_Options loose = gtext_ini_parse_options_default();
    loose.dialect = gtext_ini_dialect_generic();
    memset(&err, 0, sizeof(err));
    GTEXT_INI_Document * b = gtext_ini_parse(data, len, &loose, &err);
    const char * generic = b ? "ok" : "err";
    const char * parity = "n/a";
    char detail[256];
    detail[0] = '-';
    detail[1] = '\0';
    if (!b) {
      snprintf(detail, sizeof(detail), "generic refused: line %d: %s", err.line,
          err.message ? err.message : "-");
    }
    else if (memchr(data, '\r', len)) {
      /*
       * The generic dialect's CRLF handling is the one change that is not a
       * relaxation: it removes a CR from the content of a line the strict
       * dialect already accepted, so the two are *meant* to disagree about such
       * a value. Scoring it as a failure would make the gate assert something
       * false; scoring it as a pass would hide a real difference. So it is
       * excluded and counted.
       */
      parity = "skip:cr";
    }
    else {
      parity = trees_equal(a, b) ? "same" : "differs";
    }
    gtext_ini_error_free(&err);

    printf("%s\tok\t%s\t%s\t%s\t%s\n", line, roundtrip, generic, parity,
        detail);
    if (b) gtext_ini_free(b);
    gtext_ini_free(a);
    free(data);
  }
  return 0;
}
