#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static void cq_add(GTEXT_CSS_Candidates * set, const char * name, const char * value, GTEXT_CSS_Layer_Id layer, bool important, unsigned order) {
  GTEXT_CSS_Cascade_Entry keys;
  memset(&keys, 0, sizeof keys);
  keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  keys.layer = layer;
  keys.important = important;
  keys.sheet = order;
  gtext_css_candidates_add_text(set, &keys, name, strlen(name), value, strlen(value), NULL, NULL, NULL);
}

/* The candidate declarations of one element go in with the keys that sort them; the cascade says which declaration wins each property. A later layer
   beats an earlier one and unlayered declarations beat every layer, `!important` turns the layers round, and `revert-layer` takes a declaration's layer
   out of the cascade. */
static void cascade_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Layer_Registry * layers = NULL;
  gtext_css_layer_registry_new(NULL, &layers);
  GTEXT_CSS_Layer_Id base = 0, theme = 0;
  gtext_css_layer_declare(layers, GTEXT_CSS_ORIGIN_AUTHOR, 0, "base", 4, &base, NULL);
  gtext_css_layer_declare(layers, GTEXT_CSS_ORIGIN_AUTHOR, 0, "theme", 5, &theme, NULL);
  gtext_css_layer_freeze(layers);

  GTEXT_CSS_Candidates * set = NULL;
  gtext_css_candidates_new(registry, layers, NULL, &set);
  cq_add(set, "color", "red", base, false, 0);
  cq_add(set, "color", "blue", theme, false, 1);
  cq_add(set, "color", "green", GTEXT_CSS_LAYER_NONE, false, 2);
  cq_add(set, "width", "10px", base, false, 3);
  cq_add(set, "width", "20px", theme, false, 4);
  cq_add(set, "margin-left", "1px", base, true, 5);
  cq_add(set, "margin-left", "2px", theme, true, 6);
  cq_add(set, "height", "5px", base, false, 7);
  cq_add(set, "height", "revert-layer", theme, false, 8);

  GTEXT_CSS_Cascaded * cascaded = NULL;
  gtext_css_cascade(set, NULL, &cascaded, NULL);
  for (size_t i = 0; i < gtext_css_cascaded_count(cascaded); i++) {
    GTEXT_CSS_Cascade_Entry winner;
    gtext_css_cascaded_at(cascaded, i, &winner, NULL);
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(registry, winner.property, &info);
    char * value = NULL;
    size_t length = 0;
    gtext_css_serialize_value_buffer(gtext_css_value_set_root(winner.set), NULL, &value, &length);
    char layer[32];
    gtext_css_layer_path(layers, winner.layer, layer, sizeof layer);
    printf("%s: %s (%s)\n", info.name, value, winner.layer ? layer : "unlayered");
    gtext_css_serialize_buffer_free(NULL, value);
  }
  gtext_css_cascaded_free(cascaded);
  gtext_css_candidates_free(set);
  gtext_css_layer_registry_free(layers);
  gtext_css_registry_free(registry);
}
