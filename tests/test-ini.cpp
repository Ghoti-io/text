/**
 * @file
 *
 * The INI module: the Desktop Entry dialect, measured against its
 * specification and against what its two reference implementations actually do.
 *
 * Every rule asserted here was checked against GLib's `GKeyFile` 2.84.4 and
 * `desktop-file-validate` 0.28-1 before it was written, and the cases where the
 * two references disagree with each other - or where one of them disagrees with
 * the specification - are called out by name, because those are the assertions
 * that would otherwise look arbitrary. The evidence is in
 * notes/text/INI-DIALECTS.md §A.2, §A.3 and §A.9.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <clocale>
#include <cstring>
#include <string>
#include <gtest/gtest.h>

#include <ghoti.io/text/ini.h>

namespace {

/** Parse, expecting success. */
GTEXT_INI_Document * ok(const std::string & text,
    GTEXT_INI_Dialect dialect = gtext_ini_dialect_desktop_entry()) {
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  GTEXT_INI_Document * doc =
      gtext_ini_parse(text.data(), text.size(), &opts, &err);
  if (!doc) {
    ADD_FAILURE() << "parse failed: " << (err.message ? err.message : "?")
                  << " at line " << err.line << " col " << err.col;
  }
  gtext_ini_error_free(&err);
  return doc;
}

/** Parse, expecting a particular refusal. */
void refused(const std::string & text, GTEXT_INI_Status expect,
    GTEXT_INI_Dialect dialect = gtext_ini_dialect_desktop_entry()) {
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  GTEXT_INI_Document * doc =
      gtext_ini_parse(text.data(), text.size(), &opts, &err);
  EXPECT_EQ(doc, nullptr) << "expected a refusal of: " << text;
  if (doc) {
    gtext_ini_free(doc);
  }
  else {
    EXPECT_EQ(err.code, expect);
    EXPECT_NE(err.message, nullptr);
    EXPECT_GE(err.line, 1);
  }
  gtext_ini_error_free(&err);
}

/**
 * Build a std::string from a (pointer, length) pair.
 *
 * Two statements rather than `std::string(f(&len), len)`, because the call and
 * the read of `len` are unsequenced as arguments and the read can happen first.
 * Every assertion below that had that shape was reading a stale length.
 */
std::string take(const char * data, size_t len) {
  return data ? std::string(data, len) : std::string("<null>");
}

/** The raw value of a key, as a std::string, for readable assertions. */
std::string raw(const GTEXT_INI_Document * doc, const char * group,
    const char * key) {
  size_t len = 0;
  const char * value = gtext_ini_document_get(doc, group, key, &len);
  return value ? std::string(value, len) : std::string("<absent>");
}

/** Write a document out and return the bytes. */
std::string written(const GTEXT_INI_Document * doc,
    const GTEXT_INI_Write_Options * opts = nullptr) {
  GTEXT_INI_Sink sink;
  EXPECT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
  GTEXT_INI_Status status = gtext_ini_write(doc, &sink, opts);
  EXPECT_EQ(status, GTEXT_INI_OK);
  std::string out(gtext_ini_sink_buffer_data(&sink),
      gtext_ini_sink_buffer_size(&sink));
  gtext_ini_sink_buffer_free(&sink);
  return out;
}

// ---------------------------------------------------------------- the dialect

TEST(IniDialect, DesktopEntryIsTheDefault) {
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  EXPECT_EQ(opts.dialect.id, GTEXT_INI_DIALECT_DESKTOP_ENTRY);
  /* Comments are kept by default, unlike the TOML module, because §3 requires a
   * rewrite to preserve them. A default that dropped them could not. */
  EXPECT_TRUE(opts.retain_comments);
}

TEST(IniDialect, TheGenericDialectRelaxesSevenThingsAndNothingElse) {
  GTEXT_INI_Dialect strict = gtext_ini_dialect_desktop_entry();
  GTEXT_INI_Dialect loose = gtext_ini_dialect_generic();
  /* The seven, named. A relaxation that changed how a *value* is read would not
   * be a relaxation, so these two must not move. */
  EXPECT_STREQ(loose.escapes, strict.escapes);
  EXPECT_EQ(loose.list_separator, strict.list_separator);
  EXPECT_NE(loose.comment_semicolon, strict.comment_semicolon);
  EXPECT_NE(loose.allow_leading_whitespace, strict.allow_leading_whitespace);
  EXPECT_NE(loose.allow_preamble, strict.allow_preamble);
  EXPECT_NE(loose.allow_duplicate_groups, strict.allow_duplicate_groups);
  EXPECT_NE(loose.dupkey, strict.dupkey);
  EXPECT_NE(loose.strict_key_charset, strict.strict_key_charset);
  EXPECT_NE(loose.accept_crlf, strict.accept_crlf);
}

// ----------------------------------------------------------------- the parser

TEST(IniParse, AGroupAndItsEntries) {
  GTEXT_INI_Document * doc = ok("[Desktop Entry]\nType=Application\nName=T\n");
  ASSERT_NE(doc, nullptr);
  ASSERT_EQ(gtext_ini_document_group_count(doc), 1u);
  const GTEXT_INI_Group * g = gtext_ini_document_group_at(doc, 0);
  size_t len = 0;
  const char * name = gtext_ini_group_name(g, &len);
  EXPECT_EQ(take(name, len), "Desktop Entry");
  EXPECT_EQ(gtext_ini_group_entry_count(g), 2u);
  EXPECT_EQ(raw(doc, "Desktop Entry", "Type"), "Application");
  gtext_ini_free(doc);
}

TEST(IniParse, AnEmptyDocumentIsValid) {
  /* GKeyFile accepts an empty file and reports zero groups, so a reader that
   * refused it would refuse a document the reference reads. */
  GTEXT_INI_Document * doc = ok("");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gtext_ini_document_group_count(doc), 0u);
  gtext_ini_free(doc);
}

TEST(IniParse, ASemicolonIsNotACommentInDesktopEntry) {
  /* The dialect's most surprising rule, and both references agree: GKeyFile
   * errors, and desktop-file-validate exits 1 saying the line "is not a
   * comment, a group or an entry". */
  refused("[G]\nk=v\n; not a comment\n", GTEXT_INI_E_BAD_LINE);
  GTEXT_INI_Document * doc =
      ok("[G]\nk=v\n; a comment here\n", gtext_ini_dialect_generic());
  ASSERT_NE(doc, nullptr);
  gtext_ini_free(doc);
}

TEST(IniParse, BlankAndWhitespaceOnlyLinesAreComments) {
  /* §3.1 makes a blank line a comment. A line of spaces is not literally blank
   * and GKeyFile accepts it, so refusing it would refuse real documents. */
  GTEXT_INI_Document * doc = ok("[G]\nk=v\n   \nj=w\n");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "j"), "w");
  gtext_ini_free(doc);
}

TEST(IniParse, AHeaderMayBeFollowedByWhitespaceButNotByAnythingElse) {
  GTEXT_INI_Document * doc = ok("[G]   \nk=v\n");
  ASSERT_NE(doc, nullptr);
  size_t len = 0;
  const char * name =
      gtext_ini_group_name(gtext_ini_document_group_at(doc, 0), &len);
  EXPECT_EQ(take(name, len), "G");
  gtext_ini_free(doc);
  /* `[G] junk` - GKeyFile refuses it, so this must too. */
  refused("[G] junk\nk=v\n", GTEXT_INI_E_BAD_LINE);
  refused("[G\nk=v\n", GTEXT_INI_E_BAD_GROUP);
}

TEST(IniParse, AnEmptyGroupNameIsRefused) {
  /* §3.2 permits any ASCII but `[`, `]` and control characters, which would
   * admit the empty name; GKeyFile refuses it ("Invalid group name: "). The
   * specification being silent, the reference decides. */
  refused("[]\nk=v\n", GTEXT_INI_E_BAD_GROUP);
}

TEST(IniParse, AnEntryBeforeTheFirstGroupIsRefused) {
  refused("k=v\n[G]\nj=w\n", GTEXT_INI_E_NO_GROUP);
}

TEST(IniParse, DuplicateGroupsAndKeysAreRefused) {
  /* §3.2 "Multiple groups may not have the same name"; §3.3 the same for keys.
   * GKeyFile silently merges the first and takes the last of the second -
   * desktop-file-validate refuses both, and the specification is with the
   * validator. */
  refused("[G]\nk=v\n[G]\nj=w\n", GTEXT_INI_E_DUPGROUP);
  refused("[G]\nk=v\nk=w\n", GTEXT_INI_E_DUPKEY);
}

TEST(IniParse, AKeyOutsideTheCharsetIsRefused) {
  refused("[G]\nk_und=v\n", GTEXT_INI_E_BAD_KEY);
  refused("[G]\nk.dot=v\n", GTEXT_INI_E_BAD_KEY);
  refused("[G]\nk space=v\n", GTEXT_INI_E_BAD_KEY);
  refused("[G]\n=v\n", GTEXT_INI_E_BAD_KEY);
  /* GKeyFile accepts all four. The charset is §3.3's and the validator enforces
   * it, so this module is deliberately stricter than the library. */
  GTEXT_INI_Document * doc = ok("[G]\nk_und=v\n", gtext_ini_dialect_generic());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "k_und"), "v");
  gtext_ini_free(doc);
}

TEST(IniParse, ALineWithNoDelimiterIsRefused) {
  refused("[G]\nnovalue\n", GTEXT_INI_E_BAD_LINE);
}

TEST(IniParse, WhitespaceAroundTheEqualsSignIsIgnored) {
  GTEXT_INI_Document * doc = ok("[G]\nk  =  v\nj\t=\tw\n");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "k"), "v");
  EXPECT_EQ(raw(doc, "G", "j"), "w");
  gtext_ini_free(doc);
}

TEST(IniParse, TrailingWhitespaceIsPartOfTheValue) {
  /*
   * The one rule the specification does not settle. §3.3 says only that "space
   * before and after the equals sign should be ignored", so the leading run is
   * gone either way; GKeyFile *keeps* the trailing run, and a value that
   * disagreed with the reference would make every differential a false
   * positive. Measured: `k=v<3 spaces>` gives raw `v   `.
   */
  GTEXT_INI_Document * doc = ok("[G]\nk=v   \n");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "k"), "v   ");
  gtext_ini_free(doc);
}

TEST(IniParse, AValueOfNothingButSpacesIsEmpty) {
  /* Follows from the same rule rather than contradicting it: the leading run is
   * skipped, and for `k=   ` that is the whole remainder. GKeyFile agrees. */
  GTEXT_INI_Document * doc = ok("[G]\nk=   \nj=\n");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "k"), "");
  EXPECT_EQ(raw(doc, "G", "j"), "");
  /* Empty is not absent, which is the distinction a caller acts on. */
  size_t len = 99;
  EXPECT_NE(gtext_ini_document_get(doc, "G", "k", &len), nullptr);
  EXPECT_EQ(len, 0u);
  EXPECT_EQ(gtext_ini_document_get(doc, "G", "nosuch", &len), nullptr);
  gtext_ini_free(doc);
}

TEST(IniParse, AFinalLineNeedsNoTerminator) {
  GTEXT_INI_Document * doc = ok("[G]\nk=v");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "k"), "v");
  gtext_ini_free(doc);
  GTEXT_INI_Document * bare = ok("[G]");
  ASSERT_NE(bare, nullptr);
  EXPECT_EQ(gtext_ini_document_group_count(bare), 1u);
  gtext_ini_free(bare);
}

TEST(IniParse, CrlfIsRefusedByTheStrictDialectAndAcceptedByTheGeneric) {
  /*
   * §3: "a series of lines that are separated by linefeed characters". So under
   * a literal reading the CR is part of the line, and a group header followed by
   * one is a header followed by something that is not whitespace - refused.
   *
   * `GKeyFile` accepts CRLF everywhere, so this is a **deviation**, and a
   * deliberate one: the alternative is reading every value on a CRLF file with a
   * trailing CR nobody asked for, silently. A caller with such a file asks for
   * the generic dialect, which accepts it and strips the CR.
   */
  refused("[G]\r\nk=v\r\n", GTEXT_INI_E_BAD_LINE);
  GTEXT_INI_Document * loose = ok("[G]\r\nk=v\r\n", gtext_ini_dialect_generic());
  ASSERT_NE(loose, nullptr);
  EXPECT_EQ(raw(loose, "G", "k"), "v");
  /* And the CR survives a round trip under the generic dialect, because the
   * terminator is part of the line the tree kept. */
  EXPECT_EQ(written(loose), "[G]\r\nk=v\r\n");
  gtext_ini_free(loose);

  /* A CR inside a value, with an LF terminator, is data under both dialects -
   * there is no rule that would remove it. */
  GTEXT_INI_Document * mid = ok("[G]\nk=a\rb\n");
  ASSERT_NE(mid, nullptr);
  EXPECT_EQ(raw(mid, "G", "k"), "a\rb");
  gtext_ini_free(mid);
  /*
   * A trailing CR with no LF after it is **not** a terminator, so the two
   * dialects must read it identically. fuzz_ini.cpp's parity property found
   * this at 237k executions on its first run: the CRLF test had no "is there
   * actually an LF" clause, so the generic dialect ate the byte and gave a value
   * one shorter than the strict dialect's.
   */
  for (const std::string & subject : {std::string("[G]\nk=v\r"),
           std::string("[G]\nk=v\r\r"), std::string("[G]\nk=v\r\n"),
           std::string("[G]\nk=v\r\r\n")}) {
    GTEXT_INI_Document * a = ok(subject);
    GTEXT_INI_Document * b = ok(subject, gtext_ini_dialect_generic());
    ASSERT_NE(a, nullptr) << subject;
    ASSERT_NE(b, nullptr) << subject;
    size_t alen = 0;
    size_t blen = 0;
    const char * av = gtext_ini_document_get(a, "G", "k", &alen);
    const char * bv = gtext_ini_document_get(b, "G", "k", &blen);
    /* They agree except where an LF makes the CR a terminator. */
    const bool terminated = subject.size() >= 2 &&
                            subject[subject.size() - 1] == '\n' &&
                            subject[subject.size() - 2] == '\r';
    if (terminated) {
      EXPECT_EQ(take(av, alen), take(bv, blen) + "\r") << subject;
    }
    else {
      EXPECT_EQ(take(av, alen), take(bv, blen)) << subject;
    }
    /* And both still rewrite byte for byte, whichever way that went. */
    EXPECT_EQ(written(a), subject) << subject;
    EXPECT_EQ(written(b), subject) << subject;
    gtext_ini_free(a);
    gtext_ini_free(b);
  }
}

TEST(IniParse, ABomIsRefusedByBothReferencesAndSoByThisOne) {
  /* The BOM makes the first line none of blank, comment, group or entry, which
   * is the same category GKeyFile reports ("not a key-value pair, group, or
   * comment") and desktop-file-validate reports ("not a comment, a group or an
   * entry"). */
  refused("\xEF\xBB\xBF[G]\nk=v\n", GTEXT_INI_E_BAD_LINE);
  GTEXT_INI_Document * doc =
      ok("\xEF\xBB\xBF[G]\nk=v\n", gtext_ini_dialect_generic());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "k"), "v");
  gtext_ini_free(doc);
}

TEST(IniParse, ANulInAValueIsKept) {
  /*
   * A deliberate deviation from `GKeyFile`, which truncates at the NUL because
   * its strings are NUL-terminated. This module is length-based throughout, so
   * the value is representable; @ref format_ini records it as a deviation with
   * this reproduction rather than leaving it to be discovered.
   */
  std::string text("[G]\nk=a\0b\n", 10);
  GTEXT_INI_Document * doc = ok(text);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "G", "k"), std::string("a\0b", 3));
  gtext_ini_free(doc);
}

TEST(IniParse, InvalidUtf8ParsesAndFailsOnlyWhenDecoded) {
  /*
   * Neither reference validates UTF-8 at parse time: GKeyFile accepts the
   * document and fails at g_key_file_get_string(), and desktop-file-validate
   * exits 0 with a warning. §3.1 also lets a comment line "contain any
   * character (except for LF)", so a document with a non-UTF-8 comment is
   * legal. The validation belongs where the question is asked.
   */
  std::string text("[G]\nk=a\xFF"
                   "b\n");
  GTEXT_INI_Document * doc = ok(text);
  ASSERT_NE(doc, nullptr);
  size_t len = 0;
  const char * value = gtext_ini_document_get(doc, "G", "k", &len);
  ASSERT_NE(value, nullptr);
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_desktop_entry();
  char * decoded = nullptr;
  EXPECT_EQ(gtext_ini_unescape(&dialect, value, len, nullptr, &decoded,
                nullptr), GTEXT_INI_E_BAD_UNICODE);
  EXPECT_EQ(decoded, nullptr);
  gtext_ini_free(doc);

  GTEXT_INI_Document * commented = ok("# \xFF\n[G]\nk=v\n");
  ASSERT_NE(commented, nullptr);
  gtext_ini_free(commented);
}

TEST(IniParse, LimitsAreEnforced) {
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.max_total_bytes = 4;
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_ini_parse("[G]\nk=v\n", 8, &opts, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_INI_E_LIMIT);
  gtext_ini_error_free(&err);

  opts = gtext_ini_parse_options_default();
  opts.max_groups = 1;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_ini_parse("[A]\n[B]\n", 8, &opts, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_INI_E_LIMIT);
  gtext_ini_error_free(&err);

  opts = gtext_ini_parse_options_default();
  opts.max_entries_per_group = 1;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_ini_parse("[A]\nk=1\nj=2\n", 12, &opts, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_INI_E_LIMIT);
  gtext_ini_error_free(&err);
}

TEST(IniParse, AnErrorNamesItsLineAndColumn) {
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  const char * text = "[G]\nk=v\n; bad\n";
  EXPECT_EQ(gtext_ini_parse(text, std::strlen(text), nullptr, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_INI_E_BAD_LINE);
  EXPECT_EQ(err.line, 3);
  EXPECT_EQ(err.col, 1);
  EXPECT_NE(err.context_snippet, nullptr);
  gtext_ini_error_free(&err);
  /* Safe twice, and on a zeroed struct. */
  gtext_ini_error_free(&err);
  GTEXT_INI_Error zero;
  std::memset(&zero, 0, sizeof(zero));
  gtext_ini_error_free(&zero);
}

// ---------------------------------------------------------------- locale keys

TEST(IniLocale, APostfixedKeyIsStoredVerbatim) {
  /* g_key_file_get_keys() on a file with `Name[de]` reports a key literally
   * named `Name[de]`: the tree holds the spelling and the matching is an
   * accessor's job. */
  GTEXT_INI_Document * doc = ok("[G]\nName=n\nName[de]=nd\n");
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(doc, "G");
  size_t len = 0;
  const char * key = gtext_ini_group_key_at(g, 1, &len);
  EXPECT_EQ(take(key, len), "Name[de]");
  gtext_ini_free(doc);
}

TEST(IniLocale, AnOrphanPostfixedKeyIsRefused) {
  /* §5: "If a postfixed key occurs, the same key must be also present without
   * the postfix." Only desktop-file-validate enforces it; GKeyFile accepts an
   * orphan. */
  refused("[G]\nComment[de]=cd\n", GTEXT_INI_E_BAD_KEY);
  /* Either order is fine - neither reference requires the bare key first. */
  GTEXT_INI_Document * doc = ok("[G]\nName[de]=nd\nName=n\n");
  ASSERT_NE(doc, nullptr);
  gtext_ini_free(doc);
}

TEST(IniLocale, TheFallbackChainIsSectionFivesOrder) {
  GTEXT_INI_Document * doc = ok(
      "[G]\nName=bare\nName[de]=lang\nName[de_DE]=country\n"
      "Name[de_DE@x]=countrymod\nName[de@x]=langmod\n");
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(doc, "G");
  size_t len = 0;
  auto pick = [&](const char * locale) {
    const char * v = gtext_ini_group_get_locale(g, "Name", locale, &len);
    return take(v, len);
  };
  EXPECT_EQ(pick("de_DE.UTF-8@x"), "countrymod");
  EXPECT_EQ(pick("de_DE.UTF-8"), "country");
  EXPECT_EQ(pick("de@x"), "langmod");
  EXPECT_EQ(pick("de"), "lang");
  /* A locale with no country must not match a key that has one. */
  EXPECT_EQ(pick("fr"), "bare");
  /* "" asks for the unpostfixed key only. */
  EXPECT_EQ(pick(""), "bare");
  gtext_ini_free(doc);
}

TEST(IniLocale, TheEncodingIsIgnoredWhenMatching) {
  GTEXT_INI_Document * doc = ok("[G]\nName=bare\nName[de_DE]=country\n");
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(doc, "G");
  size_t len = 0;
  const char * v = gtext_ini_group_get_locale(g, "Name", "de_DE.ISO-8859-1",
      &len);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(take(v, len), "country");
  gtext_ini_free(doc);
}

// -------------------------------------------------------------------- escapes

TEST(IniValue, TheEscapeSetIsExactlySectionFours) {
  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  char * out = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_ini_unescape(&d, "a\\sb\\nc\\td\\re\\\\f", 16, nullptr, &out,
                &len), GTEXT_INI_OK);
  EXPECT_EQ(std::string(out, len), "a b\nc\td\re\\f");
  gtext_ini_string_free(nullptr, out);

  /* `\q` names no sequence. GKeyFile refuses it here too, at get_string(). */
  out = nullptr;
  EXPECT_EQ(gtext_ini_unescape(&d, "a\\qb", 4, nullptr, &out, &len),
      GTEXT_INI_E_BAD_ESCAPE);
  EXPECT_EQ(out, nullptr);
  /* A trailing lone backslash names none either. */
  EXPECT_EQ(gtext_ini_unescape(&d, "a\\", 2, nullptr, &out, &len),
      GTEXT_INI_E_BAD_ESCAPE);
  /* `\;` is a *list* escape: g_key_file_get_string() refuses a bare one and
   * g_key_file_get_string_list() accepts it. */
  EXPECT_EQ(gtext_ini_unescape(&d, "a\\;b", 4, nullptr, &out, &len),
      GTEXT_INI_E_BAD_ESCAPE);
}

TEST(IniValue, EscapingIsTheInverseAndRefusesWhatItCannotSpell) {
  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  char * out = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_ini_escape(&d, "a b\nc", 5, nullptr, &out, &len),
      GTEXT_INI_OK);
  /* An interior space needs no escape; a newline does. */
  EXPECT_EQ(std::string(out, len), "a b\\nc");
  gtext_ini_string_free(nullptr, out);

  /* A leading space does, because §3.3 discards the run after `=`. */
  ASSERT_EQ(gtext_ini_escape(&d, " x", 2, nullptr, &out, &len), GTEXT_INI_OK);
  EXPECT_EQ(std::string(out, len), "\\sx");
  gtext_ini_string_free(nullptr, out);

  /* A dialect with no escapes cannot spell a newline, and says so rather than
   * emitting a document that reads back as two lines. */
  GTEXT_INI_Dialect none = d;
  none.escapes = nullptr;
  out = nullptr;
  EXPECT_EQ(gtext_ini_escape(&none, "a\nb", 3, nullptr, &out, &len),
      GTEXT_INI_E_UNREPRESENTABLE);
  EXPECT_EQ(out, nullptr);
}

TEST(IniValue, EscapeAndUnescapeRoundTrip) {
  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  const std::string subjects[] = {"", "plain", " leading", "tab\there",
      "nl\nhere", "back\\slash", "cr\rhere", "all \t\n\r\\ of it"};
  for (const std::string & subject : subjects) {
    char * encoded = nullptr;
    size_t encoded_len = 0;
    ASSERT_EQ(gtext_ini_escape(&d, subject.data(), subject.size(), nullptr,
                  &encoded, &encoded_len), GTEXT_INI_OK) << subject;
    char * decoded = nullptr;
    size_t decoded_len = 0;
    ASSERT_EQ(gtext_ini_unescape(&d, encoded, encoded_len, nullptr, &decoded,
                  &decoded_len), GTEXT_INI_OK) << subject;
    EXPECT_EQ(std::string(decoded, decoded_len), subject);
    gtext_ini_string_free(nullptr, encoded);
    gtext_ini_string_free(nullptr, decoded);
  }
}

// ----------------------------------------------------------------------- lists

TEST(IniValue, ListSplittingFollowsSectionFoursTerminatorRule) {
  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  auto split = [&](const std::string & text) {
    GTEXT_INI_List * list = nullptr;
    EXPECT_EQ(gtext_ini_value_list(&d, text.data(), text.size(), nullptr,
                  &list), GTEXT_INI_OK) << text;
    std::string joined;
    for (size_t i = 0; i < gtext_ini_list_count(list); i++) {
      size_t len = 0;
      const char * item = gtext_ini_list_at(list, i, &len);
      joined += "<" + std::string(item, len) + ">";
    }
    gtext_ini_list_free(list);
    return joined;
  };
  /* "The multiple values should be separated by a semicolon and the value of
   * the key may be optionally terminated by a semicolon." */
  EXPECT_EQ(split("a;b;c"), "<a><b><c>");
  EXPECT_EQ(split("a;b;c;"), "<a><b><c>");
  /* "Trailing empty strings must always be terminated with a semicolon" - so a
   * second trailing separator introduces an empty item rather than being
   * discarded with the first. */
  EXPECT_EQ(split("a;b;;"), "<a><b><>");
  EXPECT_EQ(split("a\\;b;c"), "<a;b><c>");
  EXPECT_EQ(split(""), "");
  EXPECT_EQ(split("a"), "<a>");
  EXPECT_EQ(split(";"), "<>");
}

TEST(IniValue, AListItemStillHonoursTheEscapeSet) {
  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  GTEXT_INI_List * list = nullptr;
  EXPECT_EQ(gtext_ini_value_list(&d, "a\\qb;c", 6, nullptr, &list),
      GTEXT_INI_E_BAD_ESCAPE);
  EXPECT_EQ(list, nullptr);
  /* A dialect with no separator has no list spelling to ask about. */
  GTEXT_INI_Dialect none = d;
  none.list_separator = 0;
  EXPECT_EQ(gtext_ini_value_list(&none, "a;b", 3, nullptr, &list),
      GTEXT_INI_E_INVALID);
}

// ------------------------------------------------------------- typed accessors

TEST(IniValue, BooleanIsExactlyTrueOrFalse) {
  bool out = false;
  EXPECT_EQ(gtext_ini_value_bool("true", 4, &out), GTEXT_INI_OK);
  EXPECT_TRUE(out);
  EXPECT_EQ(gtext_ini_value_bool("false", 5, &out), GTEXT_INI_OK);
  EXPECT_FALSE(out);
  /* §4 admits nothing else, and §3 says case is significant. systemd's wider
   * set belongs to the systemd dialect. */
  for (const char * no : {"1", "0", "yes", "no", "on", "off", "True", "FALSE",
           "", " true"}) {
    EXPECT_EQ(gtext_ini_value_bool(no, std::strlen(no), &out),
        GTEXT_INI_E_TYPE) << no;
  }
}

TEST(IniValue, IntegersAndTheirBounds) {
  int64_t out = 0;
  EXPECT_EQ(gtext_ini_value_int("0", 1, &out), GTEXT_INI_OK);
  EXPECT_EQ(out, 0);
  EXPECT_EQ(gtext_ini_value_int("-42", 3, &out), GTEXT_INI_OK);
  EXPECT_EQ(out, -42);
  EXPECT_EQ(gtext_ini_value_int("+7", 2, &out), GTEXT_INI_OK);
  EXPECT_EQ(out, 7);
  EXPECT_EQ(gtext_ini_value_int("9223372036854775807", 19, &out), GTEXT_INI_OK);
  EXPECT_EQ(out, INT64_MAX);
  EXPECT_EQ(gtext_ini_value_int("-9223372036854775808", 20, &out),
      GTEXT_INI_OK);
  EXPECT_EQ(out, INT64_MIN);
  EXPECT_EQ(gtext_ini_value_int("9223372036854775808", 19, &out),
      GTEXT_INI_E_RANGE);
  EXPECT_EQ(gtext_ini_value_int("-9223372036854775809", 20, &out),
      GTEXT_INI_E_RANGE);
  for (const char * no : {"", "-", "+", "1.0", "0x10", "1 ", " 1", "1e2"}) {
    EXPECT_EQ(gtext_ini_value_int(no, std::strlen(no), &out),
        GTEXT_INI_E_TYPE) << no;
  }
}

TEST(IniValue, DoublesReadInTheCLocaleWhateverTheProcessLocaleIs) {
  /*
   * §4 defines `numeric` by `%f` in the **C locale**. strtod reads LC_NUMERIC,
   * so without a locale-independent conversion a process in a comma-decimal
   * locale reads `1.5` as 1 and accepts `1,5` as one and a half.
   */
  const char * had = std::setlocale(LC_NUMERIC, nullptr);
  std::string saved = had ? had : "C";
  bool comma = std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr ||
               std::setlocale(LC_NUMERIC, "de_DE") != nullptr;
  double out = 0;
  EXPECT_EQ(gtext_ini_value_double("1.5", 3, &out), GTEXT_INI_OK);
  EXPECT_DOUBLE_EQ(out, 1.5);
  EXPECT_EQ(gtext_ini_value_double("1,5", 3, &out), GTEXT_INI_E_TYPE);
  if (!comma) {
    GTEST_LOG_(INFO) << "no comma-decimal locale installed; the assertions "
                        "above still ran, in the C locale";
  }
  std::setlocale(LC_NUMERIC, saved.c_str());

  EXPECT_EQ(gtext_ini_value_double("-0.25", 5, &out), GTEXT_INI_OK);
  EXPECT_DOUBLE_EQ(out, -0.25);
  /* `%f` reads these and a desktop entry has no use for either. */
  EXPECT_EQ(gtext_ini_value_double("inf", 3, &out), GTEXT_INI_E_TYPE);
  EXPECT_EQ(gtext_ini_value_double("nan", 3, &out), GTEXT_INI_E_TYPE);
  EXPECT_EQ(gtext_ini_value_double("1.0", 3, &out), GTEXT_INI_OK);
  EXPECT_DOUBLE_EQ(out, 1.0);
  /*
   * `%f` skips leading whitespace; this does not. The parser has already removed
   * the run after `=`, so a stored value never begins with one, and accepting it
   * would both admit a spelling no document produces and disagree with
   * gtext_ini_value_int() about the same bytes.
   */
  for (const char * no : {"", "1.0x", " 1.0", "\t1.0", "1.0 ", "1.0\t"}) {
    EXPECT_EQ(gtext_ini_value_double(no, std::strlen(no), &out),
        GTEXT_INI_E_TYPE) << "[" << no << "]";
  }
}

TEST(IniValue, ALongSpellingOfAnOrdinaryNumberIsStillThatNumber) {
  /*
   * A number has no bound on how it may be *written*, so a buffer sized for a
   * shortest form is a bound on the wrong thing. This is the defect the TOML
   * reader had, with a 64-byte lexeme buffer; the spelling below is 82
   * characters and an ordinary double.
   */
  const std::string text =
      "0.000000000000000000000000000000000000000000000000000000000000000"
      "000000000000000024703282292062327208828439643411068618252990130716"
      "238221279284125033775363510437593264991818081799618989828234772285"
      "886546332835517796989819938739800539093906315035659515570226392290"
      "858392449105184435931802849936536152500319370457678249219365623669"
      "863658480757001585769269903706311928279558551332927834338409351978"
      "015531246597263579574622766465272827220056374006485499977096599470"
      "454020828166226237857393450736339007967761930577506740176324673600"
      "968951340535537458516661134223593193874095048265527814599318600779"
      "9416237194961983521930319";
  double out = 0;
  ASSERT_GT(text.size(), 64u);
  EXPECT_EQ(gtext_ini_value_double(text.data(), text.size(), &out),
      GTEXT_INI_OK);
  /* The claim is that a spelling longer than any stack buffer still converts,
   * not that it converts to a particular constant - the digits below are a
   * truncation of DBL_MIN's decimal expansion and the value is what it is. */
  EXPECT_GT(out, 0.0);
  EXPECT_LT(out, 1e-70);
}

// ------------------------------------------------------------ duplicate policy

TEST(IniDuplicates, TheTreeKeepsEveryOccurrenceWhateverLookupAnswers) {
  /*
   * The point of the storage shape: git config and systemd both need every
   * occurrence in order, and a tree that kept one value per key could not grow
   * the others later without changing the type a caller walks.
   */
  GTEXT_INI_Dialect d = gtext_ini_dialect_generic();
  d.dupkey = GTEXT_INI_DUPKEY_LAST_WINS;
  GTEXT_INI_Document * doc = ok("[G]\nk=one\nk=two\nk=three\n", d);
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(doc, "G");
  EXPECT_EQ(gtext_ini_group_entry_count(g), 3u);
  EXPECT_EQ(gtext_ini_group_count_key(g, "k"), 3u);
  size_t len = 0;
  const char * v = gtext_ini_group_get(g, "k", &len);
  EXPECT_EQ(take(v, len), "three");
  v = gtext_ini_group_get_nth(g, "k", 0, &len);
  EXPECT_EQ(take(v, len), "one");
  v = gtext_ini_group_get_nth(g, "k", 1, &len);
  EXPECT_EQ(take(v, len), "two");
  v = gtext_ini_group_get_nth(g, "k", 2, &len);
  EXPECT_EQ(take(v, len), "three");
  EXPECT_EQ(gtext_ini_group_get_nth(g, "k", 3, &len), nullptr);
  gtext_ini_free(doc);

  d.dupkey = GTEXT_INI_DUPKEY_FIRST_WINS;
  GTEXT_INI_Document * first = ok("[G]\nk=one\nk=two\n", d);
  ASSERT_NE(first, nullptr);
  v = gtext_ini_group_get(gtext_ini_document_group(first, "G"), "k", &len);
  EXPECT_EQ(take(v, len), "one");
  gtext_ini_free(first);
}

TEST(IniDuplicates, ARepeatedGroupHeaderMergesOnLookupAndStaysSplitInTheTree) {
  /* GLib merges repeated group headers. The tree keeps them apart so a rewrite
   * reproduces the document, and the merge lives in the lookup. */
  GTEXT_INI_Document * doc =
      ok("[G]\nk=v\n[G]\nj=w\n", gtext_ini_dialect_generic());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gtext_ini_document_group_count(doc), 2u);
  EXPECT_EQ(raw(doc, "G", "k"), "v");
  EXPECT_EQ(raw(doc, "G", "j"), "w");
  /* A group-scoped lookup sees only its own group, which is what makes the
   * document-scoped one a merge rather than the only behaviour. */
  size_t len = 0;
  EXPECT_EQ(gtext_ini_group_get(gtext_ini_document_group_at(doc, 0), "j", &len),
      nullptr);
  EXPECT_EQ(written(doc), "[G]\nk=v\n[G]\nj=w\n");
  gtext_ini_free(doc);
}

// ---------------------------------------------------------------- the round trip

TEST(IniWrite, AnUnmodifiedDocumentWritesBackByteForByte) {
  /*
   * §3 requires that a rewrite preserve fields the implementation does not
   * understand, and that comments "should be preserved across reads and
   * writes". Every oddity of spacing below is deliberate.
   */
  const std::string text =
      "# leading comment\n"
      "\n"
      "[Desktop Entry]   \n"
      "Type=Application\n"
      "Name  =  spaced out   \n"
      "# a comment between entries\n"
      "\n"
      "Exec=/bin/true\n"
      "X-Unknown-Key=kept\n"
      "\n"
      "[Desktop Action Open]\n"
      "Name=Open\n"
      "# trailing comment, no final newline after it";
  GTEXT_INI_Document * doc = ok(text);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(written(doc), text);
  gtext_ini_free(doc);
}

TEST(IniWrite, CommentsLandWhereTheyCanBeWrittenBack) {
  GTEXT_INI_Document * doc = ok("# one\n# two\n[G]\n# three\nk=v\n# four\n");
  ASSERT_NE(doc, nullptr);
  size_t len = 0;
  const char * c = gtext_ini_document_leading_comment(doc, &len);
  EXPECT_EQ(take(c, len), "# one\n# two\n");
  const GTEXT_INI_Group * g = gtext_ini_document_group(doc, "G");
  EXPECT_EQ(gtext_ini_group_leading_comment(g, &len), nullptr);
  c = gtext_ini_group_entry_comment_at(g, 0, &len);
  EXPECT_EQ(take(c, len), "# three\n");
  c = gtext_ini_document_trailing_comment(doc, &len);
  EXPECT_EQ(take(c, len), "# four\n");
  gtext_ini_free(doc);
}

TEST(IniWrite, NormalizeRewritesTheSpacingAndDropsNothingElse) {
  GTEXT_INI_Document * doc = ok("[G]   \nk  =  v\n");
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Write_Options opts = gtext_ini_write_options_default();
  opts.normalize = true;
  EXPECT_EQ(written(doc, &opts), "[G]\nk=v\n");
  gtext_ini_free(doc);
}

TEST(IniWrite, CommentsCanBeDroppedDeliberately) {
  GTEXT_INI_Document * doc = ok("# c\n[G]\n# d\nk=v\n# e\n");
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Write_Options opts = gtext_ini_write_options_default();
  opts.emit_comments = false;
  EXPECT_EQ(written(doc, &opts), "[G]\nk=v\n");
  gtext_ini_free(doc);
}

TEST(IniWrite, CrlfIsAnOptionOfTheWriterNotOfTheTree) {
  GTEXT_INI_Document * doc = ok("[G]\nk=v\n");
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Write_Options opts = gtext_ini_write_options_default();
  opts.normalize = true;
  opts.crlf = true;
  EXPECT_EQ(written(doc, &opts), "[G]\r\nk=v\r\n");
  gtext_ini_free(doc);
}

TEST(IniWrite, AValueTheDialectCannotSpellIsRefusedRatherThanMangled) {
  GTEXT_INI_Document * doc = gtext_ini_new(nullptr);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * g = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "G", &g), GTEXT_INI_OK);
  ASSERT_EQ(gtext_ini_group_set(g, "k", "a\nb", 3), GTEXT_INI_OK);
  GTEXT_INI_Sink sink;
  ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
  /* The writer refuses rather than emitting a document that reads back as two
   * lines. The caller's fix is gtext_ini_escape(), not a different writer. */
  EXPECT_EQ(gtext_ini_write(doc, &sink, nullptr), GTEXT_INI_E_UNREPRESENTABLE);
  gtext_ini_sink_buffer_free(&sink);

  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  char * escaped = nullptr;
  size_t escaped_len = 0;
  ASSERT_EQ(gtext_ini_escape(&d, "a\nb", 3, nullptr, &escaped, &escaped_len),
      GTEXT_INI_OK);
  ASSERT_EQ(gtext_ini_group_set(g, "k", escaped, escaped_len), GTEXT_INI_OK);
  gtext_ini_string_free(nullptr, escaped);
  EXPECT_EQ(written(doc), "[G]\nk=a\\nb\n");
  gtext_ini_free(doc);
}

TEST(IniWrite, ALeadingSpaceInAValueIsAlsoRefused) {
  GTEXT_INI_Document * doc = gtext_ini_new(nullptr);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * g = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "G", &g), GTEXT_INI_OK);
  ASSERT_EQ(gtext_ini_group_set(g, "k", " x", 2), GTEXT_INI_OK);
  GTEXT_INI_Sink sink;
  ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
  /* §3.3 discards the run after `=`, so this would read back as "x". */
  EXPECT_EQ(gtext_ini_write(doc, &sink, nullptr), GTEXT_INI_E_UNREPRESENTABLE);
  gtext_ini_sink_buffer_free(&sink);
  gtext_ini_free(doc);
}

// ------------------------------------------------------------------- building

TEST(IniBuild, GroupsAndEntriesCanBeAddedAndTheDialectStillApplies) {
  GTEXT_INI_Document * doc = gtext_ini_new(nullptr);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * g = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "Desktop Entry", &g),
      GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_document_add_group(doc, "Desktop Entry", nullptr),
      GTEXT_INI_E_DUPGROUP);
  EXPECT_EQ(gtext_ini_document_add_group(doc, "a[b", nullptr),
      GTEXT_INI_E_BAD_GROUP);
  EXPECT_EQ(gtext_ini_document_add_group(doc, "", nullptr),
      GTEXT_INI_E_BAD_GROUP);

  ASSERT_EQ(gtext_ini_group_set(g, "Type", "Application", 11), GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_group_set(g, "bad key", "x", 1), GTEXT_INI_E_BAD_KEY);
  /* set() replaces; add() appends, and refuses under the strict dupkey mode -
   * so a loop under the Desktop Entry dialect cannot build a document the
   * dialect would not have parsed. */
  ASSERT_EQ(gtext_ini_group_set(g, "Type", "Link", 4), GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_group_entry_count(g), 1u);
  EXPECT_EQ(gtext_ini_group_add(g, "Type", "Directory", 9),
      GTEXT_INI_E_DUPKEY);
  EXPECT_EQ(written(doc), "[Desktop Entry]\nType=Link\n");
  gtext_ini_free(doc);
}

TEST(IniBuild, AddingAGroupInvalidatesOldGroupPointers) {
  /* The header promises this rather than hiding it: the group array may move,
   * so a caller has to look a group up again after adding one. Exercised here
   * past the initial capacity, because a sweep that never grows the array would
   * not reach the realloc at all. */
  GTEXT_INI_Document * doc = gtext_ini_new(nullptr);
  ASSERT_NE(doc, nullptr);
  for (int i = 0; i < 40; i++) {
    std::string name = "G" + std::to_string(i);
    GTEXT_INI_Group * g = nullptr;
    ASSERT_EQ(gtext_ini_document_add_group(doc, name.c_str(), &g),
        GTEXT_INI_OK);
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(gtext_ini_group_set(g, "k", name.data(), name.size()),
        GTEXT_INI_OK);
  }
  EXPECT_EQ(gtext_ini_document_group_count(doc), 40u);
  EXPECT_EQ(raw(doc, "G0", "k"), "G0");
  EXPECT_EQ(raw(doc, "G39", "k"), "G39");
  gtext_ini_free(doc);
}

TEST(IniBuild, NullArgumentsAreRefusedRatherThanCrashed) {
  EXPECT_EQ(gtext_ini_document_group_count(nullptr), 0u);
  EXPECT_EQ(gtext_ini_document_group_at(nullptr, 0), nullptr);
  EXPECT_EQ(gtext_ini_document_group(nullptr, "G"), nullptr);
  EXPECT_EQ(gtext_ini_document_get(nullptr, "G", "k", nullptr), nullptr);
  EXPECT_EQ(gtext_ini_group_name(nullptr, nullptr), nullptr);
  EXPECT_EQ(gtext_ini_group_entry_count(nullptr), 0u);
  EXPECT_EQ(gtext_ini_group_key_at(nullptr, 0, nullptr), nullptr);
  EXPECT_EQ(gtext_ini_group_value_at(nullptr, 0, nullptr), nullptr);
  EXPECT_EQ(gtext_ini_group_get(nullptr, "k", nullptr), nullptr);
  EXPECT_EQ(gtext_ini_group_count_key(nullptr, "k"), 0u);
  EXPECT_EQ(gtext_ini_group_get_nth(nullptr, "k", 0, nullptr), nullptr);
  EXPECT_EQ(gtext_ini_group_set(nullptr, "k", "v", 1), GTEXT_INI_E_INVALID);
  EXPECT_EQ(gtext_ini_group_add(nullptr, "k", "v", 1), GTEXT_INI_E_INVALID);
  EXPECT_EQ(gtext_ini_document_add_group(nullptr, "G", nullptr),
      GTEXT_INI_E_INVALID);
  EXPECT_EQ(gtext_ini_write(nullptr, nullptr, nullptr), GTEXT_INI_E_INVALID);
  EXPECT_EQ(gtext_ini_write_file(nullptr, "x", nullptr), GTEXT_INI_E_INVALID);
  EXPECT_EQ(gtext_ini_group_get_locale(nullptr, "k", nullptr, nullptr),
      nullptr);
  EXPECT_EQ(gtext_ini_list_count(nullptr), 0u);
  EXPECT_EQ(gtext_ini_list_at(nullptr, 0, nullptr), nullptr);
  gtext_ini_free(nullptr);
  gtext_ini_list_free(nullptr);
  gtext_ini_string_free(nullptr, nullptr);
  gtext_ini_error_free(nullptr);
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  EXPECT_EQ(gtext_ini_parse(nullptr, 4, nullptr, &err), nullptr);
  EXPECT_EQ(err.code, GTEXT_INI_E_INVALID);
  gtext_ini_error_free(&err);
  EXPECT_EQ(gtext_ini_parse_file(nullptr, nullptr, nullptr), nullptr);
}

// --------------------------------------------------- the inherited floor

TEST(IniGeneric, EveryDocumentTheStrictDialectAcceptsParsesTheSameWay) {
  /*
   * The generic dialect's inherited claim, in the form that can be tested: six
   * of its seven changes are relaxations, and a relaxation may only *add*
   * accepted documents - so for anything the strict dialect accepts, the two
   * must agree key for key and value for value.
   *
   * **None of the subjects below contains a CR**, and that is a precondition
   * rather than an accident: `accept_crlf` is the seventh change and the one
   * that is not a relaxation, so on an input with a CR the two dialects
   * legitimately disagree. CrlfIsRefusedByTheStrictDialectAndAcceptedByTheGeneric
   * covers that case. The conformance target runs this property over the corpus
   * and excludes the CR-bearing files by the same rule; these are the shapes the
   * corpus does not contain.
   */
  const std::string subjects[] = {
      "",
      "[G]\n",
      "[G]\nk=v\n",
      "# c\n[G]\nk=v\n",
      "[G]\nk=v   \n",
      "[G]\nk=   \n",
      "[G]   \nk=v",
      "[G]\nName=n\nName[de]=nd\n",
      "[A]\nk=1\n\n[B]\nj=2\n",
      "[G]\nk=a\\nb\n",
      "[G]\nk=a;b;c;\n",
  };
  for (const std::string & subject : subjects) {
    GTEXT_INI_Document * strict = ok(subject);
    GTEXT_INI_Document * loose = ok(subject, gtext_ini_dialect_generic());
    ASSERT_NE(strict, nullptr) << subject;
    ASSERT_NE(loose, nullptr) << subject;
    ASSERT_EQ(gtext_ini_document_group_count(strict),
        gtext_ini_document_group_count(loose)) << subject;
    for (size_t g = 0; g < gtext_ini_document_group_count(strict); g++) {
      const GTEXT_INI_Group * a = gtext_ini_document_group_at(strict, g);
      const GTEXT_INI_Group * b = gtext_ini_document_group_at(loose, g);
      size_t alen = 0;
      size_t blen = 0;
      const char * an = gtext_ini_group_name(a, &alen);
      const char * bn = gtext_ini_group_name(b, &blen);
      ASSERT_EQ(take(an, alen), take(bn, blen)) << subject;
      ASSERT_EQ(gtext_ini_group_entry_count(a), gtext_ini_group_entry_count(b))
          << subject;
      for (size_t e = 0; e < gtext_ini_group_entry_count(a); e++) {
        const char * ak = gtext_ini_group_key_at(a, e, &alen);
        const char * bk = gtext_ini_group_key_at(b, e, &blen);
        EXPECT_EQ(take(ak, alen), take(bk, blen)) << subject;
        const char * av = gtext_ini_group_value_at(a, e, &alen);
        const char * bv = gtext_ini_group_value_at(b, e, &blen);
        EXPECT_EQ(take(av, alen), take(bv, blen)) << subject;
      }
    }
    /* And both write back byte for byte, which is the other half of "the same
     * way". */
    EXPECT_EQ(written(strict), subject);
    EXPECT_EQ(written(loose), subject);
    gtext_ini_free(strict);
    gtext_ini_free(loose);
  }
}

} // namespace
