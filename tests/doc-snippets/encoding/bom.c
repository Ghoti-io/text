#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* With bom_sniffing a byte order mark chooses the encoding and is not part of the text: the standard's "decode". */
static void bom(void) {
  const char bytes[] = "\xFF\xFE" "h\0i\0";   /* UTF-16LE, with its mark */
  GTEXT_Decoder_Options options = gtext_decoder_options_default();
  options.bom_sniffing = true;

  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;
  /* The encoding asked for is the one to use when there is no mark. */
  if (gtext_encoding_decode_buffer(GTEXT_ENCODING_WINDOWS_1252, &options, bytes, sizeof(bytes) - 1, &utf8, &length, &result) == GTEXT_ENCODING_OK) {
    printf("%s, from a byte order mark: %d, text: %s\n", gtext_encoding_name(result.encoding), result.bom_found, utf8);
    gtext_encoding_buffer_free(utf8);
  }
}
