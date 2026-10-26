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

static void show(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Value * value = gtext_css_computed_root(style, gtext_css_registry_find_property(registry, name, strlen(name)));
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* A style is computed from the declarations that won the cascade and the style of the parent: lengths become pixels against the font size,
   a property nobody declares is inherited or initial, a logical property is the physical one the writing mode says. */
static void computed_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  const declaration parent_css[] = {{"font-size", "20px"}, {"color", "red"}};
  const declaration child_css[] = {{"width", "2em"}, {"margin-inline-start", "5px"}, {"border-style", "none"}, {"border-width", "3px"}};
  GTEXT_CSS_Declaration_Block * parent_block = block_of(registry, parent_css, 2);
  GTEXT_CSS_Declaration_Block * child_block = block_of(registry, child_css, 4);

  GTEXT_CSS_Computed_Style * parent = NULL;
  GTEXT_CSS_Computed_Style * child = NULL;
  gtext_css_compute_style(context, parent_block, NULL, NULL, &parent, NULL);
  gtext_css_compute_style(context, child_block, parent, NULL, &child, NULL);

  show(registry, child, "width");
  show(registry, child, "color");
  show(registry, child, "margin-left");
  show(registry, child, "border-top-width");

  gtext_css_computed_free(child);
  gtext_css_computed_free(parent);
  gtext_css_block_free(child_block);
  gtext_css_block_free(parent_block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
