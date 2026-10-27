#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char text[256];
  size_t length, lines;
} collected;

static GTEXT_Markup_Status collect(void * user, const char * data, size_t length) {
  collected * c = (collected *)user;
  for (size_t i = 0; i < length; i++) {
    if (data[i] == '\n') {
      c->lines++;
    }
    if (c->length < sizeof c->text - 1 && c->lines < 1) {
      c->text[c->length++] = data[i];
    }
  }
  c->text[c->length] = '\0';
  return GTEXT_MARKUP_OK;
}

static void compute(const GTEXT_CSS_Compute_Context * context, const char * declaration, GTEXT_CSS_Computed_Style ** out) {
  GTEXT_CSS_Style_Entry entry;
  memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  entry.text.data = declaration;
  entry.text.length = strlen(declaration);
  GTEXT_CSS_Style_Options options;
  memset(&options, 0, sizeof options); /* no attributes are read: the entry is all there is */
  gtext_css_style_compute(context, NULL, NULL, &entry, 1, &options, out, NULL);
}

/* A computed style has a canonical text, one line to a property with the properties in the byte order of their names, and the SHA-256 of that text. Two
   cascades that come to the same values give the same text and the same hash, whatever declarations they came from; a document dump can embed the hash. */
static void style_dump(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  GTEXT_CSS_Computed_Style *a = NULL, *b = NULL, *c = NULL;
  compute(context, "margin-left: 1px; color: red", &a);
  compute(context, "color: red; margin-left: 1px; margin-left: 1px", &b);
  compute(context, "color: blue", &c);

  collected text;
  memset(&text, 0, sizeof text);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &text;
  sink.write = collect;
  gtext_css_computed_dump(a, &sink);
  printf("version %d, begins:\n%s\n", GTEXT_CSS_COMPUTED_DUMP_VERSION, text.text);
  size_t properties = 0, cursor = 0;
  while (gtext_css_computed_next(a, &cursor, NULL, NULL)) {
    properties++;
  }
  printf("a line for each property, after the two of the header: %s\n", text.lines == properties + 2 ? "yes" : "no");

  unsigned char ha[GTEXT_CSS_COMPUTED_HASH_LENGTH], hb[GTEXT_CSS_COMPUTED_HASH_LENGTH], hc[GTEXT_CSS_COMPUTED_HASH_LENGTH];
  gtext_css_computed_hash(a, ha);
  gtext_css_computed_hash(b, hb);
  gtext_css_computed_hash(c, hc);
  printf("hash of %d bytes\n", (int)GTEXT_CSS_COMPUTED_HASH_LENGTH);
  printf("same values, same hash: %s\n", memcmp(ha, hb, sizeof ha) == 0 ? "yes" : "no");
  printf("other values, same hash: %s\n", memcmp(ha, hc, sizeof ha) == 0 ? "yes" : "no");

  gtext_css_computed_free(a);
  gtext_css_computed_free(b);
  gtext_css_computed_free(c);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
