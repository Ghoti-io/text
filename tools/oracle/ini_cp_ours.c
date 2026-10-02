/*
 * Our side of the configparser differential.
 *
 *     ini_cp_ours [--interpolation=none|basic|extended] [--default-section=<name>]
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
 * `--interpolation` runs gtext_ini_value_interpolate() over each joined value, and
 * exists because the pinned configuration is `interpolation=None` on both sides: that
 * pin removes a behaviour from the comparison rather than excluding a document from
 * it, so without a run that turns it on, nothing here would fail if this module's
 * handling of `%` changed or if `configparser`'s did. A per-value refusal is reported
 * as `cp err <status>` for the whole document, because that is what the reference does
 * - its exception comes out of `parser.items()` and there is no partial result.
 *
 * **Joined first, interpolated second**, which is the reference's order and not a
 * choice: `read()` stores a value already joined and `get()` interpolates what it
 * stored, so a reference resolving to a multi-line value substitutes the joined text.
 * Interpolating the raw span instead differs on every value a continuation spans.
 *
 * `--default-section` points GTEXT_INI_Interpolate_Options::defaults at the group of
 * that name, which is the fifth and sixth configurations of this gate. It exists
 * because the pinned `default_section` is a name no document can spell, and that pin
 * excluded the defaults chain from the comparison **by construction** rather than by
 * naming a document - so a rule about it went wrong and nothing here could fail. See
 * ini_cp_gen.DEFAULTS_CHAIN.
 *
 * Two things about that mode, and both are needed for the two sides to line up:
 *
 *   - **The defaults group is not emitted.** With `default_section` live the reference
 *     does not list it in `sections()`, so a `G` line for it would be a section the
 *     reference never answers. This module keeps it as an ordinary group, which is
 *     right - inheritance is a lookup policy and not a rule of the grammar - so the
 *     difference is in what the comparison reads, not in what either side parsed.
 *   - **Its values are still interpolated.** The reference reaches them: with the
 *     inheritance live, `items(section)` carries the defaults' keys into every section
 *     and interpolates them there, so a bad `%` inside `[DEFAULT]` refuses the
 *     document. Skipping the group entirely would accept where the reference refuses,
 *     and the verdict comparison would blame the chain for a hole in this runner.
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

int main(int argc, char ** argv) {
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_configparser();
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  const char * default_section = NULL;
  GTEXT_INI_Interpolate_Options interp = gtext_ini_interpolate_options_default();
  /*
   * **The output bound is configured away here on purpose.** It is the one place
   * gtext_ini_value_interpolate() departs from the reference - `configparser` has no
   * bound and a C library copying that would ship an amplification - so leaving the
   * default on would make a deliberate policy read as a disagreement about
   * interpolation, which is the one thing this comparison is for. The departure is
   * asserted instead by IniInterpolation.TheOutputIsBounded, which exhibits the
   * amplification before bounding it.
   */
  interp.max_output = (size_t) -1;
  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--interpolation=none")) {
      interp.style = GTEXT_INI_INTERPOLATION_NONE;
    }
    else if (!strcmp(argv[a], "--interpolation=basic")) {
      interp.style = GTEXT_INI_INTERPOLATION_BASIC;
    }
    else if (!strcmp(argv[a], "--interpolation=extended")) {
      interp.style = GTEXT_INI_INTERPOLATION_EXTENDED;
    }
    else if (!strncmp(argv[a], "--default-section=", 18)) {
      default_section = argv[a] + 18;
      if (!*default_section) {
        fprintf(stderr, "--default-section must name a section\n");
        return 2;
      }
    }
    else {
      fprintf(stderr, "unknown argument %s\n", argv[a]);
      return 2;
    }
  }

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
    /*
     * **Two passes over the tree**, the first of which prints nothing. With
     * interpolation on, a single value can refuse the document, and the reference
     * refuses it whole - so the body cannot be emitted until every value in it has
     * been asked for. Under `--interpolation=none` nothing refuses and the two
     * passes produce what one did.
     */
    GTEXT_INI_Status refusal = GTEXT_INI_OK;
    size_t groups = gtext_ini_document_group_count(doc);
    /*
     * Per document, because the group is this document's. Looked up by the dialect's
     * own folding, which is what a caller reproducing Python's reading would use -
     * gtext_ini_document_group() is the call GTEXT_INI_Interpolate_Options::defaults
     * documents for exactly this purpose.
     */
    const GTEXT_INI_Group * defaults = default_section
        ? gtext_ini_document_group(doc, default_section) : NULL;
    interp.defaults = defaults;
    for (int pass = 0; pass < 2 && refusal == GTEXT_INI_OK; pass++) {
      if (pass == 1) printf("cp ok\n");
      for (size_t g = 0; g < groups && refusal == GTEXT_INI_OK; g++) {
        const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
        size_t name_len = 0;
        const char * name = gtext_ini_group_name(group, &name_len);
        /* Interpolated below either way; emitted only when it is not the defaults
         * group, which the reference does not list as a section. */
        int emit = pass == 1 && group != defaults;
        if (emit) {
          fputs("G ", stdout);
          put_hex(name, name_len);
          fputs("\n", stdout);
        }
        size_t entries = gtext_ini_group_entry_count(group);
        for (size_t e = 0; e < entries; e++) {
          size_t key_len = 0;
          const char * key =
              gtext_ini_group_canonical_key_at(group, e, &key_len);
          size_t value_len = 0;
          const char * value = gtext_ini_group_value_at(group, e, &value_len);
          char * joined = NULL;
          size_t joined_len = 0;
          GTEXT_INI_Status status = gtext_ini_unescape(&dialect, value,
              value_len, NULL, &joined, &joined_len);
          char * resolved = NULL;
          size_t resolved_len = 0;
          if (status == GTEXT_INI_OK) {
            status = gtext_ini_value_interpolate(group, &interp, joined,
                joined_len, &resolved, &resolved_len);
          }
          if (status != GTEXT_INI_OK) {
            refusal = status;
            gtext_ini_string_free(NULL, joined);
            gtext_ini_string_free(NULL, resolved);
            break;
          }
          if (emit) {
            fputs("E ", stdout);
            put_hex(key, key_len);
            fputs(" ", stdout);
            put_hex(resolved, resolved_len);
            fputs("\n", stdout);
          }
          gtext_ini_string_free(NULL, joined);
          gtext_ini_string_free(NULL, resolved);
        }
      }
    }
    if (refusal != GTEXT_INI_OK) {
      printf("cp err %d\n", (int) refusal);
      gtext_ini_free(doc);
      printf("END\n");
      free(data);
      continue;
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
