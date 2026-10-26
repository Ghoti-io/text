#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A caller registers a property the specifications lack, and redefines one they have, which is refused unless it says so. */
static void registry_caller(void) {
  GTEXT_CSS_Registry_Builder * builder = NULL;
  gtext_css_registry_builder_new(NULL, &builder);

  GTEXT_CSS_Property_Def weight;
  memset(&weight, 0, sizeof weight);
  weight.name = "vector-weight";
  weight.grammar = "<number [0,\xe2\x88\x9e]> | auto";
  weight.initial = "1";
  weight.inherited = true;
  GTEXT_CSS_Registry_Result result;
  printf("vector-weight: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &weight, &result)));

  GTEXT_CSS_Property_Def rule;
  memset(&rule, 0, sizeof rule);
  rule.name = "fill-rule";
  rule.grammar = "nonzero | evenodd | auto";
  rule.initial = "nonzero";
  printf("fill-rule: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &rule, &result)));
  rule.override = true;
  printf("fill-rule, overriding: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &rule, &result)));

  weight.name = "vector-bad";
  weight.initial = "heavy";
  printf("a bad initial value: %s\n", gtext_css_status_string(gtext_css_registry_register_property(builder, &weight, &result)));

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_freeze(builder, &registry, NULL);
  const char * tests[][2] = {{"vector-weight", "2.5"}, {"vector-weight", "-1"}, {"fill-rule", "auto"}};
  for (size_t i = 0; i < 3; i++) {
    GTEXT_CSS_Property_Value value;
    gtext_css_parse_property_text(registry, tests[i][0], strlen(tests[i][0]), tests[i][1], strlen(tests[i][1]), NULL, &value);
    printf("%s: %s -> %s\n", tests[i][0], tests[i][1], value.outcome == GTEXT_CSS_PROP_RESULT_VALID ? "valid" : "invalid");
    gtext_css_property_value_free(&value);
  }
  GTEXT_CSS_Property_Id by = 0, was = 0;
  for (size_t i = 0; gtext_css_registry_override_at(registry, i, &by, &was); i++) {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(registry, by, &info);
    printf("override of %s\n", info.name);
  }
  gtext_css_registry_free(registry);
}
