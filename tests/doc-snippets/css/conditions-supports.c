#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* CSS.supports(): a declaration is supported when the registry's grammar accepts its value, and an unknown thing is false. */
static void conditions_supports(void) {
  GTEXT_CSS_Registry * registry = NULL;
  GTEXT_CSS_Conditions * conditions = NULL;
  GTEXT_CSS_Conditions_Options options = gtext_css_conditions_options_default();
  if (gtext_css_registry_new(NULL, &registry) != GTEXT_CSS_OK || gtext_css_conditions_new(registry, &options, &conditions) != GTEXT_CSS_OK) {
    return;
  }
  const char * conditions_text[] = {"(display: grid)", "(display: bogus)", "not (display: bogus)", "(--x: a;b)", "selector(:is(a, b))", "(foo: red})"};
  for (size_t i = 0; i < sizeof conditions_text / sizeof *conditions_text; i++) {
    bool supported = false;
    if (gtext_css_conditions_supports_text(conditions, conditions_text[i], strlen(conditions_text[i]), &supported) == GTEXT_CSS_OK) {
      printf("%s: %s\n", conditions_text[i], supported ? "supported" : "not supported");
    }
  }
  gtext_css_conditions_free(conditions);
  gtext_css_registry_free(registry);
}
