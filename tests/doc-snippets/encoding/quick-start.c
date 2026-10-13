#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* A page labelled "latin1" holds the bytes of "café €5". The standard's latin1 is windows-1252, in which 0x80 is the euro sign. */
static void quick_start(void) {
  const char * label = " Latin1 ";   /* white space around a label is ignored, and so is case */
  const char * bytes = "caf\xE9 \x80" "5";

  GTEXT_Encoding encoding = gtext_encoding_for_label(label, strlen(label));
  if (encoding == GTEXT_ENCODING_NONE) {
    printf("no such encoding\n");
    return;
  }
  printf("%s\n", gtext_encoding_name(encoding));

  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;
  if (gtext_encoding_decode_buffer(encoding, NULL, bytes, strlen(bytes), &utf8, &length, &result) != GTEXT_ENCODING_OK) {
    printf("not decoded: %s\n", result.message);
    return;
  }
  printf("%s (%zu bytes of UTF-8, %zu replaced)\n", utf8, length, result.replacements);
  gtext_encoding_buffer_free(utf8);
}
