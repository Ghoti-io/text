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

#include <cstdio>
#include <clocale>
#include <vector>
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
  EXPECT_NE(loose.name_style, strict.name_style);
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
  GTEXT_INI_Dialect de = gtext_ini_dialect_desktop_entry();
  EXPECT_EQ(gtext_ini_value_bool(&de, "true", 4, &out), GTEXT_INI_OK);
  EXPECT_TRUE(out);
  EXPECT_EQ(gtext_ini_value_bool(&de, "false", 5, &out), GTEXT_INI_OK);
  EXPECT_FALSE(out);
  /* §4 admits nothing else, and §3 says case is significant. systemd's wider
   * set belongs to the systemd dialect. */
  for (const char * no : {"1", "0", "yes", "no", "on", "off", "True", "FALSE",
           "", " true"}) {
    EXPECT_EQ(gtext_ini_value_bool(&de, no, std::strlen(no), &out),
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

// -------------------------------------------------------- the git config dialect
//
// Every assertion in this section was measured against git 2.47.3 before it was
// written, with `git config --file <f> --list -z` so that a valueless key is
// distinguishable from an empty one and a value containing a newline is not split.
// The transcript is in notes/text/INI-DIALECTS.md §A.4 and §A.12; the generated
// differential that runs the same comparison over 20,000 documents is
// `make check-ini-git-oracle`.
//
// The cases here are the ones a differential cannot reach: a caller building a
// document rather than parsing one, and the writer's refusals - which over any
// parsed population are unreachable, because every value a parse stored is
// writable by construction. A mutation removing the writer's representability
// check passed the whole 500-document differential untouched, which is why these
// exist.

namespace {

GTEXT_INI_Dialect git() { return gtext_ini_dialect_git_config(); }

/**
 * Call a (pointer, length) accessor and build a std::string from its result.
 *
 * `take(f(&len), len)` is wrong, and wrong in a way that passes: the call and the
 * read of `len` are unsequenced as arguments, so `len` can be read before `f`
 * writes it - giving a zero-length string for any value, and *silently agreeing*
 * with any assertion whose expected value happens to be empty. Two of the
 * assertions below were reading a stale zero, and one of the two passed. Here the
 * call is a statement of its own, so the ordering is not a question.
 */
template <typename F, typename... Args>
std::string got(F fn, Args... args) {
  size_t len = 0;
  const char * data = fn(args..., &len);
  return take(data, len);
}

/** The canonical `section[.subsection].key` name of every entry, in order. */
std::vector<std::string> canonical_names(const GTEXT_INI_Document * doc) {
  std::vector<std::string> out;
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    size_t glen = 0;
    const char * gname = gtext_ini_group_canonical_name(group, &glen);
    std::string prefix;
    if (!gtext_ini_group_is_preamble(group) && glen) {
      prefix = std::string(gname, glen) + ".";
    }
    for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
      size_t klen = 0;
      const char * key = gtext_ini_group_canonical_key_at(group, e, &klen);
      out.push_back(prefix + std::string(key, klen));
    }
  }
  return out;
}

/** The decoded value of the first entry of the first group. */
std::string decoded_first(const GTEXT_INI_Document * doc) {
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  size_t len = 0;
  const char * rawv = gtext_ini_group_value_at(group, 0, &len);
  GTEXT_INI_Dialect d = git();
  char * out = nullptr;
  size_t out_len = 0;
  if (gtext_ini_unescape(&d, rawv, len, nullptr, &out, &out_len)
      != GTEXT_INI_OK) {
    return "<refused>";
  }
  std::string result = take(out, out_len);
  gtext_ini_string_free(nullptr, out);
  return result;
}

} // namespace

TEST(IniGit, TheDialectIsNotARelaxationOfDesktopEntryInEitherDirection) {
  GTEXT_INI_Dialect g = git();
  GTEXT_INI_Dialect de = gtext_ini_dialect_desktop_entry();
  EXPECT_EQ(g.id, GTEXT_INI_DIALECT_GIT_CONFIG);
  /* Accepts what Desktop Entry refuses. */
  EXPECT_TRUE(g.comment_semicolon);
  EXPECT_TRUE(g.allow_preamble);
  EXPECT_TRUE(g.valueless_keys);
  EXPECT_TRUE(g.inline_comments);
  EXPECT_TRUE(g.quoted_values);
  EXPECT_TRUE(g.subsection_syntax);
  EXPECT_EQ(g.continuation, GTEXT_INI_CONTINUATION_JOIN_EMPTY);
  /* And refuses what Desktop Entry accepts: a group name of any ASCII. */
  EXPECT_EQ(de.name_style, GTEXT_INI_NAMES_DESKTOP_ENTRY);
  EXPECT_EQ(g.name_style, GTEXT_INI_NAMES_GIT);
  /* No list separator: git spells a list as repeated lines, so inventing one
   * here would be a syntax git does not have. */
  EXPECT_EQ(g.list_separator, 0);
  EXPECT_EQ(g.dupkey, GTEXT_INI_DUPKEY_COLLECT);
}

TEST(IniGit, SectionAndKeyFoldButAQuotedSubsectionDoesNot) {
  GTEXT_INI_Document * doc = ok("[Core]\nBare = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"core.bare"}));
  /* The tree keeps the document's own spelling, so a rewrite is byte-exact. */
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  size_t len = 0;
  const char * name = gtext_ini_group_name(group, &len);
  EXPECT_EQ(take(name, len), "Core");
  EXPECT_EQ(raw(doc, "core", "bare"), "v");
  /* Folded on the query side too. */
  EXPECT_EQ(raw(doc, "CORE", "BARE"), "v");
  gtext_ini_free(doc);

  /* A quoted subsection is case-sensitive, and the dotted spelling is not:
   * measured, git answers `b.SubB.k` and `a.subb.k` and refuses the other two
   * spellings of each. */
  doc = ok("[a.SubB]\nk = 1\n[b \"SubB\"]\nk = 2\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc),
      (std::vector<std::string>{"a.subb.k", "b.SubB.k"}));
  EXPECT_EQ(raw(doc, "a.subb", "k"), "1");
  EXPECT_EQ(raw(doc, "a.SubB", "k"), "<absent>");
  EXPECT_EQ(raw(doc, "b.SubB", "k"), "2");
  EXPECT_EQ(raw(doc, "b.subb", "k"), "<absent>");
  gtext_ini_free(doc);
}

TEST(IniGit, ASubsectionMayHoldBytesTheSectionNameMayNot) {
  /* Any byte but a terminator, `]` and `[` included, and a backslash *drops* -
   * so `\t` here is the letter t, while `\t` in a value one line later is a tab.
   * One file, two escape layers, chosen by position. */
  GTEXT_INI_Document * doc =
      ok("[a \"b]c[\"]\nk = x\\ty\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.b]c[.k"}));
  EXPECT_EQ(decoded_first(doc), "x\ty");
  gtext_ini_free(doc);

  doc = ok("[a \"x\\ty\"]\nk = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.xty.k"}));
  gtext_ini_free(doc);

  doc = ok("[a \"x\\\"y\"]\nk = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.x\"y.k"}));
  gtext_ini_free(doc);

  /* An empty subsection is legal where an empty *section* is not. */
  doc = ok("[a \"\"]\nk = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a..k"}));
  gtext_ini_free(doc);
  refused("[]\nk = v\n", GTEXT_INI_E_BAD_GROUP, git());

  /* The spelling is exactly `[name "sub"]`: whitespace before the quote is
   * required and nothing may follow the closing quote. */
  refused("[a\"b\"]\nk = v\n", GTEXT_INI_E_BAD_GROUP, git());
  refused("[a \"b\" ]\nk = v\n", GTEXT_INI_E_BAD_GROUP, git());
  refused("[a \"b\" \"c\"]\nk = v\n", GTEXT_INI_E_BAD_GROUP, git());
}

TEST(IniGit, QuotingIsAToggleRatherThanAWrapper) {
  /* The rule most easily got wrong. A reader requiring the value to begin and
   * end with a quote would refuse three documents git accepts. */
  struct { const char * text; const char * want; } cases[] = {
      {"[a]\nk = x\" mid \"y\n", "x mid y"},
      {"[a]\nk = \"a\"b\n", "ab"},
      {"[a]\nk = \"a\" \"b\"\n", "a b"},
      {"[a]\nk = \"  spaced  \"\n", "  spaced  "},
      {"[a]\nk = \"v # not a comment\"\n", "v # not a comment"},
      {"[a]\nk = \"\"\n", ""},
  };
  for (const auto & c : cases) {
    GTEXT_INI_Document * doc = ok(c.text, git());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(decoded_first(doc), c.want) << c.text;
    gtext_ini_free(doc);
  }
  /* A run left open when the logical line ends is an error, at end of input as
   * well as at a newline. */
  refused("[a]\nk = \"abc\n", GTEXT_INI_E_BAD_LINE, git());
  refused("[a]\nk = \"abc", GTEXT_INI_E_BAD_LINE, git());
}

TEST(IniGit, AnInlineCommentEndsTheValueAndQuotesProtectIt) {
  struct { const char * text; const char * want; } cases[] = {
      {"[a]\nk = v # c\n", "v"},
      {"[a]\nk = v ; c\n", "v"},
      {"[a]\nk = v#tight\n", "v"},
      {"[a]\nk = # c\n", ""},
      {"[a]\nk = ;\n", ""},
  };
  for (const auto & c : cases) {
    GTEXT_INI_Document * doc = ok(c.text, git());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(decoded_first(doc), c.want) << c.text;
    /* The comment is still in the document: it lives in the entry's `eol`, so a
     * rewrite reproduces it. */
    EXPECT_EQ(written(doc), c.text) << c.text;
    gtext_ini_free(doc);
  }
}

TEST(IniGit, AContinuationJoinsWithNothingAndACommentSwallowsIt) {
  struct { const char * text; const char * want; } cases[] = {
      /* Joined with nothing, so the space that survives is the one that was
       * already in the value. */
      {"[a]\nk = one\\\ntwo\n", "onetwo"},
      {"[a]\nk = one \\\ntwo\n", "one two"},
      /* Inside a quoted run it is still a continuation. */
      {"[a]\nk = \"one\\\ntwo\"\n", "onetwo"},
      /* CRLF after the backslash, because git turns CRLF into LF as it reads. */
      {"[a]\nk = one\\\r\ntwo\n", "onetwo"},
      /* End of input right after the backslash: a continuation onto nothing. */
      {"[a]\nk = one\\", "one"},
  };
  for (const auto & c : cases) {
    GTEXT_INI_Document * doc = ok(c.text, git());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(decoded_first(doc), c.want) << c.text;
    /* The five pieces still tile the line: `value` holds the backslashes and the
     * terminators they span. */
    EXPECT_EQ(written(doc), c.text) << c.text;
    gtext_ini_free(doc);
  }

  /* Once a comment has started the backslash is comment text, so the line ends
   * at its terminator and the next line is an entry of its own. Measured. */
  GTEXT_INI_Document * doc = ok("[a]\nk = v # c\\\nj = w\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.k", "a.j"}));
  EXPECT_EQ(raw(doc, "a", "j"), "w");
  gtext_ini_free(doc);

  /* A continuation belongs to the value grammar, so a trailing backslash on a
   * header line or on a valueless key is just a stray byte. */
  refused("[a]\\\nk = v\n", GTEXT_INI_E_BAD_KEY, git());
  refused("[a]\nbare\\\n", GTEXT_INI_E_BAD_LINE, git());
}

TEST(IniGit, AContinuationFixesTheTrailingWhitespaceBoundary) {
  /*
   * The rule a first reading of `git-config(1)` does not give, and the one this
   * differential found only at 20,000 documents rather than 500: a continuation
   * marks everything written so far as **content**, so the trailing spaces before
   * it survive - while the same value without the backslash has them trimmed.
   *
   * It needed a trailing-space value and a continuation to meet in one document,
   * which is why a smaller population missed it and why a hand-written corpus
   * never would have had it at all.
   */
  struct { const char * text; const char * want; } cases[] = {
      {"[a]\nk = false   \\", "false   "},
      {"[a]\nk = false   \\\n", "false   "},
      {"[a]\nk = false   \\\n# c\n", "false   "},
      {"[a]\nk = false   \\\nj = 1\n", "false   j = 1"},
      /* Without the continuation, trimmed. */
      {"[a]\nk = false   \n", "false"},
      {"[a]\nk = false   ", "false"},
      /* A backslash with no content before it does not resurrect the leading
       * run, because that run was never written at all. */
      {"[a]\nk =    \\", ""},
      /* `\\` is the escape for a literal backslash, which is content, so the
       * spaces before it are covered too. */
      {"[a]\nk = false   \\\\", "false   \\"},
  };
  for (const auto & c : cases) {
    GTEXT_INI_Document * doc = ok(c.text, git());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(decoded_first(doc), c.want) << c.text;
    EXPECT_EQ(written(doc), c.text) << c.text;
    gtext_ini_free(doc);
  }

  /*
   * The two caller-set cases that look alike and are not, which is the whole
   * reason the writer asks the scanner instead of matching bytes:
   *
   *   - `a\` followed by a terminator is **refused**: the backslash has not
   *     consumed a terminator yet, so it takes the one written after it and the
   *     stored value comes back as `a\<LF>` instead.
   *   - `a\<LF>` is **written**: it already contains the newline its join went
   *     over, so writing a terminator after it gives `a\<LF><LF>`, whose re-read
   *     performs the same join, stops at the second terminator, and stores the
   *     same bytes.
   */
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = git();
  GTEXT_INI_Document * doc = gtext_ini_new(&opts);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * group = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "core", &group), GTEXT_INI_OK);

  ASSERT_EQ(gtext_ini_group_set(group, "url", "a\\", 2), GTEXT_INI_OK);
  GTEXT_INI_Sink sink;
  ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_write(doc, &sink, nullptr), GTEXT_INI_E_UNREPRESENTABLE);
  gtext_ini_sink_buffer_free(&sink);

  ASSERT_EQ(gtext_ini_group_set(group, "url", "a\\\n", 3), GTEXT_INI_OK);
  std::string out = written(doc);
  EXPECT_EQ(out, "[core]\nurl=a\\\n\n");
  GTEXT_INI_Document * again = ok(out, git());
  ASSERT_NE(again, nullptr);
  /* The raw value survives, not merely the decoded one. */
  EXPECT_EQ(raw(again, "core", "url"), "a\\\n");
  gtext_ini_free(again);
  gtext_ini_free(doc);
}

TEST(IniGit, AValuelessKeyIsNotAnEmptyValue) {
  GTEXT_INI_Document * doc = ok("[a]\nbare\nempty =\n", git());
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  ASSERT_EQ(gtext_ini_group_entry_count(group), 2u);

  /* The distinction NULL cannot carry: `gtext_ini_group_get` answers NULL for a
   * key that is absent *and* for one present with no value, so the predicate is
   * what separates "unset" from git's shorthand for boolean true. */
  EXPECT_FALSE(gtext_ini_group_value_present_at(group, 0));
  EXPECT_TRUE(gtext_ini_group_value_present_at(group, 1));
  EXPECT_EQ(gtext_ini_group_find(group, "bare", 0), 0u);
  EXPECT_EQ(gtext_ini_group_find(group, "empty", 0), 1u);
  EXPECT_EQ(gtext_ini_group_find(group, "missing", 0), SIZE_MAX);
  /* Folded, like every other key lookup. */
  EXPECT_EQ(gtext_ini_group_find(group, "BARE", 0), 0u);

  size_t len = 0;
  EXPECT_EQ(gtext_ini_group_value_at(group, 0, &len), nullptr);
  EXPECT_EQ(got(gtext_ini_group_value_at, group, (size_t) 1), "");

  /* Written back with no `=`, because `bare=` would mean something else. */
  EXPECT_EQ(written(doc), "[a]\nbare\nempty =\n");
  gtext_ini_free(doc);

  /* Measured, and in `git-config(1)` nowhere: a valueless key may not carry a
   * trailing comment, though a header may. */
  refused("[a]\nbare ; c\n", GTEXT_INI_E_BAD_LINE, git());
  doc = ok("[a] ; c\nk = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(written(doc), "[a] ; c\nk = v\n");
  gtext_ini_free(doc);
}

TEST(IniGit, TheRemainderOfAHeaderLineIsAnEntry) {
  GTEXT_INI_Document * doc = ok("[a] k = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.k"}));
  EXPECT_EQ(raw(doc, "a", "k"), "v");
  EXPECT_EQ(written(doc), "[a] k = v\n");
  gtext_ini_free(doc);

  /* `[a] junk` is a *valueless* key, not an error. */
  doc = ok("[a] junk\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.junk"}));
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  EXPECT_FALSE(gtext_ini_group_value_present_at(group, 0));
  EXPECT_EQ(written(doc), "[a] junk\n");
  gtext_ini_free(doc);
}

TEST(IniGit, AnEntryBeforeAnySectionIsAPreambleRatherThanAnError) {
  /* `git-config(1)` says a variable "must belong to some section". git accepts
   * one that does not, and names it with no prefix at all. */
  GTEXT_INI_Document * doc = ok("k = v\n[a]\nj = w\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"k", "a.j"}));
  const GTEXT_INI_Group * first = gtext_ini_document_group_at(doc, 0);
  EXPECT_TRUE(gtext_ini_group_is_preamble(first));
  EXPECT_EQ(got(gtext_ini_group_name, first), "");
  EXPECT_FALSE(gtext_ini_group_is_preamble(
      gtext_ini_document_group_at(doc, 1)));
  /* No header is invented for it: `[]` is not even a legal git header. */
  EXPECT_EQ(written(doc), "k = v\n[a]\nj = w\n");
  gtext_ini_free(doc);

  /* Desktop Entry still refuses one, which is what allow_preamble is for. */
  refused("Before=1\n[Desktop Entry]\nType=Application\nName=n\n",
      GTEXT_INI_E_NO_GROUP);
}

TEST(IniGit, EveryOccurrenceOfARepeatedKeyIsAValueAndTheLastIsTheAnswer) {
  GTEXT_INI_Document * doc = ok("[a]\nk = 1\nk = 2\n", git());
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  EXPECT_EQ(gtext_ini_group_count_key(group, "k"), 2u);
  /* `--get-all` order. */
  EXPECT_EQ(got(gtext_ini_group_get_nth, group, "k", (size_t) 0), "1");
  EXPECT_EQ(got(gtext_ini_group_get_nth, group, "k", (size_t) 1), "2");
  /* `--get` answers the last - measured, and the opposite of what this enum's
   * documentation claimed before any dialect used it. */
  EXPECT_EQ(got(gtext_ini_group_get, group, "k"), "2");
  EXPECT_EQ(raw(doc, "a", "k"), "2");
  gtext_ini_free(doc);

  /* A repeated *header* merges: the lookup crosses both groups, and the tree
   * keeps them apart so the rewrite is exact. */
  doc = ok("[a]\nk = 1\n[a]\nk = 2\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gtext_ini_document_group_count(doc), 2u);
  EXPECT_EQ(raw(doc, "a", "k"), "2");
  EXPECT_EQ(written(doc), "[a]\nk = 1\n[a]\nk = 2\n");
  gtext_ini_free(doc);
}

TEST(IniGit, TheEscapeSetIsGitsAndNotDesktopEntrys) {
  struct { const char * text; const char * want; } cases[] = {
      {"[a]\nk = a\\tb\n", "a\tb"},
      {"[a]\nk = a\\nb\n", "a\nb"},
      {"[a]\nk = a\\bb\n", "a\bb"},
      {"[a]\nk = a\\\"b\n", "a\"b"},
      {"[a]\nk = a\\\\b\n", "a\\b"},
      /* An escape at the very end is *content*, so it is not trimmed as trailing
       * whitespace even when it spells one. The discriminating case for the
       * scanner's last-content-byte tracking, and the one a right-to-left trim
       * gets wrong. */
      {"[a]\nk = a\\t\n", "a\t"},
  };
  for (const auto & c : cases) {
    GTEXT_INI_Document * doc = ok(c.text, git());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(decoded_first(doc), c.want) << c.text;
    gtext_ini_free(doc);
  }
  /* `\r` and `\s` are Desktop Entry's and are not git's. "Other char escape
   * sequences (including octal escape sequences) are invalid." */
  refused("[a]\nk = a\\rb\n", GTEXT_INI_E_BAD_ESCAPE, git());
  refused("[a]\nk = a\\sb\n", GTEXT_INI_E_BAD_ESCAPE, git());
  refused("[a]\nk = a\\qb\n", GTEXT_INI_E_BAD_ESCAPE, git());
  refused("[a]\nk = a\\101b\n", GTEXT_INI_E_BAD_ESCAPE, git());
  refused("[a]\nk = a\\ b\n", GTEXT_INI_E_BAD_ESCAPE, git());
  /* A backslash before a *lone* CR is neither a continuation nor an escape. */
  refused("[a]\nk = a\\\rb\n", GTEXT_INI_E_BAD_ESCAPE, git());
}

TEST(IniGit, WhitespaceIsGitsOwnCtypeAndNotTheCLibrarys) {
  /* `\v` and `\f` are control characters to git, not space. So a trailing `\v`
   * stays in the value where a trailing space would be trimmed, and a `\v` where
   * a key should start is a syntax error rather than skipped indentation. A
   * parser built on isspace() differs from git on exactly these two bytes, which
   * no corpus of real files would ever show. */
  GTEXT_INI_Document * doc = ok("[a]\nk = a\v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(decoded_first(doc), "a\v");
  gtext_ini_free(doc);
  refused("[a]\n\vk = v\n", GTEXT_INI_E_BAD_KEY, git());

  /* CR *is* space to git: interior, it is data; trailing, it is trimmed. */
  doc = ok("[a]\nk = a\rb\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(decoded_first(doc), "a\rb");
  gtext_ini_free(doc);
  /* And it may not sit between a key and its `=`, because that run is matched
   * against space and tab literally. */
  refused("[a]\nk\r= v\n", GTEXT_INI_E_BAD_LINE, git());
}

TEST(IniGit, KeyAndSectionCharsetsDifferAndBothAreStricterThanDesktopEntrys) {
  /* A key must begin with a letter; a section name need not. */
  GTEXT_INI_Document * doc = ok("[12]\nk-1 = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"12.k-1"}));
  gtext_ini_free(doc);
  refused("[a]\n1k = v\n", GTEXT_INI_E_BAD_KEY, git());
  refused("[a]\n-k = v\n", GTEXT_INI_E_BAD_KEY, git());
  refused("[a]\n= v\n", GTEXT_INI_E_BAD_KEY, git());
  /* These end the key scan at a byte outside the charset, so what follows is
   * neither `=` nor the end of the line. */
  refused("[a]\nk_1 = v\n", GTEXT_INI_E_BAD_LINE, git());
  refused("[a]\nk.1 = v\n", GTEXT_INI_E_BAD_LINE, git());
  /* A section name is `A-Za-z0-9-.`: stricter than Desktop Entry's "any ASCII
   * but [ ] and control", which is why this is not a relaxation. */
  refused("[a_b]\nk = v\n", GTEXT_INI_E_BAD_GROUP, git());
  refused("[caf\xc3\xa9]\nk = v\n", GTEXT_INI_E_BAD_GROUP, git());
  /* A dot is legal anywhere in a section name, including alone. */
  doc = ok("[a.b.c.d]\nk = v\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.b.c.d.k"}));
  gtext_ini_free(doc);
}

TEST(IniGit, AValueIsNotValidatedAsUtf8BecauseGitReturnsItUnchanged) {
  /* The axis the two references disagree about: `g_key_file_get_string()`
   * refuses a bare 0xFF, and `git config --get` hands it back. So this is a
   * property of the dialect rather than of the format's character set. */
  EXPECT_TRUE(gtext_ini_dialect_desktop_entry().utf8_values);
  EXPECT_FALSE(git().utf8_values);
  /* Split string literals: `"\xffb"` would read `ffb` as one hex escape. */
  GTEXT_INI_Document * doc = ok("[a]\nk = a\xff" "b\n", git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(decoded_first(doc), "a\xff" "b");
  gtext_ini_free(doc);
}

TEST(IniGit, ANulInAValueIsKeptWhereGitTruncates) {
  /* The one deliberate deviation, and it is in the direction of representing
   * more than the reference can: this reader is length-based throughout, so a
   * document git reads as `a` is read here as the three bytes it contains.
   * Refusing to store it would make the library unable to represent a document
   * git accepts. The differential excludes these and counts them. */
  std::string text("[a]\nk = a\0b\n", 12);
  GTEXT_INI_Document * doc = ok(text, git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(decoded_first(doc), std::string("a\0b", 3));
  EXPECT_EQ(written(doc), text);
  gtext_ini_free(doc);
}

TEST(IniGit, ABomIsSkippedAndStillWrittenBack) {
  /* Skipped means "not part of the first line", not "discarded": a document that
   * opened with one and comes back three bytes shorter is not the same document.
   * Found by the git differential rather than by the fuzzer, because the fuzzer
   * asserts the byte-identical rewrite under the *strict* dialect, which refuses
   * a BOM outright and so can never reach this path. */
  std::string text = "\xef\xbb\xbf[a]\nk = v\n";
  GTEXT_INI_Document * doc = ok(text, git());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_names(doc), (std::vector<std::string>{"a.k"}));
  EXPECT_EQ(written(doc), text);
  gtext_ini_free(doc);

  /* The generic dialect skips one too, and had the same defect. */
  doc = ok(text, gtext_ini_dialect_generic());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(written(doc), text);
  gtext_ini_free(doc);
}

TEST(IniGit, TheWriterRefusesAValueThatWouldNotReadBackAsItself) {
  /*
   * Unreachable from any parse - every value a parse stored is writable by
   * construction - so these come from a caller setting one, and a mutation
   * removing the check passed a 500-document differential untouched.
   *
   * The check is a re-scan rather than a list of dangerous bytes, which is why a
   * continuation survives it and a trailing backslash does not.
   */
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = git();
  GTEXT_INI_Document * doc = gtext_ini_new(&opts);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * group = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "core", &group), GTEXT_INI_OK);
  ASSERT_NE(group, nullptr);

  struct { const char * value; bool writable; const char * why; } cases[] = {
      {"plain", true, "nothing to quote"},
      {"a\\tb", true, "an escape re-reads as itself"},
      {"one\\\ntwo", true, "a continuation is a value that reads back the same"},
      {"\"a b\"", true, "a balanced quoted run"},
      {"a ", false, "a trailing space would be trimmed"},
      {" a", false, "a leading run is dropped before any content"},
      {"a#b", false, "the rest would come back as a comment"},
      {"a;b", false, "likewise"},
      {"a\\", false, "a trailing backslash would swallow the terminator"},
      {"a\"", false, "an unbalanced quote does not even scan"},
      {"a\nb", false, "a bare newline ends the line early"},
  };
  for (const auto & c : cases) {
    ASSERT_EQ(gtext_ini_group_set(group, "url", c.value, strlen(c.value)),
        GTEXT_INI_OK) << c.value;
    GTEXT_INI_Sink sink;
    ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
    GTEXT_INI_Status status = gtext_ini_write(doc, &sink, nullptr);
    if (c.writable) {
      EXPECT_EQ(status, GTEXT_INI_OK) << c.value << ": " << c.why;
      if (status == GTEXT_INI_OK) {
        /* And it does read back as itself, which is the property the check is
         * standing in for. */
        std::string out(gtext_ini_sink_buffer_data(&sink),
            gtext_ini_sink_buffer_size(&sink));
        GTEXT_INI_Document * again = ok(out, git());
        ASSERT_NE(again, nullptr) << out;
        EXPECT_EQ(raw(again, "core", "url"), c.value) << out;
        gtext_ini_free(again);
      }
    }
    else {
      EXPECT_EQ(status, GTEXT_INI_E_UNREPRESENTABLE)
          << c.value << ": " << c.why;
    }
    gtext_ini_sink_buffer_free(&sink);
  }
  gtext_ini_free(doc);
}

TEST(IniGit, ACallerAddsAGroupByItsHeaderSpellingNotItsCanonicalName) {
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = git();
  GTEXT_INI_Document * doc = gtext_ini_new(&opts);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * group = nullptr;
  /* The header spelling, so a caller can choose which of the two subsection
   * forms to write. The canonical name is derived from it. */
  ASSERT_EQ(gtext_ini_document_add_group(doc, "remote \"orig in\"", &group),
      GTEXT_INI_OK);
  ASSERT_EQ(gtext_ini_group_set(group, "url", "x", 1), GTEXT_INI_OK);
  EXPECT_EQ(got(gtext_ini_group_canonical_name, group), "remote.orig in");
  EXPECT_EQ(raw(doc, "remote.orig in", "url"), "x");
  EXPECT_EQ(written(doc), "[remote \"orig in\"]\nurl=x\n");
  /* And a name that is not a legal header is refused rather than stored. */
  EXPECT_EQ(gtext_ini_document_add_group(doc, "bad_section", nullptr),
      GTEXT_INI_E_BAD_GROUP);
  gtext_ini_free(doc);
}

TEST(IniGit, UnescapeAndTheParserShareOneImplementation) {
  /* The parser calls gtext_ini_scan_value() to find where the logical line ends
   * and gtext_ini_unescape() calls it again over the stored bytes. Re-scanning
   * the stored span must give the same answer, which is what makes one function
   * sound here: the span ends at a content byte, so nothing dropped can affect
   * what is kept. */
  GTEXT_INI_Dialect d = git();
  const char * cases[] = {"\"a\" \"b\"", "x\" mid \"y", "a\\tb", "one\\\ntwo",
                          "\"v # c\"", "a\\t"};
  for (const char * text : cases) {
    std::string doc_text = std::string("[a]\nk = ") + text + "\n";
    GTEXT_INI_Document * doc = ok(doc_text, git());
    ASSERT_NE(doc, nullptr) << doc_text;
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
    size_t len = 0;
    const char * stored = gtext_ini_group_value_at(group, 0, &len);
    /* The stored span is exactly the value's own bytes. */
    EXPECT_EQ(take(stored, len), text) << doc_text;
    char * first = nullptr;
    size_t first_len = 0;
    ASSERT_EQ(gtext_ini_unescape(&d, stored, len, nullptr, &first, &first_len),
        GTEXT_INI_OK) << doc_text;
    std::string once = take(first, first_len);
    gtext_ini_string_free(nullptr, first);
    gtext_ini_free(doc);

    /* Decoding the decoded form again is not the property - that would be
     * idempotence, which escapes do not have. The property is that the stored
     * span decodes to what the whole line decoded to. */
    std::string direct_text = std::string("[a]\nk = ") + text;
    GTEXT_INI_Document * trimmed = ok(direct_text, git());
    ASSERT_NE(trimmed, nullptr) << direct_text;
    EXPECT_EQ(decoded_first(trimmed), once) << direct_text;
    gtext_ini_free(trimmed);
  }
}

// ------------------------------------------------------- the EditorConfig dialect
//
// Every rule here was measured against **two** cores, editorconfig-core-c 0.12.11
// and editorconfig-core-py 0.17.1, and against specification 0.17.2 and its
// conformance suite - which is the only normative INI conformance suite that
// exists. The probe transcript is notes/text/INI-DIALECTS.md §A.16;
// `make conformance-ini-editorconfig` scores the suite's 34 grammar assertions and
// `make check-ini-editorconfig-oracle` differs against both cores over 20,000
// generated documents.
//
// **Both cores score 33 of those 34**, so this is the one dialect here where
// agreeing with the reference implementations everywhere would be a failure. The
// cases below are the ones where the specification and a core disagree, plus the
// ones no differential over parsed documents can reach: a caller building a
// document, and the writer's refusals.

namespace {

GTEXT_INI_Dialect ec() { return gtext_ini_dialect_editorconfig(); }

/** The canonical key of every entry, in document order. */
std::vector<std::string> canonical_keys(const GTEXT_INI_Document * doc) {
  std::vector<std::string> out;
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    for (size_t e = 0; e < gtext_ini_group_entry_count(group); e++) {
      out.push_back(got(gtext_ini_group_canonical_key_at, group, e));
    }
  }
  return out;
}

/** The name of every group, as the document spelled it. */
std::vector<std::string> group_names(const GTEXT_INI_Document * doc) {
  std::vector<std::string> out;
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    out.push_back(gtext_ini_group_is_preamble(group)
                      ? std::string("<preamble>")
                      : got(gtext_ini_group_name, group));
  }
  return out;
}

} // namespace

TEST(IniEditorConfig, TheDialectIsNotARelaxationOfDesktopEntryInEitherDirection) {
  GTEXT_INI_Dialect e = ec();
  GTEXT_INI_Dialect de = gtext_ini_dialect_desktop_entry();
  EXPECT_EQ(e.id, GTEXT_INI_DIALECT_EDITORCONFIG);
  /* Accepts what Desktop Entry refuses. */
  EXPECT_TRUE(e.comment_semicolon);
  EXPECT_TRUE(e.allow_preamble);
  EXPECT_TRUE(e.allow_duplicate_groups);
  EXPECT_EQ(e.dupkey, GTEXT_INI_DUPKEY_LAST_WINS);
  EXPECT_TRUE(e.accept_crlf);
  EXPECT_TRUE(e.skip_bom);
  EXPECT_EQ(e.name_style, GTEXT_INI_NAMES_EDITORCONFIG);
  /* And refuses what Desktop Entry accepts: the escape set and the list. */
  EXPECT_NE(de.escapes, nullptr);
  EXPECT_EQ(e.escapes, nullptr);
  EXPECT_EQ(de.list_separator, ';');
  EXPECT_EQ(e.list_separator, 0);
  /* Nothing git has: no inline comments, no quoting, no continuation, no
   * valueless key, no subsection, no entry after the header. */
  EXPECT_FALSE(e.inline_comments);
  EXPECT_FALSE(e.quoted_values);
  EXPECT_EQ(e.continuation, GTEXT_INI_CONTINUATION_NONE);
  EXPECT_FALSE(e.valueless_keys);
  EXPECT_FALSE(e.subsection_syntax);
  EXPECT_EQ(e.header_remainder, GTEXT_INI_HEADER_REMAINDER_ERROR);
  /* The whitespace set: C's, where git's is space-and-tab and configparser's is
   * Python's. Three dialects, three answers, six bytes between them. */
  EXPECT_EQ(e.space_set, GTEXT_INI_SPACE_CTYPE);
  EXPECT_EQ(git().space_set, GTEXT_INI_SPACE_BLANK);
  EXPECT_EQ(de.space_set, GTEXT_INI_SPACE_BLANK);
  /* Keys fold and section names do not, which is the split configparser also
   * needs and which used to be spelled by excluding a name style. */
  EXPECT_TRUE(e.fold_case);
  EXPECT_FALSE(e.fold_group_case);
  EXPECT_TRUE(git().fold_group_case);
  /* The one dialect here that accepts `[]`. */
  EXPECT_TRUE(e.allow_empty_group_name);
  EXPECT_FALSE(de.allow_empty_group_name);
}

TEST(IniEditorConfig, TheLineIsTrimmedBeforeItIsClassified) {
  /*
   * "For each line: 1. Remove all leading and trailing whitespace. 2. Process the
   * remaining text as specified for its type." The trim comes *first*, so an
   * indented header is a header and an indented comment is a comment.
   *
   * core-py gets the second one wrong - it tests `line[0] in '#;'` before
   * stripping, so `  # c` is a parse error there - and the conformance suite does
   * not cover it.
   */
  GTEXT_INI_Document * doc =
      ok("   # an indented comment\n  [a.c]   \n\t \n  k = v  \n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_names(doc), (std::vector<std::string>{"a.c"}));
  EXPECT_EQ(raw(doc, "a.c", "k"), "v");
  /* And the whole thing writes back byte for byte, indentation included. */
  EXPECT_EQ(written(doc), "   # an indented comment\n  [a.c]   \n\t \n  k = v  \n");
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, TheHeaderClosesAtTheLastBracket) {
  /*
   * "May contain any characters between the square brackets", so the name runs to
   * the *last* `]` on the line. Both cores do this - core-c with
   * `find_last_char_or_comment` and core-py with a greedy match that backtracks to
   * the same place.
   *
   * The contrast is the point: the same bytes under git close at the first `]`,
   * which is why this is a property of the name grammar rather than of the parser.
   */
  GTEXT_INI_Document * doc = ok("[a]b]\nk=v\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_names(doc), (std::vector<std::string>{"a]b"}));
  EXPECT_EQ(raw(doc, "a]b", "k"), "v");
  EXPECT_EQ(written(doc), "[a]b]\nk=v\n");
  gtext_ini_free(doc);

  /* git's names cannot hold a `]`, so it closes at the first one and reads the
   * remainder `b]` as an entry on the same line - which has no `=` after its key,
   * and a valueless key may not be followed by anything. Measured: git exits 128
   * on this document too, for the same reason. */
  refused("[a]b]\nk=v\n", GTEXT_INI_E_BAD_LINE, git());
}

TEST(IniEditorConfig, ASectionNameMayHoldAnyByteIncludingCommentIntroducers) {
  /*
   * core-py refuses all three of these: its section pattern excludes an unescaped
   * `#` or `;` outright, which the specification does not. core-c accepts them, and
   * so does this.
   */
  for (const char * text : {"[a#b]\nk=v\n", "[a;b]\nk=v\n", "[a\tb]\nk=v\n",
           "[a[b]\nk=v\n"}) {
    GTEXT_INI_Document * doc = ok(text, ec());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(written(doc), text) << text;
    gtext_ini_free(doc);
  }
}

TEST(IniEditorConfig, AnEmptySectionNameIsAcceptedAndIsNotThePreamble) {
  /*
   * `[]` is accepted - core-c accepts it, core-py refuses it, and the
   * specification says any characters. Refusing it would throw away the rest of a
   * document over a section that matches no file.
   *
   * The cost is the one ambiguity this dialect has: an empty group name is also
   * the preamble's, so gtext_ini_group_find("") can find either.
   * gtext_ini_group_is_preamble() is what separates them, which is why it is a
   * public accessor rather than an internal flag.
   */
  GTEXT_INI_Document * doc = ok("pre=1\n[]\nk=v\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_names(doc),
      (std::vector<std::string>{"<preamble>", ""}));
  EXPECT_TRUE(gtext_ini_group_is_preamble(gtext_ini_document_group_at(doc, 0)));
  EXPECT_FALSE(gtext_ini_group_is_preamble(gtext_ini_document_group_at(doc, 1)));
  EXPECT_EQ(written(doc), "pre=1\n[]\nk=v\n");
  gtext_ini_free(doc);

  /* Every other dialect here refuses it, including on the query side - GKeyFile
   * fails with "Invalid group name: " and git exits 128. */
  refused("[]\nk=v\n", GTEXT_INI_E_BAD_GROUP, git());
  refused("[]\nk=v\n", GTEXT_INI_E_BAD_GROUP, gtext_ini_dialect_generic());
}

TEST(IniEditorConfig, AKeyIsEverythingBeforeTheFirstEquals) {
  /*
   * "The key is the part before the first `=` on the line", trimmed. So a key may
   * hold a space - the suite asserts `ke y=value` - and a `:`, and a second `=` is
   * part of the value.
   *
   * core-py ends a key at a `:`, so it reads `k:e=v` as `k` = `e=v`. core-c and
   * this read the specification's answer.
   */
  GTEXT_INI_Document * doc = ok("[a.c]\nke y=value\nk:e=v\nj=a=b\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc),
      (std::vector<std::string>{"ke y", "k:e", "j"}));
  EXPECT_EQ(raw(doc, "a.c", "ke y"), "value");
  EXPECT_EQ(raw(doc, "a.c", "k:e"), "v");
  EXPECT_EQ(raw(doc, "a.c", "j"), "a=b");
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, KeysFoldAndSectionNamesDoNot) {
  /*
   * "Pair keys are case-insensitive. All keys are lowercased after parsing." The
   * specification says nothing about a section, and a section is a filepath glob
   * whose case significance is the filesystem's question rather than the format's -
   * so `[A.C]` and `[a.c]` are two groups while `Indent` and `indent` are one key.
   *
   * This is why gtext_ini_group_names_fold() exists: `fold_case` alone would fold
   * both, as it does for git.
   */
  GTEXT_INI_Document * doc = ok("[A.C]\nIndent=1\n[a.c]\nindent=2\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_names(doc), (std::vector<std::string>{"A.C", "a.c"}));
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"indent", "indent"}));
  /* Two distinct groups, each reachable only by its own spelling. */
  EXPECT_EQ(raw(doc, "A.C", "indent"), "1");
  EXPECT_EQ(raw(doc, "a.c", "INDENT"), "2");
  EXPECT_EQ(raw(doc, "a.C", "indent"), "<absent>");
  /* The document's own spelling of the key is kept, so the rewrite is exact. */
  EXPECT_EQ(written(doc), "[A.C]\nIndent=1\n[a.c]\nindent=2\n");
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, ThereAreNoInlineCommentsAndNoEscapes) {
  /*
   * **The rule both cores get wrong.** "A `;` or `#` anywhere other than at the
   * beginning of a line does *not* start a comment, but is part of the text of that
   * line", and the suite's `semicolon_or_hash_in_property` asserts it. Both cores
   * truncate a value at a whitespace-preceded `#` or `;` - inherited from Python's
   * ConfigParser - and both therefore score 33 of 34.
   *
   * No escapes either: the specification defines none, so a backslash is a byte and
   * the suite asserts that `value \; not comment` keeps it.
   */
  GTEXT_INI_Document * doc =
      ok("[a.c]\nk1=value; not comment\nk2=value # not comment\n"
         "k3=value \\; not comment\nk4=value#tight\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a.c", "k1"), "value; not comment");
  EXPECT_EQ(raw(doc, "a.c", "k2"), "value # not comment");
  EXPECT_EQ(raw(doc, "a.c", "k3"), "value \\; not comment");
  EXPECT_EQ(raw(doc, "a.c", "k4"), "value#tight");

  /* And the value layer is a copy, not a decode: `\;` stays two bytes. */
  GTEXT_INI_Dialect d = ec();
  size_t len = 0;
  const char * stored = gtext_ini_document_get(doc, "a.c", "k3", &len);
  ASSERT_NE(stored, nullptr);
  char * decoded = nullptr;
  size_t decoded_len = 0;
  ASSERT_EQ(gtext_ini_unescape(&d, stored, len, nullptr, &decoded, &decoded_len),
      GTEXT_INI_OK);
  EXPECT_EQ(take(decoded, decoded_len), "value \\; not comment");
  gtext_ini_string_free(nullptr, decoded);
  gtext_ini_free(doc);

  /* The same bytes under git are three different values, because git has both
   * inline comments and `\\` in its escape set. */
  GTEXT_INI_Document * g = ok("[a]\nk2 = value # not comment\n", git());
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(raw(g, "a", "k2"), "value");
  gtext_ini_free(g);
}

TEST(IniEditorConfig, TrailingWhitespaceIsTrimmedAndInnerWhitespaceIsKept) {
  /*
   * "Keys and values are trimmed of leading and trailing whitespace, but include
   * any whitespace that is between non-whitespace characters." The suite asserts
   * both halves, and Desktop Entry is the contrast: it *keeps* a trailing run,
   * because GLib does.
   */
  GTEXT_INI_Document * doc =
      ok("[a.c]\nkey= value with whitespace inside  \nempty=  \n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a.c", "key"), "value with whitespace inside");
  EXPECT_EQ(raw(doc, "a.c", "empty"), "");
  EXPECT_EQ(written(doc), "[a.c]\nkey= value with whitespace inside  \nempty=  \n");
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, AVerticalTabIsWhitespaceHereAndNotInGit) {
  /*
   * Two bytes, and the two dialects hold opposite rules about them. Both
   * EditorConfig cores ask the platform - core-c calls `isspace()` and core-py
   * matches `\s` - while git carries its own ctype table classing `\v` and `\f` as
   * control characters. Measured on both sides: core-c reads `k=\va\v` as `a`, and
   * git keeps the trailing vertical tab in its value.
   *
   * No corpus of real files contains either byte in either position, which is why
   * this is a field and a test rather than an assumption.
   */
  GTEXT_INI_Document * doc = ok("[a.c]\n\vk=\va\v\n\fj=\fb\f\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a.c", "k"), "a");
  EXPECT_EQ(raw(doc, "a.c", "j"), "b");
  EXPECT_EQ(written(doc), "[a.c]\n\vk=\va\v\n\fj=\fb\f\n");
  gtext_ini_free(doc);

  /*
   * And in the **separator run**, which is a third notion of whitespace in the
   * same parser: git matches the run between a key and its `=` against `' '` and
   * `'\t'` literally, so `k\r= v` is refused by git while `\rk = v` is accepted.
   * EditorConfig trims the whole line with one predicate, so `k\v=\vv` is `k` =
   * `v`. Nothing else reaches ini_sep_space()'s arm for this dialect, which is why
   * the differential has an axis for it too.
   */
  doc = ok("[a.c]\nk\v=\vv\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k"}));
  EXPECT_EQ(raw(doc, "a.c", "k"), "v");
  EXPECT_EQ(written(doc), "[a.c]\nk\v=\vv\n");
  gtext_ini_free(doc);

  /* git: the trailing vertical tab is the value's last byte, and a leading one
   * where a key should start is a syntax error. */
  GTEXT_INI_Document * g = ok("[a]\nk = a\v\n", git());
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(raw(g, "a", "k"), "a\v");
  gtext_ini_free(g);
  refused("[a]\n\vk = v\n", GTEXT_INI_E_BAD_KEY, git());
}

TEST(IniEditorConfig, ALineWithNoEqualsIsInvalidAndAnEmptyValueIsNot) {
  /*
   * "Any line that is not one of the above is invalid." A bare key is not a pair,
   * because the pair rule needs the `=`; both cores report an error too. `k=` is a
   * different thing - an empty value - and is legal, which is why
   * ::GTEXT_INI_Dialect::valueless_keys is false rather than the two being
   * conflated.
   */
  refused("[a.c]\nbare\n", GTEXT_INI_E_BAD_LINE, ec());
  GTEXT_INI_Document * doc = ok("[a.c]\nk=\n", ec());
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  /* Present, and empty - not absent. git is where those differ. */
  EXPECT_TRUE(gtext_ini_group_value_present_at(group, 0));
  EXPECT_EQ(raw(doc, "a.c", "k"), "");
  gtext_ini_free(doc);

  /* git reads the same bare key as a valueless entry, its spelling for true. */
  GTEXT_INI_Document * g = ok("[a]\nbare\n", git());
  ASSERT_NE(g, nullptr);
  EXPECT_FALSE(gtext_ini_group_value_present_at(
      gtext_ini_document_group_at(g, 0), 0));
  gtext_ini_free(g);
}

TEST(IniEditorConfig, AColonIsNotASeparatorAndAnEmptyKeyIsRefused) {
  /*
   * Three refusals where at least one core accepts, each following from the same
   * sentence: a line that is not blank, a comment, a header or a `=` pair is
   * invalid.
   *
   *   - `k:v` has no `=`. **Both** cores accept `:` as a separator, inherited from
   *     ConfigParser; the specification's pair rule names `=` and nothing else.
   *   - `=v` and `   =v` have an empty key. core-c reads a property whose name is
   *     the empty string; core-py refuses, as this does.
   *   - `[a] junk` does not end with `]` after the trim, so it is not a header
   *     either. Both cores silently ignore the remainder.
   */
  refused("[a.c]\nk:v\n", GTEXT_INI_E_BAD_LINE, ec());
  refused("[a.c]\n=v\n", GTEXT_INI_E_BAD_KEY, ec());
  refused("[a.c]\n   =v\n", GTEXT_INI_E_BAD_KEY, ec());
  refused("[a.c] junk\nk=v\n", GTEXT_INI_E_BAD_LINE, ec());
  refused("[a.c\nk=v\n", GTEXT_INI_E_BAD_GROUP, ec());

  /* With the last-`]` rule the remainder case flips: `[a.c] junk]` is one
   * section whose name holds a space, which all three agree on. */
  GTEXT_INI_Document * doc = ok("[a.c] junk]\nk=v\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_names(doc), (std::vector<std::string>{"a.c] junk"}));
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, ALoneCarriageReturnIsNotALineSeparator) {
  /*
   * "LF or CRLF line separators" - a bare CR is neither. So `[a.c]\rk=v` is one
   * line that does not end with `]`, and it is invalid.
   *
   * All three answers differ here, which is the clearest single case for why the
   * specification gets the vote: core-c reads the section and silently drops the
   * rest of the line, core-py splits the line on the CR and reads both halves.
   */
  refused("[a.c]\rk=v\n", GTEXT_INI_E_BAD_LINE, ec());
  /* A CR that *is* part of a CRLF terminator is fine, and the suite asserts it. */
  GTEXT_INI_Document * doc = ok("[a.c]\r\nk = v\r\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a.c", "k"), "v");
  EXPECT_EQ(written(doc), "[a.c]\r\nk = v\r\n");
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, TheLastAssignmentWinsAcrossDuplicateSections) {
  /*
   * The suite's `repeat_sections_ML` and `basic_cascade_ML`: two sections with the
   * same name merge, and a repeated key takes the later value. Both are stored -
   * the tree keeps the document - and the lookup answers the last.
   */
  GTEXT_INI_Document * doc =
      ok("[a.c]\nopt1=first\nopt2=keep\n[a.c]\nopt1=second\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gtext_ini_document_group_count(doc), 2u);
  EXPECT_EQ(raw(doc, "a.c", "opt1"), "second");
  EXPECT_EQ(raw(doc, "a.c", "opt2"), "keep");
  EXPECT_EQ(written(doc), "[a.c]\nopt1=first\nopt2=keep\n[a.c]\nopt1=second\n");
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, ABomIsSkippedAndWrittenBack) {
  /*
   * The suite's `bom_at_head` requires that a leading BOM be skipped. Skipped is
   * not discarded: the document carries it and the writer puts it back, which is
   * the defect the git differential found in the generic dialect - a document
   * opening with one came back three bytes shorter.
   */
  const std::string text = "\xEF\xBB\xBF; a comment\nroot = true\n[a.c]\nk = v\n";
  GTEXT_INI_Document * doc = ok(text, ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a.c", "k"), "v");
  EXPECT_EQ(written(doc), text);
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, TheWriterRefusesAValueThatWouldNotReadBackAsItself) {
  /*
   * The refusals a differential over parsed documents cannot reach, because every
   * value a parse stored is writable by construction. A mutation removing this
   * check passed the whole git differential untouched, and the same would be true
   * here, so it is a unit test rather than a score.
   *
   * What this dialect makes unwritable and git does not: a value whose **trailing**
   * byte is whitespace. git tracks the last content byte and an escape counts as
   * content, so `a\t` round-trips there; EditorConfig trims the run, so the value
   * would come back shorter. And the whitespace set is C's, so a vertical tab is
   * caught at both ends where under every other dialect it is an ordinary byte.
   */
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = ec();
  GTEXT_INI_Document * doc = gtext_ini_new(&opts);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * group = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "a.c", &group), GTEXT_INI_OK);

  for (const std::string & bad : {std::string(" lead"), std::string("trail "),
           std::string("\vlead"), std::string("trail\v"),
           std::string("trail\f"), std::string("a\nb")}) {
    ASSERT_EQ(gtext_ini_group_set(group, "k", bad.data(), bad.size()),
        GTEXT_INI_OK) << bad;
    GTEXT_INI_Sink sink;
    ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
    EXPECT_EQ(gtext_ini_write(doc, &sink, nullptr),
        GTEXT_INI_E_UNREPRESENTABLE) << bad;
    gtext_ini_sink_buffer_free(&sink);
  }

  /* Inner whitespace is fine, and so is a `#`: there are no inline comments, so
   * nothing about the value's interior can change how it reads back. */
  const std::string fine = "a value # with a hash";
  ASSERT_EQ(gtext_ini_group_set(group, "k", fine.data(), fine.size()),
      GTEXT_INI_OK);
  std::string out = written(doc);
  EXPECT_EQ(out, "[a.c]\nk=a value # with a hash\n");
  GTEXT_INI_Document * again = ok(out, ec());
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(raw(again, "a.c", "k"), fine);
  gtext_ini_free(again);
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, NoListAndNoLocalePostfix) {
  /*
   * Two Desktop Entry features that are absent, and absent for different reasons.
   * The list has no spelling - a `;` in a value is part of the value - so
   * gtext_ini_value_list() has nothing to split on and says so. The locale postfix
   * is not absent so much as unspellable: `[` and `]` are ordinary bytes in an
   * EditorConfig key, so `k[de]` is a key called `k[de]`.
   */
  GTEXT_INI_Dialect d = ec();
  GTEXT_INI_List * list = nullptr;
  EXPECT_EQ(gtext_ini_value_list(&d, "a;b", 3, nullptr, &list),
      GTEXT_INI_E_INVALID);
  EXPECT_EQ(list, nullptr);

  GTEXT_INI_Document * doc = ok("[a.c]\nk[de]=v\n", ec());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k[de]"}));
  EXPECT_EQ(raw(doc, "a.c", "k[de]"), "v");
  gtext_ini_free(doc);
}

TEST(IniEditorConfig, NoLengthCapIsImposed) {
  /*
   * "Cores must accept keys and values with lengths up to and including 1024 and
   * 4096 characters respectively" is a **floor, not a cap**, which is worth saying
   * because it is the opposite of what a reader expects from a limits clause - and
   * it is why this dialect needed no length field at all.
   *
   * core-c caps at exactly those numbers and silently *drops* anything longer, plus
   * splits a physical line at 5000 bytes; core-py and this impose nothing. So the
   * assertion here is an invariance rather than a status: one byte over the floor
   * behaves like one byte under it.
   */
  for (size_t extra : {size_t{0}, size_t{1}}) {
    std::string key(1024 + extra, 'k');
    std::string value(4096 + extra, 'v');
    std::string text = "[a.c]\n" + key + "=" + value + "\n";
    GTEXT_INI_Document * doc = ok(text, ec());
    ASSERT_NE(doc, nullptr) << extra;
    EXPECT_EQ(raw(doc, "a.c", key.c_str()), value) << extra;
    EXPECT_EQ(written(doc), text) << extra;
    gtext_ini_free(doc);
  }
}

TEST(IniEditorConfig, EveryShapeWritesBackByteForByte) {
  /*
   * The preservation property, over the shapes the differential's generator emits
   * and the conformance suite's own files use. It is asserted per dialect because a
   * property asserted under one says nothing about another that relaxes the rule it
   * depends on - which is exactly how the generic dialect's discarded BOM survived
   * a fuzzer that checked this under the strict dialect only.
   */
  for (const char * text : {
           "[a.c]\nk=v\n",
           "[a.c]\nk=v",
           "\xEF\xBB\xBF[a.c]\nk=v\n",
           "[a.c]\r\nk=v\r\n",
           "; c\n# c\n   ; indented\n[a.c]\nk=v\n",
           "root=true\n\n[a.c]\n\nk = v \n\n",
           "[]\nk=v\n",
           "[a]b]\nk=v\n",
           "[a#b]\nk=v\n",
           "[ spaced ]\nke y = a = b \n",
           "\vk=\vv\v\n",
           "[a.c]\nk=\n[a.c]\nk=2\n",
       }) {
    GTEXT_INI_Document * doc = ok(text, ec());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(written(doc), text) << text;
    /* And a second parse of the output is the same document again. */
    GTEXT_INI_Document * again = ok(written(doc), ec());
    ASSERT_NE(again, nullptr) << text;
    EXPECT_EQ(written(again), text) << text;
    gtext_ini_free(again);
    gtext_ini_free(doc);
  }
}

// ------------------------------------------------------------ the systemd dialect
//
// Every rule here was measured against **systemd 257** in a pinned container, because
// this machine has no systemd at all - no `systemd-analyze`, no `systemctl`, PID 1 is
// `init` - while carrying 165 unit files shipped by other packages. The transcript is
// notes/text/INI-DIALECTS.md §A.17; `make check-ini-systemd-oracle` runs the same
// comparison over 5,000 generated documents and `make conformance-ini-systemd` reads
// every unit file on the machine.
//
// The instrument is worth knowing about before the rules: `systemd-analyze verify`
// **exits 0 on a syntax error**. It warns, skips the line and keeps the file, so its
// exit status is about semantics and only its diagnostics are about the grammar. And
// values come back through `Environment=` alone, which echoes each parsed word after
// unquoting, unescaping and word splitting - there is no verb that prints a setting.
//
// The cases below are the ones a differential cannot reach: a caller building a
// document, the writer's refusals, and the rules whose answer the reference's
// reporting channel destroys - a CR in a value, which its logger rewrites to a
// newline.

namespace {

GTEXT_INI_Dialect sd() { return gtext_ini_dialect_systemd(); }

/** The words of a raw value under the systemd dialect, for readable assertions. */
std::vector<std::string> words(const std::string & raw) {
  GTEXT_INI_Dialect d = sd();
  GTEXT_INI_List * list = nullptr;
  std::vector<std::string> out;
  if (gtext_ini_value_words(&d, raw.data(), raw.size(), nullptr, &list)
      != GTEXT_INI_OK) {
    out.push_back("<error>");
    return out;
  }
  for (size_t i = 0; i < gtext_ini_list_count(list); i++) {
    size_t len = 0;
    const char * item = gtext_ini_list_at(list, i, &len);
    out.push_back(take(item, len));
  }
  gtext_ini_list_free(list);
  return out;
}

/** The decoded form of a raw value under the systemd dialect. */
std::string decoded(const std::string & raw) {
  GTEXT_INI_Dialect d = sd();
  char * out = nullptr;
  size_t len = 0;
  if (gtext_ini_unescape(&d, raw.data(), raw.size(), nullptr, &out, &len)
      != GTEXT_INI_OK) {
    return "<error>";
  }
  std::string result = take(out, len);
  gtext_ini_string_free(nullptr, out);
  return result;
}

} // namespace

TEST(IniSystemd, TheDialectIsItsOwnGrammarAndNotARelaxation) {
  GTEXT_INI_Dialect s = sd();
  EXPECT_EQ(s.id, GTEXT_INI_DIALECT_SYSTEMD);
  EXPECT_EQ(s.continuation, GTEXT_INI_CONTINUATION_JOIN_SPACE);
  EXPECT_EQ(s.name_style, GTEXT_INI_NAMES_SYSTEMD);
  EXPECT_EQ(s.bool_style, GTEXT_INI_BOOLS_SYSTEMD);
  EXPECT_TRUE(s.word_split);
  EXPECT_TRUE(s.numeric_escapes);
  EXPECT_TRUE(s.lone_cr_terminates);
  /* Refuses a preamble, where EditorConfig's specification names one and git accepts
   * one. Measured: "Assignment outside of section. Ignoring." */
  EXPECT_FALSE(s.allow_preamble);
  /* Quoting is not in the grammar, because the specification says it is per-setting -
   * which is this module's layering claim stated by somebody else. */
  EXPECT_FALSE(s.quoted_values);
  /* Nor is the escape set: systemd's config_parse() never looks at a backslash. git
   * is the only dialect here that refuses a bad escape while reading. */
  EXPECT_FALSE(s.escapes_in_grammar);
  EXPECT_TRUE(git().escapes_in_grammar);
  /* And a vertical tab is not whitespace, which was a live guess: systemd is a C
   * program, so `isspace()` was the plausible answer and is wrong three ways. */
  EXPECT_EQ(s.space_set, GTEXT_INI_SPACE_BLANK);
  EXPECT_EQ(ec().space_set, GTEXT_INI_SPACE_CTYPE);
  /* No list separator and no case folding. */
  EXPECT_EQ(s.list_separator, 0);
  EXPECT_FALSE(s.fold_case);
}

TEST(IniSystemd, TheContinuationJoinsWithASpace) {
  /*
   * The rule the whole dialect turns on, and the **minimal pair** that settles it.
   * Every example in `systemd.syntax(7)` indents the continued line, and with the
   * second line indented, joining with a space and joining with nothing give the same
   * answer. Flush left they differ; inside a quoted run the answer is unambiguous.
   *
   * Measured against systemd 257: `Environment="W1\` then `W2"` is the single word
   * `W1 W2`, hex 5731205732.
   */
  GTEXT_INI_Document * doc = ok("[Service]\nA=W1\\\nW2\n", sd());
  ASSERT_NE(doc, nullptr);
  /* The stored value is the document's bytes, continuation included, which is what
   * makes the rewrite byte-identical. */
  EXPECT_EQ(raw(doc, "Service", "A"), "W1\\\nW2");
  EXPECT_EQ(decoded("W1\\\nW2"), "W1 W2");
  EXPECT_EQ(written(doc), "[Service]\nA=W1\\\nW2\n");
  gtext_ini_free(doc);

  /* With a space before the backslash there are **two** spaces in the joined value -
   * the document's own and the one the backslash became. Unobservable through
   * `Environment=`, because word splitting eats a run either way, so it is asserted
   * here and by the round trip rather than by the differential. */
  EXPECT_EQ(decoded("W1 \\\nW2"), "W1  W2");
  EXPECT_EQ(words("W1 \\\nW2"), (std::vector<std::string>{"W1", "W2"}));
}

TEST(IniSystemd, TheContinuationSkipsACommentBlockAndStopsAtABlankLine) {
  /*
   * The manual's nastiest-looking rule, and it is true: a continuation jumps over a
   * `#` or `;` block and joins with whatever follows it. What the manual does **not**
   * say is the other half - a *blank* line ends the continuation instead of being
   * skipped, and the trailing backslash then simply disappears.
   */
  GTEXT_INI_Document * doc =
      ok("[Service]\nA=W1\\\n# a comment\n; another\nW2\n", sd());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(decoded(raw(doc, "Service", "A")), "W1 W2");
  EXPECT_EQ(written(doc), "[Service]\nA=W1\\\n# a comment\n; another\nW2\n");
  gtext_ini_free(doc);

  /* A blank line ends it, and `B` is its own entry. */
  doc = ok("[Service]\nA=W1\\\n\nB=W2\n", sd());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "Service", "A"), "W1");
  EXPECT_EQ(raw(doc, "Service", "B"), "W2");
  EXPECT_EQ(written(doc), "[Service]\nA=W1\\\n\nB=W2\n");
  gtext_ini_free(doc);

  /* And a comment's **own** trailing backslash does not continue it - measured, the
   * entry after it is read normally. So the comment test runs before the assembly. */
  doc = ok("[Service]\n# a comment \\\nA=W1\n", sd());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "Service", "A"), "W1");
  gtext_ini_free(doc);
}

TEST(IniSystemd, AContinuationMayAppearInAHeaderOrAKey) {
  /*
   * **The structural finding, and it is not in the manual.** systemd assembles the
   * logical line before classifying it, so a continuation works on a group header and
   * on a key - measured, `[Serv\` then `ice]` is the section `Serv ice`, which systemd
   * reports as an unknown section rather than as a syntax error.
   *
   * That is why a name here is not a span of the document: the tree keeps the bytes as
   * written, so the rewrite is exact, and the **joined form is the canonical form**,
   * which is the same two-form storage case folding already needed.
   */
  GTEXT_INI_Document * doc = ok("[Serv\\\nice]\nEnv\\\niron=1\n", sd());
  ASSERT_NE(doc, nullptr);
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  EXPECT_EQ(got(gtext_ini_group_name, group), "Serv\\\nice");
  EXPECT_EQ(got(gtext_ini_group_canonical_name, group), "Serv ice");
  EXPECT_EQ(got(gtext_ini_group_key_at, group, (size_t) 0), "Env\\\niron");
  EXPECT_EQ(got(gtext_ini_group_canonical_key_at, group, (size_t) 0), "Env iron");
  /* Reachable by the joined name, which is what a caller has. */
  EXPECT_EQ(raw(doc, "Serv ice", "Env iron"), "1");
  EXPECT_EQ(written(doc), "[Serv\\\nice]\nEnv\\\niron=1\n");
  gtext_ini_free(doc);

  /* A continuation at the very end of a key joins to a trailing space, which systemd
   * strips before the `=`. So the canonical key has no trailing space. */
  doc = ok("[Service]\nEnv\\\n=1\n", sd());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(got(gtext_ini_group_canonical_key_at,
                gtext_ini_document_group_at(doc, 0), (size_t) 0), "Env");
  EXPECT_EQ(written(doc), "[Service]\nEnv\\\n=1\n");
  gtext_ini_free(doc);
}

TEST(IniSystemd, ALoneCarriageReturnEndsALine) {
  /*
   * Measured and **not in the manual**: `A=1<CR>B=2` is two settings, and the line
   * counter advances across the CR. No other dialect here does this - to Desktop Entry
   * a lone CR is part of the value, to git and EditorConfig it is whitespace.
   */
  GTEXT_INI_Document * doc = ok("[Service]\nA=1\rB=2\n", sd());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "Service", "A"), "1");
  EXPECT_EQ(raw(doc, "Service", "B"), "2");
  EXPECT_EQ(written(doc), "[Service]\nA=1\rB=2\n");
  gtext_ini_free(doc);

  /* The same bytes under git are one entry whose value holds the CR, because git's
   * whitespace includes it and its line ends only at an LF. */
  GTEXT_INI_Document * g = ok("[a]\nA=1\rB=2\n", git());
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(gtext_ini_group_entry_count(gtext_ini_document_group_at(g, 0)), 1u);
  gtext_ini_free(g);
}

TEST(IniSystemd, WordsAreQuotedByEitherCharacterAndQuotingIsAToggle) {
  /*
   * systemd's spelling of a list, and three of its rules are not in the manual. The
   * manual says an opening quote may appear only at the start or after unquoted
   * whitespace; systemd does not enforce that.
   */
  EXPECT_EQ(words("one two"), (std::vector<std::string>{"one", "two"}));
  EXPECT_EQ(words("a \"b c\" d"), (std::vector<std::string>{"a", "b c", "d"}));
  EXPECT_EQ(words("a 'b c' d"), (std::vector<std::string>{"a", "b c", "d"}));
  /* A toggle, not a wrapper. */
  EXPECT_EQ(words("x\"y z\""), (std::vector<std::string>{"xy z"}));
  EXPECT_EQ(words("\"a\"b"), (std::vector<std::string>{"ab"}));
  EXPECT_EQ(words("a\"b c\""), (std::vector<std::string>{"ab c"}));
  /* Each quote character is literal inside the other. */
  EXPECT_EQ(words("\"a'b\""), (std::vector<std::string>{"a'b"}));
  EXPECT_EQ(words("'a\"b'"), (std::vector<std::string>{"a\"b"}));
  /* An empty quoted run is an empty **word**, not nothing. */
  EXPECT_EQ(words("\"\" x"), (std::vector<std::string>{"", "x"}));
  /* An unclosed quote is an error, which systemd reports as "Invalid syntax" while
   * discarding the setting. */
  EXPECT_EQ(words("\"a b"), (std::vector<std::string>{"<error>"}));
  /* Escapes are decoded inside single quotes too, unlike a shell. */
  EXPECT_EQ(words("'a\\tb'"), (std::vector<std::string>{"a\tb"}));
  EXPECT_EQ(words("\"a\\tb\""), (std::vector<std::string>{"a\tb"}));
  /* A trailing lone backslash is dropped - measured, `A\` is the single word `A`. */
  EXPECT_EQ(words("A\\"), (std::vector<std::string>{"A"}));
  /* A literal vertical tab does **not** split a word, because it is not whitespace. */
  EXPECT_EQ(words("a\vb"), (std::vector<std::string>{"a\vb"}));
}

TEST(IniSystemd, TheEscapeSetIsTheFullCOneWithFourNumericForms) {
  /* The letters, each checked as a byte rather than by eye: four of them are control
   * characters that a diagnostic renders as nothing, which is how the first reading of
   * this table came out wrong. */
  EXPECT_EQ(decoded("A\\aB"), std::string("A\aB"));
  EXPECT_EQ(decoded("A\\bB"), std::string("A\bB"));
  EXPECT_EQ(decoded("A\\fB"), std::string("A\fB"));
  EXPECT_EQ(decoded("A\\vB"), std::string("A\vB"));
  EXPECT_EQ(decoded("A\\nB"), std::string("A\nB"));
  EXPECT_EQ(decoded("A\\rB"), std::string("A\rB"));
  EXPECT_EQ(decoded("A\\tB"), std::string("A\tB"));
  EXPECT_EQ(decoded("A\\sB"), std::string("A B"));
  EXPECT_EQ(decoded("A\\\\B"), std::string("A\\B"));
  EXPECT_EQ(decoded("A\\\"B"), std::string("A\"B"));
  EXPECT_EQ(decoded("A\\'B"), std::string("A'B"));
  /* The four numeric forms: two hex digits, exactly three octal, four and eight hex. */
  EXPECT_EQ(decoded("A\\x41B"), "AAB");
  EXPECT_EQ(decoded("A\\101B"), "AAB");
  EXPECT_EQ(decoded("A\\u00e9B"), "A\xC3\xA9" "B");
  EXPECT_EQ(decoded("A\\U0001F600B"), "A\xF0\x9F\x98\x80" "B");
  /* A lone surrogate is **encoded**, not refused: `\ud800` is ED A0 80, which is what
   * encoding by codepoint without a surrogate check produces and what systemd
   * produces. */
  EXPECT_EQ(decoded("A\\ud800B"), "A\xED\xA0\x80" "B");
}

TEST(IniSystemd, AMalformedNumericEscapeIsAnErrorAndNotData) {
  /*
   * **The rule this module got wrong first**, and the way it got it wrong is the
   * lesson. The first probe read each diagnostic's reported value by splitting on
   * `ignoring: ` - a substring that appears in **both** "Invalid environment
   * assignment, ignoring: " (a word that parsed) and "Invalid syntax, ignoring: " (the
   * setting refused outright). A refusal echoing the raw value therefore read exactly
   * like a word that had kept its backslash, and the rule came out backwards as "a
   * malformed numeric escape is data".
   *
   * Keeping the message *type* separated them: every one of these makes systemd
   * discard the setting, exactly as an unknown letter does.
   */
  for (const char * bad : {"A\\10B", "A\\400B", "A\\u41B", "A\\U00110000B",
           "A\\xZZB", "A\\x00B", "A\\000B", "A\\u0000B", "A\\qB"}) {
    EXPECT_EQ(decoded(bad), "<error>") << bad;
    EXPECT_EQ(words(bad), (std::vector<std::string>{"<error>"})) << bad;
  }
  /* And `\x4B` is not a short escape at all - `\x` takes up to two hex digits, so this
   * is the letter K. */
  EXPECT_EQ(decoded("A\\x4B"), "AK");
}

TEST(IniSystemd, ABadEscapeParsesAndFailsAtTheAccessor) {
  /*
   * The layering, and systemd's own code is what settles it: `config_parse()` hands the
   * raw value to the setting's parser and never looks at a backslash. So a unit
   * carrying `ExecStart=/bin/foo \q` **parses**, and the complaint arrives only where a
   * caller asked a question that depends on it.
   *
   * git is the opposite and is the only dialect here that is: it unescapes while it
   * reads, so the same document is a syntax error there. Measured on both sides.
   */
  GTEXT_INI_Document * doc = ok("[Service]\nExecStart=/bin/foo \\q\n", sd());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "Service", "ExecStart"), "/bin/foo \\q");
  EXPECT_EQ(decoded("/bin/foo \\q"), "<error>");
  EXPECT_EQ(written(doc), "[Service]\nExecStart=/bin/foo \\q\n");
  gtext_ini_free(doc);

  refused("[a]\nk = a\\qb\n", GTEXT_INI_E_BAD_ESCAPE, git());
}

TEST(IniSystemd, ARefusedLineIsRefusedWhereSystemdWouldSkipIt) {
  /*
   * **The one deliberate departure**, and it is worth stating as a test rather than
   * only in prose: systemd warns about a malformed line and *skips* it, keeping the
   * rest of the file. This refuses the document.
   *
   * The reason is the caller: a library whose user cannot see a warning must not
   * silently drop a setting, because the failure then surfaces as behaviour rather
   * than as an error. The differential compares the *presence* of a grammar fault,
   * which is the part both agree on.
   */
  refused("[Service]\nbare\n", GTEXT_INI_E_BAD_LINE, sd());
  refused("[Service]\n=one\n", GTEXT_INI_E_BAD_KEY, sd());
  refused("[Service\nA=1\n", GTEXT_INI_E_BAD_GROUP, sd());
  refused("[Service] junk\nA=1\n", GTEXT_INI_E_BAD_LINE, sd());
  refused("A=1\n[Service]\nB=2\n", GTEXT_INI_E_NO_GROUP, sd());
}

TEST(IniSystemd, AKeyAndASectionMayHoldASpace) {
  /* Measured: both are reported as *unknown* rather than as syntax errors, so the
   * grammar accepts them. A key is everything before the first `=`, trimmed. */
  GTEXT_INI_Document * doc =
      ok("[Serv ice]\nEnviron ment=1\nA-b=2\nC=x=y\n", sd());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "Serv ice", "Environ ment"), "1");
  EXPECT_EQ(raw(doc, "Serv ice", "A-b"), "2");
  EXPECT_EQ(raw(doc, "Serv ice", "C"), "x=y");
  gtext_ini_free(doc);
}

TEST(IniSystemd, BooleansTakeTheWiderSet) {
  /*
   * The only accessor in the value layer that took no dialect, which was the anomaly
   * rather than the design - gtext_ini_unescape(), gtext_ini_escape() and
   * gtext_ini_value_list() all take one, because what a value *means* is the dialect's
   * business. The parameter arrived with this dialect.
   */
  GTEXT_INI_Dialect s = sd();
  GTEXT_INI_Dialect de = gtext_ini_dialect_desktop_entry();
  bool out = false;
  for (const char * yes : {"1", "yes", "true", "on"}) {
    EXPECT_EQ(gtext_ini_value_bool(&s, yes, std::strlen(yes), &out),
        GTEXT_INI_OK) << yes;
    EXPECT_TRUE(out) << yes;
  }
  for (const char * no : {"0", "no", "false", "off"}) {
    EXPECT_EQ(gtext_ini_value_bool(&s, no, std::strlen(no), &out),
        GTEXT_INI_OK) << no;
    EXPECT_FALSE(out) << no;
  }
  /* Case-sensitive, like every other systemd comparison. */
  EXPECT_EQ(gtext_ini_value_bool(&s, "YES", 3, &out), GTEXT_INI_E_TYPE);
  /* And Desktop Entry still admits exactly two words, which §4 requires. */
  EXPECT_EQ(gtext_ini_value_bool(&de, "yes", 3, &out), GTEXT_INI_E_TYPE);
  EXPECT_EQ(gtext_ini_value_bool(&de, "true", 4, &out), GTEXT_INI_OK);
}

TEST(IniSystemd, TheWriterRefusesAValueThatWouldNotReadBackAsItself) {
  /*
   * The refusals no differential over parsed documents can reach, because every value a
   * parse stored is writable by construction. What this dialect makes unwritable and
   * the others do not: a value containing a **lone CR**, which would end the line.
   */
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = sd();
  GTEXT_INI_Document * doc = gtext_ini_new(&opts);
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * group = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "Service", &group), GTEXT_INI_OK);

  for (const std::string & bad : {std::string(" lead"), std::string("trail "),
           std::string("a\nb"), std::string("a\rb"), std::string("a\r\nb")}) {
    ASSERT_EQ(gtext_ini_group_set(group, "A", bad.data(), bad.size()),
        GTEXT_INI_OK) << bad;
    GTEXT_INI_Sink sink;
    ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
    EXPECT_EQ(gtext_ini_write(doc, &sink, nullptr),
        GTEXT_INI_E_UNREPRESENTABLE) << bad;
    gtext_ini_sink_buffer_free(&sink);
  }

  /* A vertical tab is fine here and not under EditorConfig, because it is not
   * whitespace to systemd - the two dialects hold opposite rules about the same byte. */
  const std::string fine = "a\vb";
  ASSERT_EQ(gtext_ini_group_set(group, "A", fine.data(), fine.size()),
      GTEXT_INI_OK);
  std::string out = written(doc);
  EXPECT_EQ(out, "[Service]\nA=a\vb\n");
  GTEXT_INI_Document * again = ok(out, sd());
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(raw(again, "Service", "A"), fine);
  gtext_ini_free(again);
  gtext_ini_free(doc);
}

TEST(IniSystemd, EveryShapeWritesBackByteForByte) {
  /*
   * The preservation property, over the shapes the differential generates and the ones
   * its reference cannot report on. `A=a\rb` is here because systemd's diagnostic
   * channel **rewrites a CR to an LF** - measured by minimal pair, since all four
   * spellings of a carriage return arrive as 0x0a while an 0x0e escape arrives as 0x0e -
   * so the differential excludes it and this is the only place it is checked.
   */
  for (const char * text : {
           "[Service]\nA=1\n",
           "[Service]\nA=1",
           "\xEF\xBB\xBF[Service]\nA=1\n",
           "[Service]\r\nA=1\r\n",
           "[Service]\nA=1\rB=2\n",
           "# c\n; c\n   # c\n[Service]\nA=1\n",
           "[Service]\nA=W1\\\nW2\n",
           "[Service]\nA=W1 \\\n# skipped\n; skipped\nW2\n",
           "[Service]\nA=W1\\\n\nB=2\n",
           "[Service]\nA=W1\\\n",
           "[Serv\\\nice]\nEnv\\\niron=1\n",
           "[Service]\nA=a\\rb\n",
           "[Service]\nA=\"q\" 'r' \\ts\n",
           "[Service]\nA=\n[Service]\nA=2\n",
           "[Service]\n\vA=\va\v\n",
       }) {
    GTEXT_INI_Document * doc = ok(text, sd());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(written(doc), text) << text;
    GTEXT_INI_Document * again = ok(written(doc), sd());
    ASSERT_NE(again, nullptr) << text;
    EXPECT_EQ(written(again), text) << text;
    gtext_ini_free(again);
    gtext_ini_free(doc);
  }
}

// ------------------------------------------------------ the configparser dialect
//
// **This dialect has no specification**, so every assertion below is a
// measurement against CPython 3.13.5 and none of them is a citation. The probe
// transcript is notes/text/INI-DIALECTS.md §19; `make check-ini-configparser-oracle`
// differs against a pinned interpreter over generated documents, and there is no
// conformance target because there is nothing to conform to.
//
// The cases here are the ones a differential cannot reach or would not explain:
// the fields that grew for this dialect, the two channel-dependent answers, the
// writer's representability rule for a multi-line value, and the two divergences
// that are deliberate.

namespace {

GTEXT_INI_Dialect cp() { return gtext_ini_dialect_configparser(); }

/** An empty document under @p dialect, for the builder-side assertions. */
GTEXT_INI_Document * empty(GTEXT_INI_Dialect dialect) {
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  return gtext_ini_new(&opts);
}

/** The decoded - that is, indent-joined - form of a raw value. */
std::string joined(const std::string & raw_value) {
  GTEXT_INI_Dialect d = cp();
  char * out = nullptr;
  size_t len = 0;
  if (gtext_ini_unescape(&d, raw_value.data(), raw_value.size(), nullptr, &out,
          &len) != GTEXT_INI_OK) {
    return "<error>";
  }
  std::string result = take(out, len);
  gtext_ini_string_free(nullptr, out);
  return result;
}

/** Parse under configparser and return the first entry's joined value. */
std::string first_joined(const std::string & text) {
  GTEXT_INI_Document * doc = ok(text, cp());
  if (!doc) return "<refused>";
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  size_t len = 0;
  const char * value = gtext_ini_group_value_at(group, 0, &len);
  std::string out = joined(std::string(value ? value : "", len));
  gtext_ini_free(doc);
  return out;
}

} // namespace

TEST(IniConfigParser, TheDialectIsMeasuredAndNotCited) {
  GTEXT_INI_Dialect c = cp();
  EXPECT_EQ(c.id, GTEXT_INI_DIALECT_CONFIGPARSER);
  EXPECT_EQ(c.name_style, GTEXT_INI_NAMES_CONFIGPARSER);
  EXPECT_EQ(c.continuation, GTEXT_INI_CONTINUATION_INDENT);
  EXPECT_EQ(c.bool_style, GTEXT_INI_BOOLS_CONFIGPARSER);
  EXPECT_EQ(c.header_remainder, GTEXT_INI_HEADER_REMAINDER_IGNORE);
  EXPECT_EQ(c.space_set, GTEXT_INI_SPACE_PYTHON);
  EXPECT_EQ(std::string(c.separators), "=:");
  /* Keys fold, sections do not. The split EditorConfig needs too. */
  EXPECT_TRUE(c.fold_case);
  EXPECT_FALSE(c.fold_group_case);
  /* A duplicate of either kind is a refusal, which is the *default*
   * configuration - `strict=False` is what merges them, and the axis table this
   * work began from had recorded the lax answer as the dialect's. */
  EXPECT_EQ(c.dupkey, GTEXT_INI_DUPKEY_ERROR);
  EXPECT_FALSE(c.allow_duplicate_groups);
  EXPECT_FALSE(c.allow_preamble);
  EXPECT_FALSE(c.valueless_keys);
  EXPECT_FALSE(c.inline_comments);
  EXPECT_FALSE(c.quoted_values);
  EXPECT_FALSE(c.skip_bom);
  EXPECT_FALSE(c.allow_empty_group_name);
  EXPECT_EQ(c.escapes, nullptr);
  EXPECT_FALSE(c.numeric_escapes);
  EXPECT_EQ(c.list_separator, 0);
  EXPECT_FALSE(c.word_split);
  /* No dialect scans this one's values: the extent of an indent-continued value
   * is not in the value's own bytes. */
  EXPECT_TRUE(c.trim_trailing_space);
}

TEST(IniConfigParser, EitherSeparatorEndsTheKeyAndTheFirstOneWins) {
  /*
   * `(?P<option>.*?)\s*(?P<vi>=|:)` - a lazy match, so the *first* delimiter on
   * the line ends the key whichever of the two it is. Measured all four ways, and
   * the pair that matters is the last two: a reader that looked for `=` first
   * would read `k:b=c` as the key `k:b`.
   */
  struct Case { const char * text; const char * key; const char * value; };
  for (const Case & c : {
           Case{"[a]\nk=v\n", "k", "v"},
           Case{"[a]\nk:v\n", "k", "v"},
           Case{"[a]\nk=b:c\n", "k", "b:c"},
           Case{"[a]\nk:b=c\n", "k", "b=c"},
           Case{"[a]\nk = = v\n", "k", "= v"},
           Case{"[a]\nk:=v\n", "k", "=v"},
       }) {
    GTEXT_INI_Document * doc = ok(c.text, cp());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{c.key})) << c.text;
    EXPECT_EQ(raw(doc, "a", c.key), c.value) << c.text;
    gtext_ini_free(doc);
  }
  /*
   * And a `:` is therefore outside the key charset, where it is inside every
   * other dialect's - asked through the builder, which is the public spelling of
   * the same predicate.
   */
  GTEXT_INI_Document * mine = empty(cp());
  ASSERT_NE(mine, nullptr);
  GTEXT_INI_Group * g = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(mine, "a", &g), GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_group_set(g, "k:v", "1", 1), GTEXT_INI_E_BAD_KEY);
  EXPECT_EQ(gtext_ini_group_set(g, "k v", "1", 1), GTEXT_INI_OK);
  gtext_ini_free(mine);
  GTEXT_INI_Document * theirs = empty(ec());
  ASSERT_NE(theirs, nullptr);
  GTEXT_INI_Group * g2 = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(theirs, "a", &g2), GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_group_set(g2, "k:v", "1", 1), GTEXT_INI_OK);
  gtext_ini_free(theirs);
}

TEST(IniConfigParser, AKeyIsAnythingBeforeTheSeparatorAndFoldsToLowerCase) {
  GTEXT_INI_Document * doc = ok("[a]\nke y = v\nK1 = w\nk[x] = y\nk#z = 1\n", cp());
  ASSERT_NE(doc, nullptr);
  /* A space, a bracket and a comment introducer are all ordinary key bytes: the
   * key is defined by where it ends, not by a charset. */
  EXPECT_EQ(canonical_keys(doc),
      (std::vector<std::string>{"ke y", "k1", "k[x]", "k#z"}));
  gtext_ini_free(doc);
  /* An empty key is refused, and so is one that is only whitespace. */
  refused("[a]\n= v\n", GTEXT_INI_E_BAD_KEY, cp());
  refused("[a]\n  = v\n", GTEXT_INI_E_BAD_KEY, cp());
  /* A line with no separator at all is not a valueless key here. */
  refused("[a]\nk\n", GTEXT_INI_E_BAD_LINE, cp());
}

TEST(IniConfigParser, ADuplicateKeyIsComparedOnTheFoldedName) {
  /*
   * The one case that needed a **code change rather than a field**: the parser's
   * duplicate check compared raw bytes, which is right for Desktop Entry - the
   * only other dialect refusing a duplicate, and one that does not fold - and
   * wrong for a dialect that does both. `k1` then `K1` is a
   * `DuplicateOptionError`, so the fold happens before the check.
   */
  refused("[a]\nk1 = 1\nK1 = 2\n", GTEXT_INI_E_DUPKEY, cp());
  refused("[a]\nk = 1\nk = 2\n", GTEXT_INI_E_DUPKEY, cp());
  /* Across sections it is not a duplicate, and the sections themselves are
   * case-sensitive - so this document has two groups and no duplicate anything. */
  GTEXT_INI_Document * doc = ok("[A]\nk = 1\n[a]\nK = 2\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_names(doc), (std::vector<std::string>{"A", "a"}));
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k", "k"}));
  gtext_ini_free(doc);
  /* A duplicate section is refused under the default configuration. */
  refused("[a]\nk = 1\n[a]\nj = 2\n", GTEXT_INI_E_DUPGROUP, cp());
}

TEST(IniConfigParser, TheHeaderClosesAtTheLastBracketAndTheRestIsDiscarded) {
  /*
   * `\[(?P<header>.+)\]` applied with `re.match`: greedy, so the last `]` closes
   * it, and not anchored at the end, so anything after that is never looked at.
   * Three dialects give three answers to `[a]junk` - Desktop Entry refuses it,
   * git reads an entry from it, this keeps the section - which is why the axis is
   * an enum.
   */
  struct Case { const char * text; const char * name; };
  for (const Case & c : {
           Case{"[a]b]\nk=v\n", "a]b"},
           Case{"[a]junk\nk=v\n", "a"},
           Case{"[a] k = v\nj=w\n", "a"},
           Case{"[a]=v\nk=w\n", "a"},
           Case{"[ b ]\nk=v\n", " b "},
           Case{"[a=b]\nk=v\n", "a=b"},
           Case{"[a#b]\nk=v\n", "a#b"},
           Case{"[a[b]\nk=v\n", "a[b"},
           Case{"[a] ]\nk=v\n", "a] "},
       }) {
    GTEXT_INI_Document * doc = ok(c.text, cp());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(group_names(doc), (std::vector<std::string>{c.name})) << c.text;
    /* The discarded bytes are still in the document, so a rewrite reproduces
     * them: they have no meaning, which is not the same as being absent. */
    EXPECT_EQ(written(doc), c.text) << c.text;
    gtext_ini_free(doc);
  }
  /* `[]` is refused, which is the opposite of EditorConfig's answer to the same
   * three bytes - and the only axis on which the two name grammars differ. */
  refused("[]\nk=v\n", GTEXT_INI_E_BAD_GROUP, cp());
  GTEXT_INI_Document * doc = ok("[]\nk=v\n", ec());
  ASSERT_NE(doc, nullptr);
  gtext_ini_free(doc);
}

TEST(IniConfigParser, AnIndentedLineContinuesTheValueAndJoinsWithANewline) {
  struct Case { const char * text; const char * value; };
  for (const Case & c : {
           Case{"[a]\nk=1\n  2\n", "1\n2"},
           Case{"[a]\nk=1\n\t2\n", "1\n2"},
           Case{"[a]\nk=1\n 2\n", "1\n2"},
           Case{"[a]\nk=1\n  2\n  3\n", "1\n2\n3"},
           /* The indent is compared against the entry's own line, and the depth
            * of a continuation relative to earlier ones does not matter. */
           Case{"[a]\nk=1\n    2\n  3\n", "1\n2\n3"},
           /* A comment line contributes nothing and does not end it; a blank
            * line contributes an empty line and does not end it either. */
           Case{"[a]\nk=1\n#c\n  2\n", "1\n2"},
           Case{"[a]\nk=1\n  2\n#c\n  3\n", "1\n2\n3"},
           Case{"[a]\nk=1\n\n  2\n", "1\n\n2"},
           Case{"[a]\nk=1\n\t\n  2\n", "1\n\n2"},
           /* Each contributing line is stripped on both sides, the first too. */
           Case{"[a]\nk=1  \n  2  \n", "1\n2"},
           /* An empty first line still holds the newline the join inserts. */
           Case{"[a]\nk=\n  2\n", "\n2"},
           Case{"[a]\nk=\n\n  2\n", "\n\n2"},
           /* An indented line that looks like an entry, or like a header, is
            * neither: it is text. */
           Case{"[a]\nk=1\n  j=2\n", "1\nj=2"},
           Case{"[a]\nk=1\n  [b]\n", "1\n[b]"},
           /* At end of input with no terminator. */
           Case{"[a]\nk=1\n  2", "1\n2"},
       }) {
    EXPECT_EQ(first_joined(c.text), c.value) << c.text;
    GTEXT_INI_Document * doc = ok(c.text, cp());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(written(doc), c.text) << c.text;
    gtext_ini_free(doc);
  }
}

TEST(IniConfigParser, TheIndentComparisonIsStrictlyGreaterThanTheEntrysOwnLine) {
  /*
   * The rule that cannot be inferred from a single document's result, and the one
   * that had to come from reading `_read_inner()`: the comparison is against the
   * indent of the line that **started the entry**, strictly. So an entry indented
   * two spaces is not continued by a line indented two spaces - that line is read
   * as an entry of its own, has no separator, and the document is refused.
   */
  refused("[a]\n  k=1\n  2\n", GTEXT_INI_E_BAD_LINE, cp());
  refused("[a]\n    k=1\n  2\n", GTEXT_INI_E_BAD_LINE, cp());
  /* One space deeper is enough. */
  GTEXT_INI_Document * doc = ok("[a]\n  k=1\n   2\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(first_joined("[a]\n  k=1\n   2\n"), "1\n2");
  gtext_ini_free(doc);
  /* Two entries at the same indent are two entries. */
  GTEXT_INI_Document * two = ok("[a]\n  k=1\n  j=2\n", cp());
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(canonical_keys(two), (std::vector<std::string>{"k", "j"}));
  gtext_ini_free(two);
}

TEST(IniConfigParser, AGroupHeaderEndsAContinuationAndIsNeverContinued) {
  /*
   * The reference clears its current key when it reads a header, with the comment
   * "so sections can't start with a continuation line". So an indented line after
   * a header has nothing to join to and is an entry - or, with no separator, a
   * refusal. An *indented* header while a value is open never gets that far: it
   * was already absorbed into the value, which the case above asserts.
   */
  refused("[a]\nk=1\n[b]\n  2\n", GTEXT_INI_E_BAD_LINE, cp());
  /* An indented header at the start of a document is a header: the indent only
   * means anything while an entry is open. */
  GTEXT_INI_Document * doc = ok("  [a]\n  k=v\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_names(doc), (std::vector<std::string>{"a"}));
  EXPECT_EQ(raw(doc, "a", "k"), "v");
  gtext_ini_free(doc);
}

TEST(IniConfigParser, AKeysSeparatorIsSoughtOnItsOwnLineOnly) {
  /*
   * The bug this test exists for: the value's extent grows past the first
   * physical line, and the search for the separator was bounded by that extent.
   * So `k` followed by an indented `j=2` read as one entry named `k\n  j` - a key
   * containing a newline - where the reference reports the bare `k` as a
   * `ParsingError`. The separator is matched against the entry's own line and
   * every continuation line is appended without being looked at.
   *
   * systemd is the opposite case and shares the code: there a *key* may be
   * continued, so the whole logical line is in scope.
   */
  refused("[a]\nk\n  j=2\n", GTEXT_INI_E_BAD_LINE, cp());
  GTEXT_INI_Document * doc = ok("[Service]\nEnviron\\\nment=v\n", sd());
  ASSERT_NE(doc, nullptr);
  /* `Environ ment` and not `Environment`: systemd's join inserts a space, which
   * is the whole point of that dialect's continuation being JOIN_SPACE. The key
   * is a nonsense setting name and parses anyway - what matters here is that the
   * separator was found on the *second* physical line. */
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"Environ ment"}));
  gtext_ini_free(doc);
}

TEST(IniConfigParser, TheValueEndsAtItsLastContributingLine) {
  /*
   * The reference joins the pieces and then strips the result, so trailing blank
   * and comment lines are not in the value. They therefore belong to the
   * document's comment stream rather than to the entry, and a scan that ran to
   * the first non-continuation line would have swallowed them - a rewrite would
   * still have been byte-identical, which is why this needed a *value*
   * comparison to find rather than a round-trip one.
   */
  EXPECT_EQ(first_joined("[a]\nk=1\n  2\n\n\n[b]\nj=3\n"), "1\n2");
  EXPECT_EQ(first_joined("[a]\nk=1\n  2\n#c\n[b]\nj=3\n"), "1\n2");
  /* The blank lines are outside the entry, so the second group's leading comment
   * holds them and the rewrite still tiles the input exactly. */
  GTEXT_INI_Document * doc = ok("[a]\nk=1\n  2\n\n\n[b]\nj=3\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a", "k"), "1\n  2");
  EXPECT_EQ(written(doc), "[a]\nk=1\n  2\n\n\n[b]\nj=3\n");
  gtext_ini_free(doc);
}

TEST(IniConfigParser, PythonsWhitespaceIncludesFourBytesCsDoesNot) {
  /*
   * `\x1c` through `\x1f`, the ASCII separator controls. Python's `\s` and
   * `str.strip()` include them and `isspace()` does not, so this is a third
   * whitespace set rather than a reuse of EditorConfig's - and it is asked in
   * four different positions, all measured.
   */
  /*
   * Asked through the parser rather than through the predicate, because the
   * predicate is internal and because what matters is that all four *positions*
   * read it as whitespace. EditorConfig, whose set is C's, refuses the same
   * documents.
   */
  for (char raw_byte : {'\x1c', '\x1d', '\x1e', '\x1f'}) {
    std::string byte(1, raw_byte);
    std::string doc_text = "[a]\nk" + byte + "=" + byte + "v" + byte + "\n";
    GTEXT_INI_Document * mine = ok(doc_text, cp());
    ASSERT_NE(mine, nullptr) << doc_text;
    EXPECT_EQ(canonical_keys(mine), (std::vector<std::string>{"k"})) << doc_text;
    EXPECT_EQ(raw(mine, "a", "k"), "v") << doc_text;
    gtext_ini_free(mine);
    /* Under EditorConfig the same byte is part of the key and of the value. */
    GTEXT_INI_Document * theirs = ok(doc_text, ec());
    ASSERT_NE(theirs, nullptr) << doc_text;
    EXPECT_EQ(canonical_keys(theirs), (std::vector<std::string>{"k" + byte}))
        << doc_text;
    gtext_ini_free(theirs);
  }
  /* Indentation, the separator run, the trailing trim, and before a comment. */
  EXPECT_EQ(first_joined("[a]\nk=1\n\x1c" "2\n"), "1\n2");
  GTEXT_INI_Document * doc = ok("[a]\nk\x1c=\x1cv\x1c\n\x1c#c\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k"}));
  EXPECT_EQ(raw(doc, "a", "k"), "v");
  gtext_ini_free(doc);
  /* A vertical tab and a form feed are in the set as well, which EditorConfig
   * shares and git does not. */
  EXPECT_EQ(first_joined("[a]\nk=1\n\v2\n"), "1\n2");
  EXPECT_EQ(first_joined("[a]\nk=1\n\f2\n"), "1\n2");
}

TEST(IniConfigParser, ALoneCarriageReturnTerminatesALineBecauseTheFileDoes) {
  /*
   * The one place the reference has two answers, and neither is wrong: Python's
   * universal-newline translation applies to `read(path)` and not to
   * `read_string()`, so a lone CR is a terminator in a file and data in a string.
   * Of the 38 documents probed both ways it is the **only** difference between
   * the two channels. A file is what an INI document is, so this follows the file
   * - and the differential's driver reads a file for the same reason.
   */
  GTEXT_INI_Document * doc = ok("[a]\nk=v\rj=w\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k", "j"}));
  EXPECT_EQ(raw(doc, "a", "k"), "v");
  EXPECT_EQ(written(doc), "[a]\nk=v\rj=w\n");
  gtext_ini_free(doc);
  /* So a CR inside what looks like a value splits the line, and the second half
   * has no separator. */
  refused("[a]\nk=a\rb\n", GTEXT_INI_E_BAD_LINE, cp());
  /* And a CR before an indented line is a terminator, so that line continues. */
  EXPECT_EQ(first_joined("[a]\nk=a\r  b\n"), "a\nb");
  /* CRLF and CR CR LF both behave as the file channel does. */
  GTEXT_INI_Document * crlf = ok("[a]\r\nk=v\r\n", cp());
  ASSERT_NE(crlf, nullptr);
  EXPECT_EQ(raw(crlf, "a", "k"), "v");
  gtext_ini_free(crlf);
}

TEST(IniConfigParser, QuotesBackslashesAndInlineCommentsAreAllLiteral) {
  GTEXT_INI_Document * doc =
      ok("[a]\nk=\"q v\"\nj=a\\nb\nm=x ; c\nn=y # c\np=100%\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a", "k"), "\"q v\"");
  EXPECT_EQ(raw(doc, "a", "j"), "a\\nb");
  EXPECT_EQ(raw(doc, "a", "m"), "x ; c");
  EXPECT_EQ(raw(doc, "a", "n"), "y # c");
  EXPECT_EQ(raw(doc, "a", "p"), "100%");
  /* And the decoded form is the same bytes: a backslash is data, which is the
   * contract gtext_ini_unescape() has for a dialect with no escape set. */
  EXPECT_EQ(joined("a\\nb"), "a\\nb");
  gtext_ini_free(doc);
  /*
   * `100%` is the compatibility trap in this dialect and it is a **layer above
   * the grammar**: the value parses and is stored, and it is `get()` under the
   * default `BasicInterpolation` that raises. Interpolation does not ship - no
   * file on this machine uses `%(name)s` and three use a bare `%` - so the raw
   * value is the answer and @ref format_ini says so with the count.
   */
}

TEST(IniConfigParser, BooleansAreSystemdsWordsFoldedToLowerCase) {
  GTEXT_INI_Dialect c = cp();
  GTEXT_INI_Dialect s = sd();
  bool value = false;
  for (const char * yes : {"1", "yes", "true", "on", "YES", "Yes", "TRUE", "On"}) {
    EXPECT_EQ(gtext_ini_value_bool(&c, yes, strlen(yes), &value), GTEXT_INI_OK)
        << yes;
    EXPECT_TRUE(value) << yes;
  }
  for (const char * no : {"0", "no", "false", "off", "OFF", "False"}) {
    EXPECT_EQ(gtext_ini_value_bool(&c, no, strlen(no), &value), GTEXT_INI_OK) << no;
    EXPECT_FALSE(value) << no;
  }
  for (const char * bad : {"n", "t", "2", "", "true false"}) {
    EXPECT_EQ(gtext_ini_value_bool(&c, bad, strlen(bad), &value),
        GTEXT_INI_E_TYPE) << bad;
  }
  /* The same eight words as systemd's, and systemd folds none of them - which is
   * why the two sets are separate members rather than one flag. */
  EXPECT_EQ(gtext_ini_value_bool(&s, "YES", 3, &value), GTEXT_INI_E_TYPE);
  EXPECT_EQ(gtext_ini_value_bool(&s, "yes", 3, &value), GTEXT_INI_OK);
}

TEST(IniConfigParser, TheWriterIndentsASynthesizedMultiLineValue) {
  /*
   * An LF is how this dialect *spells* a continuation, so the writer cannot refuse
   * one - that would make a document it had just read unwritable, which is the
   * mistake the scanning branch of the same predicate records having made. What it
   * does instead is **supply the indentation**, which is the only way a caller's
   * `"a\nb"` can be written at all: emitted as-is it would read back as an entry
   * `a` followed by a line `b` with no separator.
   *
   * So the property is a round trip through the writer, not a byte comparison
   * against a spelling this test would have to guess.
   */
  struct Case { const char * value; bool writable; const char * why; };
  for (const Case & c : {
           Case{"a\nb", true, "an ordinary two-line value"},
           Case{"a\n\nb", true, "a blank line in the middle is representable"},
           Case{"a\nb\nc", true, "three lines"},
           Case{"\nb", true, "an empty first line"},
           Case{"a", true, "one line"},
           Case{"a\nb\n", false, "a trailing terminator would be stripped"},
           Case{"a\rb", false, "a CR ends a line and the writer emits its own"},
           Case{"a\r\nb", false, "and a CRLF would be replaced by the chosen one"},
           Case{"a\n", false, "the same with nothing after it"},
           Case{"a\n#c", false, "an indented `#` reads back as a comment"},
           Case{"a\n;c", false, "and so does the other introducer"},
           Case{"a\n b", false, "the writer's indent plus its own would be stripped"},
           Case{"a\nb ", false, "a trailing space on the last line is stripped"},
           Case{" a", false, "a leading space is eaten after the separator"},
           Case{"a ", false, "a trailing space is stripped"},
       }) {
    GTEXT_INI_Document * doc = empty(cp());
    ASSERT_NE(doc, nullptr);
    GTEXT_INI_Group * group = nullptr;
    ASSERT_EQ(gtext_ini_document_add_group(doc, "a", &group), GTEXT_INI_OK);
    ASSERT_EQ(gtext_ini_group_set(group, "k", c.value, strlen(c.value)),
        GTEXT_INI_OK) << c.why;
    GTEXT_INI_Sink sink;
    ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
    GTEXT_INI_Status status = gtext_ini_write(doc, &sink, nullptr);
    if (!c.writable) {
      EXPECT_EQ(status, GTEXT_INI_E_UNREPRESENTABLE) << c.why;
    }
    else {
      EXPECT_EQ(status, GTEXT_INI_OK) << c.why;
      if (status == GTEXT_INI_OK) {
        std::string out(gtext_ini_sink_buffer_data(&sink),
            gtext_ini_sink_buffer_size(&sink));
        /* And it reads back as the same value, which is the whole claim. */
        GTEXT_INI_Document * again = ok(out, cp());
        ASSERT_NE(again, nullptr) << out;
        const GTEXT_INI_Group * g2 = gtext_ini_document_group_at(again, 0);
        size_t len = 0;
        const char * stored = gtext_ini_group_value_at(g2, 0, &len);
        EXPECT_EQ(joined(std::string(stored ? stored : "", len)), c.value)
            << c.why << " wrote: " << out;
        gtext_ini_free(again);
      }
    }
    gtext_ini_sink_buffer_free(&sink);
    gtext_ini_free(doc);
  }
}

TEST(IniConfigParser, ASynthesizedValuesLineBreaksMustBeWhatTheWriterEmits) {
  /*
   * Found by re-reading the writer rather than by any gate, and neither the corpus nor
   * the differential could have found it: both score documents this module *parsed*, so
   * both only ever exercise the verbatim path, and this is the synthesized one - a
   * caller building a document through gtext_ini_group_set().
   *
   * The writer emits one terminator of its own choosing per line break, so a terminator
   * the value carries that is not that one is rewritten and the value does not come
   * back. Two failures, and the first is the worse of the two:
   *
   *   - `"a<CR>b"` was emitted verbatim, and the document then **failed to re-parse at
   *     all** - a CR ends a line here, so `b` is an entry with no separator. The
   *     writer had declared the value representable and produced an unreadable file.
   *   - `"a<CR><LF>b"` was emitted with its terminator replaced, so the value came back
   *     `"a<LF>b"`: silently different, which is the failure mode
   *     ::GTEXT_INI_E_UNREPRESENTABLE exists to prevent.
   *
   * This is the third time an accessor reachable only from the builder has needed a
   * unit test rather than a score, and the shape is always the same.
   */
  struct Case { const char * value; size_t len; };
  for (const Case & c : {Case{"a\rb", 3}, Case{"a\r\nb", 4}, Case{"a\rb\nc", 5}}) {
    GTEXT_INI_Document * doc = empty(cp());
    ASSERT_NE(doc, nullptr);
    GTEXT_INI_Group * group = nullptr;
    ASSERT_EQ(gtext_ini_document_add_group(doc, "a", &group), GTEXT_INI_OK);
    ASSERT_EQ(gtext_ini_group_set(group, "k", c.value, c.len), GTEXT_INI_OK);
    GTEXT_INI_Sink sink;
    ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
    EXPECT_EQ(gtext_ini_write(doc, &sink, nullptr), GTEXT_INI_E_UNREPRESENTABLE)
        << c.value;
    gtext_ini_sink_buffer_free(&sink);
    gtext_ini_free(doc);
  }
  /* A value whose break is a bare LF is fine, which is the whole point: the rule is
   * about which terminator, not about whether there is one. */
  GTEXT_INI_Document * doc = empty(cp());
  ASSERT_NE(doc, nullptr);
  GTEXT_INI_Group * group = nullptr;
  ASSERT_EQ(gtext_ini_document_add_group(doc, "a", &group), GTEXT_INI_OK);
  ASSERT_EQ(gtext_ini_group_set(group, "k", "a\nb", 3), GTEXT_INI_OK);
  EXPECT_EQ(written(doc), "[a]\nk=a\n b\n");
  gtext_ini_free(doc);
}

TEST(IniConfigParser, TheJoinStripsItsOwnResultForAValueNoParseProduces) {
  /*
   * The reference joins the pieces and then strips the result, and this asserts that
   * last step - which **no gate reaches**, and a mutation disabling it moved nothing.
   * That is not a hole in the gates: a parse never produces such a value, because the
   * span it stores ends at the last line that contributed text. It is reachable only by
   * handing gtext_ini_unescape() a raw value of one's own, which is a legal call and
   * therefore has to be right.
   *
   * Recorded rather than deleted, because the function's contract is "what
   * `configparser` would return for these bytes", and dropping the step would make that
   * false for an input a caller can construct.
   */
  EXPECT_EQ(joined("1\n  2\n\n\n"), "1\n2");
  EXPECT_EQ(joined("1\n  \n"), "1");
  EXPECT_EQ(joined("\n\n"), "");
  EXPECT_EQ(joined("v   "), "v");
}

TEST(IniConfigParser, AParsedMultiLineValueIsWritableWithItsOwnIndentation) {
  /*
   * The other half of the same predicate, and the reason it takes a flag: a
   * *verbatim* value carries the indentation its document had, so it requires
   * exactly what a synthesized one forbids. A comment line inside the span is
   * likewise fine here and refused there.
   */
  for (const std::string & text : {std::string("[a]\nk=1\n  2\n"),
           std::string("[a]\nk=1\n#c\n  2\n"),
           std::string("[a]\nk=1\n\n  2\n"),
           std::string("[a]\nk=\n  2\n")}) {
    GTEXT_INI_Document * doc = ok(text, cp());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(written(doc), text) << text;
    gtext_ini_free(doc);
  }
}

TEST(IniConfigParser, EveryAcceptedDocumentRewritesByteForByte) {
  for (const std::string & text : {
           std::string("[a]\nk = v\n"),
           std::string("[a]\nk:v\n"),
           std::string("#c\n;d\n\n[a]\nk=1\n  2\n"),
           std::string("[a]\nk=1\n\n  2\n#c\n  3\n"),
           std::string("[a]junk\nk=v\n"),
           std::string("[ b ]\nk=v\n"),
           std::string("  [a]\n  k=v\n"),
           std::string("[a]\nk=\n  2\n"),
           std::string("[a]\r\nk=v\r\n"),
           std::string("[a]\nk=v\rj=w\n"),
           std::string("[a]\nk=1\n  2"),
           std::string("[a]\nk=\"q\" ; c\n"),
           std::string("[a]\nk\x1c=\x1cv\n"),
           std::string("[a]\nk=1\n  2\n\n\n[b]\nj=3\n"),
       }) {
    GTEXT_INI_Document * doc = ok(text, cp());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(written(doc), text) << text;
    GTEXT_INI_Document * again = ok(written(doc), cp());
    ASSERT_NE(again, nullptr) << text;
    EXPECT_EQ(written(again), text) << text;
    gtext_ini_free(again);
    gtext_ini_free(doc);
  }
}

TEST(IniConfigParser, ACarriageReturnTerminatorIsNotLeadingWhitespace) {
    /*
   * Two defects in one shape, both found by the differential's `lone-cr` and `crlf`
   * axes and neither reachable from the 703-file local corpus, which contains one CRLF
   * file and no lone CR at all.
   *
   * A CR is whitespace to every dialect that accepts CRLF, and an LF is whitespace to
   * none - the line ends at an LF before anything trims. So a value whose first line is
   * empty begins at its own terminator, and under CRLF that first byte is a CR:
   *
   *   - the **parser** skipped it as leading whitespace and walked into the
   *     continuation, losing the value's empty first line: `alpha =<CR>  v` gave `v`
   *     where the reference gives `\nv`. The skip had been bounded by the end of the
   *     physical line, which stopped it before this dialect made that the end of the
   *     *logical* line;
   *   - the **writer** then called the same first byte leading whitespace and declared
   *     the value unrepresentable, so a document this module had just parsed could not
   *     be written back.
   */
  struct Case { const char * text; const char * value; };
  for (const Case & c : {
           Case{"[a]\nk =\r  v\n", "\nv"},
           Case{"[a]\nk =   \r  v\n", "\nv"},
           Case{"[a]\r\nk =\r\n  v\r\n", "\nv"},
           Case{"[a]\r\nk = 1\r\n  2\r\n", "1\n2"},
       }) {
    EXPECT_EQ(first_joined(c.text), c.value) << c.text;
    GTEXT_INI_Document * doc = ok(c.text, cp());
    ASSERT_NE(doc, nullptr) << c.text;
    /* And it is writable, which is the second half: the value's first byte is a
     * terminator rather than a space that would be eaten. */
    EXPECT_EQ(written(doc), c.text) << c.text;
    gtext_ini_free(doc);
  }
  /* A leading space still is leading whitespace, so the distinction is the
   * terminator and not the CR. */
  GTEXT_INI_Document * doc = ok("[a]\nk =  v\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a", "k"), "v");
  gtext_ini_free(doc);
}

TEST(IniConfigParser, AValueBeginningWithACommentIntroducerIsNotAComment) {
  /*
   * Found by the **local corpus**, and not by any of the 102 probe documents nor by the
   * generator: a raw value starts in the middle of its line, so a value of `;black`
   * begins with a comment introducer and is not a comment - the reference tests the
   * whole line, `j = ;black`, which does not start with `;`.
   *
   * The indent join classified every line of the raw value including the first, so
   * those values came back **empty** and the documents were unwritable. 331 of the 479
   * real configparser documents on this machine have one: Midnight Commander's skins
   * spell a default colour that way.
   */
  EXPECT_EQ(first_joined("[a]\nj = ;black\n"), ";black");
  EXPECT_EQ(first_joined("[a]\nj = #ff0000\n"), "#ff0000");
  /*
   * And here is the pair that makes the rule exact rather than approximate, measured
   * both ways against the reference: the *value* may begin with a `;` and a
   * *continuation line* may not. `j = ;black` is the value `;black`, and an indented
   * `;white` under it is a **comment** - the comment test runs on the stripped line, so
   * indentation does not protect it - while an indented `white` continues the value.
   * One introducer, two answers, decided by whether the line began with a key.
   */
  EXPECT_EQ(first_joined("[a]\nj = ;black\n  ;white\n"), ";black");
  EXPECT_EQ(first_joined("[a]\nj = ;black\n  white\n"), ";black\nwhite");
  for (const std::string & text : {std::string("[a]\nj = ;black\n"),
           std::string("[a]\nj = #ff0000\n  ;white\n")}) {
    GTEXT_INI_Document * doc = ok(text, cp());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(written(doc), text) << text;
    gtext_ini_free(doc);
  }
}

TEST(IniConfigParser, TwoDivergencesAreDeliberateAndBothAreUnicode) {
  /*
   * Python's `str` is Unicode-aware and this reader is byte-oriented, so two of
   * the reference's rules cannot be followed here and are **stated** rather than
   * approximated. Both are asserted so that a future change to either is a
   * visible decision and not a drift.
   *
   * The differential excludes documents containing a non-ASCII byte from the
   * scores these affect and counts them, which is where the exclusion is
   * enforced; this is where it is explained.
   */
  /* A no-break space indents a line for `configparser` and is data here, so the
   * document that would have been one value there is refused. U+0085 and the
   * Unicode separators are the same case. */
  refused("[a]\nk=1\n\xc2\xa0" "2\n", GTEXT_INI_E_BAD_LINE, cp());
  /* And `optionxform` is `str.lower()`, so a non-ASCII upper-case letter folds
   * there and not here: the canonical key keeps its bytes. */
  GTEXT_INI_Document * doc = ok("[a]\nK\xc3\x89 = v\n", cp());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k\xc3\x89"}));
  gtext_ini_free(doc);
}

namespace {

GTEXT_INI_Dialect w32() { return gtext_ini_dialect_win32(); }

/**
 * The canonical - trimmed and folded - name of every group.
 *
 * A different question from the file's earlier `canonical_names()`, which builds
 * git's `section.key` pairs; this is the section name alone, which is what a
 * Win32 lookup and `GetPrivateProfileSectionNames` are about.
 */
std::vector<std::string> group_canon(const GTEXT_INI_Document * doc) {
  std::vector<std::string> out;
  for (size_t g = 0; g < gtext_ini_document_group_count(doc); g++) {
    const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, g);
    out.push_back(gtext_ini_group_is_preamble(group)
                      ? std::string("<preamble>")
                      : got(gtext_ini_group_canonical_name, group));
  }
  return out;
}

/** The decoded value - quotes stripped - of a raw span, under Win32. */
std::string w32_decoded(const std::string & raw_value) {
  GTEXT_INI_Dialect d = w32();
  char * out = nullptr;
  size_t len = 0;
  if (gtext_ini_unescape(&d, raw_value.data(), raw_value.size(), nullptr, &out,
          &len) != GTEXT_INI_OK) {
    return "<error>";
  }
  std::string result = take(out, len);
  gtext_ini_string_free(nullptr, out);
  return result;
}

/** Parse under Win32 and return the first entry's decoded value. */
std::string w32_first(const std::string & text) {
  GTEXT_INI_Document * doc = ok(text, w32());
  if (!doc) return "<refused>";
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  size_t len = 0;
  const char * value = gtext_ini_group_value_at(group, 0, &len);
  std::string out = w32_decoded(std::string(value ? value : "", len));
  gtext_ini_free(doc);
  return out;
}

} // namespace

TEST(IniWin32, TheDialectIsMeasuredAndHasNoDocumentToCite) {
  /*
   * `GetPrivateProfileString` has two documented rules - quote stripping and case
   * insensitivity - and this dialect has thirty. Every field below came from a
   * probe under wine, and three of them are fields no other dialect needed.
   */
  GTEXT_INI_Dialect w = w32();
  EXPECT_EQ(w.id, GTEXT_INI_DIALECT_WIN32);
  EXPECT_EQ(w.name_style, GTEXT_INI_NAMES_WIN32);
  EXPECT_EQ(w.space_set, GTEXT_INI_SPACE_CTYPE);
  EXPECT_EQ(w.header_remainder, GTEXT_INI_HEADER_REMAINDER_IGNORE);
  EXPECT_EQ(w.dupkey, GTEXT_INI_DUPKEY_FIRST_WINS);
  EXPECT_EQ(std::string(w.separators), "=");
  /* The four axes this dialect introduced. */
  EXPECT_TRUE(w.allow_empty_key);
  EXPECT_TRUE(w.trim_group_name);
  EXPECT_TRUE(w.unclosed_header_is_line);
  EXPECT_TRUE(w.strip_wrapping_quotes);
  /* And the fifth: a lookup does not merge duplicate sections, which every other
   * dialect here does because GKeyFile does. */
  EXPECT_FALSE(w.merge_duplicate_groups);
  /* Both names fold, which only git also does - and git folds for a different
   * reason and with a charset. */
  EXPECT_TRUE(w.fold_case);
  EXPECT_TRUE(w.fold_group_case);
  /* The lone CR is this dialect's alone among the seven. */
  EXPECT_TRUE(w.accept_crlf);
  EXPECT_TRUE(w.lone_cr_terminates);
  EXPECT_TRUE(w.valueless_keys);
  EXPECT_TRUE(w.allow_empty_group_name);
  EXPECT_TRUE(w.allow_duplicate_groups);
  EXPECT_TRUE(w.allow_preamble);
  EXPECT_TRUE(w.trim_trailing_space);
  EXPECT_TRUE(w.skip_bom);
  /*
   * `;` is a comment and `#` is not, which is the dialect's one divergence and is
   * measured rather than chosen: `GetPrivateProfileSection` drops a `;` line,
   * `GetPrivateProfileString` retrieves it, and neither drops a `#` line.
   */
  EXPECT_TRUE(w.comment_semicolon);
  EXPECT_FALSE(w.comment_hash);
  /*
   * The three things that deriving this dialect from the generic one silently
   * inherited and got wrong. Each is asserted because each was a defect.
   */
  EXPECT_EQ(w.escapes, nullptr);
  EXPECT_FALSE(w.locale_postfix);
  EXPECT_FALSE(w.utf8_values);
  /* Not git's quote toggle: `strip_wrapping_quotes` above is the rule. */
  EXPECT_FALSE(w.quoted_values);
  EXPECT_FALSE(w.inline_comments);
  EXPECT_EQ(w.continuation, GTEXT_INI_CONTINUATION_NONE);
  EXPECT_EQ(w.list_separator, 0);
  EXPECT_FALSE(w.word_split);
  EXPECT_FALSE(w.subsection_syntax);
}

TEST(IniWin32, AHeaderClosesAtTheLastBracketAndTheRestIsDiscarded) {
  struct Case { const char * text; const char * name; };
  for (const Case & c : {
           Case{"[a]\nk=v\n", "a"},
           Case{"[a]junk\nk=v\n", "a"},
           Case{"[a]b]\nk=v\n", "a]b"},
           Case{"[a]]junk\nk=v\n", "a]"},
           Case{"[a[b]\nk=v\n", "a[b"},
           Case{"[a][b]\nk=v\n", "a][b"},
           Case{"[a=b]\nk=v\n", "a=b"},
           Case{"[a;b]\nk=v\n", "a;b"},
           Case{"[a#b]\nk=v\n", "a#b"},
           Case{"   [a]\nk=v\n", "a"},
           Case{"[a] ; c\nk=v\n", "a"},
       }) {
    GTEXT_INI_Document * doc = ok(c.text, w32());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(group_canon(doc), (std::vector<std::string>{c.name}))
        << c.text;
    gtext_ini_free(doc);
  }
}

TEST(IniWin32, TheGroupNameIsTrimmedInsideTheBrackets) {
  /*
   * `[ b ]` and `[b]` are one section, which no other dialect here does: the
   * three that allow a space in a name all *keep* it, and configparser's `[ b ]`
   * is a section literally named `" b "`. The trim is the dialect's whitespace
   * set, so a tab and a form feed go too.
   */
  for (const std::string & text : {std::string("[ b ]\nk=v\n"),
           std::string("[\tb\t]\nk=v\n"), std::string("[\fb\f]\nk=v\n"),
           std::string("[b]\nk=v\n")}) {
    GTEXT_INI_Document * doc = ok(text, w32());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"b"})) << text;
    /* The document's own bytes are kept, so the header writes back unchanged. */
    EXPECT_EQ(written(doc), text) << text;
    gtext_ini_free(doc);
  }
  /* A name of only whitespace trims to the empty name, so `[ ]` and `[]` are the
   * same section - and under configparser they are two different ones. */
  GTEXT_INI_Document * doc = ok("[ ]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{""}));
  gtext_ini_free(doc);
}

TEST(IniWin32, AnUnclosedHeaderIsAnOrdinaryLineAndNotAnError) {
  /*
   * Measured: `[a` with no `]` leaves the following key in whatever section was
   * current, and `GetPrivateProfileSectionA` reports `[a` itself as one of that
   * section's entries. So it is neither refused nor skipped - it is a valueless
   * entry, which is why this rule travels with `valueless_keys`.
   *
   * Every other dialect refuses it, which is what the three specifications that
   * discuss it say.
   */
  GTEXT_INI_Document * doc = ok("[a\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"<preamble>"}));
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"[a", "k"}));
  EXPECT_EQ(written(doc), "[a\nk=v\n");
  gtext_ini_free(doc);
  /* And a following good header still starts a section. */
  GTEXT_INI_Document * two = ok("[a\nk=v\n[b]\nj=w\n", w32());
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(group_canon(two),
      (std::vector<std::string>{"<preamble>", "b"}));
  gtext_ini_free(two);
  /* The generic dialect, for contrast: the same bytes are a bad group. */
  refused("[a\nk=v\n", GTEXT_INI_E_BAD_GROUP, gtext_ini_dialect_generic());
}

TEST(IniWin32, AByteBeforeTheBracketMeansTheLineIsNotAHeader) {
  /* The `[` has to be the first non-blank byte. `x[a]` is an ordinary line, and
   * since it has no `=` it is a valueless entry. */
  GTEXT_INI_Document * doc = ok("x[a]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"<preamble>"}));
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"x[a]", "k"}));
  gtext_ini_free(doc);
}

TEST(IniWin32, TheEmptyGroupNameParsesAndIsUnreachableByName) {
  /*
   * `[]` is a legal header. Its entries are then reachable by no name at all,
   * because a lookup for the empty name finds the **preamble** - measured, `p=0`
   * then `[]` then `k=v` answers `p` and not `k`. That is the duplicate-section
   * rule reaching the preamble rather than a rule of its own, and
   * gtext_ini_group_is_preamble() is how a caller tells the two apart.
   */
  GTEXT_INI_Document * doc = ok("p=0\n[]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"<preamble>", ""}));
  EXPECT_EQ(raw(doc, "", "p"), "0");
  EXPECT_EQ(raw(doc, "", "k"), "<absent>");
  gtext_ini_free(doc);
  /* EditorConfig allows `[]` too; configparser refuses it. Three answers. */
  refused("[]\nk=v\n", GTEXT_INI_E_BAD_GROUP, cp());
}

TEST(IniWin32, TheFirstSeparatorSplitsTheLineAndTheKeyIsOpen) {
  struct Case { const char * text; const char * key; const char * value; };
  for (const Case & c : {
           Case{"[a]\nk = v\n", "k", "v"},
           Case{"[a]\nk=a=b\n", "k", "a=b"},
           Case{"[a]\nk==v\n", "k", "=v"},
           Case{"[a]\nk[1]=v\n", "k[1]", "v"},
           Case{"[a]\nq;x=v\n", "q;x", "v"},
           Case{"[a]\nq#x=v\n", "q#x", "v"},
           Case{"[a]\n\"k\"=v\n", "\"k\"", "v"},
           Case{"[a]\nke y=v\n", "ke y", "v"},
           Case{"[a]\nk\t=\tv\t\n", "k", "v"},
           Case{"[a]\nk\v=\fv\v\n", "k", "v"},
           /* `\x1c`-`\x1f` are not whitespace here, which is what separates this
            * set from configparser's. */
           Case{"[a]\nk\x1c=v\n", "k\x1c", "v"},
       }) {
    GTEXT_INI_Document * doc = ok(c.text, w32());
    ASSERT_NE(doc, nullptr) << c.text;
    EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{c.key})) << c.text;
    EXPECT_EQ(raw(doc, "a", c.key), c.value) << c.text;
    gtext_ini_free(doc);
  }
}

TEST(IniWin32, TheEmptyKeyIsAKeyAndIsAddressable) {
  /*
   * `= v` is an entry that `GetPrivateProfileStringA(sec, "", ...)` returns `v`
   * for, so the empty key is addressable rather than merely tolerated. Every
   * other dialect here refuses it - core-c reads it as a property named by the
   * empty string and core-py refuses it, and the EditorConfig arm follows
   * core-py.
   */
  for (const std::string & text : {std::string("[a]\n= v\n"),
           std::string("[a]\n   = v\n")}) {
    GTEXT_INI_Document * doc = ok(text, w32());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{""})) << text;
    EXPECT_EQ(raw(doc, "a", ""), "v") << text;
    gtext_ini_free(doc);
  }
  refused("[a]\n= v\n", GTEXT_INI_E_BAD_KEY, cp());
}

TEST(IniWin32, ALineWithNoSeparatorIsAValuelessEntry) {
  /*
   * Measured, and the two APIs part company here in the other direction:
   * `GetPrivateProfileSectionA` reports `novalue` as one of the section's
   * entries, and `GetPrivateProfileStringA` cannot retrieve it at all. The
   * grammar keeps it, which is what lets the document write back.
   *
   * This is also the flag's first live use with an **open** key charset. The
   * header for valueless_keys used to say that combination was impossible.
   */
  GTEXT_INI_Document * doc = ok("[a]\nnovalue   \n]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc),
      (std::vector<std::string>{"novalue", "]", "k"}));
  const GTEXT_INI_Group * group = gtext_ini_document_group_at(doc, 0);
  EXPECT_FALSE(gtext_ini_group_value_present_at(group, 0));
  EXPECT_FALSE(gtext_ini_group_value_present_at(group, 1));
  EXPECT_TRUE(gtext_ini_group_value_present_at(group, 2));
  EXPECT_EQ(written(doc), "[a]\nnovalue   \n]\nk=v\n");
  gtext_ini_free(doc);
}

TEST(IniWin32, OneMatchingPairOfSurroundingQuotesComesOffTheValue) {
  /*
   * A **wrapper**, not git's toggle, and the four rules that separate them are
   * each measured. The last row is the discriminating one: git gives `x mid y`.
   */
  EXPECT_EQ(w32_first("[a]\nk=\"v\"\n"), "v");
  EXPECT_EQ(w32_first("[a]\nk='v'\n"), "v");
  EXPECT_EQ(w32_first("[a]\nk=\"\"x\"\"\n"), "\"x\"");
  EXPECT_EQ(w32_first("[a]\nk=\"\"\n"), "");
  EXPECT_EQ(w32_first("[a]\nk=''\n"), "");
  /* Both ends must be quotes, and the same one. */
  EXPECT_EQ(w32_first("[a]\nk=\"x\n"), "\"x");
  EXPECT_EQ(w32_first("[a]\nk=x\"\n"), "x\"");
  EXPECT_EQ(w32_first("[a]\nk=\"x'\n"), "\"x'");
  EXPECT_EQ(w32_first("[a]\nk=\"\n"), "\"");
  /* A quote in the middle is data, because neither end is one. */
  EXPECT_EQ(w32_first("[a]\nk=x\" mid \"y\n"), "x\" mid \"y");
  /* And a `;` after a closing quote is still data: there are no inline
   * comments, so the value does not end at a quote either. */
  EXPECT_EQ(w32_first("[a]\nk=\"v\" ; c\n"), "\"v\" ; c");
  /* The strip happens after the trim, so quoting is the only way to spell a
   * value with a blank at either end. */
  EXPECT_EQ(w32_first("[a]\nk=\"  x  \"\n"), "  x  ");
  EXPECT_EQ(w32_first("[a]\nk=   pad   \n"), "pad");
  EXPECT_EQ(w32_first("[a]\nk=   \n"), "");
}

TEST(IniWin32, SemicolonIsACommentAndHashIsNot) {
  /*
   * **The dialect's one divergence, and it is measured on both sides.**
   * `GetPrivateProfileSection` drops a line whose first non-blank byte is `;`;
   * `GetPrivateProfileString` retrieves it by name. So `;disabled=1` is a comment
   * to one entry point of the reference and a live setting to the other, and this
   * follows the one that does not hand back a setting its author disabled.
   *
   * `#` is a comment to **neither**, which is the half most readers of this format
   * get wrong in the other direction.
   */
  GTEXT_INI_Document * doc = ok("[a]\n;disabled=1\n   ;also=2\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k"}));
  gtext_ini_free(doc);
  GTEXT_INI_Document * hash = ok("[a]\n#hash=2\n# prose\nk=v\n", w32());
  ASSERT_NE(hash, nullptr);
  EXPECT_EQ(canonical_keys(hash),
      (std::vector<std::string>{"#hash", "# prose", "k"}));
  EXPECT_EQ(raw(hash, "a", "#hash"), "2");
  gtext_ini_free(hash);
  /* A `;` that is not leading is an ordinary key byte. */
  GTEXT_INI_Document * mid = ok("[a]\nq;x=1\n", w32());
  ASSERT_NE(mid, nullptr);
  EXPECT_EQ(canonical_keys(mid), (std::vector<std::string>{"q;x"}));
  gtext_ini_free(mid);
}

TEST(IniWin32, BothNamesFoldAndOnlyOverAscii) {
  /*
   * The one dialect in this module whose case rule a byte-oriented reader
   * implements **exactly**. Measured: `[a]` and `[A]` are one section, `[\xe9]`
   * and `[\xc9]` are two. configparser lower-cases through Python `str` and this
   * module has to record two deviations for it; here there are none.
   */
  GTEXT_INI_Document * doc = ok("[MiXeD]\nKeY = v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"mixed"}));
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"key"}));
  /* The document's spelling is kept beside the folded one. */
  EXPECT_EQ(group_names(doc), (std::vector<std::string>{"MiXeD"}));
  EXPECT_EQ(written(doc), "[MiXeD]\nKeY = v\n");
  gtext_ini_free(doc);
  GTEXT_INI_Document * high = ok("[\xc3\xa9]\nk=v\n", w32());
  ASSERT_NE(high, nullptr);
  EXPECT_EQ(group_canon(high), (std::vector<std::string>{"\xc3\xa9"}));
  gtext_ini_free(high);
}

TEST(IniWin32, ADottedSectionNameFoldsLikeAnyOtherByte) {
  /*
   * **The `.` is an ordinary byte here, and it was not.** git was the only dialect
   * folding a group name when ini_group_matches() was written, so its rule became
   * *the* rule: fold up to the first `.` and compare the rest byte for byte,
   * because `[a.SubB]` folds its subsection and `[a "SubB"]` does not. Applied to
   * Win32 - where a dotted section name is ordinary and common - that split the
   * query in two, and a lookup using the **document's own spelling** returned
   * nothing while the fully lower-cased one worked.
   *
   * The reference finds all four spellings. Neither gate could see it: both asked
   * the reference with the canonical name, so no query they generated ever carried
   * an upper-case letter after a dot. A harness that normalizes its own input
   * cannot test a normalization.
   */
  GTEXT_INI_Document * doc = ok("[Foo.Bar]\nKeY=v\n", w32());
  ASSERT_NE(doc, nullptr);
  for (const std::string & spelling : {std::string("Foo.Bar"),
           std::string("foo.bar"), std::string("FOO.BAR"),
           std::string("foo.BAR"), std::string("FoO.bAr")}) {
    EXPECT_EQ(raw(doc, spelling.c_str(), "KeY"), "v") << spelling;
    EXPECT_EQ(raw(doc, spelling.c_str(), "key"), "v") << spelling;
    EXPECT_EQ(raw(doc, spelling.c_str(), "KEY"), "v") << spelling;
  }
  gtext_ini_free(doc);
  /* git's rule is still git's: a subsection's case is significant, and only the
   * section part before the dot folds. */
  GTEXT_INI_Document * g = ok("[a \"SubB\"]\n\tk = v\n",
      gtext_ini_dialect_git_config());
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(raw(g, "a.SubB", "k"), "v");
  EXPECT_EQ(raw(g, "A.SubB", "k"), "v");
  EXPECT_EQ(raw(g, "a.subb", "k"), "<absent>");
  gtext_ini_free(g);
}

TEST(IniWin32, ALookupTrimsTheNameItIsGiven) {
  /*
   * The stored canonical name has already been trimmed, and the caller's query had
   * not been - so `[ b ]` could be found as `b` and not as `" b "`, which is the
   * spelling a caller who copied the name out of the file would have. Measured: the
   * reference finds it either way.
   *
   * Found by the differential's `lookup` score, which exists because neither of the
   * other scores reaches gtext_ini_document_get() at all.
   */
  GTEXT_INI_Document * doc = ok("[ b ]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  for (const std::string & spelling : {std::string("b"), std::string(" b "),
           std::string("\tb\t"), std::string("  B  ")}) {
    EXPECT_EQ(raw(doc, spelling.c_str(), "k"), "v") << "[" << spelling << "]";
  }
  gtext_ini_free(doc);
}

TEST(IniWin32, TheEmptyNameIsThePreambleAndNotTheFirstEmptyGroup) {
  /*
   * Two kinds of group can carry an empty canonical name - the preamble, and a `[]`
   * header - and the reference resolves the empty name to the **preamble**, always.
   * Measured: `[]` then `k=v`, with no preamble at all, answers nothing for
   * `("", "k")`. So it is not "the first group whose name matches", and a `[]`
   * section's entries are reachable by no name.
   */
  GTEXT_INI_Document * doc = ok("[]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "", "k"), "<absent>");
  /* And the entries are still in the tree, which is what the rewrite needs. */
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k"}));
  EXPECT_EQ(written(doc), "[]\nk=v\n");
  gtext_ini_free(doc);
  /* With a preamble present, the preamble answers and the `[]` group does not. */
  GTEXT_INI_Document * two = ok("p=0\n[]\nk=v\n", w32());
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(raw(two, "", "p"), "0");
  EXPECT_EQ(raw(two, "", "k"), "<absent>");
  gtext_ini_free(two);
  /*
   * EditorConfig is deliberately the other way, and its own documentation says so:
   * gtext_ini_group_find("") may find either, whichever comes first, and
   * gtext_ini_group_is_preamble() is how a caller tells them apart. That is why the
   * rule above is conditioned on merge_duplicate_groups rather than applied to
   * every dialect that allows an empty name.
   */
  GTEXT_INI_Document * ecdoc = ok("[]\nk=v\n", ec());
  ASSERT_NE(ecdoc, nullptr);
  EXPECT_EQ(raw(ecdoc, "", "k"), "v");
  gtext_ini_free(ecdoc);
}

TEST(IniWin32, TheFirstOfTwoDuplicatesWins) {
  /* For keys and for sections alike, and neither is an error. */
  GTEXT_INI_Document * doc = ok("[a]\nk=1\nK=2\nk =3\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k", "k", "k"}));
  EXPECT_EQ(raw(doc, "a", "k"), "1");
  gtext_ini_free(doc);
  GTEXT_INI_Document * two = ok("[a]\nk=1\n[b]\nj=2\n[A]\nm=3\n", w32());
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(group_canon(two), (std::vector<std::string>{"a", "b", "a"}));
  EXPECT_EQ(raw(two, "a", "k"), "1");
  /* The later duplicate's entries are unreachable by lookup, and still in the
   * tree - which is exactly what `GetPrivateProfileSectionNames` shows. */
  EXPECT_EQ(raw(two, "a", "m"), "<absent>");
  gtext_ini_free(two);
}

TEST(IniWin32, AllThreeLineTerminatorsEndALine) {
  /* The lone CR is this dialect's alone among the seven. */
  for (const std::string & text : {std::string("[a]\nk=v\n"),
           std::string("[a]\r\nk=v\r\n"), std::string("[a]\rk=v\r"),
           std::string("[a]\nk=v"), std::string("[a]\r\nk=v\rj=w\n")}) {
    GTEXT_INI_Document * doc = ok(text, w32());
    ASSERT_NE(doc, nullptr) << text;
    EXPECT_EQ(raw(doc, "a", "k"), "v") << text;
    EXPECT_EQ(written(doc), text) << text;
    gtext_ini_free(doc);
  }
  /* A CR inside a value ends the line there rather than being data. */
  GTEXT_INI_Document * doc = ok("[a]\nk=a\rb\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a", "k"), "a");
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k", "b"}));
  gtext_ini_free(doc);
}

TEST(IniWin32, ThereAreNoEscapesAndNoContinuation) {
  /* A trailing backslash is data, and the next line is its own entry. */
  GTEXT_INI_Document * doc = ok("[a]\nk=one\\\nmore=two\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k", "more"}));
  EXPECT_EQ(raw(doc, "a", "k"), "one\\");
  gtext_ini_free(doc);
  /*
   * And the decode pass leaves a backslash alone, which is the part that
   * deriving from the generic dialect got wrong: Desktop Entry's escape set came
   * along and `\n` would have become a newline.
   */
  EXPECT_EQ(w32_first("[a]\nk=a\\nb\n"), "a\\nb");
  EXPECT_EQ(w32_first("[a]\nk=a\\\\b\n"), "a\\\\b");
  EXPECT_EQ(w32_first("[a]\nk=v\\\n"), "v\\");
}

TEST(IniWin32, HighBytesPassThroughEveryPosition) {
  /*
   * There are no encoding rules: the ANSI API is byte-oriented and validates
   * nothing. Inheriting the generic dialect's `utf8_values` would have refused
   * every `.ini` written in a code page, which is most of the older ones - and a
   * lone 0xE9 is not valid UTF-8, so this is the assertion that catches it.
   */
  const std::string text = "[\xe9]\nk\xe9=v\xe9\n";
  GTEXT_INI_Document * doc = ok(text, w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"\xe9"}));
  EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{"k\xe9"}));
  EXPECT_EQ(raw(doc, "\xe9", "k\xe9"), "v\xe9");
  EXPECT_EQ(written(doc), text);
  gtext_ini_free(doc);
  /*
   * **The contrast is in the decode, not the parse**, and getting that wrong is
   * what made this test fail first: ::GTEXT_INI_Dialect::utf8_values is read by
   * gtext_ini_unescape() and never by the parser, so the generic dialect *parses*
   * a latin-1 value happily and refuses it only when asked what it means.
   */
  EXPECT_EQ(w32_decoded("v\xe9"), "v\xe9");
  GTEXT_INI_Dialect g = gtext_ini_dialect_generic();
  char * out = nullptr;
  size_t olen = 0;
  EXPECT_EQ(gtext_ini_unescape(&g, "v\xe9", 2, nullptr, &out, &olen),
      GTEXT_INI_E_BAD_UNICODE);
  gtext_ini_string_free(nullptr, out);
}

TEST(IniWin32, EntriesBeforeAnyHeaderGoToThePreamble) {
  GTEXT_INI_Document * doc = ok("p=0\nq=1\n[a]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"<preamble>", "a"}));
  EXPECT_EQ(raw(doc, "", "p"), "0");
  EXPECT_EQ(raw(doc, "a", "k"), "v");
  gtext_ini_free(doc);
}

TEST(IniWin32, ThisDialectRefusesNothing) {
  /*
   * **A property, not an observation.** The key charset is open, the empty key
   * and the empty section name are both spellable, a line with no separator is a
   * valueless entry, and an unclosed header is an ordinary line - so there is no
   * byte sequence left for this reader to reject. The reference cannot report an
   * error either: the profile API has no way to say a file is malformed.
   *
   * The differential's `intent` score is this same assertion over 83 documents,
   * and it is what caught two defects - a group name outside git's charset and an
   * empty key both failed canonicalization, and a canonicalization failure is
   * reported as ::GTEXT_INI_E_OOM two frames up.
   *
   * **Qualified: every rule of the grammar, and not the encoding check above it.**
   * `FF FE 00 01` was in this list, chosen because a mark-shaped prefix ought not
   * to be special, and it is now ::GTEXT_INI_E_ENCODING - so this test failed the
   * moment gtext_ini_detect_encoding() existed, which is the right way round for
   * it to have found out. The input moved to IniEncoding, where being refused is
   * what it asserts. Nothing about the dialect changed: `FE FF 00 01` with the
   * mark's bytes altered is still accepted below, which is the pair that shows the
   * refusal belongs to the encoding and not to any rule here.
   */
  for (const std::string & text : {
           std::string(""), std::string("\n\n   \n"), std::string("["),
           std::string("]"), std::string("="), std::string("[]"),
           std::string("[[[["), std::string("]]]]"),
           std::string("\xfe\xfd\x00\x01", 4), std::string("=\n=\n=\n"),
           std::string("[a]\n\x00\n", 7), std::string("\r\r\r"),
           std::string("; \n# \n[ \n] \n"),
       }) {
    GTEXT_INI_Error err;
    std::memset(&err, 0, sizeof(err));
    GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
    opts.dialect = w32();
    GTEXT_INI_Document * doc =
        gtext_ini_parse(text.data(), text.size(), &opts, &err);
    EXPECT_NE(doc, nullptr) << "refused: " << ::testing::PrintToString(text)
                            << " with " << (err.message ? err.message : "?");
    if (doc) {
      /* And whatever it is, it writes back. */
      EXPECT_EQ(written(doc), text) << ::testing::PrintToString(text);
      gtext_ini_free(doc);
    }
    gtext_ini_error_free(&err);
  }
}

TEST(IniWin32, ANameWhoseOwnBytesAreABomIsNotNormalizable) {
  /*
   * **Found by the fuzzer at 663,648 executions, on four bytes:** a blank, then a
   * UTF-8 BOM. `skip_bom` strips a BOM only at offset 0, so with a blank in front
   * of it the BOM is *data* - and under this dialect a line needs no separator, so
   * the whole key is the BOM. That reading is correct.
   *
   * What was not correct is writing it back **normalized**: normalizing drops the
   * leading blank, the key lands at offset 0, and a reader strips the BOM there -
   * so the document came back with no entries at all. A verbatim write keeps the
   * blank and round-trips, which is why only the normalize path is refused.
   *
   * The generic dialect has had the same defect for as long as it has had
   * `skip_bom`; the seventh dialect, whose keys need no separator, is what made it
   * a four-byte input rather than a conjunction the fuzzer had not hit in 570,000
   * executions.
   */
  const std::string bom = "\xef\xbb\xbf";
  for (const GTEXT_INI_Dialect & d : {w32(), gtext_ini_dialect_generic()}) {
    GTEXT_INI_Document * doc = ok(" " + bom + "j=v\n", d);
    ASSERT_NE(doc, nullptr);
    /* The BOM is part of the key, and the verbatim write reproduces the line. */
    EXPECT_EQ(canonical_keys(doc), (std::vector<std::string>{bom + "j"}));
    EXPECT_EQ(written(doc), " " + bom + "j=v\n");
    /* Normalizing would move it to offset 0, so it is refused instead. */
    GTEXT_INI_Write_Options norm = gtext_ini_write_options_default();
    norm.normalize = true;
    GTEXT_INI_Sink sink;
    ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
    EXPECT_EQ(gtext_ini_write(doc, &sink, &norm), GTEXT_INI_E_UNREPRESENTABLE);
    gtext_ini_sink_buffer_free(&sink);
    gtext_ini_free(doc);
  }
  /* A group name is the same case. */
  GTEXT_INI_Document * g = ok(" [" + bom + "a]\nk=v\n", w32());
  ASSERT_NE(g, nullptr);
  GTEXT_INI_Write_Options norm = gtext_ini_write_options_default();
  norm.normalize = true;
  GTEXT_INI_Sink sink;
  ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_write(g, &sink, &norm), GTEXT_INI_E_UNREPRESENTABLE);
  gtext_ini_sink_buffer_free(&sink);
  gtext_ini_free(g);
  /* And a document that *does* carry a BOM of its own is unaffected: the reader
   * will strip exactly what the writer emitted and store it again. */
  GTEXT_INI_Document * ok_doc = ok(bom + "[a]\nk=v\n", w32());
  ASSERT_NE(ok_doc, nullptr);
  GTEXT_INI_Sink s2;
  ASSERT_EQ(gtext_ini_sink_buffer(&s2), GTEXT_INI_OK);
  EXPECT_EQ(gtext_ini_write(ok_doc, &s2, &norm), GTEXT_INI_OK);
  gtext_ini_sink_buffer_free(&s2);
  gtext_ini_free(ok_doc);
}

TEST(IniWin32, ABomIsSkippedRatherThanJoiningTheFirstName) {
  GTEXT_INI_Document * doc = ok("\xef\xbb\xbf[a]\nk=v\n", w32());
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(group_canon(doc), (std::vector<std::string>{"a"}));
  EXPECT_EQ(written(doc), "\xef\xbb\xbf[a]\nk=v\n");
  gtext_ini_free(doc);
}

TEST(IniWin32, TheWriterRefusesASynthesizedValueItCouldNotReadBack) {
  /*
   * **The builder accepts and the writer refuses**, which is this module's
   * contract everywhere: gtext_ini_group_set() validates the key and stores the
   * value, and ::GTEXT_INI_E_UNREPRESENTABLE comes from gtext_ini_write(). A test
   * that expected the refusal from the setter was asking the wrong function.
   *
   * What makes this dialect's version of the question sharp is that **all three
   * line terminators are terminators here**, so a lone CR in a synthesized value
   * is as unwritable as an LF - and the writer's non-scanning branch only refused
   * a CR that was both last and followed by an LF. A value of `a\rb` was written
   * verbatim and came back as two entries.
   */
  struct Case { const char * key; const char * value; bool writable; };
  for (const Case & c : {
           Case{"plain", "v", true},
           Case{"quoted", "\"v\"", true},
           Case{"semi", "a;b", true},
           Case{"hash", "a#b", true},
           /* Trimmed on the way back in, so it cannot be spelled. */
           Case{"lead", " v", false},
           Case{"trail", "v ", false},
           Case{"tab", "v\t", false},
           /* All three terminators end a line here. */
           Case{"lf", "a\nb", false},
           Case{"cr", "a\rb", false},
           Case{"crlf", "a\r\nb", false},
           Case{"cr-last", "v\r", false},
       }) {
    GTEXT_INI_Document * doc = empty(w32());
    ASSERT_NE(doc, nullptr) << c.key;
    GTEXT_INI_Group * g = nullptr;
    ASSERT_EQ(gtext_ini_document_add_group(doc, "a", &g), GTEXT_INI_OK);
    ASSERT_EQ(gtext_ini_group_set(g, c.key, c.value, std::strlen(c.value)),
        GTEXT_INI_OK) << c.key;
    GTEXT_INI_Sink sink;
    ASSERT_EQ(gtext_ini_sink_buffer(&sink), GTEXT_INI_OK);
    GTEXT_INI_Status status = gtext_ini_write(doc, &sink, nullptr);
    if (c.writable) {
      EXPECT_EQ(status, GTEXT_INI_OK) << c.key;
      if (status == GTEXT_INI_OK) {
        /* And what it wrote reads back as the same value, which is the property
         * the refusal above exists to protect. */
        std::string bytes(gtext_ini_sink_buffer_data(&sink),
            gtext_ini_sink_buffer_size(&sink));
        GTEXT_INI_Document * again = ok(bytes, w32());
        ASSERT_NE(again, nullptr) << c.key;
        EXPECT_EQ(raw(again, "a", c.key), c.value) << c.key;
        gtext_ini_free(again);
      }
    }
    else {
      EXPECT_EQ(status, GTEXT_INI_E_UNREPRESENTABLE) << c.key;
    }
    gtext_ini_sink_buffer_free(&sink);
    gtext_ini_free(doc);
  }
}

namespace {

/** The same document, encoded as UTF-16 with a byte-order mark. */
std::string as_utf16(const std::string & utf8, bool big_endian, bool bom = true) {
  std::string out;
  if (bom) out += big_endian ? "\xFE\xFF" : "\xFF\xFE";
  /* ASCII only, which every caller below is: one code unit per byte. */
  for (unsigned char c : utf8) {
    if (big_endian) { out += '\0'; out += (char) c; }
    else { out += (char) c; out += '\0'; }
  }
  return out;
}

/** Parse with decode_utf16 on, expecting success. */
GTEXT_INI_Document * u16_ok(const std::string & text,
    GTEXT_INI_Dialect dialect = gtext_ini_dialect_win32()) {
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = dialect;
  opts.decode_utf16 = true;
  GTEXT_INI_Document * doc =
      gtext_ini_parse(text.data(), text.size(), &opts, &err);
  if (!doc) {
    ADD_FAILURE() << "decode failed: " << (err.message ? err.message : "?");
  }
  gtext_ini_error_free(&err);
  return doc;
}

/** Parse with decode_utf16 on, expecting a particular refusal. */
void decode_refused(const std::string & text, GTEXT_INI_Status expect) {
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_win32();
  opts.decode_utf16 = true;
  GTEXT_INI_Document * doc =
      gtext_ini_parse(text.data(), text.size(), &opts, &err);
  EXPECT_EQ(doc, nullptr);
  if (doc) gtext_ini_free(doc);
  else EXPECT_EQ(err.code, expect);
  gtext_ini_error_free(&err);
}

} // namespace

/**
 * The measurement this whole group exists because of.
 *
 * Before ::GTEXT_INI_E_ENCODING there was no refusal here and no diagnostic: a
 * UTF-16LE `.ini` *parsed*, into one group whose name was empty and five entries
 * whose keys held the file's bytes with NULs between them. Under the Win32
 * dialect that outcome was unconditional, because the dialect refuses nothing, so
 * a caller had no way at all to tell the nonsense from a reading.
 *
 * Asserted as the shape of the old behaviour rather than described in a comment,
 * because the sniff is what stands between the two and a test that only checked
 * the new answer would pass just as well if the sniff were moved somewhere it
 * could be skipped.
 */
TEST(IniEncoding, AUtf16DocumentUsedToParseIntoNonsense) {
  const std::string u8 = "[boot]\r\nshell=explorer.exe\r\n";
  const std::string le = as_utf16(u8, false);

  /* What it does now, for every dialect: one code, naming the encoding. */
  refused(le, GTEXT_INI_E_ENCODING, gtext_ini_dialect_win32());
  refused(le, GTEXT_INI_E_ENCODING, gtext_ini_dialect_desktop_entry());
  refused(le, GTEXT_INI_E_ENCODING, gtext_ini_dialect_generic());

  /* And the nonsense it used to be: the bytes are still there to be read that
   * way, so the assertion is that nothing reads them that way any more. */
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_win32();
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Document * doc =
      gtext_ini_parse(le.data(), le.size(), &opts, &err);
  EXPECT_EQ(doc, nullptr);
  /* The old reading produced exactly one group and five entries. If this ever
   * parses again, that is the count to look for. */
  if (doc) {
    EXPECT_EQ(gtext_ini_document_group_count(doc), 0u)
        << "a UTF-16 document parsed; the old defect is back";
    gtext_ini_free(doc);
  }
  gtext_ini_error_free(&err);
}

/** Both byte orders decode to the same tree the UTF-8 original gives. */
TEST(IniEncoding, BothByteOrdersDecodeToTheSameDocument) {
  const std::string u8 = "[boot]\r\nshell=explorer.exe\r\nrun=\r\n";
  GTEXT_INI_Document * plain = ok(u8, gtext_ini_dialect_win32());
  ASSERT_NE(plain, nullptr);
  const std::string expect = raw(plain, "boot", "shell");
  EXPECT_EQ(expect, "explorer.exe");

  for (bool big : {false, true}) {
    GTEXT_INI_Document * doc = u16_ok(as_utf16(u8, big));
    ASSERT_NE(doc, nullptr) << (big ? "BE" : "LE");
    EXPECT_EQ(gtext_ini_document_group_count(doc),
        gtext_ini_document_group_count(plain));
    EXPECT_EQ(raw(doc, "boot", "shell"), expect);
    /* The empty value survives the decode as an empty value rather than as an
     * absent key, which is the distinction a NUL-terminated converter loses. */
    EXPECT_EQ(raw(doc, "boot", "run"), "");
    EXPECT_EQ(gtext_ini_document_source_encoding(doc),
        big ? GTEXT_INI_SOURCE_UTF16BE : GTEXT_INI_SOURCE_UTF16LE);
    gtext_ini_free(doc);
  }
  gtext_ini_free(plain);
}

/**
 * A UTF-32LE mark opens with UTF-16LE's two bytes, and the order of the tests is
 * the only thing that keeps them apart.
 *
 * This is the test that would catch a sniffer rewritten "more simply" with the
 * two-byte marks first: every UTF-32LE document would then be decoded as UTF-16LE
 * into alternating text and NULs, successfully, with no diagnostic anywhere -
 * which is the same failure this group was created to remove, one encoding along.
 */
TEST(IniEncoding, Utf32IsNotMistakenForUtf16) {
  std::string u32("\xFF\xFE\x00\x00", 4);
  for (char c : std::string("[a]\nk=v\n")) {
    u32 += c;
    u32 += std::string("\x00\x00\x00", 3);
  }
  size_t bom_len = 0;
  EXPECT_EQ(gtext_ini_detect_encoding(u32.data(), u32.size(), &bom_len),
      GTEXT_INI_SOURCE_UTF32LE);
  EXPECT_EQ(bom_len, 4u);
  /* Refused with decode_utf16 *on*, which is the point: the option says "decode
   * UTF-16", and this is not UTF-16. */
  decode_refused(u32, GTEXT_INI_E_ENCODING);
  refused(u32, GTEXT_INI_E_ENCODING, gtext_ini_dialect_win32());

  std::string be("\x00\x00\xFE\xFF", 4);
  be += "junk";
  EXPECT_EQ(gtext_ini_detect_encoding(be.data(), be.size(), &bom_len),
      GTEXT_INI_SOURCE_UTF32BE);
  EXPECT_EQ(bom_len, 4u);
  decode_refused(be, GTEXT_INI_E_ENCODING);
}

/** Every mark the detector knows, and the lengths it reports for them. */
TEST(IniEncoding, TheDetectorReportsTheMarkAndItsLength) {
  struct Case {
    std::string bytes;
    GTEXT_INI_Source_Encoding expect;
    size_t bom_len;
    const char * why;
  };
  const Case cases[] = {
      {"", GTEXT_INI_SOURCE_BYTES, 0, "empty"},
      {"[a]\nk=v\n", GTEXT_INI_SOURCE_BYTES, 0, "no mark"},
      {"\xEF\xBB\xBF[a]\n", GTEXT_INI_SOURCE_UTF8, 3, "UTF-8"},
      {std::string("\xFF\xFE[\x00", 4), GTEXT_INI_SOURCE_UTF16LE, 2, "UTF-16LE"},
      {std::string("\xFE\xFF\x00[", 4), GTEXT_INI_SOURCE_UTF16BE, 2, "UTF-16BE"},
      {std::string("\xFF\xFE\x00\x00", 4), GTEXT_INI_SOURCE_UTF32LE, 4,
          "UTF-32LE"},
      {std::string("\x00\x00\xFE\xFF", 4), GTEXT_INI_SOURCE_UTF32BE, 4,
          "UTF-32BE"},
      /* Two bytes of a four-byte mark is the two-byte mark, because that is all
       * there is: `FF FE` alone is a UTF-16LE document of no content. */
      {std::string("\xFF\xFE", 2), GTEXT_INI_SOURCE_UTF16LE, 2, "bare FF FE"},
      /* And one byte of it is nothing. */
      {std::string("\xFF", 1), GTEXT_INI_SOURCE_BYTES, 0, "lone FF"},
      /* A UTF-8 mark needs all three bytes. */
      {std::string("\xEF\xBB", 2), GTEXT_INI_SOURCE_BYTES, 0, "partial UTF-8"},
  };
  for (const Case & c : cases) {
    size_t bom_len = 12345;
    EXPECT_EQ(gtext_ini_detect_encoding(c.bytes.data(), c.bytes.size(), &bom_len),
        c.expect) << c.why;
    EXPECT_EQ(bom_len, c.bom_len) << c.why;
  }
  /* NULL is bytes, and does not write through a NULL out-parameter. */
  EXPECT_EQ(gtext_ini_detect_encoding(nullptr, 0, nullptr),
      GTEXT_INI_SOURCE_BYTES);
}

/**
 * There is deliberately no content sniffing, so a BOM-less UTF-16 document still
 * reads as nonsense.
 *
 * Asserted rather than left unmentioned, because it is the one case this work
 * does not close and an untested gap reads exactly like an oversight. The shape
 * that would identify it - a NUL at every odd offset - is real and is a guess,
 * and this machine holds no BOM-less UTF-16 `.ini` file to calibrate a guess
 * against: of its 612 `.ini` files, 612 are byte-oriented.
 */
TEST(IniEncoding, ABomlessUtf16DocumentIsStillReadAsBytes) {
  const std::string le = as_utf16("[boot]\nshell=x\n", false, /*bom=*/false);
  size_t bom_len = 0;
  EXPECT_EQ(gtext_ini_detect_encoding(le.data(), le.size(), &bom_len),
      GTEXT_INI_SOURCE_BYTES);
  EXPECT_EQ(bom_len, 0u);
  /* It parses, and into the nonsense the marked form used to give: the group's
   * name carries the NULs. */
  GTEXT_INI_Document * doc = u16_ok(le);
  ASSERT_NE(doc, nullptr);
  size_t len = 0;
  const GTEXT_INI_Group * g = gtext_ini_document_group_at(doc, 0);
  const char * name = gtext_ini_group_name(g, &len);
  ASSERT_NE(name, nullptr);
  /*
   * Nine bytes, not eight: the name starts at the byte after `[`, which in UTF-16LE
   * is the high half of `[` itself, so the nonsense is not even aligned to the code
   * units. That is the clearest single statement of why a marked document had to
   * stop parsing.
   *
   * Built rather than spelled as a literal, because `"\x00b"` is not what it looks
   * like: a hex escape in C++ consumes as many hex digits as follow it, so that is
   * the one character 0x0B and the first attempt at this assertion compared against
   * `"\v\0o\0o\0t\0\0"`. A test whose expected value is built the same way the
   * input is cannot make that mistake.
   */
  std::string expect;
  expect += '\0';
  for (char c : std::string("boot")) { expect += c; expect += '\0'; }
  EXPECT_EQ(std::string(name, len), expect);
  EXPECT_EQ(gtext_ini_document_source_encoding(doc), GTEXT_INI_SOURCE_BYTES);
  gtext_ini_free(doc);
}

/**
 * The inputs that moved here out of IniWin32.ThisDialectRefusesNothing.
 *
 * `FF FE 00 01` was in that list because a mark-shaped prefix ought not to be
 * special to a dialect that refuses nothing, and the encoding check made it
 * special. Kept as a case rather than deleted, because the interesting part is
 * the pair: the same four bytes with the mark altered are still accepted, so the
 * refusal is demonstrably about the mark and not about any rule of the grammar.
 */
TEST(IniEncoding, AMarkShapedPrefixIsTheOnlyThingWin32Refuses) {
  refused(std::string("\xff\xfe\x00\x01", 4), GTEXT_INI_E_ENCODING,
      gtext_ini_dialect_win32());
  refused(std::string("\xfe\xff\x00\x01", 4), GTEXT_INI_E_ENCODING,
      gtext_ini_dialect_win32());
  /* One byte different, and it is an ordinary document again. */
  GTEXT_INI_Document * doc =
      ok(std::string("\xfe\xfd\x00\x01", 4), gtext_ini_dialect_win32());
  ASSERT_NE(doc, nullptr);
  gtext_ini_free(doc);
  /* `FF FE` with nothing after it is a UTF-16LE document of no content, and is
   * refused for that and not for being short. With the decode on it is an empty
   * document, which is the only reading. */
  refused(std::string("\xff\xfe", 2), GTEXT_INI_E_ENCODING,
      gtext_ini_dialect_win32());
  GTEXT_INI_Document * empty = u16_ok(std::string("\xff\xfe", 2));
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(gtext_ini_document_group_count(empty), 0u);
  EXPECT_EQ(written(empty), "");
  gtext_ini_free(empty);
}

/** A truncated or damaged UTF-16 document is refused, not half-read. */
TEST(IniEncoding, DamagedUtf16IsRefused) {
  /* An odd byte count: the last code unit has one byte. */
  std::string odd = as_utf16("[a]\nk=v\n", false);
  odd.pop_back();
  decode_refused(odd, GTEXT_INI_E_BAD_UNICODE);

  /* A high surrogate at the end of the document, with no low one to pair. */
  std::string high("\xFF\xFE", 2);
  high += std::string("\x00\xD8", 2);
  decode_refused(high, GTEXT_INI_E_BAD_UNICODE);

  /* A high surrogate followed by something that is not a low surrogate. */
  std::string bad_pair("\xFF\xFE", 2);
  bad_pair += std::string("\x00\xD8", 2);
  bad_pair += std::string("A\x00", 2);
  decode_refused(bad_pair, GTEXT_INI_E_BAD_UNICODE);

  /* A low surrogate with no high one before it. Refused rather than passed
   * through as WTF-8, which is the choice the unicode library states. */
  std::string lone_low("\xFF\xFE", 2);
  lone_low += std::string("\x00\xDC", 2);
  decode_refused(lone_low, GTEXT_INI_E_BAD_UNICODE);
}

/** A surrogate pair decodes to one code point above the BMP. */
TEST(IniEncoding, ASurrogatePairDecodesToOneCodePoint) {
  /* U+1F4A9 is D83D DCA9 in UTF-16, and F0 9F 92 A9 in UTF-8. */
  std::string le("\xFF\xFE", 2);
  for (char c : std::string("[a]\nk=")) { le += c; le += '\0'; }
  le += std::string("\x3D\xD8\xA9\xDC", 4);
  le += std::string("\n\x00", 2);
  GTEXT_INI_Document * doc = u16_ok(le);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a", "k"), "\xF0\x9F\x92\xA9");
  gtext_ini_free(doc);
}

/**
 * A decoded document does not rewrite byte-identically, and says so.
 *
 * Not a defect to fix later but the arithmetic of the request: the caller asked
 * for the text and the text is UTF-8. The test exists so that the claim is
 * checked rather than asserted in a doc comment, and so that a later change that
 * made gtext_ini_write() re-encode would have to come here and say so.
 */
TEST(IniEncoding, ADecodedDocumentRewritesAsUtf8WithNoMark) {
  const std::string u8 = "[boot]\r\nshell=explorer.exe\r\n";
  GTEXT_INI_Document * doc = u16_ok(as_utf16(u8, false));
  ASSERT_NE(doc, nullptr);
  /* Byte-identical to the *decoded* bytes, which is what the tree holds - the
   * line terminators and spacing are preserved, only the encoding is not. */
  EXPECT_EQ(written(doc), u8);
  EXPECT_EQ(gtext_ini_document_source_encoding(doc), GTEXT_INI_SOURCE_UTF16LE);
  gtext_ini_free(doc);
}

/** What the accessor says for every other way a document can come about. */
TEST(IniEncoding, SourceEncodingForDocumentsThatWereNeverDecoded) {
  EXPECT_EQ(gtext_ini_document_source_encoding(nullptr),
      GTEXT_INI_SOURCE_BYTES);

  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_win32();
  GTEXT_INI_Document * built = gtext_ini_new(&opts);
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(gtext_ini_document_source_encoding(built), GTEXT_INI_SOURCE_BYTES);
  gtext_ini_free(built);

  GTEXT_INI_Document * plain = ok("[a]\nk=v\n", gtext_ini_dialect_win32());
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(gtext_ini_document_source_encoding(plain), GTEXT_INI_SOURCE_BYTES);
  gtext_ini_free(plain);

  /* A UTF-8 mark is reported whether or not the dialect skipped it, because this
   * says what the file was and skip_bom says what the parser did with it. */
  GTEXT_INI_Document * marked = ok("\xEF\xBB\xBF[a]\nk=v\n",
      gtext_ini_dialect_win32());
  ASSERT_NE(marked, nullptr);
  EXPECT_EQ(gtext_ini_document_source_encoding(marked), GTEXT_INI_SOURCE_UTF8);
  gtext_ini_free(marked);
}

/**
 * A UTF-16 document whose first code point is U+FEFF carries two marks, and the
 * second one survives the decode as a UTF-8 mark.
 *
 * Worth a test because the recursion makes it happen without anybody deciding it
 * should: the decode strips the UTF-16 mark, the decoded bytes then begin
 * `EF BB BF`, and the inner parse treats that as the document's own mark. That is
 * the right answer - the inner ZWNBSP really is in the text - and it is the kind
 * of answer a recursive implementation gives by accident, so it is pinned here.
 */
TEST(IniEncoding, ADoubleMarkDecodesToADocumentWithAUtf8Mark) {
  std::string le("\xFF\xFE", 2);
  le += std::string("\xFF\xFE", 2);  /* U+FEFF as text, not as a mark. */
  for (char c : std::string("[a]\nk=v\n")) { le += c; le += '\0'; }
  GTEXT_INI_Document * doc = u16_ok(le);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(raw(doc, "a", "k"), "v");
  /* The outer mark decided the encoding; the inner one is the document's. */
  EXPECT_EQ(gtext_ini_document_source_encoding(doc), GTEXT_INI_SOURCE_UTF16LE);
  EXPECT_EQ(written(doc), "\xEF\xBB\xBF[a]\nk=v\n");
  gtext_ini_free(doc);
}

/**
 * The limit is spent on the caller's bytes and not again on the decoded ones.
 *
 * A UTF-16 document within `max_total_bytes` must not be refused because its
 * UTF-8 form is larger, and it can be: three bytes out for two in, for anything
 * from U+0800 to U+FFFF.
 */
TEST(IniEncoding, TheSizeLimitAppliesToTheBytesHandedIn) {
  /* Every value byte is U+4E2D, which is 2 bytes of UTF-16 and 3 of UTF-8. */
  std::string le("\xFF\xFE", 2);
  for (char c : std::string("[a]\nk=")) { le += c; le += '\0'; }
  for (int i = 0; i < 40; i++) le += std::string("\x2D\x4E", 2);
  le += std::string("\n\x00", 2);

  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_win32();
  opts.decode_utf16 = true;
  opts.max_total_bytes = le.size();
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Document * doc =
      gtext_ini_parse(le.data(), le.size(), &opts, &err);
  ASSERT_NE(doc, nullptr) << (err.message ? err.message : "?");
  /* The decoded value is larger than the limit the input fitted inside. */
  EXPECT_GT(raw(doc, "a", "k").size(), 80u);
  gtext_ini_free(doc);
  gtext_ini_error_free(&err);

  /* And the limit still bites on the input itself. */
  opts.max_total_bytes = le.size() - 1;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Document * refused_doc =
      gtext_ini_parse(le.data(), le.size(), &opts, &err);
  EXPECT_EQ(refused_doc, nullptr);
  if (refused_doc) gtext_ini_free(refused_doc);
  else EXPECT_EQ(err.code, GTEXT_INI_E_LIMIT);
  gtext_ini_error_free(&err);
}

/** The whole path through the file reader, which is where such a file arrives. */
TEST(IniEncoding, AUtf16FileIsReadThroughTheFileReader) {
  const std::string u8 = "[boot]\r\nshell=explorer.exe\r\n";
  std::string path = std::string(::testing::TempDir()) + "gtext-ini-utf16.ini";
  {
    std::FILE * f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    const std::string le = as_utf16(u8, false);
    ASSERT_EQ(std::fwrite(le.data(), 1, le.size(), f), le.size());
    std::fclose(f);
  }

  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_win32();
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Document * doc = gtext_ini_parse_file(path.c_str(), &opts, &err);
  EXPECT_EQ(doc, nullptr) << "the default must not read it as bytes";
  if (doc) gtext_ini_free(doc);
  else EXPECT_EQ(err.code, GTEXT_INI_E_ENCODING);
  gtext_ini_error_free(&err);

  opts.decode_utf16 = true;
  std::memset(&err, 0, sizeof(err));
  doc = gtext_ini_parse_file(path.c_str(), &opts, &err);
  ASSERT_NE(doc, nullptr) << (err.message ? err.message : "?");
  EXPECT_EQ(raw(doc, "boot", "shell"), "explorer.exe");
  EXPECT_EQ(gtext_ini_document_source_encoding(doc), GTEXT_INI_SOURCE_UTF16LE);
  gtext_ini_free(doc);
  gtext_ini_error_free(&err);
  std::remove(path.c_str());
}

namespace {

/** Parse under the configparser dialect, which is the only one with a reference
 *  for interpolation at all. */
GTEXT_INI_Document * cp_parse(const std::string & text) {
  GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
  opts.dialect = gtext_ini_dialect_configparser();
  GTEXT_INI_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_INI_Document * doc =
      gtext_ini_parse(text.data(), text.size(), &opts, &err);
  gtext_ini_error_free(&err);
  return doc;
}

/**
 * Join a raw value the way the parser's caller must, then interpolate it.
 *
 * **Two passes and in that order**, which is the reference's: `read()` stores a
 * value already joined and `get()` interpolates what it stored. Skipping the join
 * and interpolating the raw span differs on every value a continuation line spans,
 * so the helper does both and no test can accidentally do one.
 */
GTEXT_INI_Status interp(const std::string & text, const char * group,
    const char * key, GTEXT_INI_Interpolation style, std::string * out,
    const GTEXT_INI_Group * defaults = nullptr) {
  out->clear();
  GTEXT_INI_Document * doc = cp_parse(text);
  if (!doc) return GTEXT_INI_E_INVALID;
  GTEXT_INI_Dialect dialect = gtext_ini_dialect_configparser();
  const GTEXT_INI_Group * g = gtext_ini_document_group(doc, group);
  if (!g) {
    gtext_ini_free(doc);
    return GTEXT_INI_E_INVALID;
  }
  size_t raw_len = 0;
  const char * raw = gtext_ini_group_get(g, key, &raw_len);
  char * joined = nullptr;
  size_t joined_len = 0;
  GTEXT_INI_Status status = gtext_ini_unescape(&dialect, raw ? raw : "",
      raw ? raw_len : 0, nullptr, &joined, &joined_len);
  if (status == GTEXT_INI_OK) {
    GTEXT_INI_Interpolate_Options opts =
        gtext_ini_interpolate_options_default();
    opts.style = style;
    opts.defaults = defaults ? defaults
                             : gtext_ini_document_group(doc, "DEFAULT");
    char * resolved = nullptr;
    size_t resolved_len = 0;
    status = gtext_ini_value_interpolate(g, &opts, joined, joined_len,
        &resolved, &resolved_len);
    if (status == GTEXT_INI_OK) out->assign(resolved, resolved_len);
    gtext_ini_string_free(nullptr, resolved);
  }
  gtext_ini_string_free(nullptr, joined);
  gtext_ini_free(doc);
  return status;
}

/** The resolved value, for a case expected to succeed. */
std::string resolved(const std::string & text, const char * key,
    GTEXT_INI_Interpolation style) {
  std::string out;
  GTEXT_INI_Status status = interp(text, "s", key, style, &out);
  EXPECT_EQ(status, GTEXT_INI_OK) << "status " << (int) status;
  return out;
}

} // namespace

/**
 * The measurement the default rests on, asserted rather than only recorded.
 *
 * Every dialect here parses `100%` and hands back `100%`, and that is not a
 * convenience: `configparser`'s own default refuses a value in **301** of the 479
 * real documents on this machine it can read, and changes a value in **none**. A
 * reader that interpolated by default would therefore read a third of this
 * machine's INI files worse, and would gain nothing measurable on the rest.
 *
 * So the assertion is that the pass is **off unless asked for**, under every
 * spelling of "I did not ask": a NULL options pointer, a default-initialized
 * struct, and an explicitly-NONE style. A gap between those three is how a
 * feature turns itself on.
 */
TEST(IniInterpolation, TheDefaultResolvesNothing) {
  const std::string doc = "[s]\nalpha = one\na = %(alpha)s\nb = 100%\n"
                          "c = ${s:alpha}\n";
  GTEXT_INI_Document * d = cp_parse(doc);
  ASSERT_NE(d, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(d, "s");
  ASSERT_NE(g, nullptr);

  for (const char * key : {"a", "b", "c"}) {
    size_t len = 0;
    const char * raw = gtext_ini_group_get(g, key, &len);
    ASSERT_NE(raw, nullptr) << key;
    const std::string want(raw, len);

    /* NULL options. */
    char * out = nullptr;
    size_t out_len = 0;
    EXPECT_EQ(gtext_ini_value_interpolate(g, nullptr, raw, len, &out, &out_len),
        GTEXT_INI_OK);
    EXPECT_EQ(std::string(out, out_len), want) << key;
    gtext_ini_string_free(nullptr, out);

    /* A zero-initialized struct, which must mean the same thing - the field
     * order is not a contract and a caller who memsets must get the default. */
    GTEXT_INI_Interpolate_Options zeroed;
    std::memset(&zeroed, 0, sizeof(zeroed));
    out = nullptr;
    EXPECT_EQ(gtext_ini_value_interpolate(g, &zeroed, raw, len, &out, &out_len),
        GTEXT_INI_OK);
    EXPECT_EQ(std::string(out, out_len), want) << key;
    gtext_ini_string_free(nullptr, out);

    /* And the named style. */
    GTEXT_INI_Interpolate_Options none = gtext_ini_interpolate_options_default();
    none.style = GTEXT_INI_INTERPOLATION_NONE;
    out = nullptr;
    EXPECT_EQ(gtext_ini_value_interpolate(g, &none, raw, len, &out, &out_len),
        GTEXT_INI_OK);
    EXPECT_EQ(std::string(out, out_len), want) << key;
    gtext_ini_string_free(nullptr, out);
  }
  EXPECT_EQ(gtext_ini_interpolate_options_default().style,
      GTEXT_INI_INTERPOLATION_NONE);
  gtext_ini_free(d);
}

/**
 * `NONE` **copies**, so the result is owned under every style.
 *
 * Returning the caller's own pointer would be cheaper and is the sentinel that
 * means two things: a caller holding the style in a variable would then have to
 * know which styles hand back memory to free and which do not, and the one that
 * does not is the default.
 */
TEST(IniInterpolation, NoneStillReturnsAnOwnedBuffer) {
  const char * raw = "100%";
  GTEXT_INI_Interpolate_Options opts = gtext_ini_interpolate_options_default();
  char * out = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(gtext_ini_value_interpolate(nullptr, &opts, raw, 4, &out, &out_len),
      GTEXT_INI_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_NE(out, raw) << "a borrowed pointer would be freed by the caller";
  EXPECT_EQ(std::string(out, out_len), "100%");
  gtext_ini_string_free(nullptr, out);
}

/**
 * The half of `BasicInterpolation` that is not substitution.
 *
 * `'%' must be followed by '%' or '('` is the reference's own message, and it makes
 * the default configuration a **validator**: `pct = 100%` is a hard error to
 * `configparser.ConfigParser()` and reads fine with `interpolation=None`. This is
 * the rule that refuses 301 of the 479, so it is the one worth spelling out here
 * rather than leaving to the oracle.
 */
TEST(IniInterpolation, BasicRefusesABarePercent) {
  std::string out;
  EXPECT_EQ(interp("[s]\na = 100%\n", "s", "a",
      GTEXT_INI_INTERPOLATION_BASIC, &out), GTEXT_INI_E_INTERPOLATION);
  EXPECT_EQ(interp("[s]\na = %\n", "s", "a",
      GTEXT_INI_INTERPOLATION_BASIC, &out), GTEXT_INI_E_INTERPOLATION);
  EXPECT_EQ(interp("[s]\na = 50%50\n", "s", "a",
      GTEXT_INI_INTERPOLATION_BASIC, &out), GTEXT_INI_E_INTERPOLATION);

  /* The acquitting pair: one byte different and the same value is fine, so the
   * refusal is about the `%` and not about the digits around it. */
  EXPECT_EQ(resolved("[s]\na = 100%%\n", "a", GTEXT_INI_INTERPOLATION_BASIC),
      "100%");
  EXPECT_EQ(resolved("[s]\na = 100%\n", "a", GTEXT_INI_INTERPOLATION_NONE),
      "100%");
  /* And `$` is nothing to this style, as `%` is nothing to the other. */
  EXPECT_EQ(resolved("[s]\na = 100$\n", "a", GTEXT_INI_INTERPOLATION_BASIC),
      "100$");
  EXPECT_EQ(resolved("[s]\na = 100%\n", "a", GTEXT_INI_INTERPOLATION_EXTENDED),
      "100%");
  EXPECT_EQ(resolved("[s]\na = a%%b\n", "a", GTEXT_INI_INTERPOLATION_EXTENDED),
      "a%%b") << "Extended does not touch a percent at all";
}

/** `%(key)s` and the ways of spelling it wrong. */
TEST(IniInterpolation, BasicSubstitutesFromTheSameSection) {
  EXPECT_EQ(resolved("[s]\nalpha = one\na = %(alpha)s\n", "a",
      GTEXT_INI_INTERPOLATION_BASIC), "one");
  EXPECT_EQ(resolved("[s]\nalpha = one\na = x%(alpha)sy\n", "a",
      GTEXT_INI_INTERPOLATION_BASIC), "xoney");
  /* The key folds the way the dialect folds, which for configparser is how it
   * finds a key at all. */
  EXPECT_EQ(resolved("[s]\nALPHA = one\na = %(alpha)s\n", "a",
      GTEXT_INI_INTERPOLATION_BASIC), "one");

  std::string out;
  EXPECT_EQ(interp("[s]\na = %(missing)s\n", "s", "a",
      GTEXT_INI_INTERPOLATION_BASIC, &out),
      GTEXT_INI_E_INTERPOLATION_MISSING);
  /* Not a reference at all, so a syntax refusal rather than a missing one: the
   * pattern needs a name, a `)` and then an `s`. */
  for (const char * bad : {"%()s", "%(x", "%(x)d", "%(x)"}) {
    EXPECT_EQ(interp(std::string("[s]\na = ") + bad + "\n", "s", "a",
        GTEXT_INI_INTERPOLATION_BASIC, &out), GTEXT_INI_E_INTERPOLATION)
        << bad;
  }
}

/** `${key}`, `${section:key}`, and `$$`. */
TEST(IniInterpolation, ExtendedWalksTheDocument) {
  EXPECT_EQ(resolved("[s]\nalpha = one\na = ${alpha}\n", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED), "one");
  EXPECT_EQ(resolved("[s]\na = ${o:k}\n[o]\nk = far\n", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED), "far");
  EXPECT_EQ(resolved("[s]\na = $$\n", "a", GTEXT_INI_INTERPOLATION_EXTENDED),
      "$");

  std::string out;
  EXPECT_EQ(interp("[s]\na = ${nope}\n", "s", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED, &out),
      GTEXT_INI_E_INTERPOLATION_MISSING);
  EXPECT_EQ(interp("[s]\na = ${no:such}\n", "s", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED, &out),
      GTEXT_INI_E_INTERPOLATION_MISSING);
  /* "More than one ':' found" is the reference's own wording, and it is a syntax
   * refusal rather than a section whose name contains a colon. */
  EXPECT_EQ(interp("[s]\na = ${x:y:z}\n", "s", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED, &out), GTEXT_INI_E_INTERPOLATION);
  for (const char * bad : {"$", "a$b", "${}", "${x"}) {
    EXPECT_EQ(interp(std::string("[s]\na = ") + bad + "\n", "s", "a",
        GTEXT_INI_INTERPOLATION_EXTENDED, &out), GTEXT_INI_E_INTERPOLATION)
        << bad;
  }
}

/**
 * A reference resolves against the **joined** value, not the raw span.
 *
 * The one rule a reading of the tree alone gets wrong, and it is wrong on every
 * multi-line value: `read()` stores `'\n'.join(val)`, so the text substituted has
 * had its continuation indentation removed. Substituting what the tree holds would
 * put the leading spaces of every continuation line into the result.
 */
TEST(IniInterpolation, AReferenceResolvesToTheJoinedValue) {
  EXPECT_EQ(resolved("[s]\nalpha = one\n  two\na = %(alpha)s\n", "a",
      GTEXT_INI_INTERPOLATION_BASIC), "one\ntwo");
  EXPECT_EQ(resolved("[s]\nalpha = one\n      deep\na = ${alpha}\n", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED), "one\ndeep");
  EXPECT_EQ(resolved("[s]\na = ${o:m}\n[o]\nm = a\n  b\n  c\n", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED), "a\nb\nc");
  /* The raw span is still there to be read the wrong way, so this is the
   * assertion that nothing reads it that way. */
  GTEXT_INI_Document * d = cp_parse("[s]\nalpha = one\n  two\n");
  ASSERT_NE(d, nullptr);
  size_t len = 0;
  const char * raw =
      gtext_ini_group_get(gtext_ini_document_group(d, "s"), "alpha", &len);
  EXPECT_EQ(std::string(raw, len), "one\n  two") << "the span keeps the indent";
  gtext_ini_free(d);
}

/**
 * A substituted value is itself interpolated, and under Extended **in the section
 * it came from**.
 *
 * Continuing in the section being read would resolve the next hop against the
 * wrong map, which is invisible until a chain crosses a section and the two
 * sections both have the key. So the test gives them both one, with different
 * values: the pair is what discriminates, and either section alone would pass
 * whichever way the code went.
 */
TEST(IniInterpolation, NestingResolvesInTheReferredSection) {
  EXPECT_EQ(resolved("[s]\nleaf = end\nmid = %(leaf)s\na = %(mid)s\n", "a",
      GTEXT_INI_INTERPOLATION_BASIC), "end");
  EXPECT_EQ(resolved(
      "[s]\nleaf = wrong\na = ${o:mid}\n[o]\nmid = ${leaf}\nleaf = right\n",
      "a", GTEXT_INI_INTERPOLATION_EXTENDED), "right");

  /* The depth limit is a cap and not a cycle detector, so a cycle and a chain of
   * eleven stop the same way. Both are the document being wrong. */
  std::string out;
  EXPECT_EQ(interp("[s]\na = %(b)s\nb = %(a)s\n", "s", "a",
      GTEXT_INI_INTERPOLATION_BASIC, &out), GTEXT_INI_E_INTERPOLATION);
  EXPECT_EQ(interp("[s]\na = ${b}\nb = ${a}\n", "s", "a",
      GTEXT_INI_INTERPOLATION_EXTENDED, &out), GTEXT_INI_E_INTERPOLATION);
}

/**
 * `max_depth` is a cap a caller can move, and it bounds the chain rather than the
 * value.
 *
 * Asserted as an **invariance plus a bound**: the same document resolves at a
 * depth that admits it and refuses at one that does not, so the field is shown to
 * be read rather than merely present. A test that only checked the default would
 * pass with the field ignored entirely.
 */
TEST(IniInterpolation, MaxDepthBoundsTheChain) {
  /*
   * `a = %(c)s` -> `c = %(b)s` -> `b = %(leaf)s` -> `leaf = end`, which is
   * **three** frames and not four: the last substitution puts in a value with no
   * `%` in it, and that one does not recurse. The boundary was taken from the
   * reference rather than counted here - moving CPython's own
   * `MAX_INTERPOLATION_DEPTH` over this same document refuses at 1 and 2 and
   * resolves at 3. Counting it by reading the code gave 4, which is exactly the
   * kind of off-by-one a boundary asserted against a second implementation
   * catches and one asserted against the default alone does not.
   */
  const std::string doc =
      "[s]\nleaf = end\nb = %(leaf)s\nc = %(b)s\na = %(c)s\n";
  GTEXT_INI_Document * d = cp_parse(doc);
  ASSERT_NE(d, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(d, "s");
  ASSERT_NE(g, nullptr);
  size_t len = 0;
  const char * raw = gtext_ini_group_get(g, "a", &len);
  ASSERT_NE(raw, nullptr);

  for (unsigned depth = 1; depth <= 5; depth++) {
    GTEXT_INI_Interpolate_Options opts = gtext_ini_interpolate_options_default();
    opts.style = GTEXT_INI_INTERPOLATION_BASIC;
    opts.max_depth = depth;
    char * out = nullptr;
    size_t out_len = 0;
    GTEXT_INI_Status status =
        gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len);
    if (depth >= 3) {
      EXPECT_EQ(status, GTEXT_INI_OK) << "depth " << depth;
      if (status == GTEXT_INI_OK) {
        EXPECT_EQ(std::string(out, out_len), "end");
      }
    }
    else {
      EXPECT_EQ(status, GTEXT_INI_E_INTERPOLATION) << "depth " << depth;
    }
    gtext_ini_string_free(nullptr, out);
  }
  /* 0 means the reference's own limit, which admits this chain. */
  GTEXT_INI_Interpolate_Options zero = gtext_ini_interpolate_options_default();
  zero.style = GTEXT_INI_INTERPOLATION_BASIC;
  zero.max_depth = 0;
  char * out = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(gtext_ini_value_interpolate(g, &zero, raw, len, &out, &out_len),
      GTEXT_INI_OK);
  gtext_ini_string_free(nullptr, out);
  gtext_ini_free(d);
}

/**
 * `defaults` is `[DEFAULT]`, and it is consulted **second and only for a
 * reference**.
 *
 * A lookup policy over a parsed tree rather than a rule of the grammar, which is
 * the same reason the oracle pins `default_section` to a name no document can
 * spell. Three claims, and the third is the one that would otherwise leak: the
 * defaults group does not make its keys visible to gtext_ini_group_get().
 */
TEST(IniInterpolation, DefaultsAreConsultedSecond) {
  const std::string doc =
      "[DEFAULT]\nshared = fallback\nboth = from-default\n"
      "[s]\nboth = from-section\na = %(shared)s\nb = %(both)s\n";
  GTEXT_INI_Document * d = cp_parse(doc);
  ASSERT_NE(d, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(d, "s");
  const GTEXT_INI_Group * def = gtext_ini_document_group(d, "DEFAULT");
  ASSERT_NE(g, nullptr);
  ASSERT_NE(def, nullptr);

  GTEXT_INI_Interpolate_Options opts = gtext_ini_interpolate_options_default();
  opts.style = GTEXT_INI_INTERPOLATION_BASIC;
  opts.defaults = def;
  for (const std::pair<const char *, const char *> & want :
      {std::make_pair("a", "fallback"), std::make_pair("b", "from-section")}) {
    size_t len = 0;
    const char * raw = gtext_ini_group_get(g, want.first, &len);
    ASSERT_NE(raw, nullptr) << want.first;
    char * out = nullptr;
    size_t out_len = 0;
    ASSERT_EQ(gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len),
        GTEXT_INI_OK) << want.first;
    EXPECT_EQ(std::string(out, out_len), want.second) << want.first;
    gtext_ini_string_free(nullptr, out);
  }

  /* Without it, the same reference is missing rather than resolving. */
  opts.defaults = nullptr;
  size_t len = 0;
  const char * raw = gtext_ini_group_get(g, "a", &len);
  char * out = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len),
      GTEXT_INI_E_INTERPOLATION_MISSING);
  gtext_ini_string_free(nullptr, out);

  /* And it is not a second place to find a key: the tree is unchanged. */
  EXPECT_EQ(gtext_ini_group_get(g, "shared", nullptr), nullptr)
      << "defaults must not make a key visible to an ordinary lookup";
  gtext_ini_free(d);
}

/**
 * The detector says "not usable raw", not "holds a reference".
 *
 * `100%` holds no reference and is exactly what Basic refuses, so a predicate that
 * answered false for it would be silent on the 301 documents that matter most. The
 * useful question is whether interpolating could return anything other than the
 * input, and false is the load-bearing answer.
 */
TEST(IniInterpolation, TheDetectorAnswersAboutTheTriggerByte) {
  struct Case {
    const char * raw;
    bool basic;
    bool extended;
  };
  const Case cases[] = {
      {"plain", false, false},
      {"100%", true, false},
      {"a%%b", true, false},
      {"%(k)s", true, false},
      {"${k}", false, true},
      {"$$", false, true},
      {"a$b", false, true},
      {"%(a)s and ${b}", true, true},
      {"", false, false},
  };
  for (const Case & c : cases) {
    const size_t len = std::strlen(c.raw);
    EXPECT_EQ(gtext_ini_value_needs_interpolation(GTEXT_INI_INTERPOLATION_BASIC,
        c.raw, len), c.basic) << c.raw;
    EXPECT_EQ(gtext_ini_value_needs_interpolation(
        GTEXT_INI_INTERPOLATION_EXTENDED, c.raw, len), c.extended) << c.raw;
    /* NONE resolves nothing, so nothing ever needs it. */
    EXPECT_FALSE(gtext_ini_value_needs_interpolation(
        GTEXT_INI_INTERPOLATION_NONE, c.raw, len)) << c.raw;
  }
  /* A NUL is data here as everywhere, so the length decides and not a
   * terminator. */
  EXPECT_TRUE(gtext_ini_value_needs_interpolation(GTEXT_INI_INTERPOLATION_BASIC,
      "a\0%", 3));
  EXPECT_FALSE(gtext_ini_value_needs_interpolation(
      GTEXT_INI_INTERPOLATION_BASIC, "a\0%", 2));
}

/** A NUL in a value survives the pass, both around a reference and inside one. */
TEST(IniInterpolation, ANulIsData) {
  GTEXT_INI_Document * d = cp_parse(std::string("[s]\nk = a\0b\nA = %(k)s!\n", 23));
  ASSERT_NE(d, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(d, "s");
  ASSERT_NE(g, nullptr);
  size_t len = 0;
  const char * raw = gtext_ini_group_get(g, "a", &len);
  ASSERT_NE(raw, nullptr);
  GTEXT_INI_Interpolate_Options opts = gtext_ini_interpolate_options_default();
  opts.style = GTEXT_INI_INTERPOLATION_BASIC;
  char * out = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len),
      GTEXT_INI_OK);
  EXPECT_EQ(std::string(out, out_len), std::string("a\0b!", 4));
  EXPECT_EQ(out_len, 4u) << "the length is authoritative, not the terminator";
  gtext_ini_string_free(nullptr, out);
  gtext_ini_free(d);
}

/** Parameters a caller can get wrong. */
TEST(IniInterpolation, RefusesUnusableArguments) {
  GTEXT_INI_Interpolate_Options opts = gtext_ini_interpolate_options_default();
  opts.style = GTEXT_INI_INTERPOLATION_BASIC;
  char * out = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(gtext_ini_value_interpolate(nullptr, &opts, nullptr, 4, &out,
      &out_len), GTEXT_INI_E_INVALID);
  EXPECT_EQ(gtext_ini_value_interpolate(nullptr, &opts, "x", 1, nullptr,
      &out_len), GTEXT_INI_E_INVALID);
  /* An empty value is not an unusable one, and NULL with a zero length is how a
   * valueless key arrives. */
  EXPECT_EQ(gtext_ini_value_interpolate(nullptr, &opts, nullptr, 0, &out,
      &out_len), GTEXT_INI_OK);
  EXPECT_EQ(out_len, 0u);
  gtext_ini_string_free(nullptr, out);
  /* A style outside the enum, which a caller can produce from a cast or from a
   * value read out of its own configuration. */
  opts.style = (GTEXT_INI_Interpolation) 99;
  out = nullptr;
  EXPECT_EQ(gtext_ini_value_interpolate(nullptr, &opts, "x", 1, &out, &out_len),
      GTEXT_INI_E_INVALID);
  EXPECT_EQ(out, nullptr);
}

/**
 * No group is not an error, and it is not a way to resolve a reference either.
 *
 * A caller interpolating a value it assembled itself has no group to give, and
 * the honest answer for a reference is then that the key is not there - the
 * alternative would be to refuse the whole call, which would also refuse a value
 * that holds no reference at all.
 */
TEST(IniInterpolation, WithoutAGroupOnlyLiteralsResolve) {
  GTEXT_INI_Interpolate_Options opts = gtext_ini_interpolate_options_default();
  opts.style = GTEXT_INI_INTERPOLATION_BASIC;
  char * out = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(gtext_ini_value_interpolate(nullptr, &opts, "a%%b", 4, &out,
      &out_len), GTEXT_INI_OK);
  EXPECT_EQ(std::string(out, out_len), "a%b");
  gtext_ini_string_free(nullptr, out);

  out = nullptr;
  EXPECT_EQ(gtext_ini_value_interpolate(nullptr, &opts, "%(k)s", 5, &out,
      &out_len), GTEXT_INI_E_INTERPOLATION_MISSING);
  opts.style = GTEXT_INI_INTERPOLATION_EXTENDED;
  out = nullptr;
  EXPECT_EQ(gtext_ini_value_interpolate(nullptr, &opts, "${o:k}", 6, &out,
      &out_len), GTEXT_INI_E_INTERPOLATION_MISSING);
}

/**
 * The one departure from the reference, and it is a bound the reference lacks.
 *
 * `max_depth` caps how deep a chain goes and not how large it gets: each hop here
 * doubles, so ten hops is a thousandfold. `configparser` has no bound at all and
 * wears it, being Python; a C library that copied that would ship a documented
 * amplification. So the default refuses, and a caller who wants the reference's
 * behaviour back says so.
 *
 * **The amplification is exhibited first and bounded second.** A test that only
 * asserted the refusal would pass just as well if the expansion were broken and
 * never grew at all, which is the mutation this ordering catches.
 */
TEST(IniInterpolation, TheOutputIsBounded) {
  /* Six doublings from a two-byte leaf: 2 -> 4 -> 8 -> ... -> 128. */
  std::string doc = "[s]\nl0 = ab\n";
  for (int i = 1; i <= 6; i++) {
    doc += "l" + std::to_string(i) + " = ${l" + std::to_string(i - 1) +
           "}${l" + std::to_string(i - 1) + "}\n";
  }
  doc += "a = ${l6}\n";
  GTEXT_INI_Document * d = cp_parse(doc);
  ASSERT_NE(d, nullptr);
  const GTEXT_INI_Group * g = gtext_ini_document_group(d, "s");
  ASSERT_NE(g, nullptr);
  size_t len = 0;
  const char * raw = gtext_ini_group_get(g, "a", &len);
  ASSERT_NE(raw, nullptr);

  GTEXT_INI_Interpolate_Options opts = gtext_ini_interpolate_options_default();
  opts.style = GTEXT_INI_INTERPOLATION_EXTENDED;
  opts.max_output = (size_t) -1;  /* the reference's own behaviour */
  char * out = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len),
      GTEXT_INI_OK);
  EXPECT_EQ(out_len, 128u) << "the expansion must actually amplify, or the "
                              "bound below is asserting nothing";
  EXPECT_GT(out_len, len * 16) << "and by more than the default factor";
  gtext_ini_string_free(nullptr, out);

  /* Now the bound. Six bytes is below every intermediate result. */
  opts.max_output = 6;
  out = nullptr;
  EXPECT_EQ(gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len),
      GTEXT_INI_E_LIMIT);
  EXPECT_EQ(out, nullptr);

  /* A bound that admits it exactly, so the comparison is not off by one. */
  opts.max_output = 128;
  out = nullptr;
  EXPECT_EQ(gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len),
      GTEXT_INI_OK);
  EXPECT_EQ(out_len, 128u);
  gtext_ini_string_free(nullptr, out);
  opts.max_output = 127;
  out = nullptr;
  EXPECT_EQ(gtext_ini_value_interpolate(g, &opts, raw, len, &out, &out_len),
      GTEXT_INI_E_LIMIT);

  /*
   * And the default, which is proportional: `${l6}` is six bytes, so the floor of
   * 64 KiB is what applies and 128 fits well inside it. The assertion that makes
   * the default *mean* something is not this one but the pair above - this only
   * says the default is not so tight that an ordinary document trips it.
   */
  GTEXT_INI_Interpolate_Options def = gtext_ini_interpolate_options_default();
  def.style = GTEXT_INI_INTERPOLATION_EXTENDED;
  EXPECT_EQ(def.max_output, 0u) << "0 is the proportional default, not no limit";
  out = nullptr;
  EXPECT_EQ(gtext_ini_value_interpolate(g, &def, raw, len, &out, &out_len),
      GTEXT_INI_OK);
  EXPECT_EQ(out_len, 128u);
  gtext_ini_string_free(nullptr, out);
  gtext_ini_free(d);
}
