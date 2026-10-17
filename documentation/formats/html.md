@page format_html HTML

# HTML

`gtext_html_parse()` reads a whole **HTML document** the way the WHATWG HTML Living
Standard says a browser does, and builds a \ref format_markup "Markup tree" from it;
`gtext_html_parse_fragment()` does what `innerHTML` does, in a context element you
describe; `GTEXT_HTML_Parser` takes the document in pieces as it arrives; and
`gtext_html_parse_file()` reads a file; and `gtext_html_serialize()` writes a tree back as HTML (the standard's
"serializing HTML fragments" algorithm, \ref format_html "see Writing HTML" below). HTML has no malformed documents: **every input has
a tree**, and the mistakes in it are *parse errors*, listed in the result with the
standard's names and positions, which are not failures. A parse fails only for an
argument, a limit, or memory (an encoding label the library does not know is ignored, and the result says so). Back to
\ref text_format_references "Formats".

The API has the shape of the \ref format_xml "XML" one (an options struct with a
`_default()`, a status enum with a distinct status for every limit, a whole-input parse, a
file form, an incremental parser, the same allocator and file layer), and
**what differs is listed under "How this differs from the XML API"**. The tokenizer and
the tree builder under it are described, with their numbers, in the second half of this page
(\ref format_html_internals "Internals").

Include `<ghoti.io/text/html.h>`, which brings in the markup tree's headers too.

## Quick start

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Parse a document, say how it was read, and print its title. */
static void quick_start(void) {
  const char * text = "<!DOCTYPE html><title>Hi</title><p class=a>one<p>two</b>";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  if (gtext_html_parse(text, strlen(text), NULL, &doc, &result) != GTEXT_HTML_OK) {
    fprintf(stderr, "not parsed: %s\n", result.message);
    gtext_html_result_free(&result);
    return;
  }

  const char * mode = result.mode == GTEXT_MARKUP_MODE_NO_QUIRKS ? "no-quirks"
      : result.mode == GTEXT_MARKUP_MODE_LIMITED_QUIRKS ? "limited-quirks" : "quirks";
  printf("%s mode, %zu parse errors, read as %s\n", mode,
      gtext_html_result_error_count(&result), result.encoding);

  /* The title is the text of the first element of that name. */
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
  for (const GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n;
       n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
        && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "title")) {
      char * title = NULL;
      if (gtext_markup_node_text_content(n, NULL, &title, NULL) == GTEXT_MARKUP_OK) {
        printf("title: %s\n", title);
        gtext_markup_string_free(NULL, title);
      }
      break;
    }
  }

  gtext_html_result_free(&result);   /* the result and the document are separate */
  gtext_markup_document_free(doc);
}
```

`gtext_html_parse(data, len, opts, &doc, &result)` returns `GTEXT_HTML_OK` and a document the
caller frees with `gtext_markup_document_free()`, or a status and no document; `opts` NULL is
the defaults, `data` need not be NUL-terminated and is not kept, and `result` may be NULL if
you do not want the errors. The **result** is a separate object, filled for failures as well
as successes and freed with `gtext_html_result_free()`; freeing it does not touch the
document, and the other way round, in either order.

## Five ways in

| You have | Call | Notes |
|---|---|---|
| a buffer | `gtext_html_parse()` | the document is the tree of the standard's "parse a document" |
| a buffer that is the inside of an element | `gtext_html_parse_fragment()` | the context element is described by value (`GTEXT_HTML_Fragment_Context`); the result is a fragment node owned by a scratch document |
| bytes arriving in pieces | `gtext_html_parser_new()`, `_feed()`, `_finish()`, `_free()` | the tree and the errors are identical however the bytes are cut |
| pieces of a fragment | `gtext_html_parser_new_fragment()`, `_feed()`, `_finish_fragment()`, `_free()` | the same, for a fragment |
| a path | `gtext_html_parse_file()` | read through the library's file layer (a pipe works; the size limit applies while reading) |

### In pieces

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Feed a document as it arrives. The tree is the same however the bytes are cut. */
static void incremental(void) {
  const char * pieces[] = {"<!DOCTYPE html><ti", "tle>Caf\xc3", "\xa9</title><p>a&am", "p;b"};
  GTEXT_HTML_Parser * parser = NULL;
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  if (gtext_html_parser_new(NULL, &parser) != GTEXT_HTML_OK) {
    return;
  }
  for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
    GTEXT_HTML_Status status = gtext_html_parser_feed(parser, pieces[i], strlen(pieces[i]));
    if (status != GTEXT_HTML_OK) {
      printf("stopped: %s\n", gtext_html_status_string(status));
      break;                              /* every later call says the same */
    }
  }
  if (gtext_html_parser_finish(parser, &doc, &result) == GTEXT_HTML_OK) {
    printf("%zu nodes, %zu bytes\n", result.node_count, result.input_bytes);
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);
  gtext_html_parser_free(parser);        /* after the document, or before: it makes no difference */
}
```

`feed` builds as much of the tree as the bytes so far decide, and the bytes are not kept past the
call. A character, a tag, a character reference, or the byte order mark may be cut between
calls; **a cut never changes the answer** (tested at every split offset, a byte at a time and
in random pieces, on every vector and in the fuzzer). A run of text between tags is held
until something that is not text ends it, so `max_text_bytes` bounds the memory of that;
the tokens are bounded by their own limits and the tree by `max_nodes`. Once a call has
failed the parser is finished and every later call returns the same status; after a
successful `finish` a second `finish` or a `feed` is `GTEXT_HTML_E_STATE`, and the document
stays with the first caller. A `feed` the caller got wrong (a NULL buffer with a length) is
`GTEXT_HTML_E_INVALID` and does not end the parser.

### Fragments

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* What `td.innerHTML = "<p>x</p>y"` builds, and moving it into another document. */
static void fragment(void) {
  GTEXT_HTML_Fragment_Context context;
  memset(&context, 0, sizeof(context));
  context.ns = GTEXT_HTML_NS_HTML;
  context.local_name = "td";

  const char * text = "<p>x</p>y";
  GTEXT_Markup_Document * scratch = NULL;     /* owns the fragment */
  GTEXT_Markup_Node * fragment = NULL;
  if (gtext_html_parse_fragment(&context, text, strlen(text), NULL, &scratch, &fragment, NULL)
      != GTEXT_HTML_OK) {
    return;
  }
  printf("%zu children\n", gtext_markup_node_child_count(fragment));

  /* A page of your own to put it in. */
  const char * html = "<table><tr><td id=t>";
  GTEXT_Markup_Document * page = NULL;
  if (gtext_html_parse(html, strlen(html), NULL, &page, NULL) == GTEXT_HTML_OK) {
    const GTEXT_Markup_Node * root = gtext_markup_document_root(page);
    GTEXT_Markup_Node * cell = NULL;
    for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n && !cell;
         n = gtext_markup_node_next_in_order(root, n, false)) {
      if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
          && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "td")) {
        cell = n;
      }
    }
    GTEXT_Markup_Node * copy = NULL;
    /* Clone the fragment into the page, then insert the copy: a fragment inserts its children. */
    if (cell && gtext_markup_clone(page, fragment, true, &copy) == GTEXT_MARKUP_OK
        && gtext_markup_append_child(cell, copy) == GTEXT_MARKUP_OK) {
      printf("the cell now has %zu children\n", gtext_markup_node_child_count(cell));
    }
    gtext_markup_document_free(page);
  }
  gtext_markup_document_free(scratch);       /* and the fragment is gone with it */
}
```

The context element decides how the input is read (`td` takes cells' contents, `select`
takes options, `textarea` takes text, `svg` takes SVG), and is described by its namespace,
local name (in the exact case the element has, `foreignObject`), its attributes (only a MathML
`annotation-xml` reads them: its `encoding` makes it an HTML integration point), whether it
is or is inside a `form`, and the mode of its document. **The fragment belongs to the
scratch document that is returned with it**: freeing the document frees the fragment, and
the fragment must not be used afterwards. To keep the contents, clone the fragment into a
document of your own (`gtext_markup_clone()` with `deep` true, then insert the copy, which inserts
its children), as above. The input is characters already decoded, as `innerHTML`'s is, so a
leading U+FEFF is text and not a byte order mark. `max_open_elements` counts the scratch
document's `html` root, which is always open, so a fragment has room for one more level than a document with the
same limit has (which also has `body`).

### Files

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>

/* Parse a file. A path that cannot be read is an ordinary failure with a message. */
static void file(void) {
  const char * path = "doc-example.html";
  FILE * out = fopen(path, "wb");          /* something to read */
  if (!out) {
    return;
  }
  fputs("<!DOCTYPE html><title>t</title><p>hello\n", out);
  fclose(out);

  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.max_input_bytes = 1 << 20;         /* refuse a file over a megabyte while it is read */

  GTEXT_HTML_Status status = gtext_html_parse_file(path, &opts, &doc, &result);
  if (status == GTEXT_HTML_OK) {
    printf("read %zu bytes\n", result.input_bytes);
    gtext_markup_document_free(doc);
  }
  else {
    printf("%s: %s\n", gtext_html_status_string(status), result.message);
  }
  gtext_html_result_free(&result);

  remove(path);
  status = gtext_html_parse_file(path, NULL, &doc, &result);   /* gone now */
  printf("%s: %s\n", gtext_html_status_string(status), result.message);
  gtext_html_result_free(&result);
}
```

A path that cannot be opened or read is `GTEXT_HTML_E_INVALID` with a message that says
which, as `gtext_xml_parse_file()` and the CSV and JSON file functions say it: "No such file or
directory", "Permission denied opening file", "Failed to open file", "Failed to read file" (a
directory opens and then will not read). A file longer than `max_input_bytes` is
`GTEXT_HTML_E_LIMIT_INPUT`, refused while it is read, and a failed allocation is
`GTEXT_HTML_E_OOM`. The byte offsets in the parse errors are offsets into the file.

## Options and limits

`GTEXT_HTML_Parse_Options` is zeroed to mean the defaults, `gtext_html_parse_options_default()`
spells them out, and **0 in a limit selects its default and never means "unlimited"**
(`SIZE_MAX` is as close as the type allows). New fields are only ever appended.

| Field | Default | What it does when it is reached |
|---|---|---|
| `allocator` | the default allocator | every allocation of the parse, the document and the result goes through it; its `ctx` must outlive them |
| `max_input_bytes` | 64 MiB | `GTEXT_HTML_E_LIMIT_INPUT`; a whole input over it is refused before a byte is read, a byte order mark counts |
| `max_open_elements` | 512 | `GTEXT_HTML_E_LIMIT_DEPTH`: the stack of open elements, `html` and `body` included (510 `div`s fit under the defaults); this is the parser's depth limit, and what bounds the work |
| `max_nodes` | 10,000,000 | `GTEXT_HTML_E_LIMIT_NODES`, the document node included |
| `max_name_bytes` | 8192 | `GTEXT_HTML_E_LIMIT_NAME`: a tag, attribute or doctype name, a processing instruction target |
| `max_attribute_bytes` | 16 MiB | `GTEXT_HTML_E_LIMIT_VALUE`: an attribute value (after references are decoded) or a doctype identifier |
| `max_comment_bytes` | 16 MiB | `GTEXT_HTML_E_LIMIT_COMMENT`: a comment, a processing instruction's data |
| `max_text_bytes` | 64 MiB | `GTEXT_HTML_E_LIMIT_TEXT`: one run of characters between tags |
| `max_attributes` | 1024 | `GTEXT_HTML_E_LIMIT_ATTRIBUTES`: on one tag |
| `max_errors` | 1024 | not a failure: the first ones are kept, the rest counted |
| `count_errors_only` | false | keep none, count all (`max_errors` cannot say 0) |
| `scripting` | false | the standard's scripting flag: with it `noscript` is raw text, without it elements |
| `transport_encoding`, `transport_encoding_certainty` | NULL, certain | a label from a `Content-Type`, or a guess of the caller's (`GTEXT_HTML_TENTATIVE`: a `<meta>` overrides it); a label that names no encoding is ignored and `transport_encoding_ignored` says so |
| `default_encoding` | NULL (UTF-8) | the label to assume when nothing else says; one that names no encoding is ignored (`default_encoding_ignored`) |
| `ignore_meta` | false | read no declaration: no `<meta>` prescan, no `<?xml` declaration, and the parser never starts again on a `<meta>` |
| `max_reparse_bytes` | 0 (4 MiB) | the most an incremental parser keeps for a restart while the head is open (never less than 1024); past it `reparse_abandoned` and a later `<meta>` is an ordinary element; SIZE_MAX keeps without limit; `gtext_html_parse()` keeps nothing and ignores it |

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Limits keep untrusted input in bounds. Hitting one is a failure with a status of its own. */
static void limits(void) {
  const char * text = "<div><div><div><div><div>deep";
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  opts.max_open_elements = 4;            /* html, body and two more */
  GTEXT_HTML_Status status = gtext_html_parse(text, strlen(text), &opts, &doc, &result);
  if (status == GTEXT_HTML_E_LIMIT_DEPTH) {
    printf("refused: %s\n", result.message);   /* and there is no document to free */
  }
  gtext_html_result_free(&result);

  opts.max_open_elements = 0;            /* 0 is the default, 512; it is never "no limit" */
  status = gtext_html_parse(text, strlen(text), &opts, &doc, &result);
  if (status == GTEXT_HTML_OK) {
    printf("accepted under the default limit\n");
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);
}
```

Each limit is a status of its own, because a document refused for being too deep is not one
that is malformed. Under a limit that is not reached the tree is the unlimited parse's, and the
limits are exact: a limit of `n` admits `n` and refuses `n + 1` (tested for all eight, in every
entry point). The only one that depends on how the input is cut is `max_input_bytes`, which
is on what has been fed: a failed parse counts the bytes it got to.

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The scripting flag decides how <noscript> is read. Nothing is ever executed. */
static void scripting(void) {
  const char * text = "<body><noscript><p>enable scripts</p></noscript>";
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();

  for (int flag = 0; flag < 2; flag++) {
    opts.scripting = flag != 0;
    GTEXT_Markup_Document * doc = NULL;
    if (gtext_html_parse(text, strlen(text), &opts, &doc, NULL) != GTEXT_HTML_OK) {
      continue;
    }
    const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
    for (const GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n;
         n = gtext_markup_node_next_in_order(root, n, false)) {
      if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
          && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "noscript")) {
        const GTEXT_Markup_Node * child = gtext_markup_node_first_child(n);
        if (gtext_markup_node_kind(child) == GTEXT_MARKUP_KIND_TEXT) {
          printf("scripting %d: the noscript holds the text \"%s\"\n", flag, gtext_markup_node_data(child, NULL));
        }
        else {
          printf("scripting %d: the noscript holds a <%s> element\n", flag, gtext_markup_element_local_name(child));
        }
      }
    }
    gtext_markup_document_free(doc);
  }
}
```

## The result

`GTEXT_HTML_Result` holds, after every call:

| Field | |
|---|---|
| `status`, `message` | the call's status and a static description of it (for a file, which way it could not be read) |
| `mode` | the document mode, `GTEXT_MARKUP_MODE_NO_QUIRKS`, `_LIMITED_QUIRKS` or `_QUIRKS`, also set on the document (`gtext_markup_document_mode()`); for a fragment, its context's |
| `encoding`, `encoding_source` | the encoding's name by the Encoding standard ("UTF-8", "Shift_JIS"...), and how that was decided: `GTEXT_MARKUP_ENCODING_BOM`, `_CALLER` (a certain label), `_DECLARATION` (a `<meta>` or an XML declaration), `_CALLER_DEFAULT` (a tentative label, or `default_encoding`) or `_DEFAULT` (UTF-8, nothing said) |
| `encoding_confidence`, `reparsed`, `reparse_abandoned`, `bom_length` | `GTEXT_HTML_CERTAIN`, `_TENTATIVE` (nothing in the document said so, and no `<meta>` confirmed it), `_IRRELEVANT` (a fragment) or `_UNDECIDED` (`encoding` is NULL: the call failed before there was a parser; zero is `_CERTAIN`, so test `encoding` first); whether a `<meta>` made the parser start again with another encoding; whether the incremental parser let go of the bytes it keeps for that (`max_reparse_bytes`); the bytes of the byte order mark |
| `transport_encoding_ignored`, `default_encoding_ignored` | the label was given and names no encoding |
| `input_bytes` | bytes read (a failed parse: as far as it got) |
| `node_count` | nodes in the document, 0 when there is none |
| the errors | `gtext_html_result_error_count()`, `_errors_dropped()`, `_error_at()` |

On a failure the result still has the status, the message, the parse errors
raised before the failure (best effort: if the memory for the list is the thing that failed
it is not there), `input_bytes` (the bytes accepted before the one that failed: 0 for a whole
input refused for its length) and, once the encoding was decided, the encoding and how it was decided (UTF-8 and `DEFAULT` if the input ended before enough of it had arrived to decide); a failure before there was a parser (an
argument, OOM making it, a file that could not be read) has `encoding` NULL.
The document is not written.

**Reuse.** A call overwrites the result without freeing what it held, because it cannot tell
a live result from uninitialized memory: zero a result before its first use, and free it before passing it to another call.

### Parse errors

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A document that is wrong in three ways still has a tree. The mistakes are in the result. */
static void errors(void) {
  const char * text = "<p>one<b>two</p>\n</i><p a=1 a=2>";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.max_errors = 4;                   /* keep the first four, count the rest */

  if (gtext_html_parse(text, strlen(text), &opts, &doc, &result) == GTEXT_HTML_OK) {
    for (size_t i = 0; i < gtext_html_result_error_count(&result); i++) {
      GTEXT_HTML_Error e;
      if (gtext_html_result_error_at(&result, i, &e)) {
        printf("%zu:%zu (byte %zu) %s\n", e.line, e.column, e.byte_offset, e.name);
      }
    }
    printf("and %zu more\n", gtext_html_result_errors_dropped(&result));
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);
}
```

Each is a `GTEXT_HTML_Error`: a `code` (a number that does not change between releases), its
`name`, `line`, `column` and `byte_offset`. The names are the standard's (`duplicate-attribute`,
`end-tag-with-attributes`); there are 69 codes, no two share a name, and
`gtext_html_error_name()` maps a code to its name, `"unknown"` for a number that is not one.
Numbers 0 to 52 are the tokenizer's list (52 is `invalid-utf-8-in-input-stream`, which the standard leaves
unnamed; 41, `non-void-html-element-start-tag-with-trailing-solidus`, is raised by the tree builder), 64 to 79 are the tree builder's; the standard says only "this is a parse error" in the tree
builder's places, so those are named for what happened (`unexpected-end-tag`,
`misnested-formatting`, `html-in-foreign-content`...). The tree builder's reading of a U+0000
is `unexpected-null-character-in-content`, so that it is not the tokenizer's
`unexpected-null-character`. Positions: lines start at 1 and end at LF, CR or CR LF; **columns
count UTF-16 code units** (a character outside the Basic Multilingual Plane is 2), as the
test suites of the standard and a DOM script reckon them; `byte_offset` is 0-based into the bytes given, a
byte order mark counted.

With `max_errors` the first ones are kept in the order raised and the rest are only counted:
`gtext_html_result_error_count() + gtext_html_result_errors_dropped()` is every error whatever the
cap. Parse errors are not a conformance verdict about a page: the tree builder also raises them where the standard says
"parse error" and a browser silently repairs.

### Statuses

| Status | When |
|---|---|
| `GTEXT_HTML_OK` | success |
| `GTEXT_HTML_E_INVALID` | an unusable argument (NULL output, NULL data with a length, an unusable context), or a file that could not be opened or read |
| `GTEXT_HTML_E_OOM` | out of memory: no document, nothing leaked |
| `GTEXT_HTML_E_STATE` | the incremental parser: input after the end, a second finish, the wrong finish for its kind |
| `GTEXT_HTML_E_UNSUPPORTED_ENCODING` | the serializer's `encoding` is a label that names no encoding (the parsers ignore such a label and say so in the result) |
| `GTEXT_HTML_E_UNREPRESENTABLE` | the serializer was asked for an encoding that cannot write a character where a numeric reference would not be read as one (a name, a comment, raw text, a document type); `unrepresentable_code_point` says which |
| `GTEXT_HTML_E_INTERNAL` | a defect in this library (a rule did not terminate); never content |
| `GTEXT_HTML_E_LIMIT_INPUT`, `_DEPTH`, `_NODES`, `_NAME`, `_VALUE`, `_COMMENT`, `_TEXT`, `_ATTRIBUTES` | the limits above |

`gtext_html_status_string()` gives each a name of its own.

## Document mode

The mode comes from the doctype (or its absence) by the standard's table: no doctype is quirks mode,
`<!DOCTYPE html>` is no-quirks, and the legacy public and system identifiers choose quirks or limited quirks.
It matters because it changes how the rest of the document is read (a `table` closes an open `p`
only outside quirks mode). It is in `result.mode` and on the document, and a fragment is parsed in the mode its
context says.

## Encodings

The parser takes **bytes** and decides how to read them with the standard's encoding sniffing algorithm
("Determining the character encoding", HTML Standard 13.2.3). In this order, the first that answers wins:

1. a **byte order mark** (`EF BB BF`, `FE FF`, `FF FE`): the encoding is certain, and the mark is skipped (but counted
   in the offsets and the input limit). A UTF-32 mark is not one: `FF FE 00 00` is UTF-16LE and then a NUL, as the standard has it;
2. a **certain transport label** (`transport_encoding`, from a `Content-Type` header, say): certain;
3. unless `ignore_meta`, the **prescan** of the first 1024 bytes for `<meta charset>` and `<meta http-equiv=content-type content=...>`,
   and, when it finds none, for a leading `<?xml ... encoding="...">` declaration, and for the bytes of a UTF-16 `<?x`: tentative;
4. a **tentative transport label**, which is the caller's guess, then **`default_encoding`**: tentative;
5. UTF-8: tentative.

A tentative encoding is held until the document confirms or overrides it. When the tree builder meets a `<meta>` in the head that
declares another encoding (the standard's "change the encoding"), the parser throws the tree away and **starts again** with the declared
one, and the result says `reparsed`; a declaration of the same encoding makes it certain without starting again. A declaration of UTF-16
means UTF-8 (the bytes of a declaration were read as ASCII, so the document is not UTF-16), and `x-user-defined` means windows-1252,
as the standard says. **Nothing is guessed from
the content**: there is no statistical detection and there will be none, because it is not interoperable and because what it gets
wrong looks like a valid document in another encoding.

A label is matched as the Encoding standard matches one (ASCII case-insensitively, surrounding white space ignored; see
\ref format_encoding "Encoding"), so `latin1`, `UTF8` and `x-sjis` work. **A label that names no encoding is ignored**, not an error: the
next step answers, and `transport_encoding_ignored` or `default_encoding_ignored` says it happened. Two labels name encodings the library
reads but not as themselves: `replacement` (and the labels of `hz-gb-2312`, `iso-2022-kr`, `iso-2022-cn`...) decodes to one U+FFFD, as the Encoding
standard has it, and UTF-7 is not an encoding the standard knows, so a document that declares it is read as the default.

The bytes are then decoded by the \ref format_encoding "Encoding" module in its replacement mode (an invalid sequence is U+FFFD and the parse
error `invalid-utf-8-in-input-stream`, whatever the encoding: the error name is the one the standard gives for the UTF-8 decoder and is
kept), and the tokenizer reads characters. **Every position stays exact.** Offsets and the lines and columns are those of the *bytes
fed*, not of the characters decoded: a character that was two bytes in Shift_JIS has its offset and the next starts two bytes on, a
sequence that turned into U+FFFD points at the byte it began at, and the ISO-2022-JP escape sequences, which are not characters, are
counted in the bytes. An error at "the place after" a character (the end of a numeric character reference, say) is at the end of that
character in the bytes, which in UTF-16 is two bytes on and in ISO-2022-JP is before the escape sequence that may follow, so that it is
the same whatever has arrived. A document gives the same tree, errors and positions whole, fed a byte at a time or cut anywhere
(`gtext_html_parser_feed()`), and after a restart: the restarted parse reads the same bytes from the start.

**Where this departs from the standard.** The standard's rule for a `meta` start tag in the head insertion mode says that, for a `charset`
attribute that names an encoding while the confidence is tentative, the parser must "then change the encoding to the resulting encoding"
(and the same for `http-equiv=content-type` with a `content` that names one); the "in body" and the other insertion modes hand a `meta` to
those rules, so a declaration counts wherever the tree builder meets the tag, after the head as well. A browser can start again at any
point because it still has the bytes. **This library changes the encoding only while the head section is open** (the insertion modes
from the initial one to "after head", the text modes of `title` and `script` in it, and "in template"); a `meta` after that is an
ordinary element and changes nothing. The reason is that a restart needs every byte the parser was given, which an incremental parser
must otherwise let go of (see Cost). A caller who knows the encoding passes it as a certain `transport_encoding`, and then nothing is kept
and nothing is waited for; a caller who wants a declaration anywhere in the document honoured can use `gtext_html_parse()`, which has the
whole input and keeps nothing, and restarts for a declaration anywhere in the head section (and so is not limited by `max_reparse_bytes`).

**Cost.**
- *Time, UTF-8.* UTF-8 input goes straight to the tokenizer with no copy; the sniffing and the machinery for other encodings add a little to it
  (about 5% more instructions and 5 to 15% more time on a large document than reading UTF-8 alone).
- *Latency.* Unless a certain `transport_encoding` or `ignore_meta` makes the prescan unnecessary, the incremental parser **waits for 1024
  bytes, or the end of the input, before it produces anything**: the first tokens of a slow stream are late by that much. With either of them
  it decides on the first 3 bytes. A caller who wants to decide early can call `gtext_html_sniff_encoding()` on the bytes it has and
  pass the answer as `transport_encoding`.
- *Memory, decoding.* A parser that decodes (any encoding but UTF-8) holds the decoder's map of a chunk, 40 KiB, and the scratch for the
  tokenizer's form of it, 32 KiB: 72 KiB more per parser, with a 64-bit `size_t`. Nothing is copied for UTF-8.
- *Size, decoding, a 32-bit `size_t`.* The offsets that are exact in the original bytes of a document in another encoding are kept in the
  same word as the width of the character and a flag, so a build whose `size_t` has 32 bits keeps 27 bits of offset: **such a document is
  refused with `GTEXT_HTML_E_LIMIT_INPUT` at its 2^27th byte (128 MiB)**, whatever `max_input_bytes` says, and the failure is sticky like any
  other. The refusal is the status, not an offset that wrapped, and UTF-8, which is read without that map, has no such limit (only
  `max_input_bytes`). A 64-bit `size_t` has 59 bits and cannot reach it. Tested on a 32-bit build under qemu-i386
  (`HtmlSourceMapLimit` in `tests/test-html-source-map-limit.cpp`, run by `suite/tools/xarch/text.sh i686`, which requires that the tests ran and did not skip).
- *Memory, restart.* While the encoding is tentative and the builder has not left the head, the incremental parser keeps the bytes it was
  given so far, because a `meta` that declares another encoding makes it begin again with them. It lets them go when the head is left or
  the encoding becomes certain, **and in any case at `max_reparse_bytes` (4 MiB by default)**: then `reparse_abandoned` says so in the result, and a
  later `meta` is an ordinary element. A `meta` that ends within those bytes restarts the parse however the input was cut. A document that
  holds the head open (an unclosed `title`, `template` or comment) is thereby held to that size, not to `max_input_bytes`. `gtext_html_parse()`
  copies and keeps nothing: it has the whole input.

A fragment is always the UTF-8 text of its characters, never sniffed (its `encoding_confidence` is `GTEXT_HTML_IRRELEVANT`).

**Security.** Encoding confusion is a real class of bug: a filter that sees one reading of the bytes and a browser another. Two consequences:
a *certain* label is never overridden, and a document cannot change it by a `<meta>` (`transport_encoding_certainty` is the caller's
statement of that); and a document read as ISO-2022-JP or UTF-16 sees `<` where a Latin-1 reader sees other bytes, so decide the encoding once, at the
edge, and use `gtext_html_sniff_encoding()` or the result's `encoding` to say what was done.

To see the decision without parsing:

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

static const char * how(GTEXT_Markup_Encoding_Source source) {
  switch (source) {
    case GTEXT_MARKUP_ENCODING_BOM: return "from a byte order mark";
    case GTEXT_MARKUP_ENCODING_CALLER: return "from the caller's label";
    case GTEXT_MARKUP_ENCODING_DECLARATION: return "from a declaration";
    case GTEXT_MARKUP_ENCODING_CALLER_DEFAULT: return "from the caller's default";
    case GTEXT_MARKUP_ENCODING_DEFAULT: return "by default";
    default: return "unknown";
  }
}

/* Which encoding would these bytes be read as, and why? Nothing is parsed: this is the decision the parser starts with. */
static void encoding_sniff(void) {
  const char * documents[] = {
    "\xef\xbb\xbf<p>x",                                   /* a byte order mark */
    "<!doctype html><meta charset=windows-1251><p>x",     /* a declaration in the first 1024 bytes */
    "<p>x",                                               /* nothing says */
  };
  for (size_t i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
    GTEXT_HTML_Sniff_Result r;
    if (gtext_html_sniff_encoding(documents[i], strlen(documents[i]), NULL, &r) == GTEXT_HTML_OK) {
      printf("%s, %s, %s\n", r.encoding_name, how(r.source), r.confidence == GTEXT_HTML_CERTAIN ? "certain" : "tentative");
    }
  }
}
```

Prints:

```
UTF-8, from a byte order mark, certain
windows-1251, from a declaration, tentative
UTF-8, by default, tentative
```

Bytes in an encoding the document declares:

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Bytes in an encoding the document declares. The title is "Привет" in windows-1251. The first document declares it in the first 1024
   bytes, which the parser reads before it begins; in the second the declaration comes after a long comment, so the parser meets it as an
   element, throws what it has built away and begins again in that encoding. The tree is the same. */
static void encoding_meta(void) {
  static char late[1400];
  snprintf(late, sizeof(late), "<!doctype html><!--%*s--><meta charset=windows-1251><title>\xcf\xf0\xe8\xe2\xe5\xf2</title>", 1100, "");
  const char * documents[] = {
    "<!doctype html><meta charset=windows-1251><title>\xcf\xf0\xe8\xe2\xe5\xf2</title>",
    late,
  };
  for (size_t i = 0; i < 2; i++) {
    GTEXT_Markup_Document * doc = NULL;
    GTEXT_HTML_Result result;
    if (gtext_html_parse(documents[i], strlen(documents[i]), NULL, &doc, &result) == GTEXT_HTML_OK) {
      printf("%s, parsed again: %s\n", result.encoding, result.reparsed ? "yes" : "no");
      gtext_markup_document_free(doc);
    }
    gtext_html_result_free(&result);
  }
}
```

Prints:

```
windows-1251, parsed again: no
windows-1251, parsed again: yes
```

The caller's own information, which is the transport label:

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A Content-Type header says ISO-8859-2 and the document says windows-1251. A certain label wins and the declaration is not read;
   a tentative one (a guess of the caller's) yields to the declaration. An unusable label is ignored and the result says so. */
static void encoding_transport(void) {
  const char * bytes = "<meta charset=windows-1251><p>x";
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  GTEXT_HTML_Sniff_Result r;

  opts.transport_encoding = "iso-8859-2";
  gtext_html_sniff_encoding(bytes, strlen(bytes), &opts, &r);
  printf("certain label: %s\n", r.encoding_name);

  opts.transport_encoding_certainty = GTEXT_HTML_TENTATIVE;
  gtext_html_sniff_encoding(bytes, strlen(bytes), &opts, &r);
  printf("tentative label: %s\n", r.encoding_name);

  opts.transport_encoding = "no-such-encoding";
  opts.transport_encoding_certainty = GTEXT_HTML_CERTAIN;
  gtext_html_sniff_encoding(bytes, strlen(bytes), &opts, &r);
  printf("unusable label: %s, label ignored: %s\n", r.encoding_name, r.transport_encoding_ignored ? "yes" : "no");
}
```

Prints:

```
certain label: ISO-8859-2
tentative label: windows-1251
unusable label: windows-1251, label ignored: yes
```

Writing a tree in an encoding is \ref format_html "the serializer's `encoding` option": see "Output encodings" below.

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Write a tree in Shift_JIS. A character the encoding has is written as its bytes; one it has not is written as a numeric
   character reference where the standard allows one (text and attribute values), so the output reads back as the same text. */
static void encoding_serialize(void) {
  const char * text = "<p title=\"\xe2\x82\xac\">\xe6\x97\xa5\xe6\x9c\xac \xe2\x82\xac</p>";   /* "日本 €" twice */
  GTEXT_HTML_Fragment_Context context;
  memset(&context, 0, sizeof(context));
  context.local_name = "div";

  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * fragment = NULL;
  if (gtext_html_parse_fragment(&context, text, strlen(text), NULL, &doc, &fragment, NULL) != GTEXT_HTML_OK) {
    return;
  }
  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.encoding = "shift_jis";

  char * out = NULL;
  size_t length = 0;
  GTEXT_HTML_Serialize_Result result;
  if (gtext_html_serialize_buffer(fragment, GTEXT_HTML_SERIALIZE_CHILDREN, &opts, &out, &length, &result) == GTEXT_HTML_OK) {
    printf("%s, %zu bytes, %zu references: ", result.encoding, length, result.references_written);
    for (size_t i = 0; i < length; i++) {   /* (the bytes that are not ASCII, in hexadecimal) */
      if ((unsigned char)out[i] < 0x80) {
        putchar(out[i]);
      }
      else {
        printf("\\x%02x", (unsigned char)out[i]);
      }
    }
    printf("\n");
    gtext_html_serialize_buffer_free(doc, NULL, out);
  }
  gtext_markup_document_free(doc);
}
```

Prints:

```
Shift_JIS, 35 bytes, 2 references: <p title="&#8364;">\x93\xfa\x96{ &#8364;</p>
```

## What the tree is

The document is a `GTEXT_Markup_Document` in HTML mode, as the standard's tree construction leaves it:

- elements in the **HTML** namespace with lower-case names and attributes in no namespace, in source order with the
  duplicates removed (the first of two equal names stays, the second is the parse error `duplicate-attribute`);
- inside `svg` and `math`, elements in the **SVG** and **MathML** namespaces with the names and attributes the
  standard's tables give them (`foreignObject`, `viewBox`, `definitionURL`), and the attributes `xlink:*`, `xml:lang`,
  `xml:space`, `xmlns` and `xmlns:xlink` in the XLink, XML and XMLNS namespaces;
- a `template` element owns its content as a document fragment
  (`gtext_markup_element_template_content()`) and has no children of its own;
- text with character references decoded and line ends normalized to LF, and adjacent runs one node; comments;
  processing instructions where the standard's tokenizer makes them; a doctype node;
- source locations (byte offset, line, column) on elements, text, comments, processing instructions, doctypes and attributes.

A document with no `html`, `head` or `body` gets them. Nothing is ever
resolved: a `<link>`, `<img src>`, `<script src>` or a form action is only an attribute value.

## Writing HTML

`gtext_html_serialize()` writes a node as HTML by the standard's "serializing HTML fragments" algorithm (section 13.3 of
the Living Standard, **read as served on 2026-10-12**); `gtext_html_serialize_buffer()` writes to a new buffer,
`gtext_html_serialize_file()` to a file that is replaced atomically, and `GTEXT_HTML_Serializer` takes the tokens one at a
time (the walk over a tree is made of exactly that). Include `<ghoti.io/text/html.h>`.

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Parse a fragment and write it back. The output is the standard's serialization: every attribute double quoted,
   `&`, `<`, `>` and the no-break space escaped, no end tag for a void element, and nothing left out. */
static void serialize_fragment(void) {
  const char * text = "<p class=a title='x\"y'>one &amp; two<br><b>three</p>";
  GTEXT_HTML_Fragment_Context context;
  memset(&context, 0, sizeof(context));
  context.local_name = "div";

  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * fragment = NULL;
  if (gtext_html_parse_fragment(&context, text, strlen(text), NULL, &doc, &fragment, NULL) != GTEXT_HTML_OK) {
    return;
  }

  char * out = NULL;
  size_t length = 0;
  GTEXT_HTML_Serialize_Result result;
  if (gtext_html_serialize_buffer(fragment, GTEXT_HTML_SERIALIZE_CHILDREN, NULL, &out, &length, &result)
      == GTEXT_HTML_OK) {
    printf("%s\n%zu bytes, %zu nodes\n", out, result.bytes_written, result.nodes_visited);
    gtext_html_serialize_buffer_free(doc, NULL, out);
  }

  /* One element, itself: `outerHTML`. */
  const GTEXT_Markup_Node * p = gtext_markup_node_first_child(fragment);
  if (gtext_html_serialize_buffer(p, GTEXT_HTML_SERIALIZE_OUTER, NULL, &out, &length, NULL) == GTEXT_HTML_OK) {
    printf("%s\n", out);
    gtext_html_serialize_buffer_free(doc, NULL, out);
  }
  gtext_markup_document_free(doc);
}
```

### What is written

The defaults are the standard's output and nothing else; **nothing is added and nothing is removed**.

- An element is `<`, its name and its attributes in tree order, `>`; then its children and `</name>`. The name is the local name
  in the HTML, SVG and MathML namespaces and the qualified name (`prefix:local`) in any other. A **void element** (the thirteen
  of the standard, and `basefont`, `bgsound`, `frame`, `keygen` and `param`) has no end tag and its children, if the tree has
  any, are not written. A `template` writes its **contents** in place of its children. Names are written as the tree has them: a
  `BR` is not void.
- An attribute is its serialized name, an equals sign, its value in double quotes. The name is the local name for an attribute in no namespace,
  `xml:` and the local name, `xmlns` or `xmlns:` and the local name, `xlink:` and the local name, and the qualified name for any
  other namespace.
- Text is escaped for `&`, U+00A0 (`&nbsp;`), `<` and `>`; an attribute value the same and the double quote (`&quot;`). **Escaping `<` and
  `>` in a value is the standard's now** (the escaping steps are shared by the two modes; the vectors of html5lib-tests agree,
  html5lib 1.1 and parse5 8.0.1 do not, see below). Text is written **as it is** when its parent is an HTML `style`, `script`,
  `xmp`, `iframe`, `noembed`, `noframes` or `plaintext`, or a `noscript` with the scripting flag (`scripting` in the options).
- A comment is `<!--data-->`, a processing instruction `<?target data?>`, a document type `<!DOCTYPE name>` (the identifiers
  are not written; the option `doctype_identifiers` writes them). A CDATA section is text. A `pre`, `textarea` or `listing`
  gets **no** extra line feed, and a text that starts with one is written with it, which a parser then drops (the standard says
  so).
- Characters are not checked: control characters and noncharacters are written as they are. HTML has a spelling for every
  tree, so **a tree is never refused** (compare the XML writer, which refuses what XML cannot spell): a comment with `-->`, a
  `script` that holds `</script>` and a `p` that holds a `ul` are written as they are and do not read back as written (see
  "Round trips").

The standard's steps for a shadow root and for an element's `is` value have nothing to write here: the tree has neither.

### Scope: children, outer, and the context

`GTEXT_HTML_SERIALIZE_CHILDREN` is `innerHTML` (for a document or a fragment, its children; for a template its contents; for a
void element nothing) and `GTEXT_HTML_SERIALIZE_OUTER` is `outerHTML` (the node itself; for a document or fragment the same as
children). The parent of what is written decides whether text is raw: the children of an element written as `CHILDREN` are the
contents of that element, so `style`'s text comes out as it is, and a text node written alone as `OUTER` is escaped for *its*
parent. **A fragment parsed in a context element has no such parent in the tree**: to write it as `innerHTML` of that element, set
`context_name` (and `context_namespace`, when not HTML) in the options, which needs no copy of the tree, or give a token stream
the context (`gtext_html_serializer_context()`).

### Options

Zeroed is the defaults, which are the standard's. The others are a **superset of the standard**, kept so that html5lib's own
serializer vectors can be scored; each is off unless asked for, and the vector (html5lib-tests, `serializer/`) that defines it is
named. A combination that means nothing is `GTEXT_HTML_E_OPTIONS` before any byte is written.

| Option | Default | What it does | Defined by |
|---|---|---|---|
| `allocator` | the tree's | allocator of everything the call allocates, and of the buffer | |
| `max_depth` | 1024 | deepest element nesting; past it `GTEXT_HTML_E_LIMIT_DEPTH` (twice the parser's default of 512 open elements) | |
| `max_output_bytes` | 256 MiB | most bytes; past it `GTEXT_HTML_E_LIMIT_OUTPUT` | |
| `scripting` | off | a `noscript`'s text is raw text | |
| `encoding` | UTF-8 | the label (any the Encoding standard has); a label that names nothing, the empty string too, is `GTEXT_HTML_E_UNSUPPORTED_ENCODING` before a byte is written. See "Output encodings" below | `core.test` "character data" |
| `quote_attr_values` | `ALWAYS` | `WHEN_NEEDED`: quotes only for an empty value or one with ASCII white space, a double quote, an apostrophe, an equals sign, a greater-than sign or a backquote in it (a less-than sign is escaped and needs none) | `core.test` "proper attribute value non-quoting" and its neighbours |
| `quote_char` | double quote | the double quote or the apostrophe (an apostrophe in a value is then `&#39;`); not with `use_best_quote_char` | `options.test` "quote_char" |
| `use_best_quote_char` | off | the quote that needs no escape: the double quote for a value with an apostrophe, the apostrophe for one with a double quote, else `quote_char` | `core.test` "proper attribute value quoting (with ...)" |
| `omit_optional_tags` | off | leave out the start and end tags HTML makes optional, see below | `optionaltags.test` |
| `minimize_boolean_attributes` | off | a boolean attribute (the standard's attribute index) whose value is empty or its name is written as the name | `options.test` "quote_attr_values=true" |
| `use_trailing_solidus` | off | `<br/>`; with `space_before_trailing_solidus` `<br />` (without `use_trailing_solidus` that is an error). The last attribute is quoted when the solidus would follow it directly, because `<br class=a/>` has the class `a/` | `options.test` "use_trailing_solidus=true with void element" |
| `escape_rcdata` | off | the text of `style`, `script` and the other raw text parents is escaped like other text | `options.test` "rcdata" |
| `strip_whitespace` | off | every run of ASCII white space in text becomes one space, except under `pre`, `textarea`, `plaintext` and the raw text elements; adjacent text is one run | `whitespace.test` |
| `inject_meta_charset` | off | with `encoding`: a `meta` in the `head` that has `charset`, or `http-equiv=content-type` and `content`, gets the label (`text/html; charset=LABEL`); if there is none a `<meta charset=LABEL>` is the first thing in the `head`. The token stream keeps the `head`'s tokens until its end tag, in a queue bounded by eight times `max_output_bytes` and 4 KiB (the most memory a `head` can cost, apart from the output limit, which counts the bytes written) | `injectmeta.test` |
| `alphabetical_attributes` | off | attributes sorted by namespace and name | `injectmeta.test` ("head w/robots") |
| `doctype_identifiers` | off | `<!DOCTYPE html PUBLIC "p" "s">` and `SYSTEM "s"` | `core.test` "HTML 4.01 DOCTYPE" |
| `context_name`, `context_namespace` | none | the element whose contents the output is (a fragment's context, or the parent of a token stream): decides whether text is raw. `context_namespace` (NULL for HTML) needs a `context_name` | the standard, 13.3 |

Two of html5lib's options are **not** offered, and why: `escape_lt_in_attrs`, because the standard escapes `<` and `>` in an
attribute value always now, so there is nothing to switch (the vector that sets it passes with the defaults); and
`resolve_entities`, because no vector has an entity token and the tree has no entity node. `quote_attr_values` has two of
html5lib's three modes; its third, "legacy", also quotes the control characters and the Unicode spaces, which no vector needs.
`sanitize` is not a serializer option. Sanitization is a separate explicit operation, documented below.

### The optional tags

With `omit_optional_tags` the tags that section 13.1.2.4 of the standard lets a document leave out are left out, by its rules
exactly as served on 2026-10-12: `html`, `head` and `body` (start and end), `li`, `dt`, `dd`, `p`, `rt`, `rp`, `optgroup`, `option`,
`colgroup` (start and end), `caption`, `thead`, `tbody` (start and end), `tfoot`, `tr`, `td` and `th` (end), and **never a start
tag that has an attribute**. The serializer holds one tag back until it has seen the next token (that is the whole of its
lookahead), so a tag may be written a call late; `GTEXT_HTML_Serialize_Result::omitted_start_tags` and `omitted_end_tags` count
what was left out. "No more content in the parent" is the next token being an end tag, or the end of the stream with the
context element (when the stream has one) not being one of those the standard excludes for `p` (`a`, `audio`, `del`, `ins`,
`map`, `noscript`, `video`, an autonomous custom element). The standard assumes a conforming document: on a tree that is not
one, leaving tags out can change what a parser reads, which is why it is an option.

Seven of html5lib's vectors disagree with the standard's text and are **listed exceptions with the reason**
(`tools/conformance/HTML_SERIALIZER_EXCEPTIONS`), not options:

| Vector | html5lib expects | The standard (served 2026-10-12) says |
|---|---|---|
| `optionaltags.test` #3, "html start-tag followed by space character" | `<html> foo` | the start tag may be omitted "if the first thing inside the html element is not a comment" |
| #9 and #35, "html end-tag" and "body end-tag followed by space character" | `</html> foo`, `</body> foo` | may be omitted if "not immediately followed by a comment" (and the parenthesis: "any spaces after those get parsed into the body element anyway") |
| #71 and #73, "p end-tag followed by datagrid / dir start-tag" | `<datagrid>`, `<dir>` | the list that ends a `p` (`address`, `article`, `aside`, ...) has neither |
| #156, "tfoot end-tag followed by tbody start-tag" | `<tbody>` | a `tfoot` end tag may be omitted "if there is no more content in the parent element", and no other case |
| `options.test` #3, "quote_attr_values=true with irrelevant" | `<div irrelevant>` | `irrelevant` is not in the attribute index's boolean attributes (html5lib's table is older) |

Rules no vector reaches (`option`/`optgroup` before an `hr`, a `p` before `dialog` or `search`, a `p` at the end of an `a`) are
held by `tests/test-html-serialize.cpp`.

### Token by token

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

static GTEXT_Markup_Status to_stdout(void * user, const char * bytes, size_t length) {
  (void)user;
  return fwrite(bytes, 1, length, stdout) == length ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_WRITE;
}

/* Write HTML that is not a markup tree: the token stream the tree walk is made of. The serializer holds back at most one
   tag, so `</li>` can be left out when `<li>` follows. */
static void serialize_stream(void) {
  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.omit_optional_tags = true;
  GTEXT_Markup_Sink sink = {to_stdout, NULL};
  GTEXT_HTML_Serializer * ser = NULL;
  if (gtext_html_serializer_new(&opts, &sink, &ser) != GTEXT_HTML_OK) {
    return;
  }

  GTEXT_Markup_Attribute id;
  memset(&id, 0, sizeof(id));
  id.local = "id";
  id.value = "list";
  id.value_length = 4;

  gtext_html_serializer_start_tag(ser, NULL, "ul", &id, 1);
  for (int i = 1; i <= 3; i++) {
    char item[32];
    snprintf(item, sizeof(item), "item <%d>", i);
    gtext_html_serializer_start_tag(ser, NULL, "li", NULL, 0);
    gtext_html_serializer_characters(ser, item, strlen(item));
    gtext_html_serializer_end_tag(ser, NULL, "li");
  }
  gtext_html_serializer_end_tag(ser, NULL, "ul");

  GTEXT_HTML_Serialize_Result result;
  gtext_html_serializer_finish(ser, &result);
  printf("\n%zu bytes, %zu end tags left out\n", result.bytes_written, result.omitted_end_tags);
  gtext_html_serializer_free(ser);
}
```

The stream need not be balanced (a vector is a few tokens cut out of a document): the serializer keeps a stack of the elements it
has seen open, pops one for every end tag, and ignores an end tag with nothing open. A namespace of NULL is HTML; a void HTML
element given as a start tag is written as an empty tag and its end tag is dropped; text is passed in pieces cut **between
characters** (a U+00A0 cut in two is not escaped). Once a call has failed every later call returns the failure.

The options that change tokens are applied in the order html5lib's filters are: the meta charset first, then the sorted
attributes, the collapsed white space, and the optional tags.

### Options, limits and files

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The options are a superset of the standard's, off unless asked for. Here the tags HTML lets a document leave out are
   left out, and an attribute is quoted only where the syntax needs it. */
static void serialize_options(void) {
  const char * text = "<!DOCTYPE html><html><head><title>Hello</title></head><body>"
                      "<ul><li class=a>one<li>two</ul><p title=\"a b\">Welcome</p></body></html>";
  GTEXT_Markup_Document * doc = NULL;
  if (gtext_html_parse(text, strlen(text), NULL, &doc, NULL) != GTEXT_HTML_OK) {
    return;
  }

  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.omit_optional_tags = true;
  opts.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;

  char * out = NULL;
  size_t length = 0;
  GTEXT_HTML_Serialize_Result result;
  if (gtext_html_serialize_buffer(gtext_markup_document_root(doc), GTEXT_HTML_SERIALIZE_CHILDREN, &opts, &out,
          &length, &result) == GTEXT_HTML_OK) {
    printf("%s\n%zu start tags and %zu end tags left out\n", out, result.omitted_start_tags,
        result.omitted_end_tags);
    gtext_html_serialize_buffer_free(doc, &opts, out);
  }

  /* A combination that means nothing is refused before anything is written. */
  opts.space_before_trailing_solidus = true;
  GTEXT_HTML_Status status = gtext_html_serialize_buffer(gtext_markup_document_root(doc),
      GTEXT_HTML_SERIALIZE_CHILDREN, &opts, &out, &length, NULL);
  printf("%s\n", gtext_html_status_string(status));
  gtext_markup_document_free(doc);
}
```

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A file is replaced atomically: a write that fails leaves the previous one as it was. The limits are statuses of their own. */
static void serialize_file(void) {
  const char * path = "doc-example-out.html";
  const char * text = "<!DOCTYPE html><title>t</title><div><div><div>deep</div></div></div>";
  GTEXT_Markup_Document * doc = NULL;
  if (gtext_html_parse(text, strlen(text), NULL, &doc, NULL) != GTEXT_HTML_OK) {
    return;
  }
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);

  GTEXT_HTML_Serialize_Result result;
  GTEXT_HTML_Status status = gtext_html_serialize_file(root, GTEXT_HTML_SERIALIZE_CHILDREN, path, NULL, &result);
  printf("%s: %zu bytes\n", gtext_html_status_string(status), result.bytes_written);

  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.max_depth = 4;                        /* html, body and two more; 0 is the default, 1024, never "none" */
  status = gtext_html_serialize_file(root, GTEXT_HTML_SERIALIZE_CHILDREN, path, &opts, &result);
  printf("%s\n", gtext_html_status_string(status));

  FILE * in = fopen(path, "rb");             /* the first file is still there */
  if (in) {
    char line[200];
    printf("%s\n", fgets(line, sizeof(line), in) ? line : "");
    fclose(in);
  }
  remove(path);
  gtext_markup_document_free(doc);
}
```

The statuses (`GTEXT_HTML_Status`, shared with the parser): `GTEXT_HTML_E_INVALID` (a NULL or unusable argument, a scope out of
range, a path that cannot be opened), `GTEXT_HTML_E_OPTIONS`, `GTEXT_HTML_E_UNSUPPORTED_ENCODING` (both before any byte is
written), `GTEXT_HTML_E_OOM`, `GTEXT_HTML_E_WRITE` (the sink returned anything but `GTEXT_MARKUP_OK`, or the file layer
failed), `GTEXT_HTML_E_LIMIT_DEPTH`, `GTEXT_HTML_E_LIMIT_OUTPUT` and, for a call out of order, `GTEXT_HTML_E_STATE`.
**There is no first pass**: the sink receives bytes as the walk produces them, in chunks of up to 4 KiB cut anywhere, so a failing call has given the sink
a *prefix* (never more than `max_output_bytes`, and for a depth failure the start tags down to the limit), and a sink that has refused is not
called again. The XML writer checks the whole document first and writes nothing on a refusal; HTML has nothing to refuse, so the
second walk would buy nothing. The file form writes a temporary file and replaces the destination only when the whole output was written, so a failure leaves the previous file (or none) and the result counts nothing. The tree must not
be modified, from another thread or from the sink, while it is written.

### Round trips

**`parse(serialize(tree))` is not the tree for every tree**, and the standard says so itself; for a tree the parser made from
conforming input it is, and then `serialize(parse(serialize(t)))` is `serialize(t)`. The standard's own examples are tested
(`tests/test-html-serialize-roundtrip.cpp`): a `form` inside a `form` made by foster parenting, `<a><table><a>` (the second `a` is
a child of the first and comes back as its sibling), the line feed of a `pre`; and the cases that follow from the algorithm: a
`script` that holds its own end tag or opens the double-escaped state (`<!--<script`), a comment with `-->`, a `textarea` with a
comment, a `p` with a `ul`, a `plaintext` element (its end tag is written and read back as text), a carriage return that came
from `&#13;` (written as it is, read as a line feed), and a document type with identifiers (the standard writes the name only).
An omitted tag is no loss for a conforming document and one for a document that is not.

Over the **2,095 inputs of the tree-construction vectors** (the 2,091 in scope and the four `none dom`, each parsed under the scripting
settings it names or both: 4,150 runs, as a document or as a fragment in its context), `make conformance-html-serializer` parses, writes with the standard's defaults, parses again
and writes again: **3,969 runs come back as the same tree and the same bytes**; 181 runs of 91 inputs do not, and each has a cause
the gate recognizes in the first tree (a predicate on the vectors' dump, see `tools/conformance/html_serializer_suite.py`) --
`plaintext` 57, `script-double-escape` 48, `doctype-identifiers` 42, `leading-newline` 14, `nested-a` 12, `p-contains-block` 8,
`carriage-return` 2, `nested-form` 2, `nested-nobr` 2 (a run can have several) -- and a cause only counts for the difference
it can make: the first line at which the two trees differ must be the line of the node that carries the cause or a later one (identifiers
change the document type line and nothing else), so a defect before the cause is not hidden by it, and the gate checks that rule on a
planted pair on every run. A run that is neither stable nor explained
fails the gate, and so does a tree that comes back the same with other bytes. The gate is shown to fail by 41 controls: 32 defects planted in
the serializer, one that asks the runner 18 requests it must refuse, and eight that corrupt the vectors, the exceptions and the floors.

### Compared with html5lib and parse5

`make check-html-serialize-oracle` puts **one tree to three serializers** -- html5lib 1.1's, parse5 8.0.1's and this library's, at the
standard's settings -- and compares the bytes, over the trees of the tree-construction vectors and of generated and mutated documents
(parsed by this library) and over trees no parser makes (a void element with children, a script that holds markup, elements in
other namespaces, attributes of every namespace, every character that has an escape). The tree travels as a list of operations,
not as markup (that would compare parsers) and not as the vectors' dump (it sorts the attributes). Every divergence is explained by
features of the tree *verified by taking them out*, and is a line of `tools/oracle/html_serialize_divergences.txt`; the 14 kinds:

| Reference | Feature | The difference |
|---|---|---|
| html5lib | `nbsp-in-text`, `nbsp-in-attribute` | does not escape U+00A0 |
| html5lib, parse5 | `angle-in-attribute` | do not escape `<` and `>` in a value (the standard now does; the vectors do) |
| html5lib | `attribute-namespace` | writes the local name only (`href` for `xlink:href`) |
| parse5 | `attribute-namespace-without-prefix` | writes `undefined:name` |
| html5lib | `doctype-identifiers` | always writes the public and system identifiers |
| html5lib | `noscript`, `plaintext`, `raw-text-with-elements`, `raw-name-in-foreign-namespace` | its raw text flag is set by an element's name, whatever the scripting flag and the namespace, has no `plaintext`, and outlasts an element's children |
| html5lib | `void-set` | has `command` and `event-source`, lacks `wbr`, `keygen`, `basefont`, `bgsound`, `frame` |
| html5lib, parse5 | `processing-instruction` | neither has a node for one |

Every reference deviation has a **witness**, a small tree and the output worked out by hand from the standard's text, that this library
gives and the reference does not. Checked on every run: 14 of 14.

### Security

Serialization is **not sanitization**. Writing a tree writes what is in it, `javascript:` URLs and `on*` attributes included, and
the standard warns that the output may not read back as the same tree: that is the mutation XSS this algorithm is known for
(a `style` that holds `</style><script>`, a comment that holds `-->`). Escape for the place the output is going and sanitize the tree
before writing it. Limits are finite by default; set `max_output_bytes` and `max_depth` to what you can hold. The walk is iterative: no
tree is deep enough to exhaust the C stack (a 100,000-deep tree is tested), and the work is linear in the tree (counted: nodes
visited, allocations, sink calls).

#### Sanitizing untrusted fragments

`gtext_html_sanitize_fragment()` and `gtext_html_sanitize_buffer()` apply the built-in `ghoti-html-flow-1` policy to an
HTML fragment parsed in a synthetic `div` context. The guarantee is limited to insertion as HTML body/flow content. It does
not cover attributes, scripts, styles, URLs used outside anchors/images, SVG, MathML, or another parser context. Parsing and
ordinary serialization remain unsanitized operations.

Use `gtext_html_sanitize_options_default()` as the starting configuration. Safe links are enabled by default; images and
tables require `allow_images` and `allow_tables`. The link policy keeps `http`, `https`, `mailto`, and relative `href` values,
adds `rel="nofollow"`, and drops `target`. Image URLs, when enabled, are limited to `http`, `https`, and relative URLs;
enabling images can cause the browser displaying the result to request the image. Tables, when enabled, retain only the
documented OWASP structure and attributes, with `colspan` in 1–1000 and `rowspan` in 0–65534. Forms, embedded media, CSS,
event attributes, SVG, MathML and script-capable subtrees are never enabled by these options.

The default element profile is `a`, `p`, `div`, `h1`–`h6`, `blockquote`, `ul`, `ol`, `li`, `br`, `strong`, `em`, `b`, `i`,
`code` and `pre`. Benign unsupported formatting wrappers are unwrapped after their contents are sanitized; active, raw-text,
embedded, custom and foreign-namespace subtrees are discarded. Only policy-listed attributes survive. No scripts execute and
no URL is fetched during sanitization.

`max_work_units` bounds the iterative traversal and attribute validation (default 20,000,000); the parser's finite input,
depth, node and attribute limits also apply. `max_output_bytes` bounds buffer output (default 64 MiB). A parse, allocation,
work or output failure returns no tree or buffer. `GTEXT_HTML_Sanitize_Result` reports the status, message, policy version and
work charged. The returned fragment is fresh and owned by the returned document; free the document with
`gtext_markup_document_free()`. Any later mutation requires sanitizing again. The buffer belongs to the same call and must be
released with `gtext_html_sanitize_buffer_free()`.

### Departures from the XML writer and from the HTML parsing API

The shape is the XML writer's -- an options struct with a `_default()`, the same sink type, a buffer form with a `_free` that takes
the document and the options, a file form through the same atomic layer, a result with counts, distinct statuses for the limits -- and
these are the departures and why:

- **No refusals, so no first pass and no path.** XML refuses a tree it cannot spell and says where (`/a/b[2]/@x`); HTML refuses
  nothing, so the walk is one pass, a failure has given the sink a prefix, and the result has no path.
- **The statuses are `GTEXT_HTML_Status`'s**, appended: `GTEXT_HTML_E_WRITE`, `GTEXT_HTML_E_LIMIT_OUTPUT` and `GTEXT_HTML_E_OPTIONS`
  (XML's `E_INVALID` covers a bad option; here an option combination that means nothing has a status of its own).
- **The functions take a node and a scope**, `(node, scope, opts, sink, result)`, not a document: `innerHTML` and `outerHTML` of
  any node are what HTML is written for, where XML writes documents. The buffer form's `_free` takes the node's *document*, as XML's.
- **The result has no `free` and no error list** (it holds nothing but counts and a static message), unlike `GTEXT_HTML_Result`; the
  parsing API's `max_open_elements` is this API's `max_depth`, with the default twice as large so that a tree the parser built with its
  defaults is always written.
- **A token-stream serializer**, which XML's writer has no analogue of (it writes documents), because the optional-tag rules are rules
  about the next token and html5lib's vectors are token streams.
- **A `GTEXT_Markup_Sink` that is called with a prefix after a failure**, where XML's writer promises it nothing until the document is checked.
- **Any encoding the Encoding standard has can be written** (see "Output encodings"), where XML writes five; an unusable `encoding` label is an error here and is ignored by the parser.
- **Options beyond the standard** (the table above), where XML's writer has `xhtml` and `indent`. Pretty printing is not part of
  this stage: it changes the tree, and the standard's algorithm has no whitespace of its own to add.

### Output encodings

`encoding` names the encoding the bytes are written in, and the output is bytes in it (`result.encoding` says which, by the standard's name). The
standard's "get an output encoding" applies: a label for UTF-16LE, UTF-16BE or `replacement` has no encoder, so the output is UTF-8 and
`encoding_substituted` says so. In **text and in attribute values**, a character the encoding cannot write is written as a decimal numeric character
reference (`&#8364;`), which an HTML parser reads back as that character, and `references_written` counts them. **Anywhere else** (tag and attribute
names, comments, raw text such as `script` and `style`, the doctype, a processing instruction), a reference would be read as the characters it is
made of, so the character has no spelling: the status is `GTEXT_HTML_E_UNREPRESENTABLE` and `unrepresentable_code_point` names the character
(a prefix of the output has been written, as for any failure that comes during the walk). ISO-2022-JP is written in its own state: the escape sequences
are emitted as needed, the text before a reference or a tag is back in ASCII, and the output ends in ASCII. After a failure the prefix that was delivered ends in ASCII as well: the escape back (`ESC ( B`) is appended when the output was in another set, the failure is an unrepresentable character or a limit, and the output limit has room for the three bytes; it is not appended after a sink that refused or when memory ran out. With `inject_meta_charset` the `meta`
says the encoding by its lower-case name (`shift_jis`), which is a label a parser accepts. The example (`gtext_html_serialize_buffer()` with
`opts.encoding = "shift_jis"`) is the last one in "Encodings" above.

## The media-type trap

The same bytes are two different trees depending on the media type they arrive under, and this library
never guesses. `text/html` is read by this API; `application/xhtml+xml` is XML and is read by
`gtext_xml_parse()`, which refuses what is not well-formed and reads `<br/>`, `<script/>` and a CDATA section as XML
does. XHTML served as `text/html` is HTML to every browser: `<p/>` opens a paragraph, `<script/>` swallows the rest of the
document, a processing instruction is a comment. The XML writer's `xhtml` mode exists to write trees that both
readings agree on, and refuses or reports the ones they do not (see \ref format_xml "XML", "XHTML-compatible output").
Choose the parser by the media type, not by the bytes; no function here sniffs for "HTML-ness".

## Security

- **Limits are the defence against untrusted input**, and every default is finite. The depth of the stack
  of open elements bounds the work (the tree builder is linear in the input at a fixed depth: no pattern of input
  makes it quadratic in the input alone, and nothing recurses, so no input exhausts the C stack; `make check-html-quadratic` holds the work of every shape that makes a naive parser quadratic to a ratio bound at three sizes, and the instructions callgrind counts agree with the counters); the other limits bound memory. Set `max_input_bytes`
  to what you can hold, and `max_open_elements` low (the browsers' 512 is generous) for hostile input. A run of text is
  held in memory until it ends, so `max_text_bytes` bounds that for a stream.
- **Nothing is loaded.** No `link`, `img`, `script`, `iframe`, `object`, stylesheet `@import`, `meta refresh`, or entity is ever fetched
  or resolved; `gtext_html_parse_file()` opens exactly the path it is given.
- **No script is executed**, and `scripting` is only a flag about how `noscript` is read. Scripts and event handler
  attributes are text and attribute values in the tree.
- **Parsing and ordinary serialization do not sanitize.** A parsed page contains whatever it contained, including
  `javascript:` URLs, `on*` attributes and `<script>` elements. Use the explicit flow-fragment sanitizer for its documented
  context; serializing and parsing again is not sanitization either (mutation XSS is a property of the serialization, see
  "Writing HTML").
- The parse errors and positions are for the author of the page; do not treat their absence as a verdict.
- A fragment is parsed in a context you choose; choose the one the result will be inserted in.

## Threads

Independent parses share nothing, and any number may run at once (`make test-tsan` runs eight threads parsing
the same documents with the default allocator). A parser and a result are not synchronized: use each from one thread at a time.
A custom allocator is called from the threads that use it.

## How this differs from the XML API

Everything the two share is the same on purpose. These are the departures and why:

- **A result struct instead of an error struct.** XML's `GTEXT_XML_Error` says why one parse failed and where; HTML has no
  failure that has a position, and has a list of errors, a mode and an encoding to report on success, so `GTEXT_HTML_Result` carries those
  (freed by `gtext_html_result_free()`, which XML's error has no need of beyond a reset), and every function fills it on success and failure.
- **No `GTEXT_HTML_E_SYNTAX` family**: there are no malformed HTML documents. The statuses are arguments, memory, state, the encoding, and
  one per limit.
- **`max_open_elements`, not `max_depth`**, for the option (it is the standard's stack of open elements and counts `html` and `body`);
  its status is `GTEXT_HTML_E_LIMIT_DEPTH`, as XML's is.
- **The parser is created with a status** (`gtext_html_parser_new(opts, &parser)`), where XML's `gtext_xml_stream_new()` returns NULL on
  failure: there is more than one way for it to fail (an encoding label, an unusable context), and a caller wants to tell them apart.
- **The incremental parser builds the tree**, and `finish` returns the document (once) and the result; XML's stream delivers events and a
  separate DOM function builds a tree. There is no pull reader and no event stream for HTML.
- **A fragment function and a fragment parser**, which XML has no analogue of.
- **The writer is under "Writing HTML"**, with its own list of departures.
- **The parser reads every encoding the Encoding standard has**, by the standard's own sniffing algorithm (see "Encodings"), where XML reads five; a label it does not know is ignored and flagged, not refused.

## What is not here yet

| Planned | Where |
|---|---|
| sanitization | separate explicit flow-fragment API; see "Sanitizing untrusted fragments" above; not part of parsing or ordinary serialization |
| script execution | never |
| the `scripted_*.dat` vectors (6), which need scripts | out of scope |

## Measured

Numbers are from the gates' output; the commands are named so they can be run again. **They are a log**: each block says which stage it measured and on what date, and none is kept current by hand. The figures of today's tree - vectors, exceptions, plants, seeds, divergence entries, shapes, tests - are the output of `tools/phase3-counts.py` (and `tools/html/counts.py` and `tools/encoding/counts.py` under it), which is what the plan record (`notes/text/XML-HTML-PLAN.md`, "Phase 3k as built") quotes.

Public API (phase 3f), measured 2026-10-12 on the development machine:

- `make conformance-html-tree`: the pinned tree-construction vectors through the public API
  (`tools/conformance/html_api_suite.c`, the same scorer and exceptions as the builder's own runner): 2,095 in
  scope, **2,091 pass, 4 tagged `none dom`**, identical to the builder's; `#errors` exact on 1,792, never fewer.
  Cut every way (every split, a byte at a time, pieces of 2 to 9, random) with the identical answer. The gate is shown
  to fail 146 ways (136 plants, 8 of them in the public layer, and 22 bad requests asked of both runners). `make
  check-conformance-vectors` runs both passes in `make test`; `--sanitize` runs both under ASan and UBSan.
- Tests: `testHtmlApi` (140: the arguments, the eight limits at their exact boundaries, the options, the error cap and
  count-only, the encoding labels and the byte order mark, ownership, the sticky failure, the fragment context, the names, and
  more than 5,000 cut comparisons including every split offset, a byte at a time and random pieces, for documents and nine
  fragment contexts, and eight threads), `testHtmlApiOom` (19: the n-th allocation failed for every n, through nine
  entry points, with and without a result), `testHtmlApiFile` (12, every limit's boundary for a file too), `testHtmlApiFuzzSeeds` (750 seeds: 0.3 s, 1.9 s under ASan, 3.5 s under TSan, bounded at a minute), `testHtmlDocExamples`
  (the 8 snippets of this page compiled and run, and held byte for byte to the page). `make test`: 166 programs, 4,059 tests,
  0 failures; `test-asan` 161 programs and `test-tsan` 161, 0 reports; `check-windows-cross` 104 sources, 0 failed.
- Fuzz: `fuzz_html` 750 seeds; 111,917 runs in four searches (240 s, 421 s, 301 s and an earlier harness) clean; 12 planted defects in the public
  layer, all found by replaying a seed, and the 29 earlier plants and three controls still found or silent
  (`make check-html-fuzz-plants`, in batches).
- Documentation: 53 installed headers compile standalone as C and C++; no Doxygen warning in the public headers,
  this page or the new sources.
- The oracle (`make check-html-oracle`, scoring half): 18,461 documents compared, 58 divergence kinds, 19 of 19
  witnesses, as before this phase. Its controls are 98 plants and over an hour: **65 were run, all caught** (every one whose
  anchor is in `html_tree.c`, `html_tokenizer.c` or `html_tree_fragment.c`, 45, and 20 spread over the rest of the list), and every tree
  plant's anchor was checked to be still in the source.


Serializer (phase 3g), measured 2026-10-12 on the development machine:

- `make conformance-html-serializer`: html5lib-tests' serializer vectors, **248 (the 230 and the 18 that have an XHTML variant) -- 241 pass,
  7 are listed exceptions with a reason** (4 where the standard's text decides otherwise, 3 where the vector depends on a table of
  html5lib's), none stale; floors 248 / 241 / 3,950. The round trip of the **2,095 tree-vector inputs** (4,150 runs under the
  scripting settings they name): 3,969 stable, 181 runs of 91 inputs not, all 181 explained by a case the standard names, 0 unexplained.
  Shown to fail by 41 controls (32 planted defects, 18 bad requests in one, 8 corrupting the vectors, the exceptions and the floors),
  all caught; `make check-conformance-vectors` runs the scoring in `make test`, and `conformance-html-serializer-asan` under ASan and UBSan.
- `make check-html-serialize-oracle`: 13,241 trees (10,211 parsed by this library from the tree-construction vectors' inputs and from
  generated and mutated documents, foreign content and fragments, and 3,030 built with no parser), each written two ways, 26,398 questions asked of each
  reference; ours equals html5lib's on 16,130 and parse5's on 24,902; **11,764 divergences in 14 kinds, every one explained by
  features of the tree verified by taking them out**, none unexplained, 14 of 14 witnesses hold; shown to fail by 30 controls
  (6 corrupting the answer, the list and a witness, 24 planted defects), all caught. **Hunts of 222,326 trees** (six seeds, in chunks of 32,041 to 39,561)
  found no kind the list lacks and nothing unexplained.
- Tests: `testHtmlSerialize` (52, ten of them added by the review: the matrix, escaping per context, the serialized name of every attribute kind, templates, void elements
  with children, every option, the combinations refused, the standard's own optional-tag examples and each rule, the token stream's
  arguments and states), `testHtmlSerializeLimits` (13: the sink refusing every call of a 70 KiB document, the allocator refusing every
  allocation through the sink, the buffer and the token stream and the file, the output limit at every value, the depth limit around every
  depth, a tree 100,000 deep, the work counted, the file form, eight threads), `testHtmlSerializeRoundtrip` (3: 250 generated
  conforming documents under five option sets come back as the same tree and the same bytes, and the cases that do not), and
  `testHtmlSerializeFuzzSeeds` (751 seeds), `testHtmlDocExamples` (12 snippets). `make test`: 170 programs, 4,124 tests, 0 failures;
  `test-asan` 166 programs and `test-tsan` 166, 0 reports; `check-windows-cross` 105 sources, 0 failed.
- Fuzz: `fuzz_html_serialize` 751 seeds; the first search found a defect in nine seconds (the held head counted against the output
  limit, fixed, its reproducer kept), and then 1,023,111 runs in 762 s were clean; 11 planted defects, eight found by replaying a seed and three by
  searching, and the unedited control silent (`make check-html-fuzz-plants`).
- Documentation: 54 installed headers compile standalone as C and C++; the Doxygen warning count of the whole project is 1,628 before and
  after, none in this stage's headers, sources or this page.


\anchor format_html_internals
# Internals: the tokenizer and the tree builder

Everything above is the public layer (`src/html/html_api.c`, `html_core.c`, `html_file_io.c`). It drives the two
stages below, whose interfaces (`src/html/html_tokenizer_internal.h`, `src/html/html_tree_internal.h`) are internal:
nothing in them is installed or exported. The tokenizer comes first, the tree builder after it
(\ref format_html_tree "below"). The names below that read `gtext_html_tree_*` are the internal builder's;
its whole-buffer functions were `gtext_html_parse()` and `gtext_html_parse_fragment()` until the public ones took those names, and are
`gtext_html_tree_parse()` and `gtext_html_tree_parse_fragment()` now.

# The tokenizer

## Specification

WHATWG HTML, "Tokenization" (13.2.5, all 84 states) and "Preprocessing the input
stream" (13.2.3.5), **as served at html.spec.whatwg.org/multipage/parsing.html on
2026-10-11** (Living Standard, last updated 9 October 2026), read state by state. That
text has processing instruction states (13.2.5.72 to 13.2.5.76) that html5lib 1.1 and
parse5 8.0.1 predate. The vectors are html5lib-tests at the commit pinned in
`tools/html/HTML5LIB_TESTS_COMMIT`. The C1 table of the numeric reference end state is
in `html_tokenizer.c`, checked against the Encoding standard's windows-1252 index by a
test and by `numericEntities.test`.

## Interface

Pull-driven: `feed()` a chunk (borrowed until `next()` says `NEED_INPUT`), `next()`
for tokens, `finish()` at the end. A token is delivered the moment it is complete and
nothing past it has been read, so `set_state()` between tokens applies to the very next
character. `set_last_start_tag()`, `allow_cdata` (the option's callback, overridden by
`set_allow_cdata(DEFAULT | ALLOW | REFUSE)`, which stands until changed), and a work counter
for tests.

**Character runs are held.** A run of characters is coalesced, so it is delivered only when
something that is not a character ends it, or the input ends; it is held in memory. A run
longer than `max_text_bytes` is therefore a hard failure (`E_LIMIT_TEXT`), not a split in two
tokens. The default for it is the default for the input (64 MiB), so by default the input limit
is reached first. Text that merely looks like an end tag (`</styl` and ten thousand letters in
RCDATA) is text and has no limit but that one.

**Limits.** Six are failures, each its own sticky status: input bytes fed, name, value,
comment, text run, attributes on a tag. `max_errors` is not one: it counts and drops
(`count_errors_only` keeps none). 0 selects a default and never means unlimited (`SIZE_MAX`
is as near as the type allows). The input limit is the one exception to "the answer is the
same however the input is cut": it is on what has been fed, so a whole input over it fails
before any token and a cut one delivers the tokens before the byte that crosses it.

**Memory.** When a few bytes are waiting for the rest of a character or a look-ahead and a
large chunk arrives, the tail and at most 256 bytes of the chunk are joined and the rest is
read in place; the copy is bounded by the tail and that window, not by the chunk.

**Attribute names** are found by a seeded hash table (a seed per tokenizer, `hash_seed` to
fix it), so a tag of a thousand attributes costs a thousand probes, and names built to collide
for one tokenizer do not for another.

Input is UTF-8; an invalid sequence is one U+FFFD per maximal subpart (the Encoding
standard's decoder) and the non-standard error `invalid-utf-8-in-input-stream`. A byte
order mark is not stripped by the tokenizer (the public layer skips one and tells it the three bytes with `gtext_html_tokenizer_skip_input()`, so offsets still count them) and other encodings are not read by the tokenizer, which takes UTF-8: the public layer decodes the rest (see "Encodings", and a mapped feed that carries the original byte offsets). The
`wtf8` option reads the three-byte forms of lone surrogates, which the html5lib vectors
contain.

## Errors

53 codes: 51 the tokenizer raises, `non-void-html-element-start-tag-with-trailing-solidus`
(the standard's, declared for the tree builder, which owns it), and the non-standard
`invalid-utf-8-in-input-stream`. Each has line, column and byte offset; kept up to
`max_errors`, the rest counted. Positions follow the vectors: the position of the
current input character; the numeric reference end state and a missing semicolon after a
named reference at the character after; `incorrectly-opened-comment` at the character
after the `!`; **columns count UTF-16 code units** (an astral character is 2); lines count
normalized newlines.

## Measured

Numbers are from the gates' output; the commands are named so they can be run again.

- `make conformance-html-tokenizer`: html5lib-tests tokenizer vectors, 7,053 runs (6,827
  cases in their initial states); 7,030 pass and 23 are exceptions with reasons
  (`tools/conformance/HTML_TOKENIZER_EXCEPTIONS`: vectors that predate the processing
  instruction states, put an end-of-file error at the end of a target, order two errors
  differently, or expect the XML infoset coercion). Each run is also cut a byte at a time and
  in pieces of 1 to 9 bytes, read as strict UTF-8 where the input has no lone surrogate, and run
  under seeded tiny limits (what it delivered must be the beginning of the roomy run's answer);
  the answers must be identical, with no exceptions list. The gate is made to fail 31 ways
  (`run-html-tokenizer.sh --controls`).
- `make check-html-oracle` (the tokenizer part, `run-html-oracle.sh --tokens`): differential
  against html5lib 1.1 and parse5 8.0.1, 13,999 inputs in the default population (the vectors'
  inputs, the tree-construction documents, 6,000 generated and mutated). parse5's tokens agree on
  all of them, its errors on all but 30, html5lib's tokens on all but 130; every divergence is
  one of the classified causes in `tools/oracle/html_tokenizer_divergences.txt` (parse5
  miscounts a line feed after `&`, omits unknown-named-reference in attributes, orders a stream
  error after reference errors, throws on two lone low surrogates; html5lib names a missing
  DOCTYPE name `""`, replaces NUL in CDATA, and keeps the comment-start state on NUL). The
  list describes the default population only; a hunt is `html_tokenizer_diff.py --count N
  --seed S` (three of 100,000 on seeds 21 to 23 were run for this page: nothing unclassified).
  27 controls: the answer corrupted five ways, the list three, 19 planted tokenizer defects,
  and the two that probe the attribute-value scanner.
- **Known gap: processing instructions have no differential coverage.** Neither reference has
  the states, so inputs containing `<?` are left out (except PLAINTEXT) and the vectors are the
  only external check. A hand-written test (`ProcessingInstructionsEndWhereTheBogusComment...`)
  checks where they end.
- `make fuzz-html-tokenizer`: `fuzz_html_tokenizer` (whole, byte-at-a-time, pieces; token
  properties; limits; allocation failure at every point; linear work), 2,980 seeds from the
  vectors plus 7 recorded reproducers and a 900-attribute seed, replayed by `make test`.
  `make check-html-fuzz-plants` plants 8 defects: 6 are *replayed* from a recorded input (that
  shows the harness can see the defect), 2 (`whitespace-flag`, `upper-case-tag`) are *searched
  for* from the seeds within the time budget, and so is the unedited control, which must stay
  silent.
- `tests/test-html-tokenizer.cpp` (83): every row of the specification's matrix, every two-way
  cut of a corpus in each state, an out-of-memory failure at every allocation, work counts with
  the limits raised.
- Work: fed a byte at a time the tokenizer does at most `GTEXT_HTML_TOKENIZER_WORK_BOUND(n)`
  (48 n + 4096) units; the unit tests, the fuzzer and this page use that one bound. No
  throughput figure is claimed.

## Not done (tokenizer)

Scripting beyond the states a tree builder sets. (Encoding determination is the public layer's.)


\anchor format_html_tree
# The tree builder (stages 3c, 3d and 3e)

`src/html/html_tree.c`, `html_tree_modes.c`, `html_tree_foreign.c`, `html_tree_fragment.c`, `html_tree_internal.h` (the contract) and
`html_tree_private.h`. It drives the tokenizer above and builds a \ref format_markup "Markup"
document: elements in the HTML namespace with attributes in no namespace (and, in foreign content, elements in the SVG and MathML
namespaces with the names and attributes the standard's tables give them), a doctype node,
comments, processing instructions, text with adjacent runs merged, the document mode. Internal
only, as the tokenizer is.

## Specification

WHATWG HTML, "Tree construction" (13.2.4 "Parse state", 13.2.6 "Tree construction"), **as served
at html.spec.whatwg.org/multipage/parsing.html on 2026-10-11** (Living Standard, last updated 9
October 2026, the revision the tokenizer was written from; fetched again for 3c, and again **2026-10-12 04:25 UTC for
3d**: the same revision, Last-Modified 9 October 2026 08:28 GMT; and a third time **2026-10-12 09:42 UTC for 3e**: Last-Modified
Fri, 09 Oct 2026 08:28:19 GMT, the same revision, so there is no difference from the 3d text to note), read mode by mode. Implemented:

| Section | What |
| --- | --- |
| 13.2.4.1 to 13.2.4.5 | the insertion mode, **reset the insertion mode appropriately** (complete: the table, caption, column group and template branches, the template's branch reading the current template insertion mode; tested over 33 stacks, templates with their modes included), the stack of open elements and its scopes (general, list item, button, table), the list of active formatting elements with **the Noah's Ark clause** (three), reconstruct the active formatting elements, clear up to the last marker, the head, form and frameset-ok state |
| 13.2.6.1, 13.2.6.2, 13.2.6.3 | inserting nodes, the generic raw text and RCDATA parsing algorithms, generate implied end tags |
| 13.2.6.4.1 to 13.2.6.4.8 | the modes initial, before html, before head, in head, in head noscript (the scripting flag is an option, off by default), after head, **in body (every start and end tag rule)**, text |
| 13.2.6.4.9 to 13.2.6.4.16 | **(3d)** in table, in table text, in caption, in column group, in table body, in row, in cell, in template |
| 13.2.6.1, 13.2.4 | **(3d)** the appropriate place for inserting a node with foster parenting, the stack of template insertion modes, the template contents, "generate all implied end tags thoroughly" |
| 13.2.6.4.17 to 13.2.6.4.21 | after body, in frameset, after frameset, after after body, after after frameset |
| 13.2.6.4.7 | **the adoption agency algorithm**, outer-loop limit 8 and inner-loop limit 3, and "any other end tag" |
| 13.2.6.4.1 | the quirks-mode determination from the doctype (public and system identifier tables) |
| 13.2.6, 13.2.4.2 | **(3e)** the tree construction dispatcher (which rules a token is handled by), the adjusted current node, the MathML text integration points and the HTML integration points (SVG `foreignObject`, `desc`, `title`; MathML `annotation-xml` whose start tag had `encoding` text/html or application/xhtml+xml, ASCII case-insensitively) |
| 13.2.6.1 | **(3e)** "adjust MathML attributes" (`definitionURL`), "adjust SVG attributes" (58 names), "adjust foreign attributes" (the eleven `xlink:*`, `xml:*`, `xmlns` names), "insert a foreign element", and the parse error for an `xmlns` that is not the element's namespace |
| 13.2.6.4.7 | **(3e)** the `math` and `svg` start tags of "in body" |
| 13.2.6.5 | **(3e)** the rules for parsing tokens in foreign content: characters (NUL, white space, other), comments, processing instructions, DOCTYPE, the 44 start tags (and `font` with `color`, `face` or `size`) and the end tags `br` and `p` that break out, "any other start tag" (the adjusted current node's namespace, the SVG element names of the 37-name table), the SVG `script` end tag and a self-closing `script`, and "any other end tag" (the walk down the stack) |
| 13.4 | **(3e)** the HTML fragment parsing algorithm, step by step (`html_tree_fragment.c`); the "fragment case" branches of the modes |

Five things in the standard as served that the task statements did not expect, and what was done (the quirks rule
for the 4.01 Frameset and Transitional identifiers says "the system identifier is *missing or the empty string*" in
the copy served 2026-10-11, so an empty one counts as absent for it, as the code has it; absent and empty stay two
states from the tokenizer's flag to the tree's NULL and ""):

- **There is no "in select" insertion mode.** The `select` element is parsed by ordinary "in body" rules
  (`select`, `option`, `optgroup`, `hr`, `input` and the `select` end tag), and `select` is one of the
  boundaries of "has an element in scope". They are implemented, not stubbed, and `<select>` parses.
  (html5lib 1.1 and parse5 8.0.1 parse it by the older rules. Until 3d their documents with a `select` were
  left out of the differential; now they are asked, and a divergence in one is explained by removing the select
  elements, a method with a blind spot stated under "Measured".)
- **Processing instructions are nodes.** The standard's tokenizer has processing instruction states and the
  tree builder inserts them as nodes (`<?php echo?>` is a processing instruction node, `<?a$>` a comment,
  per the tokenizer); the pinned vectors expect nodes (`processing-instructions.dat`). The task statement said
  "no PIs (bogus comments instead)"; the standard and the vectors win. Neither reference has them.

- **The template start tag has two branches this parser cannot take**: declarative shadow roots (it needs the
  "allow declarative shadow roots" option, false by default, and a host element) and content patching by a `for`
  attribute (it needs a live document to patch). Without those both end in "insert an HTML element for the token",
  which is what is done; the "insertion target" and "insertion end marker" the appropriate-place algorithm reads are never
  set, so a template's place is its contents.
- **A form start tag in a table inside a template inserts the form** (and sets no pointer), and after head's anything-else
  sets frameset-ok to "ok" again when it makes the body: html5lib 1.1 and parse5 8.0.1 read both by the older rules.
  Both are the served text, re-read 2026-10-12 (the page unchanged: Last-Modified 9 October 2026 08:28 GMT). "In table",
  start tag "form": "Parse error. If the form element pointer is not null, and the parser is not parsing template
  contents, then ignore the token. Otherwise: Insert an HTML element for the token, and, if the parser is not parsing
  template contents, set the form element pointer to point to the element created. Pop that form element off the stack of
  open elements." (The older text, which both references follow, ignored the token when there was a template on the
  stack or the pointer was set.) "After head", anything else: "Insert an HTML element for a "body" start tag token with no
  attributes. Set the frameset-ok flag to "ok". Switch the insertion mode to "in body". Reprocess the current token.", and
  the template start tag in "in head" says "Set the frameset-ok flag to "not ok"." The witnesses
  `<form><template><table><form>` and `<head><template></template></head><p><frameset>` pin both.
- **The chapter names no error for an end tag `table` that pops open elements**, but the vectors list one, so one is raised
  (`open-elements-at-end-tag`). The `#errors` count is not gating beyond the floors.

## Foreign content and fragments (stage 3e)

**Foreign content.** An element is the pair (namespace, name): a name slot of the builder is keyed by both, so `<svg><td>` is not an HTML
`td` to the stack's per-name indexes (the topmost open element of a name, the scope floors, "any other end tag"), and the nine
elements the standard makes special and scope boundaries (MathML `mi mo mn ms mtext annotation-xml`, SVG `foreignObject desc title`)
carry those flags while the HTML ones of the same names are never confused with them. "Is this the HTML namespace" is a comparison,
and the foreign end tag rule's walk down the stack is two index lookups and a comparison with the nearest HTML element below the
current node, kept per stack entry, so it costs O(1) (the pops of a breakout are the pushes, paid once). The tables (37 SVG element
names, 58 SVG attribute names, `definitionURL`, the eleven namespaced attributes) are data in `html_tree_foreign.c`, extracted from
the served text by script and checked by a test against the vocabulary the writers use (`gtext_markup_html_foreign_name_survives()`).
**That check found a gap**: the vocabulary had been copied from parse5 8.0.1, which lacks `feDropShadow` (`fedropshadow` is in the
standard's table); it is added, and the XHTML writer's differential lists the references' gap as a class of its own
(`ref:both:feDropShadow`, `tools/oracle/html_divergences.txt`). The tokenizer's CDATA decision is the adjusted current node's
namespace (`ht_allow_cdata`), evaluated when `<![CDATA[` is read; every token before it has been handled by then, and characters never
change the stack, so the answer is the same however the input is cut.

**Fragments.** `gtext_html_tree_new_fragment(options, &context, &status)` runs the algorithm's steps 1 to 21 (the document's mode from
the context's document, the tokenizer's state from the context element (RCDATA for `title` and `textarea`, RAWTEXT for `style xmp
iframe noembed noframes`, script data for `script`, RAWTEXT for `noscript` when scripting is on, PLAINTEXT; HTML elements only: an SVG
`title` is not RCDATA, which the vectors say), a root `html` as the stack's only node, "in template" pushed for a template context, the
fake start tag's attributes for an `annotation-xml` context, **reset the insertion mode appropriately** reading the context element,
the form element pointer when the context or an ancestor is a form); feed and `finish` as for a document; `gtext_html_tree_take_fragment()`
moves the root's children into a document fragment and hands over the document that owns it. The context is described **by value**
(`GTEXT_HTML_FragmentContext`: namespace, local name, attributes, form-ancestor flag, quirks mode, scripting flag); no tree node is made
for it and nothing of the caller's is read after the call (a test frees the name and attributes at once; under a sanitizer a kept
pointer fails). The standard as served makes the fragment the "root insertion target" (nodes whose target parent is the root go to
the fragment); here they are inserted into the root and moved at the end, which is the same tree.

Branches of the standard that are marked "fragment case" and were stubs until now: `</html>` after the body (ignored), `</frameset>` (no
switch to "after frameset"), the `select` and `input` start tags when the context is a `select`, "parsing template contents" for a
template context (the `form` rules), the end of the file in "in template" with no template on the stack (stop), foster parenting that
returns the root, table-context clearing and the table and caption end tags that find nothing in scope.

The public API is the layer above; the builder's `<meta>` encoding step only reports (the public layer restarts), no serialization (that is 3g, below), no scripts. The "not yet" status of the earlier stages
(`GTEXT_HTML_TREE_E_NOT_YET` and its accessors) is removed: nothing raised it once foreign content was built. The scorer's exceptions
format keeps its stage column, for a future construct that is not.

## Interface

`gtext_html_tree_new(&options)`, `feed()` (any chunking; the chunk is not kept), `finish()`,
`take_document()`, `free()`, or `gtext_html_tree_parse()` for a whole buffer. The tree and the errors are
identical however the input is cut (tested at every split offset, a byte at a time and in random pieces,
on every vector, and in the fuzzer). Options: the allocator (used for the document too), `max_open_elements`
(default 512 and it counts `html` and `body`, as the browsers' limit does: 510 `div`s fit, the 511th is
`GTEXT_HTML_TREE_E_LIMIT_DEPTH`), `max_nodes` (10,000,000, `E_LIMIT_NODES`), the tokenizer's six limits
(each its own status, passed through), `max_errors`, `count_errors_only`, `scripting`, `hash_seed`. A failure is sticky and
returns no document and no partial tree; an allocation failure at any point is `E_OOM` and leaks nothing
(swept at every allocation, on three documents and two cuts, in the tests and in the fuzzer). The document is
made with no depth limit of its own: the parser's limit is the stack's.

**Errors.** Both stages' parse errors, in the order raised, with code, line, column (UTF-16 units, as the
tokenizer's) and byte offset, capped by `max_errors`, the rest counted. The standard names one tree-stage
error (`non-void-html-element-start-tag-with-trailing-solidus`, raised for a start tag with a trailing
solidus that nothing acknowledged, an ignored one included); for the rest it says "parse error", and the 16
codes here are named for what happened (`missing-doctype`, `unexpected-end-tag`, `misnested-formatting`...; stage 3e added `html-in-foreign-content`, for a breakout, and `xmlns-attribute-mismatch`, which the standard describes at the end of "create an element for a token").
A tree-stage error is positioned at the first character of the token that caused it, except the errors raised for each character of a run (text foster parented out of a table, characters ignored in a column group, a NUL in the white space that waits in a table), which are at their own character: the tokenizer gives a character token the places its pieces came from (`segments`: a character reference, a carriage return and line feed, a replaced byte each start a piece), the first 1,024 of a run exact and the rest counted on from the last, so the memory is bounded however many references a run holds. Tokenizer errors are at the character. Ten of the tree stage's kinds have tests with exact name, line, column and byte offset (`ParseErrorsAreNamedAndPositioned`), and two more, `html-in-foreign-content` and `xmlns-attribute-mismatch`, have one in the foreign content tests (`TheForeignErrorsAreNamedAndPositioned`); the other kinds are named, and the vectors count them.

## Design

Tag names are looked up once per tag in a table of the 119 names the algorithms mention (sorted, binary
search; its flags are checked against the HTML vocabulary's `gtext_markup_html_is_special()` and
`_is_formatting()` by a test, so the category is written once) and in a seeded hash table for the rest. The
stack of open elements is an array. For each name it keeps the index of the topmost open element of that
name, and each entry holds, for each kind of scope, the index of the nearest boundary at or below it (its
"floor"). So **"has a `p` in button scope" is one comparison**, and so are "any other end tag", the `li`, `dd`
and `dt` start tag loops, and "the last `a` after the last marker" (each formatting-list entry is chained to
the previous one of its name). The few operations that change the middle of the stack (the adoption agency
algorithm, `</form>`, the head element after the head) unwind the entries above, edit the array and derive
them again: O(entries above), the cost of the shift itself. No code recurses. Elements are created from
names interned once (`markup_element_create_atoms`), attributes are appended without the duplicate check the
tokenizer has already done, and a run of text appended to a text node grows its buffer geometrically.

Where the cost is what the standard forces: reconstructing the formatting elements costs the number reopened;
the adoption agency algorithm costs the distance it walks and the entries above the formatting element that it
shifts; a repeated `<html>` or `<body>` start tag costs the product of its attribute count and the element's
(O(1) each: a hash index of the names of the html and body elements, so any number of start tags with new names is linear, and the elements keep every distinct name, as the standard has no limit); the Noah's Ark clause compares a new formatting element with the entries of its name after the last marker, and every formatting start tag reconstructs first, so all of them are open: at most the depth of the stack, 512 by default, per token (a test with distinct attributes, nested, misnested and reconstructed, shows a unit of fixed depth is linear in its repetitions); `</body>`,
`</html>` and the end of the file scan the stack once.

## Measured

Numbers are from the gates' output, one run of each after the last change to the code (2026-10-12); the commands are
named so they can be run again.

- `make test` runs the vector gates too (`check-conformance-vectors`): the XML suite, the HTML tokenizer's vectors with their controls and the
  tree builder's vectors, foreign content and fragments included. **The decision**: measured on the development machine they take 24, 28 and 20
  seconds, under two minutes together, so they are in `ALL_TEST_GATES` and not a subset; the data being absent is a failure that names
  `tools/html/fetch.sh` or `tools/xml/fetch.sh`, as for every gate that reads `third_party/`. What stays outside `make test` is the tree gate's
  controls (`make conformance-html-tree`, about five minutes: it builds and runs 128 planted defects), the oracle and the fuzz plants.
- `make check-foreign-tables` (also in `make test`): the SVG and MathML name tables are kept by hand in `html_tree_foreign.c`, `markup_html.c`,
  `test-markup-html.cpp`, the oracle's generator and the fuzz seed generator. `tools/html/foreign_tables.txt` is the one transcription of the
  chapter's tables (37 SVG tag names, 58 SVG attributes, `definitionURL`, 11 foreign attributes) and `tools/html/check-foreign-tables.py` reads each
  copy from its declaration and fails on any difference in either direction, or on a declaration it cannot find; dropping `feDropShadow` from
  one copy was seen to fail it.
- `make conformance-html-tree`: the web-platform-tests tree-construction vectors at the commit pinned in
  `tools/html/WPT_COMMIT`: 1,962 vectors in 58 files, and 133 hand-chosen ones
  (`tools/conformance/html_tree_extra.dat`: expected trees that html5lib and parse5 both give, rechecked by
  `tools/oracle/html_tree_extra_check.py`; 13 are the table modes', 61 are stage 3e's: the breakout list in six groups, `font`,
  the formatting elements reopened before an `svg`, the foreign end tag's walk, the special foreign elements, CDATA, the
  attribute adjustments and thirty-two fragments; the ones in an SVG or MathML context are asked of parse5 alone, and the
  checker verifies that html5lib still cannot take one). The four `scripted_*.dat` files (6 vectors, trees that scripts make)
  are not run. **What the denominators are**: 1,962 + 133 = 2,095 vectors are run, **all in scope**: 1,857 initial documents and
  238 fragments (206 vectors and 32 extras); **2,091 pass; 4 are listed because no tree builder following the chapter can pass
  them** (`webkit02.dat` 45 to 48 expect a `selectedcontent` element to be filled with the selected option's content, a DOM
  behaviour of the form controls). The **tagged exceptions** (`tools/conformance/HTML_TREE_EXCEPTIONS`) are those 4: stage 3e
  **0** (it was 424: 218 foreign, 206 fragment), 3d 0, none 4. **Where the vectors came from**: the pinned html5lib-tests holds no
  tree-construction vectors (only tokenizer, serializer and encoding data), so the web-platform-tests copy is the whole of
  them. The order the plan asks for, reproduced by undoing the groups in scratch copies on the same 2,095 vectors (the
  integration points, the breakout and the end tag rules together, then the fragment algorithm): with foreign elements and their
  adjustments alone, **133 of the 1,857 documents fail** (1,724 pass; each of the three groups alone: integration points 31
  documents and 31 fragments, breakout 40 and 16, the end tag walk 6 and 0); with the integration points, the breakout and the
  end tags, **all 1,857 pass** (no fragment can, there is no algorithm yet); with the fragment algorithm, **2,091 of 2,095 pass,
  the four being the `selectedcontent` ones**. `make conformance-html-tree-asan` runs the same gate with the library and the runner
  built with ASan and UBSan (the runner frees each chunk at once). A vector written for a scripting setting runs with it, the
  others with the flag off and on, a fragment vector in the context its `#document-fragment` line names (the runner takes
  `ctx:x<hex>`, `form` and a quirks word). Every run is also cut every way (every two-way split, a byte at a time, pieces of 2 to
  9 bytes, random pieces) and must give the same answer, errors and their places included, and the tree passes the invariant
  checker. `#errors` is compared by count and reported, not gating: 1,792 of the 1,958 passing vectors that list errors have
  exactly the count (every difference is ours being higher, 124 of them in the processing instruction vectors, whose expected counts
  predate the states, 11 in `plain-text-unsafe.dat`, 6 in `html5test-com.dat`). The gate fails on an unlisted failure, a stale
  exception, and below 2,085 passing, 2,050 loaded and 1,792 with the exact count. **It is made to fail 159 ways**
  (`run-html-tree.sh --controls`, 138 lines of output because the twenty-two bad requests print as one; `--control NAME` runs one):
  31 that are not planted defects (the vectors lie, the list lies five ways, three floors, twenty-two bad requests), and **128
  planted defects**, one rule undone each: 66 of 3c and 3d, and **62 of 3e**: the SVG element, SVG attribute, MathML attribute
  and namespaced attribute adjustments (and one entry missing from each table), the annotation-xml encodings, the breakout (never;
  three names missing from the list; `font` with `face`; `font` without attributes; the end tags `p` and `br`; popping past an
  integration point; leaving foreign elements open), the foreign end tag (popping one element, walking past HTML elements,
  looking only in SVG, never reaching the HTML rules, the SVG `script` end tag, self-closing elements), the integration points
  (MathML text, SVG, as scope boundaries, as special elements, `mglyph`, `malignmark`, `svg` in annotation-xml, characters and start tags
  in them), NUL and frameset-ok in foreign text, CDATA never or always allowed, the adjusted current node ignoring the fragment
  context, and the fragment algorithm (the tokenizer state, `title`, `script`, `plaintext`, `noscript`, the reset from the context, the
  result not moved, the select context, the template context, `</html>` and `</frameset>`, the integration point contexts, the
  context's namespace). **Rules no vector reaches, so no plant here**: the form element pointer of a context with a form
  ancestor, the quirks mode of the context's document and the encoding attribute of an annotation-xml context (none can be
  written in the vectors' format; `tests/test-html-tree-foreign.cpp` holds them, and a plant of each was made by hand and fails
  it), the seeding of the stack of template insertion modes by a template context (no effect anyone can see: the reset falls back to
  "in template", and the first start tag replaces the mode), and the parse error for an `xmlns` that is not the element's namespace
  (an error-count-only rule; its test names the error and its place).
- `tests/test-html-tree.cpp` (85 tests; two rewritten for 3e: the not-yet status and the foreign content in a cell) and
  `tests/test-html-tree-foreign.cpp` (44; 3e): the matrix rows by name (inline SVG, MathML and annotation-xml, breakout, foreignObject,
  foreign attributes, CDATA, NUL, the fragment contexts, the form pointer, the depth limit, out of memory), **the adjustment tables as
  data** (sizes 37, 58 and 11; every row applied in SVG and not in MathML or HTML; each target survives the vocabulary
  `gtext_markup_html_foreign_name_survives()` and each folded form does not; the nine special foreign elements against
  `gtext_markup_html_is_special()`), every one of the 44 breakout names and 64 names that are not on the list, `font`'s three
  attributes, each integration point (five MathML text, annotation-xml with ten encodings, SVG `foreignObject`, `desc`, `title`) with HTML
  and foreign content inside, the boundary of scope (`</p>` in a `desc`, an `li` in a `foreignObject`), the end tag walk (case,
  nearest, across namespaces, across integration points, not past an HTML element), the SVG `script`, "reset the insertion mode" over
  29 contexts, the tokenizer's state for each context (HTML elements only), the table, template, select, frameset, html, body
  and integration point contexts, argument checks of the fragment API, the context described by value (freed at once), every
  split offset of every one of the 238 fragment vectors in both scripting settings (over 7,700 cut runs) and three-way cuts of ten
  foreign-heavy inputs in documents and in seven contexts, an allocation failure at every point (seven documents and fragments, two
  cuts; and in `new_fragment`), the limits (600 nested `svg`, `math`, `g`, `foreignObject` each its own status and no tree; the
  boundary 510 and 511; fragments; nodes), **and the work counted at two sizes** (n and 4n): deep SVG and MathML, strays and
  self-closing elements at depth 3,000, runs of breakouts popping foreign elements, many integration point switches (`foreignObject`,
  `desc`, `title`, `mi`, `annotation-xml`), large fragments in `td`, `tbody`, `template`, `svg`, `foreignObject`, `mi` and `select`
  contexts, and 16,000 distinct foreign element names: the ratio stays under 6 and the work under
  `GTEXT_HTML_TREE_WORK_BOUND(n)` (64 n + 4096). The older tests' figures (3c and 3d) stand.
- `make check-html-oracle` (the tree part, `run-html-oracle.sh --tree`): **18,461 documents** (1,622 initial-document vectors without a
  processing instruction and 206 fragment vectors, the 133 extras, 6,000 generated and 3,000 mutated as before, and since 3e 3,000
  with `svg` and `math` embedded in HTML (every SVG name whose case is restored, every attribute that needs adjusting, the integration points
  with HTML and foreign content inside, annotation-xml with each encoding, the breakout tags, `font`, `</p>` and `</br>`,
  CDATA, scripts, end tags that match, differ in case and match nothing; in formatting elements, paragraphs, tables and cells, templates, lists,
  selects and forms), 1,500 of those mutated, and 3,000 fragments in random contexts (55 % HTML elements of every mode and tokenizer state,
  25 % SVG, 20 % MathML)) put to html5lib 1.1 and parse5 8.0.1 and this builder; none stopped. **This builder equals html5lib on 11,730 and
  parse5 on 16,604** (both on 11,703). **1,460 fragments are in an SVG or MathML context, which html5lib's API cannot take** (it must say so,
  and the check that it still does is in every run); parse5 alone judges them. **Every divergence (5,411 documents) is one of 58 kinds** in
  `tools/oracle/html_tree_divergences.txt`, each verified like the 3d ones, but, since 3d's lesson, **an explanation of foreign content is not
  the deletion of an element where it can be a transformation that keeps the rest of the document in the question**: `@endtags`
  (delete the end tags that name an element of foreign content in this builder's tree: both references find the element an end tag
  closes by its name alone in the HTML rules, so `</ms>` closes a MathML `ms` the standard leaves open and an `</td>` finds an SVG
  `td`), `@cdata` (a CDATA section becomes its text: parse5 makes a comment in an integration point), `@context` (a foreign context named
  like an HTML element is renamed: parse5 reads the name without the namespace), `@noscript` (the scripting flag on for the noscript
  context: both references ignore the flag), `@selectctx` (a select context becomes a div: both read the fragment by the replaced "in
  select" mode), `@rootend` and `@resetnames` (found by the hunts: parse5's foreign end tag walk stops before the root element of a
  fragment, so an unmatched end tag is never handed to the HTML rules; both references' reset of the insertion mode reads names without
  namespaces, so a MathML `tr` is "in row"); and by removal only where the reference is older than the standard and the other reference
  corroborates (`html5lib:alone:` kinds: html5lib 1.1 has `foreignObject` as the only special foreign element, so `mi`, `mo`, `mn`, `ms`, `mtext`,
  `annotation-xml`, `desc` and `title` do not stop its "any other end tag"; and it keeps `</p>` and `</br>` inside foreign content), plus
  `fedropshadow`, which **neither reference has in its table of SVG names** (the standard has it; a finding of this stage, see above). Every
  deviation of a reference has a **witness**, a document whose tree was worked out from the chapter by hand: 19, eleven of them new
  (checked on every run, with the scripting flag both ways unless the entry fixes it, and a context for a fragment). The count of
  **documents where nothing but the witnesses and the vectors stands behind this builder** (both references differ and the cause is one
  that could hide a defect; or only parse5 could be asked and it differs) is **1,519** and is gated like a kind's. **Made to fail 87
  ways** (`run-html-oracle.sh --control NAME`): the answer corrupted two ways, the list three, the witnesses once, the uncorroborated
  count once, the extra cases once, 14 planted defects of 3c, 22 of 3d, and **43 of 3e** (one per rule group and for the fragment
  algorithm; run in parallel scratch copies, each caught). **Hunts**: `HTML_TREE_ORACLE_COUNT=15000`, seeds 21 to 24: 4 x 43,211 =
  **172,844 documents**, run in bounded chunks, found six causes the first time and none unclassified after (each is a `hunt` entry of
  the list; html5lib 1.1 stops on an assertion in its template handling for two documents, which parse5 then judges alone).
- `make fuzz-html-tree`: `fuzz_html_tree` now parses fragments too (bit 1 of the first option byte; its high six bits choose among 64 contexts: HTML
  elements of every mode, SVG and MathML elements, each integration point, an annotation-xml with and without the encoding, a form, a quirks
  mode), holds foreign trees to the shapes a parser can make (names the standard's table gives, attributes only in the XLink, XML and XMLNS
  namespaces and only on foreign elements, no NUL in text, a fragment's root empty) and sweeps allocation failure through `new_fragment`
  as well; 2,136 seeds (1,839 + 297: the fragment vectors in their contexts and 40 foreign and fragment inputs at the sizes that make a naive
  implementation quadratic), replayed by `make test`; **52,810 runs in 853 seconds** (three runs of 281, 286 and 286 s) after the last change found nothing.
  `make check-html-fuzz-plants` plants twenty-one defects in the tree builder: thirteen are searched for, eight replayed from a seed, and so is
  the unedited control, which stays silent. Six are 3e's: the foreign namespaces' atoms not released (the invariant checker's reference counts), the SVG
  element and attribute names left lower case (the shape property), a NUL kept in foreign text, an allocation failure absorbed while a foreign
  attribute is added, and the fragment's result not moved out of the root.

## Not done

Sanitization, scripts (the `scripted_*.dat` vectors stay
out of scope: `scripted_adoption01.dat`, `scripted_ark.dat`, `scripted_foster01.dat`, `scripted_webkit01.dat`, 6 vectors).

# The serializer (stage 3g)

## Specification

The WHATWG HTML Living Standard, "Serializing HTML fragments" (13.3) and "Optional tags" (13.1.2.4), **retrieved 2026-10-12**; html5lib-tests'
`serializer/*.test` (230 vectors, 18 with an XHTML variant) pinned by `tools/html/HTML5LIB_TESTS_COMMIT`. Where the standard and html5lib's
serializer differ, the standard is the default and the vectors are scored through options (`quote_attr_values`, `omit_optional_tags`, ...);
where a vector asks for something the standard does not say and no option stands for it, it is a listed exception with the reason.

## Design

Three layers in `src/html/html_serialize.c`. **The front** checks arguments and, with `inject_meta_charset`, keeps a `head`'s tokens (as bytes in
one growing queue with varint lengths, bounded by eight times the output limit) until its end tag. **The stream** turns white space into one
space where asked, keeps the stack of open elements (two bytes each: the parent of a text, of an end tag), and holds **one** tag back when
the HTML syntax may leave it out: a start tag of `html`, `head`, `body`, `colgroup` or `tbody` with no attributes, or the end tag of one of
nineteen elements; the next token (its kind, its name, whether its text starts with white space) or the end of the stream decides it. Nothing
else is ever asked of the future, which is why a single slot is the whole lookahead. **The writer** is one door that counts bytes against the
limit and batches them for the sink (4 KiB), the escapes, and the attributes (sorted through a scratch array only when asked). The tree walk is an
explicit stack of (element, next sibling), so depth is a heap array bounded by `max_depth`; a template's contents are entered in place of its
children, and a void element's children are skipped.

Two decisions came from the tests. A solidus after an unquoted value is part of the value (`<br class=a/>`), so with no space before it the last
attribute is quoted. And the tokens held for a meta charset are bigger than the text they make, so they are bounded apart from the output limit:
an output exactly at the limit was refused until the fuzzer found it.

## Not done (serializer)

Pretty printing; sanitization; `escape_lt_in_attrs` and `resolve_entities` (html5lib's options with
nothing to switch or no token to act on); html5lib's "legacy" quoting mode; a shadow root and an element's `is` value (the tree has neither).

