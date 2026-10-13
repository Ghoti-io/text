#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* The input is UTF-8. A code point the encoding cannot write is, in HTML mode, a numeric character reference: what an HTML form sends. */
static void encode(void) {
  const char * text = "price \xE2\x82\xAC" "5";   /* "price €5" */
  GTEXT_Encoder_Options options = gtext_encoder_options_default();
  options.error_mode = GTEXT_ENCODER_HTML;

  char * bytes = NULL;
  size_t length = 0;
  GTEXT_Encoder_Result result;
  /* ISO-8859-2 has no euro sign ... */
  if (gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, &options, text, strlen(text), &bytes, &length, &result) == GTEXT_ENCODING_OK) {
    printf("ISO-8859-2: %s (%zu written as a reference)\n", bytes, result.replacements);
    gtext_encoding_buffer_free(bytes);
  }
  /* ... and windows-1252, which is what the label "iso-8859-1" names, has it at 0x80. */
  if (gtext_encoding_encode_buffer(GTEXT_ENCODING_WINDOWS_1252, &options, text, strlen(text), &bytes, &length, &result) == GTEXT_ENCODING_OK) {
    printf("windows-1252: the euro sign is byte 0x%02X, %zu references\n", (unsigned char)bytes[6], result.replacements);
    gtext_encoding_buffer_free(bytes);
  }
  /* The default mode refuses what cannot be written. */
  options.error_mode = GTEXT_ENCODER_FATAL;
  GTEXT_Encoding_Status status = gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, &options, text, strlen(text), &bytes, &length, &result);
  printf("fatal: %s, U+%04X at byte %zu\n", gtext_encoding_status_string(status), (unsigned)result.error_code_point, result.error_offset);
}
