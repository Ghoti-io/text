#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* A sink is a function that is given each chunk of output. This one prints it. */
static bool print_chunk(void * user, const char * bytes, size_t length) {
  (void)user;
  printf("[%.*s]", (int)length, bytes);
  return true;
}

/* UTF-8 arrives in pieces cut anywhere, here in the middle of the euro sign. The output is the same however it is cut. */
static void streaming(void) {
  const char piece1[] = "price \xE2\x82";
  const char piece2[] = "\xAC 5";
  const GTEXT_Encoding_Sink sink = {print_chunk, NULL};

  GTEXT_Decoder * decoder = NULL;
  if (gtext_decoder_new(GTEXT_ENCODING_UTF_8, NULL, &decoder) != GTEXT_ENCODING_OK) {
    return;
  }
  gtext_decoder_decode(decoder, piece1, sizeof(piece1) - 1, false, &sink, NULL);   /* not the end: the euro sign is held back */
  gtext_decoder_decode(decoder, piece2, sizeof(piece2) - 1, true, &sink, NULL);    /* the end */
  printf("\n");
  gtext_decoder_free(decoder);
}
