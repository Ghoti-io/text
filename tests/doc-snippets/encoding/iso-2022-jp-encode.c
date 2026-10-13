#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* ISO-2022-JP is stateful: escape sequences switch between ASCII, the Roman set and JIS X 0208, and the flush at the end of the stream writes ESC ( B when the last one entered was not ASCII. */
static void iso_2022_jp_encode(void) {
  const char * text = "\xE3\x81\x82\xC3\xA9";   /* あ (hiragana A), then é, which JIS X 0208 does not have */
  GTEXT_Encoder_Options options = gtext_encoder_options_default();
  options.error_mode = GTEXT_ENCODER_HTML;

  char * bytes = NULL;
  size_t length = 0;
  GTEXT_Encoder_Result result;
  if (gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_2022_JP, &options, text, strlen(text), &bytes, &length, &result) == GTEXT_ENCODING_OK) {
    for (size_t i = 0; i < length; i++) {
      const unsigned char c = (unsigned char)bytes[i];
      if (c == 0x1B) {
        printf("<ESC>");
      } else {
        printf("%c", c);
      }
    }
    /* The escape back to ASCII comes before the reference, so that the reference reads as text. */
    printf(" (%zu bytes, %zu written as a reference)\n", length, result.replacements);
    gtext_encoding_buffer_free(bytes);
  }
}
