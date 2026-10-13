#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* The multi-byte encodings decode as the standard says, which is not always what iconv or Python do. */
static void cjk_decode(void) {
  /* A page labelled "x-sjis" (a label of Shift_JIS): the three characters of 日本語, then a lead byte followed by a space. */
  const char * label = "x-sjis";
  const char shift_jis[] = "\x93\xFA\x96\x7B\x8C\xEA\x81 end";
  GTEXT_Encoding encoding = gtext_encoding_for_label(label, strlen(label));

  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;
  if (gtext_encoding_decode_buffer(encoding, NULL, shift_jis, sizeof(shift_jis) - 1, &utf8, &length, &result) == GTEXT_ENCODING_OK) {
    /* The space after the lead byte is not part of the error: the standard puts it back. */
    printf("%s: %s (%zu replaced)\n", gtext_encoding_name(encoding), utf8, result.replacements);
    gtext_encoding_buffer_free(utf8);
  }

  /* gb18030 has four-byte characters; 0x80 is the euro sign in GBK and in gb18030 alike. */
  const char gb18030[] = "\x81\x30\x81\x30\x80";
  if (gtext_encoding_decode_buffer(GTEXT_ENCODING_GB18030, NULL, gb18030, sizeof(gb18030) - 1, &utf8, &length, &result) == GTEXT_ENCODING_OK) {
    printf("gb18030: %zu bytes of UTF-8 for 5 bytes (U+0080 and U+20AC), %zu replaced\n", length, result.replacements);
    gtext_encoding_buffer_free(utf8);
  }
}
