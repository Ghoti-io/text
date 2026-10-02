/**
 * @file json_ndjson.c
 * @brief NDJSON / JSON Lines: one input holding several JSON texts
 *
 * This example demonstrates:
 * - GTEXT_JSON_Parse_Options::records, and why it is an enumeration
 * - reading a sequence of records with the streaming parser
 * - reading the same sequence into one DOM value at a time
 * - writing records back with the matching framing
 *
 * A JSON text is one value, and by default a token after it is
 * GTEXT_JSON_E_TRAILING_GARBAGE. A log, an export or a network stream is
 * usually a *sequence* of values, and there is no single specification for
 * that - so `records` names which reading you mean, and the three disagree
 * about inputs that occur. The run below shows the same bytes read three ways.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/json.h>
#include <stdio.h>
#include <string.h>

/** Counts records, and reports the first key of each. */
typedef struct {
  size_t records; /**< Records seen so far, counted from EVT_RECORD_END. */
  int in_record;  /**< Whether this record's first key has been reported. */
} ndjson_state;

static GTEXT_JSON_Status on_event(
    void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err) {
  (void)err;
  ndjson_state * st = (ndjson_state *)user;

  switch (evt->type) {
  case GTEXT_JSON_EVT_KEY:
    if (!st->in_record) {
      printf("    record %zu begins with key \"%.*s\"\n", st->records + 1,
          (int)evt->as.str.len, evt->as.str.s);
      st->in_record = 1;
    }
    break;

  case GTEXT_JSON_EVT_RECORD_END:
    /* The event that makes a sequence legible. Without it the events alone
       cannot say where one record ended: `1 2` is two records and emits two
       NUMBER events, which is also what the single value `[1,2]` emits between
       its array markers. */
    st->records++;
    st->in_record = 0;
    break;

  default:
    break;
  }
  return GTEXT_JSON_OK;
}

/** Read @p text as a sequence, in @p chunk-byte pieces, and say what happened.
 */
static void read_stream(const char * label, GTEXT_JSON_Records mode,
    const char * text, size_t chunk) {
  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
  opts.records = mode;

  ndjson_state st = {0, 0};
  GTEXT_JSON_Stream * stream = gtext_json_stream_new(&opts, on_event, &st);
  if (!stream) {
    printf("  %s: could not create the parser\n", label);
    return;
  }

  const size_t len = strlen(text);
  size_t off = 0;
  GTEXT_JSON_Status status = GTEXT_JSON_OK;
  /* The error belongs to whichever call first saw it. Passing NULL to feed()
     and reading only finish()'s error reports "Stream is in error state",
     which is true and says nothing: the reason was in the feed. */
  GTEXT_JSON_Error err;
  memset(&err, 0, sizeof(err));
  while (off < len && status == GTEXT_JSON_OK) {
    const size_t n = (chunk && chunk < len - off) ? chunk : len - off;
    status = gtext_json_stream_feed(stream, text + off, n, &err);
    off += n;
  }
  GTEXT_JSON_Status finish = GTEXT_JSON_OK;
  if (status == GTEXT_JSON_OK) {
    finish = gtext_json_stream_finish(stream, &err);
  }

  if (status == GTEXT_JSON_OK && finish == GTEXT_JSON_OK) {
    printf("  %s: %zu records\n", label, st.records);
  }
  else {
    printf("  %s: refused (%s)\n", label,
        err.message ? err.message : "no message");
  }
  gtext_json_error_free(&err);
  gtext_json_stream_free(stream);
}

/** The same sequence, one DOM value at a time. */
static void read_dom(const char * text, GTEXT_JSON_Records mode) {
  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
  opts.records = mode;

  const size_t len = strlen(text);
  size_t off = 0;
  size_t n = 0;
  while (off < len) {
    size_t used = 0;
    GTEXT_JSON_Error err;
    memset(&err, 0, sizeof(err));
    /* gtext_json_parse_multiple() is the DOM half of this feature: it returns
       one value and says where the next one begins, so a loop over it reads a
       sequence without ever holding more than one record in memory. It honours
       `records` too, so the framing is checked rather than merely tolerated. */
    GTEXT_JSON_Value * v =
        gtext_json_parse_multiple(text + off, len - off, &opts, &err, &used);
    if (!v) {
      printf("    stopped at byte %zu: %s\n", off,
          err.message ? err.message : "no message");
      gtext_json_error_free(&err);
      return;
    }
    n++;
    const GTEXT_JSON_Value * name = gtext_json_object_get(v, "name", 4);
    const char * s = NULL;
    size_t s_len = 0;
    if (name && gtext_json_get_string(name, &s, &s_len) == GTEXT_JSON_OK) {
      printf("    record %zu: name = %.*s\n", n, (int)s_len, s);
    }
    gtext_json_free(v);
    off += used;
  }
  printf("    %zu records, whole input consumed\n", n);
}

int main(void) {
  const char * const ndjson = "{\"name\":\"Ada\",\"n\":1}\n"
                              "{\"name\":\"Grace\",\"n\":2}\n"
                              "{\"name\":\"Alan\",\"n\":3}\n";

  printf("=== One input, three readings of it ===\n\n");
  printf("The bytes:\n%s\n", ndjson);

  printf("As one JSON text (the default):\n");
  read_stream("RECORDS_OFF", GTEXT_JSON_RECORDS_OFF, ndjson, 0);

  printf("\nAs a sequence:\n");
  read_stream("RECORDS_WHITESPACE", GTEXT_JSON_RECORDS_WHITESPACE, ndjson, 0);
  read_stream("RECORDS_LINE      ", GTEXT_JSON_RECORDS_LINE, ndjson, 0);
  read_stream("RECORDS_SEQ       ", GTEXT_JSON_RECORDS_SEQ, ndjson, 0);

  printf("\nThe modes differ, and that is the point. Run together with no\n"
         "separator at all:\n");
  read_stream("RECORDS_WHITESPACE", GTEXT_JSON_RECORDS_WHITESPACE,
      "{\"a\":1}{\"b\":2}", 0);
  read_stream(
      "RECORDS_LINE      ", GTEXT_JSON_RECORDS_LINE, "{\"a\":1}{\"b\":2}", 0);

  printf("\nAnd a value printed across lines:\n");
  read_stream("RECORDS_WHITESPACE", GTEXT_JSON_RECORDS_WHITESPACE,
      "{\n  \"a\": 1\n}\n", 0);
  read_stream(
      "RECORDS_LINE      ", GTEXT_JSON_RECORDS_LINE, "{\n  \"a\": 1\n}\n", 0);

  printf("\nThe answer does not depend on how the input arrives - the same\n"
         "sequence, one byte at a time:\n");
  read_stream("RECORDS_LINE      ", GTEXT_JSON_RECORDS_LINE, ndjson, 1);

  printf("\n=== The same sequence through the DOM, one record at a time ===\n");
  read_dom(ndjson, GTEXT_JSON_RECORDS_LINE);

  printf("\n=== Writing records back ===\n");
  /* One value per call, with the framing in the write options. The incremental
     writer produces the same bytes for the same records; a records mode is
     also what makes a second top-level value legal at all, since without one
     the writer refuses it rather than emitting {"a":1}{"b":2}. */
  GTEXT_JSON_Write_Options wopts = gtext_json_write_options_default();
  wopts.records = GTEXT_JSON_RECORDS_LINE;

  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    fprintf(stderr, "could not create a sink\n");
    return 1;
  }

  for (int i = 1; i <= 3; i++) {
    GTEXT_JSON_Value * row = gtext_json_new_object();
    if (!row) {
      break;
    }
    gtext_json_object_put(row, "n", 1, gtext_json_new_number_i64(i));
    if (gtext_json_write_value(&sink, &wopts, row, NULL) != GTEXT_JSON_OK) {
      fprintf(stderr, "write failed\n");
      gtext_json_free(row);
      break;
    }
    gtext_json_free(row);
  }

  printf("%.*s", (int)gtext_json_sink_buffer_size(&sink),
      gtext_json_sink_buffer_data(&sink));
  printf("(read back: ");
  read_stream("RECORDS_LINE", GTEXT_JSON_RECORDS_LINE,
      gtext_json_sink_buffer_data(&sink), 0);
  printf(")\n");

  gtext_json_sink_buffer_free(&sink);
  return 0;
}
