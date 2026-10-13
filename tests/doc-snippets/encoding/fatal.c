#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* The standard's default is to replace what is not well-formed with U+FFFD. Fatal mode stops instead, and says where. */
static void fatal(void) {
  const char * bytes = "ok \xE2\x82 oops";   /* a truncated euro sign */
  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;

  GTEXT_Decoder_Options options = gtext_decoder_options_default();
  GTEXT_Encoding_Status status = gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &options, bytes, strlen(bytes), &utf8, &length, &result);
  printf("replacement mode: %s, %zu replaced\n", gtext_encoding_status_string(status), result.replacements);
  gtext_encoding_buffer_free(utf8);

  options.error_mode = GTEXT_DECODER_FATAL;
  status = gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &options, bytes, strlen(bytes), &utf8, &length, &result);
  printf("fatal mode: %s at byte %zu\n", gtext_encoding_status_string(status), result.error_offset);
}
