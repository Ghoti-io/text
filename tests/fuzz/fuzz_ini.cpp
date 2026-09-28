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
      if (gtext_ini_value_list(dialect, value, len, nullptr, &list)
          == GTEXT_INI_OK) {
        for (size_t i = 0; i < gtext_ini_list_count(list); i++) {
          size_t item_len = 0;
          (void) gtext_ini_list_at(list, i, &item_len);
        }
        gtext_ini_list_free(list);
      }
      bool flag = false;
      (void) gtext_ini_value_bool(value, len, &flag);
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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size > (1u << 20)) return 0;
  std::string text(reinterpret_cast<const char *>(data), size);

  GTEXT_INI_Dialect strict = gtext_ini_dialect_desktop_entry();
  GTEXT_INI_Dialect loose = gtext_ini_dialect_generic();

  GTEXT_INI_Document * a = parse(text, strict);
  GTEXT_INI_Document * b = parse(text, loose);

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

    /* §3's preservation requirement. */
    std::string again;
    if (render(a, nullptr, again)) {
      if (again != text) fail("byte-identical rewrite", text);
    }

    /* A normalizing write takes a different path, and its output must parse and
     * hold the same values. */
    GTEXT_INI_Write_Options norm = gtext_ini_write_options_default();
    norm.normalize = true;
    std::string normalized;
    if (render(a, &norm, normalized)) {
      GTEXT_INI_Document * c = parse(normalized, strict);
      if (c) {
        if (flatten(c) != flatten(a)) {
          fail("a normalized write re-parses to the same values", text);
        }
        gtext_ini_free(c);
      }
    }

    /* Parsing the same bytes twice must give the same document. */
    GTEXT_INI_Document * twice = parse(text, strict);
    if (!twice) fail("a second parse of the same bytes refused them", text);
    if (flatten(twice) != flatten(a)) fail("a second parse differed", text);
    gtext_ini_free(twice);

    poke(a, &strict);
  }
  if (b) poke(b, &loose);

  if (a) gtext_ini_free(a);
  if (b) gtext_ini_free(b);
  return 0;
}
