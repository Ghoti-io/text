/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <cstddef>
#include <cstdint>
#include <ghoti.io/text/html.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto options = gtext_html_sanitize_options_default();
  options.parse.max_input_bytes = 1024 * 1024;
  options.parse.max_nodes = 100000;
  options.max_work_units = 200000;
  options.max_output_bytes = 1024 * 1024;
  char *output = nullptr;
  size_t output_length = 0;
  (void)gtext_html_sanitize_buffer(data, size, &options, &output,
      &output_length, nullptr);
  gtext_html_sanitize_buffer_free(output);
  return 0;
}
