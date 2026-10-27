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

/* `var()` and the other substitution functions are replaced when a style is computed: the custom properties first, then every property that holds one.
   Nothing else has to be called; a declaration block with `var()` in it is computed as any other is. */
static void variables_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  const declaration css[] = {
      {"--gap", "4px 8px"},
      {"--pad", "var(--gap) 2px"},
      {"--cycle-a", "var(--cycle-b)"},
      {"--cycle-b", "var(--cycle-a)"},
      {"margin", "var(--gap)"},
      {"width", "var(--missing, 10px)"},
      {"height", "var(--cycle-a, 3px)"},
  };
  GTEXT_CSS_Declaration_Block * block = block_of(registry, css, 7);
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_compute_style(context, block, NULL, NULL, &style, NULL);

  show_custom(style, "--pad");
  show_custom(style, "--cycle-a");
  show(registry, style, "margin-top");
  show(registry, style, "margin-right");
  show(registry, style, "width");
  show(registry, style, "height");

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
