/**
 * @file
 *
 * libFuzzer harness for the YAML *writers*.
 *
 * fuzz_yaml.cpp parses and walks and never calls a writer, so until this
 * existed the most productive tool applied to this module had never been
 * pointed at half of it.  Four defects were found by hand that this would
 * have found: a character 5.1 forbids written raw, an anchor name written
 * without being checked, a tag gaining a layer of percent-encoding on every
 * round trip, and the streaming writer dropping the %TAG directive that said
 * what a tag spelling meant.
 *
 * The property is the one all four broke:
 *
 *     if the writer says OK, the bytes it wrote must parse,
 *     and must hold the same values.
 *
 * Two families of input reach the writers here, and they are not the same:
 *
 *   - documents that came from parsing, which is what a round trip over
 *     yaml-test-suite measures; and
 *   - documents built through the DOM API, which is where the interesting
 *     failures were, because a corpus of YAML text can only carry values the
 *     parser accepts.  A DEL never reaches the writer from the first
 *     direction and is ordinary in the second.
 *
 * A harness can only find defects in documents it can *construct*, and that
 * is the bound that has mattered most here.  An hour-long campaign over a
 * merged corpus found nothing, while the two defects before it each needed
 * something the corpus alone could not reach: one needed the DOM API, and the
 * other needed YAML 1.1 mode, which nothing here had ever turned on.  So the
 * axes are drawn from the input rather than fixed:
 *
 *   - the parse options, so 1.1 mode, the three schemas and the key policies
 *     are reachable at all;
 *   - the write options, so indentation, line width, the five scalar styles,
 *     the three flow styles, canonical form and all five *encodings* are -
 *     which puts the writer's UTF-16 and UTF-32 output back through the
 *     reader, an axis yaml-test-suite does not have;
 *   - the node kinds, so !!set, !!omap and !!pairs, typed scalars, stored
 *     scalar styles and comments reach the writer, none of which the three
 *     kinds this built before could express;
 *   - multi-document output, which had no path here at all.
 *
 * Build with: make fuzz-yaml-writer     Run: make fuzz-run-yaml-writer
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <ghoti.io/text/yaml.h>
#include <ghoti.io/text/json.h>
}

namespace {

/* Bytes as something legible.
 *
 * The failure messages printed the writer's output raw, which was fine while
 * everything it wrote was UTF-8 and became useless the moment the encoding
 * became an axis: a UTF-16 document reaches the terminal as a screenful of
 * nothing, and the whole point of printing it is that the artifact says
 * nothing on its own. */
std::string legible(const char * p, size_t n) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)p[i];
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c >= 0x20 && c < 0x7F) out.push_back((char)c);
    else {
      out += "\\x";
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

/** The JSON of a document, as the comparable form of "the same values". */
std::string values_of(const GTEXT_YAML_Document * doc, bool * ok) {
  GTEXT_JSON_Value * json = nullptr;
  *ok = false;
  if (gtext_yaml_to_json(doc, &json, nullptr) != GTEXT_YAML_OK || !json) {
    if (json) gtext_json_free(json);
    return std::string();
  }
  GTEXT_JSON_Sink sink;
  if (gtext_json_sink_buffer(&sink) != GTEXT_JSON_OK) {
    gtext_json_free(json);
    return std::string();
  }
  std::string out;
  GTEXT_JSON_Error jerr;
  memset(&jerr, 0, sizeof(jerr));
  if (gtext_json_write_value(&sink, nullptr, json, &jerr) == GTEXT_JSON_OK) {
    out.assign(gtext_json_sink_buffer_data(&sink),
               gtext_json_sink_buffer_size(&sink));
    *ok = true;
  }
  gtext_json_sink_buffer_free(&sink);
  gtext_json_free(json);
  return out;
}

/** Everything the writer produced, or the empty string if it refused. */
bool write_document(
    const GTEXT_YAML_Document * doc,
    const GTEXT_YAML_Write_Options * opts,
    std::string * out) {
  GTEXT_YAML_Sink sink;
  if (gtext_yaml_sink_buffer(&sink) != GTEXT_YAML_OK) return false;
  GTEXT_YAML_Status st = gtext_yaml_write_document(doc, &sink, opts);
  if (st == GTEXT_YAML_OK) {
    out->assign(gtext_yaml_sink_buffer_data(&sink),
                gtext_yaml_sink_buffer_size(&sink));
  }
  gtext_yaml_sink_buffer_free(&sink);
  return st == GTEXT_YAML_OK;
}

/* A refusal is always allowed - not every value has a YAML spelling, and
   saying so is the correct answer.  What is never allowed is claiming
   success and producing something this library cannot read back. */
void must_round_trip(
    const GTEXT_YAML_Document * doc,
    const std::string & text,
    const GTEXT_YAML_Parse_Options * read_with,
    bool compare_values) {
  GTEXT_YAML_Error err;
  memset(&err, 0, sizeof(err));
  /* Read back under the dialect the document was written in, not under the
     default.  @p compare_values says whether the *values* can be compared
     afterwards as well, or only whether the output parses at all.

     They cannot always.  The writer emits core-schema YAML and has no way to
     be told otherwise - GTEXT_YAML_Write_Options carries no schema - so a
     document parsed under the JSON schema, where "~" is not a null spelling,
     is written as "[~]" and reads back under that same schema as the string
     "~".  Nothing is wrong with the writer there; it was never given the
     question.  Under the failsafe schema every scalar is a string and the
     same applies.  So the value comparison is made where the writer's
     dialect and the reader's agree, and the weaker property - that what the
     writer produced parses - is checked everywhere.  "0755" is the integer 493 in 1.1 and the string "0755" in 1.2,
     and a writer that emits the digits it was given is right both times - so
     reading its output under the other dialect would report a defect that is
     the harness changing the question.  Reading it under the same one asks
     what this property is for: did the spelling survive the trip. */
  GTEXT_YAML_Parse_Options popts = *read_with;
  /* The question is whether this library can read back what it wrote, not
     whether the default duplicate-key policy likes the document. A DOM may
     legitimately hold two equal keys - nothing in the API prevents it - and
     writing it faithfully is the writer doing its job; refusing to read it
     again is the reader doing its own. "{: , : }" was the fuzzer's way of
     asking, and it is not a defect. */
  popts.dupkeys = GTEXT_YAML_DUPKEY_KEEP_ALL;
  GTEXT_YAML_Document * back =
      gtext_yaml_parse(text.data(), text.size(), &popts, &err);
  if (!back) {
    /* Printed before the trap, because an artifact is a handful of bytes that
       says nothing on its own: what makes one triageable is seeing what the
       writer produced. */
    fprintf(stderr, "the writer wrote what the parser refuses:\n  %s\n",
            legible(text.data(), text.size()).c_str());
    gtext_yaml_error_free(&err);
    __builtin_trap();
  }
  gtext_yaml_error_free(&err);

  bool a_ok = false, b_ok = false;
  std::string a = values_of(doc, &a_ok);
  std::string b = values_of(back, &b_ok);
  gtext_yaml_free(back);
  /* Not every document converts to JSON - a non-string key, a cycle - and
     then there is nothing to compare.  Only a pair that both converted says
     anything. */
  if (compare_values && a_ok && b_ok && a != b) {
    fprintf(stderr, "the writer wrote a different document:\n"
                    "  wrote  %s\n  before %s\n  after  %s\n",
            legible(text.data(), text.size()).c_str(), a.c_str(), b.c_str());
    __builtin_trap();
  }
}

/** A cursor over the fuzzer's bytes, used to drive the DOM builder. */
struct Bytes {
  const uint8_t * p;
  size_t n;
  size_t i = 0;
  uint8_t next() { return i < n ? p[i++] : 0; }
  bool done() const { return i >= n; }
  /* A run of bytes, used raw: this is the point of the exercise, since a
     value built through the API is under no obligation to be text the parser
     would have accepted.

     @p mask bounds the run.  Fifteen bytes was the only length available
     before, which is shorter than every rule that depends on length: a line
     width to fold at, a literal block worth writing as one, a scalar long
     enough for the writer to have to choose. */
  std::string chunk(uint8_t mask = 0x0F) {
    size_t len = next() & mask;
    std::string s;
    for (size_t k = 0; k < len && !done(); k++) s.push_back((char)next());
    return s;
  }
};

/* The dialect, from the five spare bits of the mode byte.
 *
 * Fixed defaults before, which left whole branches of the resolver with no
 * way in: 1.1's sexagesimal integers, octals and extra booleans, the failsafe
 * and JSON schemas, and the key policies.  The sexagesimal rows held a defect
 * for as long as this harness has existed and could not have found it. */
GTEXT_YAML_Parse_Options parse_options_from(uint8_t knobs) {
  GTEXT_YAML_Parse_Options p = gtext_yaml_parse_options_default();
  /* Bounded so a document cannot spend the whole run expanding aliases. */
  p.max_alias_expansion = 1000;
  p.yaml_1_1 = (knobs & 0x01) != 0;
  switch ((knobs >> 1) & 0x03) {
    case 0: p.schema = GTEXT_YAML_SCHEMA_FAILSAFE; break;
    case 1: p.schema = GTEXT_YAML_SCHEMA_JSON; break;
    default: p.schema = GTEXT_YAML_SCHEMA_CORE; break;
  }
  p.allow_merge_keys = (knobs & 0x08) == 0;
  p.require_string_keys = (knobs & 0x10) != 0;
  return p;
}

/* How to write it.  One byte for the choices that have to be reachable from
 * the mode header alone, and the rest off the cursor where there is one.
 *
 * The encoding is the one worth naming.  yaml-test-suite is UTF-8 throughout,
 * so nothing in the conformance run has ever put the writer's UTF-16 or
 * UTF-32 output back through the reader - and the parser's offsets were
 * counted in the decoded stream while its positional helpers read the raw
 * buffer, which broke every non-UTF-8 encoding and went unnoticed for exactly
 * that reason.  A round trip through all five is cheap here. */
GTEXT_YAML_Write_Options write_options_from(
    uint8_t sel, Bytes * extra, const GTEXT_YAML_Parse_Options * read_as) {
  GTEXT_YAML_Write_Options w = gtext_yaml_write_options_default();
  /* Write for the dialect this document will be read back in, which is the
     one it was read *from*.  Without this the round trip asked the writer to
     produce output for the 1.2 core schema and then read it under something
     else, and every disagreement between the two looked like a writer bug:
     "0:0" is the integer 0 in 1.1 and a string in 1.2, so the writer quoted
     it by the 1.2 rule and the 1.1 reader got a string back.  That one was a
     real find, and it is the missing option rather than the quoting. */
  if (read_as) {
    w.schema = read_as->schema;
    w.yaml_1_1 = read_as->yaml_1_1;
  }
  w.flow_style = (GTEXT_YAML_Flow_Style)(sel % 3);
  w.pretty = (sel & 0x04) != 0;
  w.encoding = (GTEXT_YAML_Encoding)(((sel >> 3) & 0x07) % 5);
  w.canonical = (sel & 0x40) != 0;
  w.emit_bom = (sel & 0x80) != 0;
  if (extra) {
    const uint8_t a = extra->next();
    /* Zero and one are both in range on purpose: they are the widths at which
       block nesting has no room to express itself. */
    w.indent_spaces = (int)(a & 0x0F);
    w.line_width = (int)extra->next() * 2;
    const uint8_t c = extra->next();
    w.scalar_style = (GTEXT_YAML_Scalar_Style)(c % 5);
    w.trailing_newline = (c & 0x08) != 0;
    w.newline = (c & 0x10) ? "\r\n" : "\n";
  }
  return w;
}

/* NULL for "no property", so the writer sees both the absent and the hostile
   case. */
const char * maybe(Bytes & b, std::string * store) {
  if ((b.next() & 1) == 0) return nullptr;
  *store = b.chunk();
  return store->c_str();
}

/* Anything a node can carry that is not its value.
 *
 * A stored scalar style is the one that matters most: only a plain scalar is
 * resolved by its contents (10.3.2), so the style is what separates the
 * integer 1 from the string "1", and the writer has to read it.  Nothing here
 * ever set one, so every node this harness built was in whatever style the
 * constructor chose. */
void decorate(
    GTEXT_YAML_Document * doc,
    GTEXT_YAML_Node * n,
    Bytes & b,
    bool inline_comments_safe) {
  if (!n) return;
  const uint8_t how = b.next();
  if (how & 0x01) {
    gtext_yaml_node_set_scalar_style(
        n, (GTEXT_YAML_Scalar_Style)((how >> 1) % 5));
  }
  /* A comment is text the writer puts in its own output and the reader must
     then step over.  One containing a line break is the interesting case and
     is left in, because what the writer does with it is the question. */
  if (how & 0x10) {
    std::string c = b.chunk();
    gtext_yaml_node_set_leading_comment(doc, n, c.c_str());
  }
  /* Inline comments are left out, and this is a gate to remove rather than a
   * decision.
   *
   * An inline comment inside a *flow* collection swallows the rest of the
   * line, closing bracket included: "[x # note, y]" is an unterminated flow
   * sequence, and the writer says OK.  That is an open defect on the YAML
   * format page.  Nothing here can route around it, because whether a given
   * node ends up in flow style is the writer's decision and not one the
   * builder can predict - a tagged collection and an empty one both reach
   * flow style with the block option set.  So this stays off until the
   * writer breaks the line after a comment in flow context, rather than
   * having every campaign rediscover the same defect on its way to anything
   * else.
   *
   * A leading comment has a line of its own and is safe everywhere: in flow
   * context the writer drops it, which loses a comment and no values. */
  (void)inline_comments_safe;
}

GTEXT_YAML_Node * build(
    GTEXT_YAML_Document * doc, Bytes & b, int depth, bool inline_comments_safe) {
  std::string tag_s, anchor_s;
  const char * tag = maybe(b, &tag_s);
  const char * anchor = maybe(b, &anchor_s);
  /* Seven kinds where there were three.  !!set, !!omap and !!pairs are node
     types of their own with their own writer paths, and a DOM holding one
     could not be built here at all; a typed scalar is how a caller says the
     text and the type disagree, which is the shape that reached the
     undefined (int64_t) of an infinity. */
  uint8_t kind = b.done() || depth > 6 ? 0 : (b.next() % 7);

  if (kind == 0 || kind == 1) {
    /* 0x3F rather than 0x0F: long enough for a line width to bite and for a
       literal block to be worth writing as one. */
    std::string v = b.chunk(0x3F);
    GTEXT_YAML_Node * n = nullptr;
    if (kind == 0) {
      n = gtext_yaml_node_new_scalar(doc, v.c_str(), tag, anchor);
    }
    else {
      /* The type the text carries, or string, which carries any text.
       *
       * Not an arbitrary type, for two reasons that used to be one.
       *
       * It used to be a correctness problem: the typed constructor took the
       * caller's word where no tag was present, so an arbitrary type built a
       * node declared null whose text was "NO", and *nothing* could write
       * that node correctly.  Canonical form emitted '!!null "NO"', which
       * this library refuses to read; plain form emitted NO, which reads back
       * as a string.  Asserting a round trip on it asked the writer for
       * something that does not exist.  The constructor refuses that claim
       * now, tag or no tag, so the trap is gone.
       *
       * What is left is coverage.  An arbitrary type is refused far more
       * often than not, and a NULL here just makes the document smaller -
       * so guessing would spend the fuzzer's budget on documents that were
       * never built.  The refusal itself is a unit test, where it belongs. */
      /* _n, with the length: a chunk can hold a NUL, and c_str() would stop
         the probe there while the node below takes the whole run - so the
         probe answered "int" for "622222222222\0\0..." and built a node
         declared int whose text was not one.  That contradiction is the
         harness's own, and it cost a triage to find that out. */
      /* With the tag, because a tag decides the type too: one this library
         does not resolve makes a scalar a string (10.3.2's failsafe), so
         probing without it declared a node null that its own tag says is a
         string - and the writer then emitted the tag with no value, where
         the reader found the empty string. */
      GTEXT_YAML_Node * probe =
          gtext_yaml_node_new_scalar_n(doc, v.data(), v.size(), tag, nullptr);
      GTEXT_YAML_Node_Type t = probe ? gtext_yaml_node_type(probe)
                                     : GTEXT_YAML_STRING;
      if (b.next() & 1) t = GTEXT_YAML_STRING;
      n = gtext_yaml_node_new_scalar_typed(
          doc, v.data(), v.size(), t, tag, anchor);
    }
    decorate(doc, n, b, inline_comments_safe);
    return n;
  }
  if (kind == 2 || kind == 3 || kind == 4) {
    /* !!omap and !!pairs are sequence-shaped and take the same appender. */
    GTEXT_YAML_Node * seq =
        kind == 2 ? gtext_yaml_node_new_sequence(doc, tag, anchor)
      : kind == 3 ? gtext_yaml_node_new_omap(doc, tag, anchor)
                  : gtext_yaml_node_new_pairs(doc, tag, anchor);
    size_t count = b.next() & 0x07;
    for (size_t k = 0; k < count && !b.done() && seq; k++) {
      GTEXT_YAML_Node * child = build(doc, b, depth + 1, inline_comments_safe);
      if (!child) break;
      seq = gtext_yaml_sequence_append(doc, seq, child);
    }
    decorate(doc, seq, b, inline_comments_safe);
    return seq;
  }
  /* And !!set is mapping-shaped. */
  GTEXT_YAML_Node * map =
      kind == 5 ? gtext_yaml_node_new_mapping(doc, tag, anchor)
                : gtext_yaml_node_new_set(doc, tag, anchor);
  size_t count = b.next() & 0x07;
  for (size_t k = 0; k < count && !b.done() && map; k++) {
    GTEXT_YAML_Node * key = build(doc, b, depth + 1, inline_comments_safe);
    GTEXT_YAML_Node * value = build(doc, b, depth + 1, inline_comments_safe);
    if (!key || !value) break;
    map = gtext_yaml_mapping_set(doc, map, key, value);
  }
  decorate(doc, map, b, inline_comments_safe);
  return map;
}

/** One built document, or null if nothing could be made of the bytes left. */
GTEXT_YAML_Document * build_document(Bytes & b, bool inline_comments_safe) {
  GTEXT_YAML_Document * doc = gtext_yaml_document_new(nullptr, nullptr);
  if (!doc) return nullptr;
  GTEXT_YAML_Node * root = build(doc, b, 0, inline_comments_safe);
  if (root) gtext_yaml_document_set_root(doc, root);
  return doc;
}

/** Documents the parser could never hand the writer. */
void fuzz_built_dom(const uint8_t * data, size_t size, uint8_t sel) {
  Bytes b{data, size};
  GTEXT_YAML_Write_Options wopts = write_options_from(sel, &b, nullptr);
  GTEXT_YAML_Document * doc = build_document(
      b, wopts.flow_style == GTEXT_YAML_FLOW_STYLE_BLOCK);
  if (!doc) return;

  std::string text;
  if (write_document(doc, &wopts, &text)) {
    /* A built document was never parsed, so there is no dialect it came from;
       the default is the one a caller reading the file back would use. */
    GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
    popts.max_alias_expansion = 1000;
    must_round_trip(doc, text, &popts, true);
  }
  gtext_yaml_free(doc);
}

/* Several built documents, written as a stream.
 *
 * gtext_yaml_write_documents() had no path here at all, and it is not
 * gtext_yaml_write_document() in a loop: it has to put a "---" in front of
 * each one and decide what a "..." closes, which is where a directive that
 * did not close the document before it was found by hand. */
void fuzz_built_multidoc(const uint8_t * data, size_t size, uint8_t sel) {
  Bytes b{data, size};
  GTEXT_YAML_Write_Options wopts = write_options_from(sel, &b, nullptr);
  const size_t count = (b.next() & 0x03) + 1;

  GTEXT_YAML_Document * docs[4] = {nullptr, nullptr, nullptr, nullptr};
  size_t made = 0;
  for (size_t k = 0; k < count; k++) {
    docs[made] = build_document(
        b, wopts.flow_style == GTEXT_YAML_FLOW_STYLE_BLOCK);
    if (!docs[made]) break;
    made++;
  }
  if (made == 0) return;

  GTEXT_YAML_Sink sink;
  if (gtext_yaml_sink_buffer(&sink) == GTEXT_YAML_OK) {
    if (gtext_yaml_write_documents(docs, made, &sink, &wopts) == GTEXT_YAML_OK) {
      std::string text(gtext_yaml_sink_buffer_data(&sink),
                       gtext_yaml_sink_buffer_size(&sink));
      GTEXT_YAML_Error err;
      memset(&err, 0, sizeof(err));
      GTEXT_YAML_Parse_Options popts = gtext_yaml_parse_options_default();
      popts.max_alias_expansion = 1000;
      popts.dupkeys = GTEXT_YAML_DUPKEY_KEEP_ALL;
      size_t back_count = 0;
      GTEXT_YAML_Document ** back = gtext_yaml_parse_all(
          text.data(), text.size(), &back_count, &popts, &err);
      if (!back) {
        fprintf(stderr, "the multi-document writer wrote what the parser "
                        "refuses:\n  %s\n",
                legible(text.data(), text.size()).c_str());
        gtext_yaml_error_free(&err);
        __builtin_trap();
      }
      /* How many came back is the property this path is here for: a stream of
         three documents that reads back as one has lost a marker. */
      if (back_count != made) {
        fprintf(stderr, "wrote %zu documents and read back %zu:\n  %s\n",
                made, back_count, legible(text.data(), text.size()).c_str());
        __builtin_trap();
      }
      for (size_t k = 0; k < back_count; k++) gtext_yaml_free(back[k]);
      free(back);
      gtext_yaml_error_free(&err);
    }
    gtext_yaml_sink_buffer_free(&sink);
  }
  for (size_t k = 0; k < made; k++) gtext_yaml_free(docs[k]);
}

/** Documents that did come from parsing - the round trip, unbounded. */
void fuzz_parsed(const uint8_t * data, size_t size, uint8_t knobs, uint8_t sel) {
  GTEXT_YAML_Error err;
  memset(&err, 0, sizeof(err));
  GTEXT_YAML_Parse_Options popts = parse_options_from(knobs);
  GTEXT_YAML_Document * doc = gtext_yaml_parse(
      reinterpret_cast<const char *>(data), size, &popts, &err);
  gtext_yaml_error_free(&err);
  if (!doc) return;

  GTEXT_YAML_Write_Options wopts = write_options_from(sel, nullptr, &popts);
  std::string text;
  if (write_document(doc, &wopts, &text)) {
    must_round_trip(doc, text, &popts,
                    popts.schema == GTEXT_YAML_SCHEMA_CORE);
  }
  gtext_yaml_free(doc);
}

/** The same, for a stream of them. */
void fuzz_parsed_multidoc(
    const uint8_t * data, size_t size, uint8_t knobs, uint8_t sel) {
  GTEXT_YAML_Error err;
  memset(&err, 0, sizeof(err));
  GTEXT_YAML_Parse_Options popts = parse_options_from(knobs);
  popts.dupkeys = GTEXT_YAML_DUPKEY_KEEP_ALL;
  size_t count = 0;
  GTEXT_YAML_Document ** docs = gtext_yaml_parse_all(
      reinterpret_cast<const char *>(data), size, &count, &popts, &err);
  gtext_yaml_error_free(&err);
  if (!docs) return;

  if (count > 0) {
    GTEXT_YAML_Write_Options wopts = write_options_from(sel, nullptr, &popts);
    GTEXT_YAML_Sink sink;
    if (gtext_yaml_sink_buffer(&sink) == GTEXT_YAML_OK) {
      if (gtext_yaml_write_documents(docs, count, &sink, &wopts)
          == GTEXT_YAML_OK) {
        std::string text(gtext_yaml_sink_buffer_data(&sink),
                         gtext_yaml_sink_buffer_size(&sink));
        memset(&err, 0, sizeof(err));
        size_t back_count = 0;
        GTEXT_YAML_Document ** back = gtext_yaml_parse_all(
            text.data(), text.size(), &back_count, &popts, &err);
        if (!back) {
          /* The parser's own complaint, first: this file's triage lesson is
             that it is shorter and truer than anything read off the bytes, and
             this path was printing the bytes alone - so every find here began
             with decoding a screenful of UTF-16 and guessing. */
          fprintf(stderr, "the multi-document writer wrote what the parser "
                          "refuses: %s (line %d col %d)\n"
                          "  in    %s\n  wrote %s\n",
                  err.message ? err.message : "(no message)", err.line,
                  err.col,
                  legible(reinterpret_cast<const char *>(data), size).c_str(),
                  legible(text.data(), text.size()).c_str());
          gtext_yaml_error_free(&err);
          __builtin_trap();
        }
        if (back_count != count) {
          fprintf(stderr, "wrote %zu documents and read back %zu:\n"
                          "  in    %s\n  wrote %s\n",
                  count, back_count,
                  legible(reinterpret_cast<const char *>(data), size).c_str(),
                  legible(text.data(), text.size()).c_str());
          __builtin_trap();
        }
        for (size_t k = 0; k < back_count; k++) gtext_yaml_free(back[k]);
        free(back);
        gtext_yaml_error_free(&err);
      }
      gtext_yaml_sink_buffer_free(&sink);
    }
  }
  for (size_t k = 0; k < count; k++) gtext_yaml_free(docs[k]);
  free(docs);
}

/* The streaming parser straight into the streaming writer, with no DOM in
 * between.  The types fit and the two are not a pipe: the writer takes
 * composed events, and the parser reports structure as it was written - the
 * ":" of a block mapping, the "-" of a block sequence, the "," between two
 * flow entries - leaving composing to its consumer.  So almost everything is
 * refused here, which is the point: the writer used to answer an indicator
 * with OK and write nothing, and a caller who joined the two got no error and
 * a document with its structure gone.
 *
 * What still reaches the writer is a lone scalar with its directives, which
 * is the one shape the two agree on, and that is worth holding to the same
 * property as everything else. */
struct Pipe {
  GTEXT_YAML_Writer * writer;
  GTEXT_YAML_Status status;
};

GTEXT_YAML_Status pipe_event(GTEXT_YAML_Stream * s, const void * ev, void * user) {
  (void)s;
  Pipe * p = static_cast<Pipe *>(user);
  if (p->status != GTEXT_YAML_OK) return p->status;
  p->status = gtext_yaml_writer_event(
      p->writer, static_cast<const GTEXT_YAML_Event *>(ev));
  return p->status;
}

void fuzz_event_pipe(const uint8_t * data, size_t size, uint8_t knobs) {
  /* The event API resolves nothing - an alias to an anchor nobody declared is
     an event like any other - so the stream accepts input the DOM parser
     refuses, and the writer is right to hand back something equally
     unresolvable.  The property only holds where the input was a document:
     "*c%" was the fuzzer's first find, and it was this harness being wrong
     rather than the writer. */
  {
    GTEXT_YAML_Error probe;
    memset(&probe, 0, sizeof(probe));
    GTEXT_YAML_Parse_Options popts = parse_options_from(knobs);
    GTEXT_YAML_Document * doc = gtext_yaml_parse(
        reinterpret_cast<const char *>(data), size, &popts, &probe);
    gtext_yaml_error_free(&probe);
    if (!doc) return;
    gtext_yaml_free(doc);
  }

  GTEXT_YAML_Sink sink;
  if (gtext_yaml_sink_buffer(&sink) != GTEXT_YAML_OK) return;
  GTEXT_YAML_Parse_Options popts = parse_options_from(knobs);
  GTEXT_YAML_Write_Options wopts = gtext_yaml_write_options_default();
  /* The dialect the events came from is the one the output goes back to. */
  wopts.schema = popts.schema;
  wopts.yaml_1_1 = popts.yaml_1_1;
  Pipe pipe{gtext_yaml_writer_new(sink, &wopts), GTEXT_YAML_OK};
  if (!pipe.writer) { gtext_yaml_sink_buffer_free(&sink); return; }

  popts.dupkeys = GTEXT_YAML_DUPKEY_KEEP_ALL;
  GTEXT_YAML_Stream * st = gtext_yaml_stream_new(&popts, pipe_event, &pipe);
  if (!st) {
    gtext_yaml_writer_free(pipe.writer);
    gtext_yaml_sink_buffer_free(&sink);
    return;
  }
  GTEXT_YAML_Status status = gtext_yaml_stream_feed(
      st, reinterpret_cast<const char *>(data), size);
  if (status == GTEXT_YAML_OK) status = gtext_yaml_stream_finish(st);
  if (status == GTEXT_YAML_OK) status = pipe.status;
  if (status == GTEXT_YAML_OK) status = gtext_yaml_writer_finish(pipe.writer);

  if (status == GTEXT_YAML_OK) {
    std::string text(gtext_yaml_sink_buffer_data(&sink),
                     gtext_yaml_sink_buffer_size(&sink));
    GTEXT_YAML_Error err;
    memset(&err, 0, sizeof(err));
    GTEXT_YAML_Document * back =
        gtext_yaml_parse(text.data(), text.size(), &popts, &err);
    gtext_yaml_error_free(&err);
    if (!back) {
      /* Printed before the trap, for the same reason must_round_trip() prints:
         an artifact is a handful of bytes and says nothing on its own. This
         path used to trap in silence, which made every find here a fresh
         reverse-engineering job. */
      fprintf(stderr, "the event pipe wrote what the parser refuses:\n"
                      "  in    %s\n  wrote %s\n",
              legible(reinterpret_cast<const char *>(data), size).c_str(),
              legible(text.data(), text.size()).c_str());
      __builtin_trap();
    }

    /* And it must be the same document, not merely a readable one. */
    GTEXT_YAML_Document * from = gtext_yaml_parse(
        reinterpret_cast<const char *>(data), size, &popts, &err);
    gtext_yaml_error_free(&err);
    if (from) {
      bool a_ok = false, b_ok = false;
      std::string a = values_of(from, &a_ok);
      std::string b = values_of(back, &b_ok);
      gtext_yaml_free(from);
      if (a_ok && b_ok && a != b) {
        fprintf(stderr, "the event pipe wrote a different document:\n"
                        "  in     %s\n  wrote  %s\n"
                        "  before %s\n  after  %s\n",
                legible(reinterpret_cast<const char *>(data), size).c_str(),
                legible(text.data(), text.size()).c_str(),
                a.c_str(), b.c_str());
        __builtin_trap();
      }
    }
    gtext_yaml_free(back);
  }

  gtext_yaml_stream_free(st);
  gtext_yaml_writer_free(pipe.writer);
  gtext_yaml_sink_buffer_free(&sink);
}

}  // namespace

/* Two header bytes, then the document - the shape the CSV harness already
 * uses.  The first picks the path and, in its upper five bits, the dialect to
 * parse in; the second is how to write.  Block against flow used to be two
 * separate paths, which is a coarser way of saying one field of the write
 * options and left the third, AUTO, unreachable.
 *
 * A corpus unit written for the one-byte header still selects a path, but its
 * first document byte is now the write-options byte, so what the old corpus
 * means has shifted.  It regrows in minutes, and the reproducers that matter
 * are unit tests. */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 3) return 0;
  const uint8_t mode = data[0];
  const uint8_t knobs = mode >> 3;
  const uint8_t sel = data[1];
  const uint8_t * body = data + 2;
  const size_t len = size - 2;

  switch (mode % 5) {
    case 0: fuzz_built_dom(body, len, sel); break;
    case 1: fuzz_built_multidoc(body, len, sel); break;
    case 2: fuzz_parsed(body, len, knobs, sel); break;
    case 3: fuzz_parsed_multidoc(body, len, knobs, sel); break;
    default: fuzz_event_pipe(body, len, knobs); break;
  }
  return 0;
}
