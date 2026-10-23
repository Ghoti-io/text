#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Units: looked up by name in any case, converted when their ratio is fixed, left alone when it is not. */
static void values_units(void) {
  const char * names[] = {"IN", "mm", "Q", "turn", "Khz", "em", "vmax", "parsec"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    const GTEXT_CSS_Unit unit = gtext_css_unit_lookup(names[i], strlen(names[i]));
    if (unit == GTEXT_CSS_UNIT_NONE) {
      printf("%s: not a unit\n", names[i]);
      continue;
    }
    double canonical = 0;
    if (gtext_css_unit_to_canonical(unit, 1.0, &canonical)) {
      printf("1%s is %g%s\n", gtext_css_unit_name(unit), canonical, gtext_css_unit_name(gtext_css_unit_canonical(unit)));
    }
    else {
      printf("1%s is relative: the environment decides\n", gtext_css_unit_name(unit));
    }
  }
}
