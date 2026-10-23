#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The types a grammar names come from the caller: the resolver hands out their definitions. */
typedef struct {
  GTEXT_CSS_Definition * length_percentage;
  GTEXT_CSS_Definition * x;
} grammar_types_known;

static const GTEXT_CSS_Definition * grammar_types_resolve(void * user, GTEXT_CSS_Ref_Kind kind, const char * name,
    size_t length) {
  grammar_types_known * known = (grammar_types_known *)user;
  (void)kind;
  if (length == 17 && memcmp(name, "length-percentage", 17) == 0) {
    return known->length_percentage;
  }
  if (length == 1 && name[0] == 'x') {
    return known->x;
  }
  return NULL;
}

static void grammar_types(void) {
  grammar_types_known known;
  const char * lp = "<length> | <percentage>";
  const char * x = "<x> | a"; /* a type that can match itself without consuming anything */
  gtext_css_definition_parse(lp, strlen(lp), NULL, &known.length_percentage, NULL);
  gtext_css_definition_parse(x, strlen(x), NULL, &known.x, NULL);

  const char * grammars[] = {"<length-percentage [0,\xe2\x88\x9e]>#", "<nosuch> | auto", "<x>"};
  const char * css = "1px, 50%, -1px";
  for (size_t i = 0; i < 3; i++) {
    GTEXT_CSS_Compiled * compiled = NULL;
    GTEXT_CSS_Grammar_Result result;
    const GTEXT_CSS_Status status = gtext_css_compile(grammars[i], strlen(grammars[i]), grammar_types_resolve, &known,
        NULL, &compiled, &result);
    if (status != GTEXT_CSS_OK) {
      printf("%s: %s\n", result.name, result.message);
      continue;
    }
    GTEXT_CSS_Tree * tree = NULL;
    gtext_css_parse_component_values(css, strlen(css), NULL, &tree, NULL);
    bool matched = true;
    gtext_css_match(compiled, tree->values, tree->value_count, NULL, &matched, NULL, NULL);
    printf("\"%s\" against %s: %s\n", css, grammars[i], matched ? "matches" : "does not match");
    gtext_css_tree_free(tree);
    gtext_css_compiled_free(compiled);
  }
  gtext_css_definition_free(known.length_percentage);
  gtext_css_definition_free(known.x);
}
