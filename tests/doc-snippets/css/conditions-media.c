#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* matchMedia(): a media query list is data, and the answer depends on the environment it is asked in. */
static void conditions_media(void) {
  GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(NULL);
  env.viewport.width = 600;
  GTEXT_CSS_Conditions_Options options = gtext_css_conditions_options_default();
  options.media = &env;
  GTEXT_CSS_Conditions * conditions = NULL;
  if (gtext_css_conditions_new(NULL, &options, &conditions) != GTEXT_CSS_OK) {
    return;
  }
  const char * queries[] = {"(min-width: 40em)", "(width > 700px)", "(color: foo) or (color)", "not all"};
  for (size_t i = 0; i < sizeof queries / sizeof *queries; i++) {
    bool matches = false;
    if (gtext_css_conditions_matches_media(conditions, queries[i], strlen(queries[i]), NULL, &matches) == GTEXT_CSS_OK) {
      printf("%s: %s\n", queries[i], matches ? "true" : "false");
    }
  }
  gtext_css_conditions_free(conditions);
}
