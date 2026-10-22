#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A stylesheet that arrives in pieces: the answer does not depend on where they were cut. */
static void stream(void) {
  const char * pieces[] = {"a { col", "or: re", "d } b", " { margin: 0 }"};
  GTEXT_CSS_Stream * s = NULL;
  size_t total = 0;

  if (gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, NULL, &s) != GTEXT_CSS_OK) {
    return;
  }
  for (size_t i = 0; i < sizeof pieces / sizeof pieces[0]; i++) {
    total += strlen(pieces[i]);
    gtext_css_stream_feed(s, pieces[i], strlen(pieces[i]));
  }
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;
  if (gtext_css_stream_finish(s, &tree, &result) == GTEXT_CSS_OK) {
    printf("%zu rules from %zu bytes; the second is bytes %zu to %zu\n", tree->rule_count, total,
        tree->rules[1].span.offset, tree->rules[1].span.end);
    gtext_css_tree_free(tree);
  }
  gtext_css_result_free(&result);
  gtext_css_stream_free(s);

  /* The input limit is met as the input arrives. */
  GTEXT_CSS_Parse_Options opts = gtext_css_parse_options_default();
  opts.max_input_bytes = 10;
  gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, &opts, &s);
  printf("%s\n", gtext_css_status_string(gtext_css_stream_feed(s, pieces[0], strlen(pieces[0]))));
  printf("%s\n", gtext_css_status_string(gtext_css_stream_feed(s, pieces[1], strlen(pieces[1]))));
  gtext_css_stream_free(s);
}
