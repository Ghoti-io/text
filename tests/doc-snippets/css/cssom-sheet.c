#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Parse a sheet, walk its rules, edit the declarations of one of them, and give back the memory of a sheet most of whose rules are gone. */
static void cssom_sheet(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "a { color: red }\n@media print { b { width: 1px } }\nbogus >{ }\n";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);

  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  printf("rules: %zu, dropped: %zu\n", gtext_cssom_rule_list_length(rules), gtext_cssom_sheet_diagnostic_total(sheet));
  for (GTEXT_CSSOM_Rule * rule = gtext_cssom_rule_list_first(rules); rule;) {
    GTEXT_CSSOM_Source_Info where;
    gtext_cssom_rule_source(rule, &where);
    printf("%s (type %u) at line %zu\n", gtext_cssom_rule_interface_name(rule), gtext_cssom_rule_type(rule), where.span.line);
    GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(rules, rule);
    gtext_cssom_rule_release(rule);
    rule = next;
  }

  /* cssRules[1].cssRules[0].style.setProperty('width', '2px'): the rule inside the @media rule, and the sheet, are changed */
  GTEXT_CSSOM_Rule * media = gtext_cssom_rule_list_at(rules, 1);
  GTEXT_CSSOM_Rule * inner = gtext_cssom_rule_list_at(gtext_cssom_rule_css_rules(media), 0);
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_rule_style(inner, &style);
  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_decl_set_property(style, "width", 5, "2px", 3, "", 0, &outcome);
  char * css = NULL;
  size_t length = 0;
  gtext_cssom_decl_css_text(style, &css, &length);
  GTEXT_CSSOM_Sheet * owner = gtext_cssom_rule_parent_sheet(inner);
  printf("style: %s (sheet version %llu, owner is the sheet: %s)\n", css, (unsigned long long)gtext_cssom_sheet_version(sheet), owner == sheet ? "yes" : "no");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), css);
  gtext_cssom_sheet_release(owner);

  /* a held rule keeps its sheet: let go of the sheet and the document handle, and read through the rule */
  gtext_cssom_decl_release(style);
  gtext_cssom_rule_release(media);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  GTEXT_CSSOM_Sheet * still = gtext_cssom_rule_parent_sheet(inner);
  const GTEXT_CSSOM_Rule_List * again = NULL;
  gtext_cssom_sheet_rules(still, &again);
  printf("after the release: %zu rules\n", gtext_cssom_rule_list_length(again));
  gtext_cssom_sheet_release(still);
  gtext_cssom_rule_release(inner);
  gtext_css_registry_free(registry);
}
