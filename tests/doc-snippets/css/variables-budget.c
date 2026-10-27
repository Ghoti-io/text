#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A few bytes of CSS can name a value of any size: each of these thirty custom properties is two copies of the one before. The values are token ropes,
   so building them costs what was written and not what they name, and a value that would have more tokens than the budget allows is the guaranteed-invalid
   value, as are all that are made of it (the bytes of a value, 1 MiB, are past the budget before its tokens are). The result of the computation counts what happened. */
static void variables_budget(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);
  gtext_css_block_add_text(block, "--v0", 4, "lol", 3, false, NULL, NULL, NULL);
  for (int i = 1; i <= 30; i++) {
    char name[16], value[48];
    int n = snprintf(name, sizeof name, "--v%d", i);
    int m = snprintf(value, sizeof value, "var(--v%d) var(--v%d)", i - 1, i - 1);
    gtext_css_block_add_text(block, name, (size_t)n, value, (size_t)m, false, NULL, NULL, NULL);
  }
  GTEXT_CSS_Computed_Style * style = NULL;
  GTEXT_CSS_Compute_Result result;
  gtext_css_compute_style(context, block, NULL, NULL, &style, &result);

  const GTEXT_CSS_Custom_Value * value = NULL;
  printf("--v18 %s\n", gtext_css_computed_custom_find(style, "--v18", 5, &value) ? "has a value" : "is guaranteed-invalid");
  printf("--v19 %s\n", gtext_css_computed_custom_find(style, "--v19", 5, &value) ? "has a value" : "is guaranteed-invalid");
  printf("--v30 %s\n", gtext_css_computed_custom_find(style, "--v30", 5, &value) ? "has a value" : "is guaranteed-invalid");
  printf("values stopped by the budget: %llu\n", (unsigned long long)result.substitution_limited);
  printf("work under 200000: %s\n", result.work < 200000 ? "yes" : "no");

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
