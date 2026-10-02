/**
 * @file json_stream.c
 * @brief Streaming parser example
 *
 * This example demonstrates:
 * - Using the streaming parser for incremental parsing
 * - Handling events from the streaming parser
 * - Processing large JSON documents without building a full DOM
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/json.h>
#include <stdio.h>
#include <string.h>

// Event callback function
static GTEXT_JSON_Status event_callback(
    void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err) {
  (void)err; // Unused in this example

  int * depth = (int *)user;

  switch (evt->type) {
  case GTEXT_JSON_EVT_OBJECT_BEGIN:
    printf("%*s{\n", *depth * 2, "");
    (*depth)++;
    break;

  case GTEXT_JSON_EVT_OBJECT_END:
    (*depth)--;
    printf("%*s}\n", *depth * 2, "");
    break;

  case GTEXT_JSON_EVT_ARRAY_BEGIN:
    printf("%*s[\n", *depth * 2, "");
    (*depth)++;
    break;

  case GTEXT_JSON_EVT_ARRAY_END:
    (*depth)--;
    printf("%*s]\n", *depth * 2, "");
    break;

  case GTEXT_JSON_EVT_KEY:
    printf(
        "%*sKey: %.*s\n", *depth * 2, "", (int)evt->as.str.len, evt->as.str.s);
    break;

  case GTEXT_JSON_EVT_STRING:
    printf("%*sString: %.*s\n", *depth * 2, "", (int)evt->as.str.len,
        evt->as.str.s);
    break;

  case GTEXT_JSON_EVT_NUMBER:
    printf("%*sNumber: %.*s\n", *depth * 2, "", (int)evt->as.number.len,
        evt->as.number.s);
    break;

  case GTEXT_JSON_EVT_BOOL:
    printf("%*sBool: %s\n", *depth * 2, "", evt->as.boolean ? "true" : "false");
    break;

  case GTEXT_JSON_EVT_NULL:
    printf("%*sNull\n", *depth * 2, "");
    break;

  case GTEXT_JSON_EVT_RECORD_END:
    // Only reached when GTEXT_JSON_Parse_Options::records asks for a sequence
    // of top-level values; see json_ndjson.c. Listed rather than defaulted so
    // that -Wswitch keeps saying something if the enumeration grows again.
    printf("%*s--- end of record ---\n", *depth * 2, "");
    break;
  }

  return GTEXT_JSON_OK;
}

int main(void) {
  // JSON input (can be processed in chunks)
  const char * json_input = "{\"name\":\"Charlie\",\"scores\":[95,87,92]}";
  size_t json_len = strlen(json_input);

  // Create streaming parser
  GTEXT_JSON_Parse_Options opt = gtext_json_parse_options_default();
  int depth = 0;
  GTEXT_JSON_Stream * stream =
      gtext_json_stream_new(&opt, event_callback, &depth);
  if (!stream) {
    fprintf(stderr, "Failed to create stream\n");
    return 1;
  }

  printf("Streaming parser events:\n");

  // Feed input (can be done in chunks)
  // Note: If feeding multiple chunks, call feed() for each chunk
  GTEXT_JSON_Error err = {0};
  if (gtext_json_stream_feed(stream, json_input, json_len, &err) !=
      GTEXT_JSON_OK) {
    fprintf(stderr, "Feed error: %s\n", err.message);
    gtext_json_error_free(&err);
    gtext_json_stream_free(stream);
    return 1;
  }

  // Finish parsing - IMPORTANT: Always call finish() after all input is fed.
  // The last value may not be emitted until finish() is called, especially
  // if it was incomplete at the end of the final chunk.
  if (gtext_json_stream_finish(stream, &err) != GTEXT_JSON_OK) {
    fprintf(stderr, "Finish error: %s\n", err.message);
    gtext_json_error_free(&err);
    gtext_json_stream_free(stream);
    return 1;
  }

  // Cleanup
  gtext_json_stream_free(stream);

  return 0;
}
