#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char * name;
  const char * value;
} declaration;

static GTEXT_CSS_Declaration_Block * block_of(const GTEXT_CSS_Registry * registry, const declaration * list, size_t count) {
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);
  for (size_t i = 0; i < count; i++) {
    gtext_css_block_add_text(block, list[i].name, strlen(list[i].name), list[i].value, strlen(list[i].value), false, NULL, NULL, NULL);
  }
  return block;
}

static void show_custom(const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Custom_Value * value = NULL;
  if (!gtext_css_computed_custom_find(style, name, strlen(name), &value)) {
    printf("%s: (the guaranteed-invalid value)\n", name);
    return;
  }
  char * text = NULL;
  size_t length = 0;
  gtext_css_custom_value_text(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* A registered custom property has a syntax, whether it inherits and an initial value. Its value is checked against the syntax and computed by the rules of
   its type: `2em` is a length, so it becomes pixels against the element's font size, and `red` is not one, so the property is invalid at computed-value time
   and takes its initial value. A registration is made as `CSS.registerProperty()` makes one: through the builder, before the registry is frozen. */
static void variables_registered(void) {
  GTEXT_CSS_Registry_Builder * builder = NULL;
  gtext_css_registry_builder_new(NULL, &builder);
  GTEXT_CSS_Custom_Def def;
  memset(&def, 0, sizeof def);
  def.name = "--len";
  def.syntax = "<length>";
  def.inherits = false;
  def.initial_value = "0px";
  gtext_css_registry_register_custom(builder, &def, NULL);
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_freeze(builder, &registry, NULL);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  const declaration good[] = {{"font-size", "20px"}, {"--len", "2em"}};
  const declaration bad[] = {{"--len", "red"}};
  GTEXT_CSS_Declaration_Block * good_block = block_of(registry, good, 2);
  GTEXT_CSS_Declaration_Block * bad_block = block_of(registry, bad, 1);
  GTEXT_CSS_Computed_Style * good_style = NULL;
  GTEXT_CSS_Computed_Style * bad_style = NULL;
  gtext_css_compute_style(context, good_block, NULL, NULL, &good_style, NULL);
  gtext_css_compute_style(context, bad_block, NULL, NULL, &bad_style, NULL);

  show_custom(good_style, "--len");
  show_custom(bad_style, "--len");

  gtext_css_computed_free(bad_style);
  gtext_css_computed_free(good_style);
  gtext_css_block_free(bad_block);
  gtext_css_block_free(good_block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
