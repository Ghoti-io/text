/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  GTEXT_HTML_Sanitize_Options options = gtext_html_sanitize_options_default();
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--images") == 0) options.allow_images = true;
    else if (strcmp(argv[i], "--tables") == 0) options.allow_tables = true;
    else return 2;
  }
  size_t used = 0, capacity = 4096;
  unsigned char *input = malloc(capacity);
  if (!input) return 2;
  for (;;) {
    if (used == capacity) {
      if (capacity >= options.parse.max_input_bytes) {
        int extra = fgetc(stdin);
        if (extra == EOF && !ferror(stdin)) break;
        free(input);
        return ferror(stdin) ? 2 : 3;
      }
      size_t next = capacity > options.parse.max_input_bytes / 2 ? options.parse.max_input_bytes : capacity * 2;
      void *grown = realloc(input, next);
      if (!grown) { free(input); return 2; }
      input = grown;
      capacity = next;
    }
    size_t n = fread(input + used, 1, capacity - used, stdin);
    used += n;
    if (n == 0) {
      if (ferror(stdin)) { free(input); return 2; }
      break;
    }
  }
  char *output = NULL;
  size_t output_length = 0;
  GTEXT_HTML_Sanitize_Result result;
  GTEXT_HTML_Status status = gtext_html_sanitize_buffer(input, used, &options,
      &output, &output_length, &result);
  free(input);
  if (status != GTEXT_HTML_OK) {
    fprintf(stderr, "%s: %s\n", gtext_html_status_string(status), result.message);
    return 1;
  }
  int bad = output_length && fwrite(output, 1, output_length, stdout) != output_length;
  gtext_html_sanitize_buffer_free(output);
  return bad ? 2 : 0;
}
