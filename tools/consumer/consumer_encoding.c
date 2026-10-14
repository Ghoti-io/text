/* SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.  An out-of-tree consumer: it includes one public header
 * family through the installed include path and links through pkg-config (tools/consumer/run.sh). */
#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const GTEXT_Encoding e = gtext_encoding_for_label(" Latin1 ", 8);
  if (e == GTEXT_ENCODING_NONE) {
    return 1;
  }
  printf("%s\n", gtext_encoding_name(e));
  char * out = NULL;
  size_t len = 0;
  const unsigned char in[] = {'a', 0xE9, 'b'};
  GTEXT_Decoder_Result r;
  if (gtext_encoding_decode_buffer(e, NULL, in, sizeof(in), &out, &len, &r) != GTEXT_ENCODING_OK) {
    return 1;
  }
  printf("%zu bytes of UTF-8: %s\n", len, out);
  int ok = len == 4 && memcmp(out, "a\xc3\xa9" "b", 4) == 0;
  gtext_encoding_buffer_free(out);
  return ok ? 0 : 1;
}
