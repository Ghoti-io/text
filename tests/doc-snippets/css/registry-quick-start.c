#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Every property of the specifications is in the registry; a declaration's value becomes a typed value, or a reason it is not one. */
static void registry_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);

  const char * declarations[][2] = {
      {"margin-top", "10px"}, {"margin", "1px 2px"}, {"WIDTH", "revert-layer"}, {"margin", "var(--m)"},
      {"width", "-1px"}, {"-moz-nosuch", "1"}, {"-webkit-transform", "rotate(3deg)"}, {"nav-up", "auto"},
  };
  for (size_t i = 0; i < sizeof declarations / sizeof declarations[0]; i++) {
    const char * name = declarations[i][0];
    const char * value = declarations[i][1];
    GTEXT_CSS_Property_Value result;
    const GTEXT_CSS_Status status =
        gtext_css_parse_property_text(registry, name, strlen(name), value, strlen(value), NULL, &result);
    printf("%s: %s -> ", name, value);
    if (status != GTEXT_CSS_OK) {
      printf("%s\n", gtext_css_status_string(status));
    }
    else if (result.outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
      printf("invalid\n");
    }
    else if (result.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN) {
      printf("unknown property\n");
    }
    else {
      char * text = NULL;
      size_t length = 0;
      gtext_css_property_value_serialize_buffer(&result, NULL, &text, &length);
      printf("%s %s\n", result.outcome == GTEXT_CSS_PROP_RESULT_WIDE ? "wide keyword" :
                        result.outcome == GTEXT_CSS_PROP_RESULT_PENDING ? "pending" : "valid", text);
      gtext_css_serialize_buffer_free(NULL, text);
    }
    gtext_css_property_value_free(&result);
  }
  gtext_css_registry_free(registry);
}
