@page format_css CSS

# CSS

`gtext_css_parse_stylesheet()` and the eight other entry points read **CSS as
CSS Syntax Module Level 3 says a browser does**, from decoded text, and
`gtext_css_parse_stylesheet_bytes()` and the incremental parser from the bytes of a stylesheet, into an immutable tree; `gtext_css_serialize_tree()` writes a tree back
as text that reads back as the same tree. CSS has no malformed input: **every
input has a tree**, the mistakes in it are parse errors with positions, and what
the standard drops is dropped and reported. A parse fails only for an argument,
memory or a limit, and each limit is its own status. This is the syntax layer:
values, properties, the cascade and selectors are built on it and are not here
yet (see "Not implemented"). Back to \ref text_format_references "Formats".

Include `<ghoti.io/text/css.h>`.

## Quick start

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Parse a stylesheet: a rule with a declaration, a nested rule, and the declarations after it. */
static void quick_start(void) {
  const char * css = "a, b { color: red; &:hover { color: blue } margin: 0 !important }";
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, &result) != GTEXT_CSS_OK) {
    fprintf(stderr, "not parsed: %s\n", result.message);
    gtext_css_result_free(&result);
    return;
  }
  const GTEXT_CSS_Rule * rule = &tree->rules[0];
  printf("%zu rule: %zu declaration, %zu child rules\n", tree->rule_count, rule->declaration_count,
      rule->rule_count);
  printf("%s: %s\n", rule->declarations[0].name.data, rule->declarations[0].value[0].value.data);
  for (size_t i = 0; i < rule->rule_count; i++) {
    const GTEXT_CSS_Rule * child = &rule->rules[i];
    if (child->kind == GTEXT_CSS_RULE_NESTED_DECLARATIONS) {
      printf("then %s%s\n", child->declarations[0].name.data,
          child->declarations[0].important ? " !important" : "");
    }
    else {
      printf("nested rule at line %zu, column %zu\n", child->span.line, child->span.column);
    }
  }
  gtext_css_tree_free(tree);
  gtext_css_result_free(&result);
}
```

prints

```
1 rule: 1 declaration, 2 child rules
color: red
nested rule at line 1, column 20
then margin !important
```

The rule's block holds a declaration, a nested rule and a declaration after it,
in that order, and that order is kept: CSS Nesting's model, in which the
declarations after a nested rule are a **nested declarations rule** of their
own and not hoisted into the rule's list.

## Normative references

- **CSS Syntax Module Level 3, Editor's Draft, 20 October 2026**,
  <https://drafts.csswg.org/css-syntax/>, as served on 2026-10-21. It names the
  Candidate Recommendation Draft of 1 October 2026 as its previous version (its
  changes section calls it "the 10 October 2026 Candidate Recommendation
  Draft") and records one change since: "Removed 'parse a block's contents'
  [...] and replaced it with instead restoring parse a list of declarations".
- **CSSOM, Editor's Draft, 13 October 2026**, section 2.1 "Common Serializing
  Idioms" (escape a character, escape a character as code point, serialize an
  identifier, serialize a string), for the serializer's spelling.
- **CSS Nesting** is in the Syntax draft itself (the block contents algorithm,
  nested declarations rules, the semicolon stop token, the custom-property
  rules); nothing of css-nesting-1 beyond that is needed to parse.

Who owns what: a tree owns every node, string and array in it and is released
by gtext_css_tree_free() (the tree struct is in that memory too, so the tree
pointer and every pointer into it are dead after the call); a result owns its
parse errors and is released by gtext_css_result_free() (the struct is the
caller's and is left zeroed); a buffer from a `_buffer` serializer is released by
gtext_css_serialize_buffer_free() with the options it was written with. Every
allocation of all three goes through the options' allocator.

## Parts implemented

- **The input stream** (3.3, "filter code points"): CR, CR LF and FF are one
  LF; U+0000 and a surrogate are U+FFFD. The input is UTF-8; an ill-formed
  sequence is U+FFFD for each maximal subpart, as the Encoding Standard's UTF-8
  decoder reads it, and the parse error `invalid-utf-8` (the library's own: the
  standard's input is code points). `input_wtf8` reads a surrogate encoded in
  three bytes as one code point, so that a string from UTF-16 with a lone
  surrogate in it is one U+FFFD and not three, which is what WPT's
  input-preprocessing table asks.
- **The tokenizer** (4.3.1 to 4.3.15): every token type, the escapes (one to six
  hex digits and one white space; zero, a surrogate and anything past U+10FFFF
  are U+FFFD; an escaped end of input is U+FFFD in a name and nothing in a
  string), the numbers (the value, the "integer" or "number" type flag, the sign
  character, and the spelling as written, kept as `repr`), unquoted urls and the
  remnants of a bad one, strings and bad strings, the non-ASCII ident code
  points of the draft's list (4.2), the `-->` check before an ident, and the
  unicode-range token, which is produced only where the draft produces it.
- **Every entry point** (5.4): parse a stylesheet (with its location), a
  stylesheet's contents, a list of declarations (at-rules read and discarded,
  as the draft now says: what a `style` attribute holds), a rule, a
  declaration, a component value, a list of component values (what a
  presentation attribute holds), a comma-separated list of component values,
  and the two "according to a CSS grammar" hooks, for a string and for values
  already parsed, which the value layer fills in. "Consume a block's contents"
  is an entry point too (gtext_css_parse_block_contents()): the draft removed
  the entry point, and the algorithm is the one inside every block.
- **Nesting** (5.5.3, 5.5.5): each item of a block is tried as a declaration
  and, if that returns nothing, read again as a qualified rule with `;` as its
  stop token; a `{}`-block is allowed only as the whole value of a non-custom
  property; a qualified rule whose prelude begins `--name:` is the custom
  property it looks like (nested) or is dropped with its block (top level); a
  qualified rule's block is materialized into its declarations and its child
  rules, the later runs of declarations as nested declarations rules; an
  at-rule's block is left as its items, because how they are materialized
  "depends on the rule".
- **Consume a declaration** (5.5.6): `!important` as the last two tokens that
  are not white space, case-insensitively; white space before and after the
  value removed; a custom property's **original text**, the source of its value
  tokens after preprocessing; and a `unicode-range` declaration's value read
  again from its source with unicode ranges allowed.
- **"Valid in the current context"**: the draft returns a rule or a declaration
  only if it is valid where it is, and leaves validity to the specifications
  above syntax. By default everything is valid. `GTEXT_CSS_Parse_Options::validity`
  takes the layer that knows: an invalid declaration in a block is then read
  again as a rule, an invalid nested qualified rule is the draft's "invalid rule
  error" that ends the run of declarations before it, and so on, each with the
  parse error `invalid-declaration-in-context` or `invalid-rule`.
- **An+B** (6.2, 9.1): gtext_css_anb_parse() over component values, by the
  grammar's productions on tokens (so an escaped digit is a digit), and
  gtext_css_anb_serialize().
- **The tree**: every rule, declaration and component value has an `id`, from 1
  in document order and a function of the tree alone, and a span (byte offset,
  end, line and column, the column in UTF-16 code units as the HTML module
  counts them). Nothing in the tree changes after the parse, so one tree may be
  read from any number of threads.
- **Parse errors**: named for the place the draft says "this is a parse error",
  each with its position, ordered by position, `max_errors` of them kept and the
  rest counted.

### Parse errors

| Code | Name | Where |
|---|---|---|
| 0 | `invalid-escape` | 4.3.1, a `\` that does not begin a valid escape |
| 1 | `eof-in-comment` | 4.3.2 |
| 2 | `eof-in-string` | 4.3.5 |
| 3 | `newline-in-string` | 4.3.5, a bad-string token |
| 4 | `eof-in-url` | 4.3.6 |
| 5 | `invalid-character-in-url` | 4.3.6, a quote, `(` or non-printable code point: a bad-url token |
| 6 | `invalid-escape-in-url` | 4.3.6 |
| 7 | `whitespace-in-url` | 4.3.6 has no "this is a parse error" for white space inside a url; 5.2 says a bad-url token always is one |
| 8 | `eof-in-escape` | 4.3.7 |
| 9 | `invalid-utf-8` | not the standard's |
| 32 | `eof-in-qualified-rule` | 5.5.3 |
| 33 | `semicolon-in-qualified-rule` | 5.5.3, the stop token of a nested qualified rule |
| 34 | `close-brace-in-qualified-rule` | 5.5.3 |
| 35 | `unmatched-close-brace` | 5.5.7, and 5.2: a `}` kept as a token |
| 36 | `unmatched-close-paren` | 5.2: a `)` kept as a token |
| 37 | `unmatched-close-bracket` | 5.2: a `]` kept as a token |
| 38 | `invalid-declaration` | 5.5.6, a declaration dropped by "consume the remnants of a bad declaration" |
| 39 | `at-rule-in-declaration-list` | 5.4.5, an at-rule read and discarded |
| 40 | `rule-looks-like-custom-property` | 5.5.3 |
| 41 | `invalid-rule` | the validity callback refused a rule |
| 42 | `invalid-declaration-in-context` | the validity callback refused a declaration |

A parse error is placed where the algorithm raises it: an `eof-in-*` error at
the end of the input, the others at the code point or token in question.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* CSS never fails on content: what is wrong is dropped, and each drop is a parse error. */
static void errors(void) {
  const char * css = "a { color: 'red\n; b c; } d e\n@media x { f: g }";
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, &result) == GTEXT_CSS_OK) {
    for (size_t i = 0; i < gtext_css_result_error_count(&result); i++) {
      GTEXT_CSS_Error e;
      if (gtext_css_result_error_at(&result, i, &e)) {
        printf("%zu:%zu (byte %zu) %s\n", e.line, e.column, e.byte_offset, e.name);
      }
    }
    printf("%zu rules\n", tree->rule_count);
    gtext_css_tree_free(tree);
  }
  gtext_css_result_free(&result);
}
```

prints

```
1:16 (byte 15) newline-in-string
2:6 (byte 21) semicolon-in-qualified-rule
2 rules
```

The newline ends the string as a bad-string token (still a value, so `color`
is a declaration); `b c;` is a nested qualified rule that met its stop token;
and `d e` swallows what follows it into its prelude up to the next `{` - the
`@media` is a token of the prelude there, not an at-rule - so the second rule
is `d e @media x` with the block `{ f: g }`. CSS recovers; it does not guess.

### A style attribute

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A style attribute is a list of declarations. A custom property keeps its original text. */
static void style_attribute(void) {
  const char * style = "color: RED ! Important; --gap: { 1px  2px } ; @media x { y: z } margin: 0";
  GTEXT_CSS_Tree * tree = NULL;

  if (gtext_css_parse_declaration_list(style, strlen(style), NULL, &tree, NULL) != GTEXT_CSS_OK) {
    return;
  }
  for (size_t i = 0; i < tree->declaration_count; i++) {
    const GTEXT_CSS_Declaration * d = &tree->declarations[i];
    char * value = NULL;
    size_t length = 0;
    if (gtext_css_serialize_values_buffer(d->value, d->value_count, NULL, &value, &length) == GTEXT_CSS_OK) {
      printf("%s = [%s]%s", d->name.data, value, d->important ? " important" : "");
      if (d->original_text.data) {
        printf(", written [%s]", d->original_text.data);
      }
      printf("\n");
      gtext_css_serialize_buffer_free(NULL, value);
    }
  }
  gtext_css_tree_free(tree);
}
```

prints

```
color = [RED] important
--gap = [{ 1px 2px }], written [{ 1px  2px }]
margin = [0]
```

The case of a value is kept: that `RED` is a color is for the value layer to
say. The custom property's value is tokens like any other, and its original
text is what was written.

### Validity from the layer above

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The layer that knows grammars says what is valid; the parse then recovers as the standard says. */
static bool known_property(void * user, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Declaration * d) {
  (void)user;
  (void)parent;
  return strcmp(d->name.data, "color") == 0;
}

static void validity(void) {
  const char * css = "a { color: red; colr: blue; color: green }";
  GTEXT_CSS_Parse_Options opts = gtext_css_parse_options_default();
  opts.validity.declaration = known_property;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet(css, strlen(css), &opts, &tree, &result) == GTEXT_CSS_OK) {
    const GTEXT_CSS_Rule * rule = &tree->rules[0];
    printf("%zu declarations kept, %zu parse errors\n", rule->declaration_count,
        gtext_css_result_error_count(&result));
    gtext_css_tree_free(tree);
  }
  gtext_css_result_free(&result);
}
```

prints

```
2 declarations kept, 2 parse errors
```

`colr: blue` is not a declaration the grammar accepts, so the block reads it
again as a qualified rule, which ends at its semicolon with no block and is
dropped: `invalid-declaration-in-context`, then `semicolon-in-qualified-rule`.

### An+B

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* An+B, as :nth-child() holds it: read from component values and written back. */
static void anb(void) {
  const char * inputs[] = {"odd", " -n + 3 ", "2N- 1", "+ 2n"};
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
    GTEXT_CSS_Tree * tree = NULL;
    if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
      continue;
    }
    int64_t a = 0, b = 0;
    char text[44];
    if (gtext_css_anb_parse(tree->values, tree->value_count, &a, &b)) {
      gtext_css_anb_serialize(a, b, text, sizeof text);
      printf("'%s' is A=%lld B=%lld, written %s\n", inputs[i], (long long)a, (long long)b, text);
    }
    else {
      printf("'%s' is not An+B\n", inputs[i]);
    }
    gtext_css_tree_free(tree);
  }
}
```

prints

```
'odd' is A=2 B=1, written 2n+1
' -n + 3 ' is A=-1 B=3, written -n+3
'2N- 1' is A=2 B=-1, written 2n-1
'+ 2n' is not An+B
```

## Stylesheets as bytes

A stylesheet arrives as bytes, and CSS Syntax 3.2 says how they become the code points the tokenizer reads.
gtext_css_parse_stylesheet_bytes() decodes and parses, and **every position it reports is in the caller's bytes**
whatever the encoding: the spans of the tree, the byte offsets of the parse errors and the place of a limit. The
encoding is, in order, the first that applies:

1. a **byte order mark** (UTF-8, UTF-16BE or UTF-16LE), over everything below ("decode");
2. the **protocol's label** (`GTEXT_CSS_Bytes_Options::protocol_label`), if it names an encoding; one that does not
   is ignored;
3. a leading **`@charset "...";`** in the first 1024 bytes, byte for byte: `@charset "`, bytes from 0x01 to 0x7F
   except the quote, then `";`. No second space, no single quotes, no comment, no `@Charset`. UTF-16 named there
   is UTF-8, because the declaration is ASCII; a label that names nothing is ignored;
4. the **environment encoding** (`environment_encoding`), if the caller has one;
5. UTF-8.

The result says which and why (`encoding`, `encoding_source`, `bom_length`), and gtext_css_determine_encoding()
makes the decision alone. UTF-8 is parsed in place, a byte order mark skipped but counted in every offset. Another
encoding is decoded through the Encoding module into a copy (up to three times the size of the bytes, so
`max_input_bytes` limits the bytes and not the text) and a map back to the bytes: a run per stretch of code points
that stay in step, an index per 4 KiB of text so a lookup costs a constant. An ill-formed sequence is U+FFFD with no
parse error of its own; in UTF-8 read in place it is `invalid-utf-8`. Lines and columns count the decoded text's
characters.

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A stylesheet's bytes: the encoding is found as CSS Syntax 3.2 says, and every position is in the bytes. */
static void bytes(void) {
  /* "@charset "windows-1251"; .привет { color: red }", in windows-1251 */
  const char css[] = "@charset \"windows-1251\"; .\xef\xf0\xe8\xe2\xe5\xf2 { color: red }";
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet_bytes(css, sizeof css - 1, NULL, NULL, &tree, &result) != GTEXT_CSS_OK) {
    gtext_css_result_free(&result);
    return;
  }
  const GTEXT_CSS_Rule * rule = &tree->rules[1];
  const GTEXT_CSS_Component_Value * name = &rule->prelude[1];
  printf("read as %s (%s)\n", gtext_encoding_name(result.encoding), gtext_css_encoding_source_name(result.encoding_source));
  printf("%s is %zu bytes of text and %zu of the stylesheet\n", name->value.data, name->value.length,
      name->span.end - name->span.offset);
  printf("the rule is bytes %zu to %zu of %zu\n", rule->span.offset, rule->span.end, result.input_bytes);
  gtext_css_tree_free(tree);
  gtext_css_result_free(&result);
}
```

prints

```
read as windows-1251 (charset-rule)
привет is 12 bytes of text and 6 of the stylesheet
the rule is bytes 25 to 47 of 47
```

The `@charset` is not a rule, and the tree has it anyway: the syntax layer reads the generic at-rule named
`charset`, and CSS Syntax 8.3's "dropped as invalid when the stylesheet is grammar-checked" is the validity
callback's to do (`testCssBytes` does it). A validity callback sees positions in the decoded text; the tree and
the result are in the bytes.

## Streams

A stylesheet that arrives in pieces goes to a stream. **The tree, the parse errors and the verdict do not depend on
where it was cut**: the tests cut every input at every byte, a byte at a time, in random pieces with empty feeds
between them, and in six legacy encodings, and compare every field (spans, identities, flags, serialization,
errors, encoding) with the one-call parse. CSS cannot be parsed before its end (the tokenizer matches brackets over
the whole input and a rule is not known until its block closes), so a stream **buffers**: the bytes, or for another
encoding than UTF-8 the decoded text and its map, bounded by `max_input_bytes`, which the feed that goes past it
meets with `GTEXT_CSS_E_LIMIT_INPUT` before keeping the piece. The buffers double, so the work is in proportion to
the bytes and never to the number of calls. The encoding is decided when the first kilobyte has arrived (or the
input ends), and a stream of text (gtext_css_stream_new()) has none to decide.

```c
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
```

prints

```
2 rules from 32 bytes; the second is bytes 17 to 32
success
input longer than max_input_bytes
```

## Limits

Every limit has a default; 0 selects it, and 0 never means "unlimited". A parse
that passes a limit fails with that limit's status and no tree, and the
result's `failure` holds the byte, line and column of what went past it. Each
is tested at its bound and one past it (`tests/test-css-limits.cpp`).

| Option | Default | Status | Counts |
|---|---|---|---|
| `max_input_bytes` | 64 MiB | `GTEXT_CSS_E_LIMIT_INPUT` | bytes of input |
| `max_tokens` | 4,194,304 | `GTEXT_CSS_E_LIMIT_TOKENS` | tokens, white space included, comments not |
| `max_depth` | 512 | `GTEXT_CSS_E_LIMIT_DEPTH` | nesting of blocks and functions, a rule's block included |
| `max_rules` | 1,048,576 | `GTEXT_CSS_E_LIMIT_RULES` | rules made, nested declarations rules and rules inside a dropped block included |
| `max_declarations` | 4,194,304 | `GTEXT_CSS_E_LIMIT_DECLARATIONS` | declarations made (one the validity callback refuses is not counted) |
| `max_total_errors` | 1,048,576 | `GTEXT_CSS_E_LIMIT_ERRORS` | parse errors raised |
| `max_errors` | 1024 | (none) | parse errors kept in the result; the rest are counted, which is not a failure. It does not bound memory: every error raised is held (about 40 bytes each, up to `max_total_errors`) until the parse ends, so that the lowest in the input are the ones kept |
| `max_output_bytes` (serializer) | 256 MiB | `GTEXT_CSS_E_LIMIT_OUTPUT` | bytes written; the sink has a valid prefix of the output: whole code points and whole escapes, up to the limit |

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A limit is its own status, with no tree, and says where it was reached. */
static void limits(void) {
  const char * css = "a { b { c {\n d { } } } }";
  GTEXT_CSS_Parse_Options opts = gtext_css_parse_options_default();
  opts.max_depth = 3;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  GTEXT_CSS_Status status = gtext_css_parse_stylesheet(css, strlen(css), &opts, &tree, &result);
  printf("%s at %zu:%zu, tree %s\n", gtext_css_status_string(status), result.failure.line,
      result.failure.column, tree ? "made" : "not made");
  gtext_css_result_free(&result);

  opts.max_depth = 4;
  status = gtext_css_parse_stylesheet(css, strlen(css), &opts, &tree, &result);
  printf("%s with depth 4\n", gtext_css_status_string(status));
  gtext_css_tree_free(tree);
  gtext_css_result_free(&result);
}
```

prints

```
blocks and functions nested deeper than max_depth at 2:4, tree not made
success with depth 4
```

**Nothing recurses.** The depth of a parse costs memory, which `max_depth`
bounds, never stack: a hundred thousand nested rules, or functions, parse,
are given their ids and are written back out under a raised limit
(`CssDepth` in the tests). The bracket structure of the input does not depend
on where rules begin and end, so the tokenizer matches the brackets and
measures the depth once, for every entry point.

**Nothing is quadratic.** Read literally, "consume a block's contents" is: each
item of a block is first read as a declaration, and read again as a rule when
that returns nothing, so the blocks inside a block inside a block are read once
for every block around them. The draft's implementation note gives the early
exits that avoid it; the parser decides whether "consume a declaration" would
return something by looking at the top level of the value only, stepping over
each block and function by the bracket matching, and stops at the first token
that settles it, and only a declaration that will be returned is built. The
quadratic suite (`make check-html-quadratic`, group `css`) holds 28 CSS shapes
to a linear bound on a work counter, among them every shape that would make the
literal reading quadratic, and `make check-html-quadratic-plants` plants five
quadratics in the CSS code and shows the suite fail on them.

## Save

```c
#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Write a tree back out: the text reads back as the same tree. */
static void serialize(void) {
  const char * css = "#\\31 23 > .a\\ b { width: calc( 1px+2PX ); content: '\\'q\\'' ; } /* gone */";
  GTEXT_CSS_Tree * tree = NULL;

  if (gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, NULL) != GTEXT_CSS_OK) {
    return;
  }
  char * text = NULL;
  size_t length = 0;
  if (gtext_css_serialize_tree_buffer(tree, NULL, &text, &length) == GTEXT_CSS_OK) {
    printf("%s\n", text);
    gtext_css_serialize_buffer_free(NULL, text);
  }
  GTEXT_CSS_Serialize_Options opts = gtext_css_serialize_options_default();
  opts.numbers = GTEXT_CSS_NUMBERS_CSSOM;
  const GTEXT_CSS_Declaration * width = &tree->rules[0].declarations[0];
  if (gtext_css_serialize_values_buffer(width->value, width->value_count, &opts, &text, &length) == GTEXT_CSS_OK) {
    printf("%s\n", text);
    gtext_css_serialize_buffer_free(&opts, text);
  }
  gtext_css_tree_free(tree);
}
```

prints

```
#\31 23 > .a\ b {width:calc( 1px/**/+2PX );content:"'q'";}
calc( 1px/**/2PX )
```

CSS Syntax 9 asks one thing of a serializer, that parsing its output gives the
same data structures, "except for consecutive whitespace-tokens, which may be
collapsed into a single token". The serializer is held to that over every vector
and every fuzz input (the canonical form that "the same" means is
`tools/conformance/css_tree_canon.h`: every field but the spans, the ids, a
custom property's original text and whether the end of the input closed a
token). What it writes:

- tokens as CSSOM spells them: an identifier (an ident, a function's name, an
  at-keyword, an id hash, a dimension's unit) by "serialize an identifier", a
  string by "serialize a string", always in double quotes;
- whitespace as one space, and two whitespace tokens in a row (a comment was
  between them) as one, so a second serialization is the same text;
- an empty comment between two tokens that would run together, where the table
  of section 9 says so;
- a `<delim-token>` of U+005C as U+005C and a newline (section 9);
- numbers as they were written (`GTEXT_CSS_NUMBERS_SOURCE`, the default, which
  keeps the type flag and the sign), or as CSSOM serializes a `<number>`
  (`GTEXT_CSS_NUMBERS_CSSOM`: base ten, at most six decimals, no exponent), in
  which case the text may read back as a different token (`1.0` is written `1`,
  an integer) and the round trip is not claimed;
- rules and declarations compactly: a qualified rule as its prelude, `{`, its
  declarations each ended by `;`, its child rules, `}`; a statement at-rule
  ended by `;`; one space before `!important`.

It never writes a comment of its own other than the empty one, and never
writes white space that was not a token, but for the space before
`!important`.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|-----------------------|
| Preprocessing (3.3) | CR, CR LF, FF, U+0000, surrogates (WTF-8 on request) | none |
| Bytes (3.2) | the byte order mark, the protocol's label, `@charset` in the first 1024 bytes, the environment encoding, UTF-8; positions in the bytes | `@charset` is read from the bytes and is the generic at-rule in the tree (8.3: dropped at the grammar check, which is the validity callback's) |
| Incremental parsing | the same tree, errors and verdict at every cut, bounded buffering, work in proportion to the bytes | a stream buffers the stylesheet and parses at the end: CSS cannot be parsed before it |
| Tokens (4) | all 25 types; escapes; numbers with value, type, sign and spelling | none |
| Unicode-range tokens (4.3.14) | in a `unicode-range` declaration's value | elsewhere `u+1` is ident, number: the draft's |
| Entry points (5.4) | all nine of the draft and the CRD's "parse a block's contents"; the grammar hooks over text and over values | the "unspecified structure" of a match is the grammar's to keep |
| Nesting (5.5) | interleaved items, nested declarations rules, the stop token, the custom-property rules | none |
| Validity | a callback for declarations and for rules | by default everything is valid: the grammars are above syntax |
| An+B (6) | every production; 9.1 | A and B are read exactly; their magnitudes saturate at INT64_MAX (INT64_MIN is never produced). The serializer follows the snprintf convention |
| Serialization (9) | the round trip; CSSOM's idioms; the table of section 9 and the pairs it misses | CSSOM's "serialize a URL" and function-name lowercasing are not used (below) |
| Diagnostics | 21 parse errors, positions, order, the cap and the count | names are the library's: the draft names none |
| Limits | seven, each its own status, each exact | none |

## Deviations

None of these is a departure from the draft's text; each is a place where the
text leaves a choice, or where following CSSOM's idiom would break the round
trip the draft requires. Each has a test.

| Case | This parser | Why, and the test |
|---|---|---|
| The value of a number | the correctly rounded double of the decimal as written, by the `math` library's `gmath_decimal_parse` (ties to even, any number of digits, no locale, no rounding mode of the host), with `GTEXT_CSS_FLAG_NUMBER_{INEXACT,OVERFLOW,UNDERFLOW}` on the token | 4.3.13 computes it as "number part" times ten to the "exponent part"; done in floating point that is two roundings. Read as exact arithmetic. `testCssNumbers` holds 427 rows (the halfway points between doubles among them) against CPython's `float()` under every rounding mode |
| `1e999` | infinity (flag OVERFLOW), `1e-999` zero (flag UNDERFLOW) | the draft gives no range; the value layer clamps. `AnExponentPastTheRangeOfADoubleIsInfiniteOrZero`, `testCssNumbers` |
| A number written for CSSOM (`GTEXT_CSS_NUMBERS_CSSOM`) | the shortest digits that round back (`gmath_decimal_shortest`), in fixed notation, rounded half away from zero to six decimals on those digits; a NaN is `0` and an infinity the largest double | CSSOM 6.7.2 gives the layout ("shortest form possible", "more than 6 decimals" rounded, "scientific notation is not used") and says nothing of a tie, a NaN or an infinity. `CssNumbers.CssomWritesTheShortestDigits...` |
| A validity callback in a byte parse | sees positions in the decoded text | the callbacks run during the parse; the tree and the result are in the bytes (equal for UTF-8 with no byte order mark) |
| `E_LIMIT_INPUT` in a byte parse | `failure` has the byte offset and a line and column of 0 | nothing was decoded to count them in |
| The value of `unicode-range: U+0 !important` | `U+0`, white space, `!`, `important`, and the important flag | 5.5.6 reads the value again from "the segment of the original source text string corresponding to the tokens returned by the consume a list of component values call", which is the list before `!important` and white space are taken off. A descriptor takes no `!important`, so only an invalid declaration is affected. `AUnicodeRangeValueIsItsSourceReadAgainAsTheDraftWritesIt` |
| A url token | written unquoted, its stops escaped | CSSOM's "serialize a URL" writes `url("...")`, which reads back as a function with a string in it. `AUrlIsWrittenUnquotedAndABadUrlStaysBad` |
| A function's name | written as it came (`RGB(1)`) | CSSOM's "serialize a function" lowercases it; a token sequence keeps it. `AFunctionKeepsTheCaseOfItsName` |
| An unrestricted hash (`#0a`) | its name is not escaped at the start | "serialize an identifier" would write `#\30 a`, which reads back as an id hash. Section 9 allows it: an unrestricted hash "may not need as much escaping". |
| A dimension's unit `e5` | written `\65 5` | section 9: the unit "may need escaping to disambiguate with scientific notation" |
| Pairs the table of section 9 does not list | separated too: `<` then `!` (with `--` they are a CDO token), `u` then `+`, and a unicode-range token then what it would take (hex digits, `?`, `-`, a name) | the table is "to satisfy this requirement", and these break it. `PairsTheTableDoesNotListAreSeparatedToo` |
| A bad-string token and a bad-url token | written `"` and a newline, and `url(()` | each reads back as that token; the values they had are not kept, as the draft keeps none |
| A nested qualified rule whose prelude is `name:` and white space, last in its block | followed by `!` | written alone it would read back as the declaration `name:{...}`; the `!` makes that value two things, so the declaration fails and the rule stands, and the `!` is then dropped. Nothing is added to the tree. `ANestedRuleThatWouldReadAsADeclarationGetsAFillerThatIsDropped` |
| An empty last group of a comma-separated list | written with its comma (`a,,`) | the list `a,` has one group; `a,,` has two |
| `max_rules` | counts every rule the algorithms return, a rule inside a block that is then dropped included | the bound is on work as much as on the tree |
| Parse errors | ordered by position, whichever stage raised them; the first `max_errors` by position are kept | the stages run one after the other, so the order they were raised in is not the input's |

Where css-parsing-tests and WPT disagree with the draft, the vector gate lists
the case with the draft's answer and the sentence that decides it
(`tools/conformance/CSS_SYNTAX_EXCEPTIONS`): css-parsing-tests was written for
the Candidate Recommendation Draft of 2021, whose model had no nested
declarations and kept leading and trailing white space in a declaration's
value, CSS 2.1's match tokens, and unicode-range tokens everywhere.

## Tested scope

- **css-parsing-tests**, pinned by commit (`tools/css/CSS_PARSING_TESTS_COMMIT`,
  fetched by `tools/css/fetch.sh`; CC0): its nine files for syntax and An+B,
  through the entry point each names. Its color files are the value layer's,
  and `stylesheet_bytes.json` is scored through the byte decode (`gtext_css_parse_stylesheet_bytes()`, the encoding it names in the result); the gate prints
  how many cases each leaves unscored and fails on a file of the pin it does not
  name. The suite's representation is the 2021 model's: a rule's block is
  compared as the component values of its text, and its `["error",
  "invalid"]` markers for dropped constructs are removed from the expectation;
  nothing else is normalized.
- **web-platform-tests `css/css-syntax`**, pinned by commit
  (`tools/css/WPT_COMMIT`, the HTML pin's commit): `tools/css/extract-wpt.py`
  turns the tables of thirteen files into vectors (An+B parsing and
  serialization, decimal points, input preprocessing, the non-ASCII ident code
  points, escaped end of input, identifier escaping in a round trip, whitespace
  trimming, and the nesting files' structure), each translated to what syntax
  must do for the browser assertion to hold and nothing more; the extractor says
  how, file by file. `non-ascii-codepoints.html` has a missing comma in its
  range list that makes the harness throw in a browser before any invalid code
  point is tested; the extractor reads the ranges as written, and one of the
  invalid code points it then tests (U+D800, a lone surrogate) is a valid one by
  the draft's preprocessing, which the exceptions record.
- **The gate** (`make conformance-css-syntax`; scoring in `make test` through
  `check-conformance-vectors`): every case passes or is listed with the draft's
  answer, which it must then give exactly; every case round-trips; a stale or
  unknown entry, an entry without a quoted sentence, an unscored file and a
  crashing runner each fail it; and there is a floor. `--controls` shows it fail
  on purpose 26 ways: vectors that lie, exceptions missing, stale, about
  nothing, without a quote or with the wrong answer, the floor, the runner's
  refusals, a crash, and fifteen rules of the draft undone one at a time in a
  copy of the library.
- **What the vectors do not reach**, found by planting: no vector escapes a
  surrogate, and none has a position (line, column, offset) in it. The unit tests
  hold those, and the validity seam, which no vector can reach because the
  vectors have no grammar.
- **Unit tests**: `testCssTokenizer`, `testCssParser`, `testCssSerialize`,
  `testCssLimits` (each limit at its bound and one past it, and a hundred
  thousand levels of nesting), `testCssOom` (every allocation of every entry
  point failed in turn: E_OOM with no tree or the same answer, and nothing
  left), `testCssDocExamples` (the snippets of this page, compiled, run and held
  to the page byte for byte), and a test of eight threads parsing and reading one
  tree at once, which `make test-tsan` runs.
- **The fuzzer** `fuzz_css_syntax` (`make fuzz-run-css-syntax`): every entry
  point, with tiny limits, a validity callback, WTF-8 and failed allocations
  chosen by the input, held to a well-formed tree, the round trip, every limit
  exact at what the parse counted, the failed allocation's answer, linear work,
  two implementations of the comma-separated split agreeing, and An+B read back.
  Its seeds (`tests/fuzz/corpus/css-syntax`, `tools/css/make-fuzz-seeds.py`) are
  replayed by `make test`; `make check-css-fuzz-plants` plants eleven defects and
  shows it find each.

- **Byte decode and incremental parser** (`testCssBytes`, `testCssStream`,
  `testCssNumbers`, `testCssBytesFuzzSeeds`): every position is checked against
  the caller's bytes through the BOM, the `@charset` sequence, the transport and
  environment encodings and the legacy encodings; the incremental parser is held
  to the one-shot parse at every cut of every input, and its work to be linear
  in the number of feeds. `fuzz_css_bytes` (`make fuzz-run-css-bytes`) adds the
  bytes and the cut points to the fuzzer's inputs; `make check-css-fuzz-plants`
  plants seven defects of the offset map, the stream and the limits besides the
  eleven of the syntax fuzzer, and shows it find each.
- **Numbers** (`make check-css-numbers`): the tokenizer reads a number with
  `gmath_decimal_parse` (correctly rounded, so `1e-400`, `5e-324` and a
  two-hundred-digit literal have one answer), the serializer writes the shortest
  decimal that reads back as the same double, and the CSSOM form
  (`GTEXT_CSS_NUMBERS_CSSOM`) rounds half away from zero to six decimals. The
  gate fails if the source refers to a number routine of the standard library or
  to a private stand-in, and is shown to fail by planting each.
- **Differentials** (`make check-css-oracle`; `tools/oracle/run-css-oracle.sh`,
  in containers pinned by digest): tinycss2, cssparser and csstree each parse
  generated inputs, the css-parsing-tests and WPT inputs, and this library
  parses the same, for the tokenizer, the stylesheet, declarations, blocks, the
  top-level rule list and, for tinycss2, the bytes. The answers are normalized
  to one shape and compared. Every divergence must fall in a *class*, and a class
  is an experiment: an input transformation that takes one cause out (the
  references' CSS 2.1 match tokens, their reading of every non-ASCII code point as
  an ident code point, unicode-range tokens everywhere, and so on), after which the two
  must agree. `tools/oracle/css_divergences.txt` lists one line per (mode,
  reference, class) with a count, the shortest witness, and the reason with the
  sentence of the draft that decides it; a divergence in no class, a class with no
  divergence, a count that moved and a reason that is not the class's each fail.
  A class found only by a hunt with another seed is listed with a count of 0 and
  held to 0 in the default run. `--controls` shows the gate fail on purpose:
  library defects planted across the decode, the stream, the parser and the
  serializer, each seen caught: a divergence that the unplanted library does not have is never excused by a class (an experiment that drops the non-ASCII text drops a byte order mark with it, and would otherwise explain away a defect in the BOM rule). The
  references agree with each other on less than they agree with the draft; where
  all three differ from this library the draft decides, and the reason says so.
  A differential proves agreement with other implementations of the same
  specification, which is evidence and not proof; the parsers it compares are
  older drafts, and the classes record exactly where.

## Not implemented

- **Values, properties, the cascade, selectors, at-rule semantics, CSSOM**:
  the layers after it. The validity callback and the grammar hooks are where they
  attach; the tree's identities and spans are what CSSOM and invalidation will
  key on.

---
Back to \ref text_format_references "Formats".
