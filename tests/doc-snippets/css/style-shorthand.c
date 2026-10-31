#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static void compute(const GTEXT_CSS_Compute_Context * context, const char * declarations, GTEXT_CSS_Computed_Style ** out) {
  GTEXT_CSS_Style_Entry entry;
  memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  entry.text.data = declarations;
  entry.text.length = strlen(declarations);
  GTEXT_CSS_Style_Options options;
  memset(&options, 0, sizeof options);
  gtext_css_style_compute(context, NULL, NULL, &entry, 1, &options, out, NULL);
}

/* getComputedStyle(element).getPropertyValue("background-position") asks a computed style for a shorthand: the text is written from the computed longhands,
   and is empty where they form no value of it. A color that needs nothing but itself has a computed text of its own. */
static void style_shorthand(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  GTEXT_CSS_Computed_Style * style = NULL;
  compute(context, "background-position: right 11% bottom; corner: 4px 2% / 3px notch", &style);
  const char * names[2] = {"background-position", "corner"};
  for (int i = 0; i < 2; i++) {
    char * text = NULL;
    size_t length = 0;
    gtext_css_computed_shorthand_text(style, gtext_css_registry_find_property(registry, names[i], strlen(names[i])), &text, &length, NULL);
    printf("%s: %s\n", names[i], text);
    gtext_css_serialize_buffer_free(NULL, text);
  }

  const char * source = "color-mix(in srgb, red, blue)";
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_component_values(source, strlen(source), NULL, &tree, NULL);
  GTEXT_CSS_Color_Options options = gtext_css_color_options_default();
  GTEXT_CSS_Color * color = NULL;
  bool valid = false;
  gtext_css_color_parse(&tree->values[0], &options, &valid, &color, NULL);
  char computed[GTEXT_CSS_COLOR_COMPUTED_TEXT_MAX + 1];
  size_t computed_length = 0;
  gtext_css_color_compute_text(color, false, NULL, computed, &computed_length);
  printf("%s is %s\n", source, computed);

  gtext_css_color_free(color);
  gtext_css_tree_free(tree);
  gtext_css_computed_free(style);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
