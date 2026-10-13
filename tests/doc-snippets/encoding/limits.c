#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* The output of a stream has a limit, 256 MiB unless the options say otherwise; 0 is never "no limit". */
static void limits(void) {
  const char * bytes = "\xC3\xA9\xC3\xA9\xC3\xA9";   /* three letters e with an acute accent */
  GTEXT_Decoder_Options options = gtext_decoder_options_default();
  options.max_output_bytes = 5;

  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;
  GTEXT_Encoding_Status status = gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &options, bytes, strlen(bytes), &utf8, &length, &result);
  /* The sink would have had the first two letters (4 bytes), never half of the third. */
  printf("%s after %zu bytes of output\n", gtext_encoding_status_string(status), result.output_bytes);
}
