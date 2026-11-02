#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

struct Out {
  char data[512];
  size_t used;
};

static GTEXT_Markup_Status collect(void * user, const char * bytes, size_t n) {
  struct Out * o = (struct Out *)user;
  memcpy(o->data + o->used, bytes, n);
  o->used += n;
  return GTEXT_MARKUP_OK;
}

/* An editor's round trip: parse a sheet, insert and delete rules, read the exceptions, write the sheet back. */
static void cssom_insert(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  const char * text = "@namespace s url(urn:s);\na { color: red }\n";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);

  /* sheet.insertRule("@media print { s|b { width: 1px } }", 2): the rule, and the rule inside it, are objects at once */
  const char * media = "@media print { s|b { width: 1px } }";
  GTEXT_CSSOM_Rule * made = NULL;
  GTEXT_CSS_Status status = gtext_cssom_sheet_insert_rule(sheet, media, strlen(media), 2, &made, NULL);
  printf("insert media: %s\n", gtext_css_status_string(status));

  /* the exceptions have names a binding throws: an @import after a style rule, a prefix nobody declared, text that is not a rule, an index past the end */
  const char * bad[] = {"@import url(a.css);", "t|c { }", "a { } b { }", "d { }"};
  const size_t where[] = {2, 2, 0, 9};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    status = gtext_cssom_sheet_insert_rule(sheet, bad[i], strlen(bad[i]), where[i], NULL, NULL);
    printf("%s -> %s\n", bad[i], gtext_cssom_exception_name(status));
  }

  /* into a style rule: a block of declarations becomes a nested-declarations rule, a relative selector gets its & */
  GTEXT_CSSOM_Rule * a = NULL;
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  a = gtext_cssom_rule_list_at(rules, 1);
  gtext_cssom_rule_insert_rule(a, "> c { }", 7, 0, NULL, NULL);
  gtext_cssom_rule_insert_rule(a, "width: 2px", 10, 1, NULL, NULL);

  /* deleteRule detaches the rule: it stays readable, and no sheet holds it */
  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_sheet_delete_rule(sheet, 2, &outcome);
  char * css = NULL;
  size_t length = 0;
  gtext_cssom_rule_css_text(made, &css, &length);
  GTEXT_CSSOM_Sheet * parent = gtext_cssom_rule_parent_sheet(made);
  printf("deleted: %s (parentStyleSheet is %s)\n", css, parent ? "set" : "null");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), css);

  /* the whole sheet, as text */
  struct Out out;
  memset(&out, 0, sizeof out);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = collect;
  gtext_cssom_sheet_serialize(sheet, &sink);
  printf("%.*s", (int)out.used, out.data);

  gtext_cssom_rule_release(a);
  gtext_cssom_rule_release(made);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
