/* Evaluate one JSONPath query for the compliance test suite.
 *
 *   jsonpath-cts < request
 *
 * The request is the selector's byte length on a line of its own, then that
 * many bytes of selector, then the document. Not argv: the suite has cases
 * whose selector contains a NUL - `$["\u0000"]` is a legal query naming a
 * member whose name is one - and an argument cannot carry one.
 *
 * Prints "OK <json array>" with the selected nodes in order and then a second
 * line, "PATHS <json array>", with the normalized path of each - so the suite's
 * result and result_paths can both be checked. Or one line: "INVALID <message>"
 * for a query this library says is not well-formed, "UNSUPPORTED <message>" for
 * one it says is well-formed but does not evaluate, or "ERROR <message>" for
 * anything else.
 *
 * The three refusals are kept apart because the suite's invalid_selector
 * cases are asking for the first one. Counting "unsupported" as a pass there
 * would score an unimplemented feature as conformance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/text/json.h>

int main(void) {
  static char request[1 << 22];
  const size_t total = fread(request, 1, sizeof(request), stdin);

  /* The length line, then the selector, then the document. */
  size_t at = 0;
  size_t selector_len = 0;
  int have_digits = 0;
  while (at < total && request[at] >= '0' && request[at] <= '9') {
    selector_len = selector_len * 10 + (size_t)(request[at] - '0');
    at++;
    have_digits = 1;
  }
  if (!have_digits || at >= total || request[at] != '\n'
      || at + 1 + selector_len > total) {
    printf("ERROR malformed request\n");
    return 0;
  }
  at++;
  const char * selector = request + at;
  const char * document = request + at + selector_len;
  const size_t n = total - at - selector_len;

  /* The query is compiled first, because an invalid_selector case may come
     with no document at all. */
  GTEXT_JSON_Error perr;
  memset(&perr, 0, sizeof(perr));
  GTEXT_JSON_Path * path =
      gtext_json_path_compile(selector, selector_len, NULL, &perr);
  if (!path) {
    const char * message = perr.message ? perr.message : "?";
    if (perr.code == GTEXT_JSON_E_PATH_UNSUPPORTED) {
      printf("UNSUPPORTED %s\n", message);
    }
    else if (perr.code == GTEXT_JSON_E_PATH) {
      printf("INVALID %s\n", message);
    }
    else {
      printf("ERROR %s\n", message);
    }
    return 0;
  }

  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
  opts.preserve_number_lexeme = true;
  opts.parse_int64 = true;
  opts.parse_double = true;
  GTEXT_JSON_Error jerr;
  memset(&jerr, 0, sizeof(jerr));
  GTEXT_JSON_Value * doc = gtext_json_parse(document, n, &opts, &jerr);
  if (!doc) {
    printf("ERROR document: %s\n", jerr.message ? jerr.message : "?");
    gtext_json_path_free(path);
    gtext_json_error_free(&jerr);
    return 0;
  }

  GTEXT_JSON_Path_Result result;
  const GTEXT_JSON_Status status =
      gtext_json_path_select_paths(path, doc, &result);
  if (status != GTEXT_JSON_OK) {
    printf("ERROR select: %d\n", (int)status);
    gtext_json_free(doc);
    gtext_json_path_free(path);
    return 0;
  }

  /* The nodes are written one at a time and joined here: they belong to the
     document, so putting them in an array value would mean either copying them
     or handing the array something it would try to own. */
  printf("OK [");
  for (size_t i = 0; i < result.count; i++) {
    GTEXT_JSON_Sink sink;
    if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
      printf("] \nERROR sink\n");
      return 0;
    }
    GTEXT_JSON_Write_Options wopts = gtext_json_write_options_default();
    GTEXT_JSON_Error werr;
    memset(&werr, 0, sizeof(werr));
    if (gtext_json_write_value(&sink, &wopts, result.nodes[i], &werr)
        != GTEXT_JSON_OK) {
      printf("]\nERROR write: %s\n", werr.message ? werr.message : "?");
      return 0;
    }
    if (i) {
      printf(",");
    }
    fwrite(gtext_json_sink_buffer_data(&sink), 1,
        gtext_json_sink_buffer_size(&sink), stdout);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_error_free(&werr);
  }
  printf("]\n");

  /* The paths, written as JSON strings by the library rather than by hand: a
     normalized path may contain a backslash of its own - `$['x\\ty']` - and
     escaping that correctly is the writer's job. */
  printf("PATHS [");
  for (size_t i = 0; i < result.count; i++) {
    GTEXT_JSON_Value * as_string =
        gtext_json_new_string(result.paths[i], strlen(result.paths[i]));
    GTEXT_JSON_Sink sink;
    if (!as_string || gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
      printf("]\nERROR path sink\n");
      return 0;
    }
    GTEXT_JSON_Write_Options wopts = gtext_json_write_options_default();
    GTEXT_JSON_Error werr;
    memset(&werr, 0, sizeof(werr));
    if (gtext_json_write_value(&sink, &wopts, as_string, &werr)
        != GTEXT_JSON_OK) {
      printf("]\nERROR path write\n");
      return 0;
    }
    if (i) {
      printf(",");
    }
    fwrite(gtext_json_sink_buffer_data(&sink), 1,
        gtext_json_sink_buffer_size(&sink), stdout);
    gtext_json_sink_buffer_free(&sink);
    gtext_json_free(as_string);
    gtext_json_error_free(&werr);
  }
  printf("]\n");

  gtext_json_path_result_free(&result);
  gtext_json_free(doc);
  gtext_json_path_free(path);
  gtext_json_error_free(&jerr);
  return 0;
}
