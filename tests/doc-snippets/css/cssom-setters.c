#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static void show(const char * label, GTEXT_CSS_Status status, char * text, GTEXT_CSSOM_Document * document) {
  printf("%s: %s\n", label, status == GTEXT_CSS_OK ? text : gtext_css_status_string(status));
  if (status == GTEXT_CSS_OK) {
    gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);
  }
}

/* An editor's panel: change a selector, a counter style and a media list, read an @page block, and replace a constructed sheet. */
static void cssom_setters(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "a, b { color: red } @media screen { c {} } @counter-style n { system: cyclic; symbols: a b } @page { size: a3; margin: 1em 2em }";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);

  /* style.selectorText = "d > e": the rule is replaced as a whole, and keeps its identity; text that is not a selector is ignored */
  GTEXT_CSSOM_Rule * a = gtext_cssom_rule_list_at(rules, 0);
  GTEXT_CSSOM_Outcome outcome;
  GTEXT_CSS_Status status;
  gtext_cssom_rule_set_selector_text(a, "d > e", 5, &outcome);
  printf("selectorText: changed %d\n", outcome.kind == GTEXT_CSSOM_CHANGED);
  gtext_cssom_rule_set_selector_text(a, "d >", 3, &outcome);
  printf("selectorText: ignored %d\n", outcome.kind == GTEXT_CSSOM_IGNORED_INVALID_SELECTOR);
  char * css = NULL;
  size_t length = 0;
  status = gtext_cssom_rule_css_text(a, &css, &length);
  show("cssText", status, css, document);

  /* media.mediaText, appendMedium(), deleteMedium(): a text always parses, a medium that is not valid is `not all`, a text that is not one query does nothing, a medium that is not there is a NotFoundError */
  GTEXT_CSSOM_Rule * media = gtext_cssom_rule_list_at(rules, 1);
  GTEXT_CSSOM_Media_List * list = NULL;
  gtext_cssom_rule_media(media, &list);
  gtext_cssom_media_set_text(list, "print, (bogus: 1) and", 21, NULL);
  gtext_cssom_media_append(list, "tv", 2, NULL);
  status = gtext_cssom_media_delete(list, "handheld", 8, NULL);
  printf("deleteMedium: %s\n", gtext_cssom_exception_name(status));
  status = gtext_cssom_media_text(list, &css, &length);
  show("mediaText", status, css, document);

  /* a counter style's descriptors are attributes; the empty string removes one */
  GTEXT_CSSOM_Rule * counter = gtext_cssom_rule_list_at(rules, 2);
  gtext_cssom_rule_set_attribute(counter, "symbols", 7, "x y z", 5, NULL);
  gtext_cssom_rule_set_attribute(counter, "system", 6, "bogus (", 7, &outcome);
  printf("system: ignored %d\n", outcome.kind == GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  status = gtext_cssom_rule_css_text(counter, &css, &length);
  show("counter style", status, css, document);

  /* the block of an @page rule: margins are properties, size is a descriptor; what a page does not take is refused */
  GTEXT_CSSOM_Rule * page = gtext_cssom_rule_list_at(rules, 3);
  GTEXT_CSSOM_Decl * block = NULL;
  gtext_cssom_rule_style(page, &block);
  size_t count = 0;
  gtext_cssom_decl_length(block, &count);
  gtext_cssom_decl_set_property(block, "transform", 9, "none", 4, "", 0, &outcome);
  printf("page block: %zu declarations, transform ignored %d\n", count, outcome.kind == GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  gtext_cssom_decl_set_property(block, "size", 4, "landscape", 9, "", 0, NULL);
  status = gtext_cssom_rule_css_text(page, &css, &length);
  show("page", status, css, document);

  /* new CSSStyleSheet() and replaceSync(): the @import is dropped, and every old rule is detached, the nested ones too */
  GTEXT_CSSOM_Sheet * constructed = NULL;
  gtext_cssom_sheet_new_constructed(document, NULL, &constructed);
  gtext_cssom_sheet_replace_sync(constructed, "x { y { } }", 11, NULL);
  const GTEXT_CSSOM_Rule_List * crules = NULL;
  gtext_cssom_sheet_rules(constructed, &crules);
  GTEXT_CSSOM_Rule * old = gtext_cssom_rule_list_at(crules, 0);
  gtext_cssom_sheet_replace_sync(constructed, "@import 'a.css'; z { }", 22, NULL);
  GTEXT_CSSOM_Sheet * parent = gtext_cssom_rule_parent_sheet(old);
  printf("old rule: parentStyleSheet is %s; sheet rules: %zu\n", parent ? "set" : "null", gtext_cssom_rule_list_length(crules));
  status = gtext_cssom_sheet_replace_sync(sheet, "a {}", 4, NULL);
  printf("replaceSync on a parsed sheet: %s\n", gtext_cssom_exception_name(status));

  gtext_cssom_rule_release(old);
  gtext_cssom_sheet_release(constructed);
  gtext_cssom_decl_release(block);
  gtext_cssom_rule_release(page);
  gtext_cssom_rule_release(counter);
  gtext_cssom_media_release(list);
  gtext_cssom_rule_release(media);
  gtext_cssom_rule_release(a);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
