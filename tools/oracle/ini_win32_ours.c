/*
 * Our side of the Win32 profile-API differential.
 *
 * Per document: `<len>\n` then the document's bytes. `-1\n` ends the stream. Output:
 *
 *     BEGIN
 *     w32 ok | w32 err <status>
 *     G <hex canonical name> <hex document name> P|N
 *     E <hex document key> <hex raw value>|- <hex decoded value>|-
 *     L <hex query name> <hex query key> <hex looked-up value>|-
 *     S                                  (a lookup skipped: a NUL in a name)
 *     W <hex rewrite>
 *     END
 *
 * **Three forms of every name and two of every value**, because the reference has
 * two entry points that answer different layers and this comparison scores both:
 *
 *   - `GetPrivateProfileSectionNames` lists the *document's* section names, so the
 *     `G` line carries that spelling as well as the canonical one a lookup uses.
 *     `P` marks the preamble, which that API does not list at all.
 *   - `GetPrivateProfileSection` renders `key=value` from the **raw** value - it
 *     does not strip quotes, measured - so the second field of an `E` line is the
 *     stored span and `-` means a valueless entry, which that API renders as the
 *     bare key.
 *   - `GetPrivateProfileString` returns the **decoded** value, quotes stripped, so
 *     the third field is gtext_ini_unescape()'s output.
 *
 * That the two APIs line up with this module's two layers is a finding rather than
 * a convenience: it is why the dialect's quote rule lives in the value accessor and
 * not in the parser.
 *
 * The `W` line needs no reference: a document this module parses must write back
 * byte for byte.
 *
 * Hex throughout, `.` for empty, shared with the harness's own `hexed()`.
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

int main(int argc, char ** argv) {
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_win32();
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  /*
   * `--decode-utf16` for the encoding population, and nothing else changes.
   *
   * A flag rather than a second runner, because the two must read the same
   * documents through the same accessors: the whole claim the encoding scores make
   * is that a document's UTF-16 form gives the same tree as its UTF-8 form, and a
   * second program would be free to differ for reasons that had nothing to do with
   * the encoding.
   */
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--decode-utf16")) opts.decode_utf16 = true;
  }

  for (;;) {
    size_t len = 0;
    char * data = read_block(&len);
    if (!data) break;

    printf("BEGIN\n");
    GTEXT_INI_Error err;
    GTEXT_INI_Document * doc = gtext_ini_parse(data, len, &opts, &err);
    if (!doc) {
      printf("w32 err %d\n", (int) err.code);
      gtext_ini_error_free(&err);
      printf("END\n");
      free(data);
      continue;
    }
    printf("w32 ok\n");
    size_t groups = gtext_ini_document_group_count(doc);
    for (size_t g = 0; g < groups; g++) {
      const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
      size_t name_len = 0;
      const char * name = gtext_ini_group_name(group, &name_len);
      size_t canon_len = 0;
      const char * canon = gtext_ini_group_canonical_name(group, &canon_len);
      fputs("G ", stdout);
      put_hex(canon, canon_len);
      fputs(" ", stdout);
      put_hex(name, name_len);
      printf(" %c\n", gtext_ini_group_is_preamble(group) ? 'P' : 'N');
      size_t entries = gtext_ini_group_entry_count(group);
      for (size_t e = 0; e < entries; e++) {
        size_t key_len = 0;
        const char * key = gtext_ini_group_key_at(group, e, &key_len);
        fputs("E ", stdout);
        put_hex(key, key_len);
        fputs(" ", stdout);
        if (!gtext_ini_group_value_present_at(group, e)) {
          /* A valueless entry, which `GetPrivateProfileSection` renders as the
           * bare key and `GetPrivateProfileString` cannot retrieve at all. Both
           * halves are `-` so the harness never has to guess which. */
          fputs("- -\n", stdout);
          continue;
        }
        size_t value_len = 0;
        const char * value = gtext_ini_group_value_at(group, e, &value_len);
        put_hex(value, value_len);
        fputs(" ", stdout);
        char * decoded = NULL;
        size_t decoded_len = 0;
        GTEXT_INI_Status status = gtext_ini_unescape(&dialect, value, value_len,
            NULL, &decoded, &decoded_len);
        if (status == GTEXT_INI_OK) {
          put_hex(decoded, decoded_len);
        }
        else {
          printf("<decode-failed-%d>", (int) status);
        }
        fputs("\n", stdout);
        gtext_ini_string_free(NULL, decoded);
      }
    }
    /*
     * **The lookup, asked through the library's own accessor.** Everything above
     * walks the tree by index, so none of it reaches gtext_ini_document_get() -
     * and a differential that compares a tree cannot see a defect in a lookup.
     * One did: git's `section.subsection` case rule was applied to every folding
     * dialect, so a Win32 lookup for `[Foo.Bar]` spelled as the file spells it
     * found nothing, and both gates stayed green because both resolved
     * first-wins in the harness instead of asking the library.
     *
     * The query uses the **document's own spelling**, which is what a caller who
     * read the name out of the file would type, and is the only spelling that
     * exercises the fold. A name or key containing a NUL cannot be asked for
     * through a NUL-terminated API at all and is skipped with `S`, so the count
     * is in the output rather than silently absent.
     */
    for (size_t g = 0; g < groups; g++) {
      const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
      size_t name_len = 0;
      const char * name = gtext_ini_group_name(group, &name_len);
      bool preamble = gtext_ini_group_is_preamble(group);
      size_t entries = gtext_ini_group_entry_count(group);
      for (size_t e = 0; e < entries; e++) {
        size_t key_len = 0;
        const char * key = gtext_ini_group_key_at(group, e, &key_len);
        if (memchr(name, 0, name_len) || memchr(key, 0, key_len)) {
          fputs("S\n", stdout);
          continue;
        }
        char qname[4096];
        char qkey[4096];
        if (name_len >= sizeof(qname) || key_len >= sizeof(qkey)) {
          fputs("S\n", stdout);
          continue;
        }
        memcpy(qname, preamble ? "" : name, preamble ? 0 : name_len);
        qname[preamble ? 0 : name_len] = '\0';
        memcpy(qkey, key, key_len);
        qkey[key_len] = '\0';
        size_t got_len = 0;
        const char * got = gtext_ini_document_get(doc, qname, qkey, &got_len);
        fputs("L ", stdout);
        put_hex(qname, preamble ? 0 : name_len);
        fputs(" ", stdout);
        put_hex(qkey, key_len);
        fputs(" ", stdout);
        if (!got) {
          fputs("-", stdout);
        }
        else {
          char * decoded = NULL;
          size_t decoded_len = 0;
          if (gtext_ini_unescape(&dialect, got, got_len, NULL, &decoded,
                  &decoded_len) == GTEXT_INI_OK) {
            put_hex(decoded, decoded_len);
            gtext_ini_string_free(NULL, decoded);
          }
          else {
            fputs("-", stdout);
          }
        }
        fputs("\n", stdout);
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
