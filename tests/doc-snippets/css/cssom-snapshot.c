#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The cascade reads a snapshot: the compiled form of a live sheet as it is now, the same object until something in the sheet changes. */
static void cssom_snapshot(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "@import url(base.css); a { color: red } @media print { b { width: 1px } }";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);
  GTEXT_CSS_Sheet * first = NULL;
  gtext_cssom_sheet_snapshot(sheet, &first);
  printf("records: %zu, imports: %zu\n", gtext_css_sheet_record_count(first), gtext_css_sheet_import_count(first));

  /* the caller fetched base.css: the import rule holds the sheet it loaded, and the importer changed */
  const char * base = "c { margin: 0 }";
  GTEXT_CSSOM_Sheet * child = NULL;
  gtext_cssom_sheet_new_parsed(document, base, strlen(base), NULL, &child);
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  GTEXT_CSSOM_Rule * import = gtext_cssom_rule_list_at(rules, 0);
  gtext_cssom_import_rule_complete(import, child, NULL);

  GTEXT_CSS_Sheet * second = NULL;
  GTEXT_CSS_Sheet * again = NULL;
  gtext_cssom_sheet_snapshot(sheet, &second);
  gtext_cssom_sheet_snapshot(sheet, &again);
  printf("after the import: records %zu, imports %zu, imported records %zu\n", gtext_css_sheet_record_count(second), gtext_css_sheet_import_count(second),
      gtext_css_sheet_record_count(gtext_css_sheet_import(second, 0)));
  printf("asked again: %s object\n", again == second ? "the same" : "another");

  /* a change inside the imported sheet is a change of the sheet that imports it; a snapshot handed out is never altered */
  gtext_cssom_sheet_insert_rule(child, "d { top: 0 }", 12, 0, NULL, NULL);
  GTEXT_CSS_Sheet * third = NULL;
  gtext_cssom_sheet_snapshot(sheet, &third);
  printf("after a change in the import: %s object, imported records %zu; the second still has %zu\n", third == second ? "the same" : "a new",
      gtext_css_sheet_record_count(gtext_css_sheet_import(third, 0)), gtext_css_sheet_record_count(gtext_css_sheet_import(second, 0)));

  gtext_css_sheet_free(first);
  gtext_css_sheet_free(second);
  gtext_css_sheet_free(again);
  gtext_css_sheet_free(third);
  gtext_cssom_rule_release(import);
  gtext_cssom_sheet_release(child);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
