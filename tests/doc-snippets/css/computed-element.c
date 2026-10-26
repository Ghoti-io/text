#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct node {
  const char * name;
  const struct node * parent;
  const struct node * inherits_from;
  const char * font_size;
  GTEXT_CSS_Declaration_Block * block;
} node;

static GTEXT_CSS_Element_Handle tree_parent(void * user, GTEXT_CSS_Element_Handle element) {
  (void)user;
  return ((const node *)element)->parent;
}

static GTEXT_CSS_Element_Handle inheritance_parent(void * user, GTEXT_CSS_Element_Handle element) {
  (void)user;
  return ((const node *)element)->inherits_from;
}

static GTEXT_CSS_Status declarations_of(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Declaration_Block ** block) {
  (void)user;
  *block = ((const node *)element)->block;
  return GTEXT_CSS_OK;
}

/* The tree and the inheritance are two things: a clone made for an SVG <use> sits in the shadow tree and inherits from the <use>, not from the
   element it was cloned from. The caller says both. */
static void computed_element(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  node root = {"svg", NULL, NULL, "10px", NULL};
  node source = {"symbol", &root, &root, "40px", NULL};
  node use = {"use", &root, &root, "20px", NULL};
  node clone = {"symbol clone", &source, &use, NULL, NULL};
  node * all[] = {&root, &source, &use, &clone};
  for (size_t i = 0; i < 4; i++) {
    gtext_css_block_new(registry, NULL, &all[i]->block);
    if (all[i]->font_size) {
      gtext_css_block_add_text(all[i]->block, "font-size", 9, all[i]->font_size, strlen(all[i]->font_size), false, NULL, NULL, NULL);
    }
    gtext_css_block_add_text(all[i]->block, "width", 5, "2em", 3, false, NULL, NULL, NULL);
  }

  GTEXT_CSS_Element_Interface iface;
  memset(&iface, 0, sizeof iface);
  iface.size = sizeof iface;
  iface.parent = tree_parent;
  iface.inheritance_parent = inheritance_parent;
  GTEXT_CSS_Declaration_Source source_of = {NULL, declarations_of};

  for (size_t i = 0; i < 4; i++) {
    GTEXT_CSS_Computed_Style * style = NULL;
    gtext_css_compute_element(context, &iface, &source_of, all[i], &style, NULL);
    char * text = NULL;
    size_t length = 0;
    gtext_css_serialize_value_buffer(gtext_css_computed_root(style, gtext_css_registry_find_property(registry, "width", 5)), NULL, &text, &length);
    printf("%s: width %s\n", all[i]->name, text);
    gtext_css_serialize_buffer_free(NULL, text);
    gtext_css_computed_free(style);
  }
  for (size_t i = 0; i < 4; i++) {
    gtext_css_block_free(all[i]->block);
  }
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
