@page format_html HTML

# HTML

`gtext_html_parse()` reads a whole **HTML document** the way the WHATWG HTML Living
Standard says a browser does, and builds a \ref format_markup "Markup tree" from it;
`gtext_html_parse_fragment()` does what `innerHTML` does, in a context element you
describe; `GTEXT_HTML_Parser` takes the document in pieces as it arrives; and
`gtext_html_parse_file()` reads a file. HTML has no malformed documents: **every input has
a tree**, and the mistakes in it are *parse errors*, listed in the result with the
standard's names and positions, which are not failures. A parse fails only for an
argument, a limit, memory, or an encoding this release cannot read. Back to
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
| `transport_encoding`, `transport_encoding_certainty` | NULL, certain | a label from a `Content-Type`; UTF-8's only, until 3h |
| `default_encoding` | NULL (UTF-8) | the label to assume when nothing else says; UTF-8's only, until 3h |
| `ignore_meta` | false | skip the `<meta>` prescan; accepted, and changes nothing yet |

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
| `encoding`, `encoding_source` | "UTF-8", and how that was decided: `GTEXT_MARKUP_ENCODING_BOM`, `_CALLER`, `_DECLARATION` or `_DEFAULT` |
| `input_bytes` | bytes read (a failed parse: as far as it got) |
| `node_count` | nodes in the document, 0 when there is none |
| the errors | `gtext_html_result_error_count()`, `_errors_dropped()`, `_error_at()` |

On a failure the result still has the status, the message, the parse errors
raised before the failure (best effort: if the memory for the list is the thing that failed
it is not there), `input_bytes` (the bytes accepted before the one that failed: 0 for a whole
input refused for its length) and, once a parser was made, the encoding (`"UTF-8"`, source
`DEFAULT`, `CALLER` or, if the mark had been seen, `BOM`); a failure before there was a parser (an
argument, an unsupported label, OOM making it, a file that could not be read) has `encoding` NULL.
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
| `GTEXT_HTML_E_UNSUPPORTED_ENCODING` | a label that is not UTF-8's |
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

```c
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The encoding the bytes were read as, and how that was decided. */
static void encoding(void) {
  const char * with_mark = "\xef\xbb\xbf<p>x";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  if (gtext_html_parse(with_mark, strlen(with_mark), NULL, &doc, &result) == GTEXT_HTML_OK) {
    printf("%s, from a byte order mark: %d\n", result.encoding,
        result.encoding_source == GTEXT_MARKUP_ENCODING_BOM);
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);

  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.transport_encoding = "windows-1252";   /* a Content-Type header said so */
  GTEXT_HTML_Status status = gtext_html_parse("<p>x", 4, &opts, &doc, &result);
  printf("%s\n", gtext_html_status_string(status));
  gtext_html_result_free(&result);
}
```

This release reads **UTF-8**. A leading UTF-8 byte order mark is skipped (and the source is then
`BOM`, whatever the transport label says, as the standard orders it), counted in the offsets and the input limit; bytes that
are not UTF-8 are replaced by U+FFFD with the error `invalid-utf-8-in-input-stream`, one per maximal
invalid subpart as the Encoding standard's decoder does. The document records `"UTF-8"` and the source
(`gtext_markup_document_encoding()`), and so does the result. A fragment's result reports the encoding and its source, but its scratch document records none: it is characters, not a document's bytes, so a leading `EF BB BF` is the text U+FEFF there and is never a byte order mark. The option fields for the rest are already here and final, so that adding the rest changes no structure:
`transport_encoding` with its certainty, `default_encoding` and `ignore_meta`. A label is matched as the Encoding standard
matches one (ASCII case-insensitively, surrounding white space ignored): `utf-8`, `utf8`, `unicode-1-1-utf-8`,
`unicode11utf8`, `unicode20utf8` and `x-unicode20utf8` are accepted in either label field, and **any other string,
the empty one included, is `GTEXT_HTML_E_UNSUPPORTED_ENCODING`** before the document is examined, so a program
can pass what it knows today and get its encoding when the library does. UTF-16, the legacy encodings of the Encoding
standard, the `<meta>` prescan and the transcoding are planned (phases 3h and 3i).

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
  makes it quadratic in the input alone, and nothing recurses, so no input exhausts the C stack); the other limits bound memory. Set `max_input_bytes`
  to what you can hold, and `max_open_elements` low (the browsers' 512 is generous) for hostile input. A run of text is
  held in memory until it ends, so `max_text_bytes` bounds that for a stream.
- **Nothing is loaded.** No `link`, `img`, `script`, `iframe`, `object`, stylesheet `@import`, `meta refresh`, or entity is ever fetched
  or resolved; `gtext_html_parse_file()` opens exactly the path it is given.
- **No script is executed**, and `scripting` is only a flag about how `noscript` is read. Scripts and event handler
  attributes are text and attribute values in the tree.
- **The tree is not sanitized.** A parsed page contains whatever it contained, `javascript:` URLs, `on*` attributes and
  `<script>` elements included; there is no sanitizer here yet. Parsing, serializing and parsing again is not a
  sanitization either (mutation XSS is a property of the serialization, which is not here yet, see below).
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
- **No writer yet.** Serialization is phase 3g.
- **Encoding options** that are validated but mostly inert until 3h; XML reads five encodings.

## What is not here yet

| Planned | Where |
|---|---|
| serialization (the fragment serialization algorithm; parse, serialize, parse round trips) | phase 3g |
| other encodings: UTF-16, the legacy encodings, the `<meta>` prescan, `ignore_meta`, a tentative label overridden by a declaration | phases 3h and 3i |
| sanitization | not planned in this phase |
| script execution | never |
| the `scripted_*.dat` vectors (6), which need scripts | out of scope |

## Measured

Numbers are from the gates' output; the commands are named so they can be run again.

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
order mark is not stripped by the tokenizer (the public layer skips one and tells it the three bytes with `gtext_html_tokenizer_skip_input()`, so offsets still count them) and other encodings are not read: that is phases 3h and 3i. The
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

Encoding determination beyond a UTF-8 byte order mark, which the public layer skips (3h), scripting beyond the states a tree builder sets.


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

The public API is the layer above; the builder has no `<meta>` encoding step (3h), no serialization (3g), no scripts. The "not yet" status of the earlier stages
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

Encoding determination and the `<meta>` steps (3h, 3i), serialization (3g), sanitization, scripts (the `scripted_*.dat` vectors stay
out of scope: `scripted_adoption01.dat`, `scripted_ark.dat`, `scripted_foster01.dat`, `scripted_webkit01.dat`, 6 vectors).
