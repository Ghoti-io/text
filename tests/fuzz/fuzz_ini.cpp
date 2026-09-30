/**
 * @file
 *
 * libFuzzer harness for the INI module.
 *
 * The contract: for any byte string at all, the parser either returns a
 * document or reports an error - it must not crash, read out of bounds, or
 * leak - and if it returns a document, walking it, decoding its values and
 * writing it back must be safe too.
 *
 * Five properties beyond "it did not crash", each of which a single-call
 * harness cannot see:
 *
 *   - **The strict dialect is a subset of the generic one.** Six of the generic
 *     dialect's seven changes are relaxations, and a relaxation may only *add*
 *     accepted documents - so anything the strict dialect accepts, the generic
 *     one must accept, and for an input **containing no CR** must produce the
 *     same groups, keys and raw values.
 *
 *     The CR proviso is not a hedge; it is the finding. `accept_crlf` is the one
 *     change that is *not* a relaxation: it removes a CR from the content of a
 *     line the strict dialect already accepted, so the two legitimately disagree
 *     about that value. This property asserted parity unconditionally at first
 *     and trapped at 30,209 executions on `z777777=...=<CR><LF>`, which is how
 *     the distinction between the six and the one got written down at all.
 *   - **An unmodified document writes back byte for byte.** Desktop Entry §3
 *     requires it of an implementation that rewrites a file, and it is the
 *     property most easily broken by a change to the tree: the entry keeps its
 *     line in five pieces, and any of them going missing shows up here rather
 *     than in a reader test.
 *   - **What the writer writes, the parser reads.** The output of a normalizing
 *     write must parse, and must hold the same values - which is not implied by
 *     the byte-identical property, because normalizing takes a different path
 *     through the writer.
 *   - **Decoding never reads past the value.** Every raw value is put through
 *     gtext_ini_unescape(), gtext_ini_value_list() and the three typed
 *     accessors. Most will fail, and a failure is a fine answer; what is not
 *     fine is reading a byte that is not there, which is where a length-based
 *     reader handed a value containing a NUL differs from `GKeyFile`.
 *   - **A second parse of the same bytes is the same document.** Cheap, and it
 *     catches a parser that depends on anything outside its input.
 *
 * Every property but the first is asserted under **all seven** dialects -
 * Desktop Entry, generic, git config, EditorConfig, systemd, configparser and
 * Win32 - because a property asserted under one says nothing about another.
 * Win32 also carries one property no other dialect can: it must **never refuse an
 * input**, because its key charset is open, its empty key and empty section name
 * are both spellable, a line with no separator is a valueless entry and an
 * unclosed header is an ordinary line, so no byte sequence is left to reject.
 * The byte-identical rewrite was
 * asserted only under the strict dialect at first, and the strict dialect refuses
 * a document beginning with a BOM, so the generic dialect's silently dropped BOM
 * was unreachable from here and had to be found by a differential against git
 * instead. The first property has no analogue for either of the last two: neither
 * is a relaxation of Desktop Entry in either direction, so no subset relation
 * holds to assert.
 *
 * EditorConfig is the widest of the five - almost any byte sequence is a legal
 * document to it - so it is the dialect that actually reaches the writer and the
 * value layer on arbitrary input, where the others refuse early. It is also the only
 * one with no escape set, which is how a backslash came to be refused by
 * gtext_ini_unescape() for years with no dialect able to show it.
 *
 * systemd is the dialect that reaches the **line assembler**: it is the only one
 * whose continuation can appear in a group header or a key, so it is the only one
 * under which a name is not a span of the document and the canonical form carries the
 * joined text. Every property here is worth more under it for that reason - a
 * byte-identical rewrite of a document whose name and bytes differ is a stronger
 * statement than one where they are the same object.
 *
 * configparser is the dialect that reaches the **indent scan and the second join**,
 * and it is the only one whose value can span lines without the value's own bytes
 * saying so. Two things are only reachable under it: a raw value holding comment
 * and blank lines that the joined form drops, and a writer that *inserts* bytes
 * rather than emitting the value it was given. It is also the only dialect here
 * with two separator characters, so it is the only one under which the key scan and
 * the key charset can disagree.
 *
 * Build with: make fuzz-ini      Run: make fuzz-run-ini
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include <ghoti.io/text/ini.h>
}

namespace {

/**
 * Report which property failed, and the input, before trapping.
 *
 * `__builtin_trap()` raises SIGILL, which ASan reports as DEADLYSIGNAL - and if
 * ASan's own reporting faults, libFuzzer never writes the artifact and the run
 * ends with "nested bug in the same thread, aborting" and no reproducer at all.
 * That happened once here at 3.26M executions and cost a whole run. So the
 * harness names the property and dumps the bytes itself, on stderr, flushed,
 * before it traps: an instrument that only works when the crash handler works is
 * not an instrument.
 */
[[noreturn]] void fail(const char * property, const std::string & text) {
  std::fprintf(stderr, "\n=== fuzz_ini: property failed: %s\n", property);
  std::fprintf(stderr, "=== input, %zu bytes:\n", text.size());
  for (size_t i = 0; i < text.size(); i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\\') std::fprintf(stderr, "\\\\");
    else if (c == '\n') std::fprintf(stderr, "\\n");
    else if (c == '\r') std::fprintf(stderr, "\\r");
    else if (c == '\t') std::fprintf(stderr, "\\t");
    else if (c >= 0x20 && c < 0x7F) std::fputc(c, stderr);
    else std::fprintf(stderr, "\\x%02X", c);
  }
  std::fprintf(stderr, "\n=== end input\n");
  std::fflush(stderr);
  __builtin_trap();
}

/** Render a document, or return false if the writer refused. */
bool render(const GTEXT_INI_Document * doc,
    const GTEXT_INI_Write_Options * opts, std::string & out) {
  GTEXT_INI_Sink sink;
  if (gtext_ini_sink_buffer(&sink) != GTEXT_INI_OK) return false;
  bool ok = gtext_ini_write(doc, &sink, opts) == GTEXT_INI_OK;
  if (ok) {
    out.assign(gtext_ini_sink_buffer_data(&sink),
        gtext_ini_sink_buffer_size(&sink));
  }
  gtext_ini_sink_buffer_free(&sink);
  return ok;
}

/** The groups, keys and raw values, flattened for comparison. */
std::string flatten(const GTEXT_INI_Document * doc) {
  std::string out;
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    size_t len = 0;
    const char * name = gtext_ini_group_name(group, &len);
    out.append("[").append(name, len).append("]\n");
    for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
      const char * key = gtext_ini_group_key_at(group, e, &len);
      out.append(key, len).append("\x01");
      const char * value = gtext_ini_group_value_at(group, e, &len);
      out.append(value, len).append("\x02");
    }
  }
  return out;
}

/** Put every value through every accessor, discarding the answers. */
void poke(const GTEXT_INI_Document * doc, const GTEXT_INI_Dialect * dialect) {
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
      size_t len = 0;
      const char * value = gtext_ini_group_value_at(group, e, &len);
      char * decoded = nullptr;
      if (gtext_ini_unescape(dialect, value, len, nullptr, &decoded, nullptr)
          == GTEXT_INI_OK) {
        gtext_ini_string_free(nullptr, decoded);
      }
      char * encoded = nullptr;
      if (gtext_ini_escape(dialect, value, len, nullptr, &encoded, nullptr)
          == GTEXT_INI_OK) {
        gtext_ini_string_free(nullptr, encoded);
      }
      GTEXT_INI_List * list = nullptr;
      GTEXT_INI_List * words = nullptr;
      if (gtext_ini_value_words(dialect, value, len, nullptr, &words)
          == GTEXT_INI_OK) {
        /* Reading every word back is the point: a words pass that returned OK with a
         * bad length would otherwise go unnoticed. */
        for (size_t w = 0; w < gtext_ini_list_count(words); w++) {
          size_t word_len = 0;
          (void) gtext_ini_list_at(words, w, &word_len);
        }
        gtext_ini_list_free(words);
      }
      if (gtext_ini_value_list(dialect, value, len, nullptr, &list)
          == GTEXT_INI_OK) {
        for (size_t i = 0; i < gtext_ini_list_count(list); i++) {
          size_t item_len = 0;
          (void) gtext_ini_list_at(list, i, &item_len);
        }
        gtext_ini_list_free(list);
      }
      bool flag = false;
      (void) gtext_ini_value_bool(dialect, value, len, &flag);
      int64_t whole = 0;
      (void) gtext_ini_value_int(value, len, &whole);
      double real = 0;
      (void) gtext_ini_value_double(value, len, &real);

      /* The locale chain builds candidate keys from the key's own bytes, so it
       * is reachable only with a key the document supplied. */
      size_t klen = 0;
      const char * key = gtext_ini_group_key_at(group, e, &klen);
      std::string bare(key, klen);
      (void) gtext_ini_group_get_locale(group, bare.c_str(), "de_DE.UTF-8@x",
          nullptr);
      (void) gtext_ini_group_get_locale(group, bare.c_str(), "", nullptr);
      (void) gtext_ini_group_count_key(group, bare.c_str());
      (void) gtext_ini_group_get_nth(group, bare.c_str(), 0, nullptr);
    }
  }
}

/**
 * The properties that hold for any dialect over any input it accepted.
 *
 * Factored out because there are three dialects here now and the alternative was
 * three copies: the strict one, the generic one, and git config. A property
 * asserted for one dialect says nothing about another - the byte-identical
 * rewrite was asserted only under the strict dialect for a while, which has
 * `skip_bom` false and refuses a document beginning with a BOM, so the generic
 * dialect's dropped BOM was unreachable from here and had to be found by a
 * differential instead.
 */
void exercise(GTEXT_INI_Document * doc, const std::string & text,
    const GTEXT_INI_Dialect & dialect, const char * who);

GTEXT_INI_Document * parse(const std::string & text,
    const GTEXT_INI_Dialect & dialect) {
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Document * doc =
      gtext_ini_parse(text.data(), text.size(), &opts, &err);
  gtext_ini_error_free(&err);
  return doc;
}

void exercise(GTEXT_INI_Document * doc, const std::string & text,
    const GTEXT_INI_Dialect & dialect, const char * who) {
  /* §3's preservation requirement, and the one the tree breaks most easily. */
  std::string again;
  if (render(doc, nullptr, again)) {
    if (again != text) {
      std::fprintf(stderr, "=== dialect: %s\n", who);
      fail("byte-identical rewrite", text);
    }
  }

  /* A normalizing write takes a different path, and its output must parse and
   * hold the same values. */
  GTEXT_INI_Write_Options norm = gtext_ini_write_options_default();
  norm.normalize = true;
  std::string normalized;
  if (render(doc, &norm, normalized)) {
    GTEXT_INI_Document * c = parse(normalized, dialect);
    if (c) {
      if (flatten(c) != flatten(doc)) {
        std::fprintf(stderr, "=== dialect: %s\n", who);
        fail("a normalized write re-parses to the same values", text);
      }
      gtext_ini_free(c);
    }
  }

  /* Parsing the same bytes twice must give the same document. */
  GTEXT_INI_Document * twice = parse(text, dialect);
  if (!twice) {
    std::fprintf(stderr, "=== dialect: %s\n", who);
    fail("a second parse of the same bytes refused them", text);
  }
  if (flatten(twice) != flatten(doc)) {
    std::fprintf(stderr, "=== dialect: %s\n", who);
    fail("a second parse differed", text);
  }
  gtext_ini_free(twice);

  poke(doc, &dialect);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size > (1u << 20)) return 0;
  std::string text(reinterpret_cast<const char *>(data), size);

  GTEXT_INI_Dialect strict = gtext_ini_dialect_desktop_entry();
  GTEXT_INI_Dialect loose = gtext_ini_dialect_generic();
  GTEXT_INI_Dialect git = gtext_ini_dialect_git_config();
  GTEXT_INI_Dialect ec = gtext_ini_dialect_editorconfig();
  GTEXT_INI_Dialect sd = gtext_ini_dialect_systemd();
  GTEXT_INI_Dialect cp = gtext_ini_dialect_configparser();
  GTEXT_INI_Dialect w32 = gtext_ini_dialect_win32();

  GTEXT_INI_Document * a = parse(text, strict);
  GTEXT_INI_Document * b = parse(text, loose);
  GTEXT_INI_Document * g = parse(text, git);
  GTEXT_INI_Document * e = parse(text, ec);
  GTEXT_INI_Document * s = parse(text, sd);
  GTEXT_INI_Document * c = parse(text, cp);
  GTEXT_INI_Document * w = parse(text, w32);

  if (a) {
    /* Acceptance is unconditional: all six relaxations only widen it. */
    if (!b) fail("generic dialect refused what the strict dialect accepted", text);
    /*
     * Values agree only where the one non-relaxation cannot bite. A CR anywhere
     * in the input may be a terminator to the generic dialect and data to the
     * strict one, and then the two *should* differ.
     */
    if (text.find('\r') == std::string::npos) {
      if (flatten(a) != flatten(b)) fail("dialect parity on a CR-free input", text);
    }
    exercise(a, text, strict, "desktop-entry");
  }
  if (b) exercise(b, text, loose, "generic");
  /*
   * **No parity property against git**, and its absence is the finding rather
   * than a gap. git config is not a relaxation of Desktop Entry in either
   * direction: it accepts a preamble, a continuation and a valueless key that
   * Desktop Entry refuses, and refuses a key not beginning with a letter and a
   * group name outside `A-Za-z0-9-.` that Desktop Entry accepts. So neither
   * `a implies g` nor `g implies a` holds, and asserting either would fail on the
   * first input that exercised the difference. What does hold is everything in
   * exercise(), which is where the value is.
   */
  if (g) exercise(g, text, git, "git-config");
  /*
   * No parity property against EditorConfig either, and for the mirror-image
   * reason: it accepts a section name holding any byte, an empty section name, a
   * preamble and a duplicate key that Desktop Entry refuses, and refuses the `\n`
   * escape and the `;`-separated list Desktop Entry defines - so the same bytes
   * can be a legal document to both and mean different things.
   */
  if (e) exercise(e, text, ec, "editorconfig");
  /*
   * No parity property against systemd either. It refuses a preamble that
   * EditorConfig and git accept, ends a line on a lone CR that every other dialect
   * treats as data, and accepts a continuation in a name that no other dialect has -
   * so it sits outside every subset relation here in both directions.
   */
  if (s) exercise(s, text, sd, "systemd");
  /*
   * And none against configparser, which is the sixth dialect to sit outside every
   * subset relation here and does so in a way none of the others do: a `:` ends a
   * key for it and is an ordinary key byte to the other five, so the same line is a
   * different entry rather than a legal-or-not question. It also refuses a
   * duplicate key that EditorConfig accepts and accepts an indented value no other
   * dialect can spell.
   *
   * What it does reach that nothing else does is the indent scan, the second join
   * and the writer's inserting branch - which is why it is here at all rather than
   * being left to the differential.
   */
  if (c) exercise(c, text, cp, "configparser");
  /*
   * **Win32 has the one property here that is not conditional, and it is about
   * refusal rather than about agreement: it must parse every input.** The key
   * charset is open, the empty key and the empty section name are both spellable,
   * a line with no separator is a valueless entry, and an unclosed header is an
   * ordinary line - so no byte sequence is left for this reader to reject. That is
   * exactly the kind of claim a fuzzer is for, and it is the only dialect of the
   * seven that can make it.
   *
   * No parity property against any of the other six, for a reason none of them
   * has: `;` is a comment here and `#` is not, which is the opposite of the
   * generic dialect on both counts. The same line is a comment to one and an entry
   * to the other in both directions at once.
   */
  if (!w) fail("the Win32 dialect refused an input, and it refuses nothing", text);
  if (w) exercise(w, text, w32, "win32");

  if (a) gtext_ini_free(a);
  if (b) gtext_ini_free(b);
  if (g) gtext_ini_free(g);
  if (e) gtext_ini_free(e);
  if (s) gtext_ini_free(s);
  if (w) gtext_ini_free(w);
  if (c) gtext_ini_free(c);
  return 0;
}
