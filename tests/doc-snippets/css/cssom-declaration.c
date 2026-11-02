#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static const char * outcome_name(GTEXT_CSSOM_Outcome_Kind kind) {
  switch (kind) {
  case GTEXT_CSSOM_CHANGED: return "changed";
  case GTEXT_CSSOM_UNCHANGED: return "unchanged";
  case GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY: return "ignored: unknown property";
  case GTEXT_CSSOM_IGNORED_INVALID_VALUE: return "ignored: invalid value";
  case GTEXT_CSSOM_IGNORED_BAD_PRIORITY: return "ignored: bad priority";
  default: return "ignored";
  }
}

/* A declaration block is an object: setProperty reports what became of each call, and the block serializes as CSSOM does. */
static void cssom_declaration(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_decl_new(document, &style);

  const char * calls[][3] = {{"margin-top", "5px", ""}, {"margin", "1px 2px", ""}, {"margin", "1px 2px", ""}, {"color", "nonsense", ""},
      {"color", "red", "IMPORTANT"}, {"width", "1px", "later"}, {"bogus", "1", ""}};
  for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
    GTEXT_CSSOM_Outcome outcome;
    gtext_cssom_decl_set_property(style, calls[i][0], strlen(calls[i][0]), calls[i][1], strlen(calls[i][1]), calls[i][2], strlen(calls[i][2]), &outcome);
    printf("%s: %s -> %s\n", calls[i][0], calls[i][1], outcome_name(outcome.kind));
  }
  char * text = NULL;
  size_t length = 0;
  gtext_cssom_decl_css_text(style, &text, &length);
  printf("cssText: %s\n", text);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);

  gtext_cssom_decl_remove_property(style, "margin", 6, &text, &length, NULL);
  printf("removed margin: %s\n", text);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);
  size_t count = 0;
  gtext_cssom_decl_length(style, &count);
  printf("length: %zu\n", count);

  gtext_cssom_decl_release(style);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
