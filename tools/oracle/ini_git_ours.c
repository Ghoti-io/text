/**
 * @file
 *
 * This library's answers to the same questions `git config --list -z` answers,
 * in the same framed, hex-encoded protocol the Desktop Entry differential uses,
 * so that tools/oracle/ini_git_diff.py compares two structures rather than two
 * texts.
 *
 * Read `<len>\n` then that many bytes, repeated; `-1\n` ends the stream. Every
 * byte string out is hex, so a document containing a newline, a NUL or invalid
 * UTF-8 cannot corrupt the framing.
 *
 * **The unit of comparison is git's flat canonical name**, not the tree: git has
 * no concept of a group object, and `--list` prints `section[.subsection].key`
 * with the section and key folded and a quoted subsection left alone. Emitting
 * the tree instead would mean the differential had to reconstruct that mapping
 * on the Python side, where a mistake in it would look like a library defect.
 *
 * A `V` line carries the canonical name and the **decoded** value, because that
 * is what git prints - unlike the Desktop Entry driver, where the raw value is
 * the thing both references agree about. `-` for a value that is absent, which
 * is git's valueless key and is distinct from its empty one.
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

/** Emit `<group-canon>.<key-canon>`, or just the key for the preamble group. */
static void put_full_name(const GTEXT_INI_Group * group, const char * key,
    size_t key_len) {
  size_t glen = 0;
  const char * gname = gtext_ini_group_canonical_name(group, &glen);
  if (!gtext_ini_group_is_preamble(group) && glen) {
    for (size_t i = 0; i < glen; i++) printf("%02x", (unsigned char) gname[i]);
    printf("2e"); /* '.' */
  }
  for (size_t i = 0; i < key_len; i++) printf("%02x", (unsigned char) key[i]);
}

int main(void) {
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_git_config();
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
    opts.dialect = dialect;
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
        for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
          size_t klen = 0;
          size_t vlen = 0;
          const char * key =
              gtext_ini_group_canonical_key_at(group, e, &klen);
          const char * raw = gtext_ini_group_value_at(group, e, &vlen);
          printf("V ");
          /* The canonical key, not the document's spelling: git folds it, and
           * folding it here rather than asking the library would be a second
           * implementation of the rule under test. */
          put_full_name(group, key, klen);
          printf(" ");
          if (!gtext_ini_group_value_present_at(group, e)) {
            /* A valueless key. git prints the name with no value at all, which
             * its porcelain reads as boolean true. */
            printf("-\n");
            continue;
          }
          char * decoded = NULL;
          size_t dlen = 0;
          GTEXT_INI_Status status = gtext_ini_unescape(&dialect, raw, vlen,
              NULL, &decoded, &dlen);
          if (status != GTEXT_INI_OK) {
            /*
             * A value the parser accepted and the decoder refuses. For this
             * dialect that should be impossible - the parser scanned the same
             * bytes with the same function - so it is reported rather than
             * hidden, and the differential fails on it.
             */
            printf("!%d\n", (int) status);
          }
          else {
            put_hex(decoded, dlen);
            printf("\n");
          }
          gtext_ini_string_free(NULL, decoded);
        }
      }
      /*
       * The single-value lookup, once per distinct canonical name, in
       * first-appearance order.
       *
       * This exists because the `V` lines above are produced by walking the
       * tree, so they never call gtext_ini_document_get() - and a mutation making
       * GTEXT_INI_DUPKEY_COLLECT answer the *first* occurrence instead of the
       * last passed the whole differential untouched. `git config --get` answers
       * the last, and the expected answer is derivable from git's own `--list`
       * order, so the gate can check it without asking git again.
       */
      for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
        const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
        size_t glen = 0;
        const char * gname = gtext_ini_group_canonical_name(group, &glen);
        for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
          size_t klen = 0;
          const char * key = gtext_ini_group_canonical_key_at(group, e, &klen);
          /* Only on the name's first appearance, so the output has one line per
           * distinct name the way git's `--get` has one answer per name. */
          bool seen = false;
          for (size_t pg = 0; pg <= g && !seen; pg++) {
            const GTEXT_INI_Group * prev =
                gtext_ini_document_group_at(doc, pg);
            size_t plen = 0;
            const char * pname = gtext_ini_group_canonical_name(prev, &plen);
            if (plen != glen || memcmp(pname, gname, glen) != 0) continue;
            size_t limit = (pg == g) ? e : gtext_ini_group_entry_count(prev);
            for (size_t pe = 0; pe < limit; pe++) {
              size_t pklen = 0;
              const char * pk =
                  gtext_ini_group_canonical_key_at(prev, pe, &pklen);
              if (pklen == klen && memcmp(pk, key, klen) == 0) {
                seen = true;
                break;
              }
            }
          }
          if (seen) continue;
          char gbuf[512];
          char kbuf[512];
          if (glen >= sizeof(gbuf) || klen >= sizeof(kbuf)) continue;
          memcpy(gbuf, gname, glen);
          gbuf[glen] = '\0';
          memcpy(kbuf, key, klen);
          kbuf[klen] = '\0';
          size_t rlen = 0;
          const char * raw = gtext_ini_document_get(doc, gbuf, kbuf, &rlen);
          printf("L ");
          put_full_name(group, key, klen);
          printf(" ");
          if (!raw) {
            /*
             * NULL here means the last occurrence is a valueless key: the name is
             * present by construction, so the other reading of NULL - no such
             * key - cannot apply. gtext_ini_group_value_present_at() is the
             * unambiguous spelling, and this is the one place the ambiguity is
             * closed by context instead.
             */
            printf("-\n");
            continue;
          }
          char * decoded = NULL;
          size_t dlen = 0;
          if (gtext_ini_unescape(&dialect, raw, rlen, NULL, &decoded, &dlen)
              == GTEXT_INI_OK) {
            put_hex(decoded, dlen);
          }
          else {
            printf("!");
          }
          printf("\n");
          gtext_ini_string_free(NULL, decoded);
        }
      }
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
