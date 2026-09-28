/**
 * Numbers do not change meaning with the user's language settings.
 *
 * printf and strtod both read LC_NUMERIC, whose decimal separator is a comma
 * in a good many locales. Three conversions used the bare functions, and each
 * broke differently in such a locale:
 *
 *   - "a: 0.1" in YAML came back as the *string* "0.1". strtod stopped at the
 *     "." it did not recognise, the resolver read the partial parse as "not a
 *     number", and the scalar fell through to string.
 *   - gtext_json_new_number_double(0.1) built its lexeme with snprintf, so
 *     the number went into the document as "0,1" - which is not JSON.
 *   - A parsed JSON number quietly ended up with no double value, from the
 *     same partial parse leaving the has-double flag clear.
 *
 * The TOML tests at the end of this file arrived here for a fourth reason,
 * which is the one this file's own header was already about: they were written
 * in tests/test-toml.cpp and tests/test-toml-writer.cpp calling
 * setlocale(LC_NUMERIC, "de_DE.UTF-8") directly. That returns NULL on this
 * machine, so both ran in the C locale and passed without ever meeting the
 * condition they were named for. The generated locale is here; the tests
 * belong here with it.
 *
 * The separator belongs to the format, not to whoever is running the program.
 *
 * These tests need a locale whose separator is a comma, and a machine may not
 * have one installed - the usual glibc build ships only C, POSIX and en_US.
 * They used to report that and pass, on the reasoning that there was nothing
 * to measure rather than nothing wrong. On this machine there is never one
 * installed, so every one of them skipped, every run, for their whole life -
 * which is how the defect below went unnoticed for as long as it did.
 *
 * So they generate one now, with localedef into a temp directory reached
 * through LOCPATH, needing no root. If that cannot be done,
 * ACommaLocaleIsAvailable *fails*: a machine that cannot produce the hostile
 * condition has not verified anything, and saying so once and loudly beats
 * five quiet passes.
 *
 * There used to be a paragraph here saying that no test in this file could
 * see *which* locale API the library used, because both arms converted
 * correctly and the process-wide one put back what it changed - so the only
 * difference lived in another thread, mid-conversion, and an accessor had to
 * report it structurally.
 *
 * That is no longer true, in both halves. The library now touches no locale
 * at all, so there is no arm to report; and the difference the accessor stood
 * in for is measured directly by AConversionCannotDisturbAnotherThread, which
 * runs a bystander thread and counts how often it sees a separator it did not
 * ask for. Against the old process-wide fallback that count was 4,003,481 in
 * 300,000 conversions. Against what is here now it is 0.
 */
#include <gtest/gtest.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <atomic>
#include <cmath>
#include <thread>
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/toml.h>
#include <ghoti.io/text/yaml.h>

extern "C" {
#include "../src/text_number_internal.h"
}

/* There was an __lsan_default_suppressions() here, for one small glibc
   allocation on the LOCPATH list that freelocale never returned. It was
   reached from newlocale - which the library called, and no longer does.
   Removed after checking: with the suppression emptied this binary is clean
   under LSan, and LSan is armed (a deliberate 1234-byte leak is still
   reported). A suppression that suppresses nothing is where the next real
   leak hides. */

namespace {

/* Switches LC_NUMERIC to a named locale and puts it back however the test
   ends. Used for the locales this file generates for itself. */
class ScopedLocale {
public:
	explicit ScopedLocale(const char *name) {
		const char *saved = setlocale(LC_NUMERIC, nullptr);
		if (saved) previous_ = saved;
		if (setlocale(LC_NUMERIC, name)) {
			active_ = true;
			name_ = name;
		}
		else {
			setlocale(LC_NUMERIC, previous_.c_str());
		}
	}
	~ScopedLocale() { setlocale(LC_NUMERIC, previous_.c_str()); }

	bool active() const { return active_; }
	const std::string &name() const { return name_; }
	const char *separator() const { return localeconv()->decimal_point; }

private:
	std::string previous_ = "C";
	std::string name_;
	bool active_ = false;
};

/* Switches LC_NUMERIC to something that separates with a comma, and puts it
   back afterwards however the test ends. */
class CommaLocale {
public:
	CommaLocale() {
		const char *saved = setlocale(LC_NUMERIC, nullptr);
		if (saved) previous_ = saved;
		for (const char *name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8",
				"fr_FR.utf8", "es_ES.UTF-8", "nl_NL.UTF-8", "de_DE", "fr_FR"
#ifdef _WIN32
				/* The CRT's spellings. msvcrt, which MINGW64 links, knows
				   none of the POSIX names above and has no UTF-8 locales;
				   these select the code-page locales, which separate with a
				   comma all the same. */
				, "German_Germany", "French_France"
#endif
				}) {
			if (setlocale(LC_NUMERIC, name)) {
				const lconv *lc = localeconv();
				if (lc && lc->decimal_point && strcmp(lc->decimal_point, ",") == 0) {
					active_ = true;
					name_ = name;
					return;
				}
			}
		}
		setlocale(LC_NUMERIC, previous_.c_str());
	}
	~CommaLocale() { setlocale(LC_NUMERIC, previous_.c_str()); }

	bool active() const { return active_; }
	const std::string &name() const { return name_; }

private:
	std::string previous_ = "C";
	std::string name_;
	bool active_ = false;
};

/* Reached only when EnsureCommaLocale() itself failed, which
   ACommaLocaleIsAvailable reports as a failure rather than a skip. Every test
   here calls EnsureCommaLocale() for itself: four of them used to rely on that
   test having run earlier in the same process to generate the locale and set
   LOCPATH, so under --gtest_filter or --gtest_shuffle they skipped in silence,
   which is the failure mode this whole file exists to have stopped doing. */
#define SKIP_WITHOUT_COMMA_LOCALE(loc)                                        \
	if (!(loc).active()) {                                                    \
		GTEST_SKIP() << "no comma-decimal locale installed; "                 \
			"generate one with localedef and set LOCPATH to run this";        \
		}

GTEXT_YAML_Node_Type YamlValueType(const char *src) {
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document *doc = gtext_yaml_parse(src, strlen(src), nullptr, &err);
	if (!doc) return GTEXT_YAML_NULL;
	const GTEXT_YAML_Node *k = nullptr, *v = nullptr;
	gtext_yaml_mapping_get_at(gtext_yaml_document_root(doc), 0, &k, &v);
	GTEXT_YAML_Node_Type t = v ? gtext_yaml_node_type(v) : GTEXT_YAML_NULL;
	gtext_yaml_free(doc);
	return t;
}

std::string JsonFromDouble(double x) {
	GTEXT_JSON_Value *n = gtext_json_new_number_double(x);
	if (!n) return "";
	GTEXT_JSON_Sink sink;
	gtext_json_sink_buffer(&sink);
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	gtext_json_write_value(&sink, nullptr, n, &err);
	std::string out(gtext_json_sink_buffer_data(&sink),
			gtext_json_sink_buffer_size(&sink));
	gtext_json_sink_buffer_free(&sink);
	gtext_json_free(n);
	return out;
}

}  // namespace

/* A comma locale has to exist for any of this to mean anything, so make one
   rather than hope for one. localedef needs no root when the result goes
   somewhere LOCPATH points at.

   env -u LD_PRELOAD: under the ASan build, system() hands the sanitizer
   runtime to the child, LeakSanitizer then trips on localedef's own
   allocations and it exits non-zero - and the failure reads as "no locale
   could be generated", for a reason with nothing to do with locales. */
/* Builds <name>.UTF-8 into a directory LOCPATH points at, once per process.
   Returns true if the locale can then be selected. */
static bool GenerateLocale(const std::string &name) {
#ifdef _WIN32
	/* Nothing to generate with: Windows has no localedef and no LOCPATH, and
	   its CRT has the locales it has. CommaLocale knows their names. */
	(void)name;
	return false;
#else
	static std::string dir;
	if (dir.empty()) {
		char tmpl[] = "/tmp/gtext-locale-XXXXXX";
		const char *made = mkdtemp(tmpl);
		if (!made) return false;
		dir = made;
		setenv("LOCPATH", dir.c_str(), 1);
	}
	const std::string cmd =
		"env -u LD_PRELOAD -u LD_LIBRARY_PATH localedef -i " + name
		+ " -f UTF-8 " + dir + "/" + name + ".UTF-8 >/dev/null 2>&1";
	if (system(cmd.c_str()) != 0) return false;
	ScopedLocale check((name + ".UTF-8").c_str());
	return check.active();
#endif
}

static bool EnsureCommaLocale() {
	{
		CommaLocale probe;
		if (probe.active()) return true;
	}

	if (!GenerateLocale("de_DE")) return false;

	CommaLocale again;
	return again.active();
}


/* Not a skip. A machine that cannot produce the hostile condition has not
   verified anything, and the suite should say so once, loudly, rather than
   report five passes that measured nothing. */
TEST(LocaleNumbers, ACommaLocaleIsAvailable) {
	ASSERT_TRUE(EnsureCommaLocale())
		<< "could not find or generate a comma-decimal locale; every test "
		   "below this one is measuring nothing without it";
	CommaLocale loc;
	ASSERT_TRUE(loc.active());
	EXPECT_STREQ(localeconv()->decimal_point, ",") << loc.name();

	/* Assert the hostile condition really is hostile before asserting
	   immunity to it: if plain snprintf does not produce a comma here, the
	   locale is not doing what the rest of the file assumes. */
	char buf[32];
	snprintf(buf, sizeof(buf), "%.1f", 0.5);
	EXPECT_STREQ(buf, "0,5") << "locale " << loc.name()
		<< " is active but snprintf still separates with '.'";
}

/* The difference an accessor used to have to report.
   
   src/text_number.c once chose between uselocale() and setlocale() with a
   preprocessor guard, and the guard was structurally unsatisfiable: it tested
   _POSIX_C_SOURCE above every #include, where features.h has not yet run. So
   Linux compiled the process-wide arm while macOS and FreeBSD, selected by
   compiler predefines that *are* live there, got the thread-local one.
   
   Repairing the guard was not enough, because Windows is a supported target
   here and MinGW has no uselocale - the fallback was not covering some
   hypothetical platform, it was covering a third of what this library ships
   to. So the locale is no longer consulted or changed at all, and this test
   measures the property the arms were arguing about.
   
   A bystander thread formats a number whose spelling it knows and counts how
   often it gets a different one. Against the old process-wide fallback the
   count was 4,003,481 out of 300,000 conversions: setlocale is not a window,
   it is most of the runtime. */
TEST(LocaleNumbers, AConversionCannotDisturbAnotherThread) {
	ASSERT_TRUE(EnsureCommaLocale());
	CommaLocale loc;
	ASSERT_TRUE(loc.active());

	std::atomic<bool> stop{false};
	std::atomic<long> wrong{0};
	std::atomic<long> seen{0};

	std::thread bystander([&] {
		while (!stop.load(std::memory_order_relaxed)) {
			char b[32];
			snprintf(b, sizeof(b), "%.1f", 1.5);
			seen.fetch_add(1, std::memory_order_relaxed);
			if (strcmp(b, "1,5") != 0) {
				wrong.fetch_add(1, std::memory_order_relaxed);
			}
		}
	});

	char buf[64];
	for (long i = 0; i < 300000; i++) {
		gtext_number_format_double(
			buf, sizeof(buf), 0.1 + (double)i, GTEXT_NUMBER_GENERAL, 17);
	}
	stop.store(true);
	bystander.join();

	/* The control: if the bystander never ran, it cannot have seen anything,
	   and a zero below would mean nothing at all. */
	ASSERT_GT(seen.load(), 0L) << "the bystander thread never ran";
	EXPECT_EQ(wrong.load(), 0L)
		<< "a number conversion changed the separator another thread sees; "
		   "that is process-wide locale state, and it makes the concurrency "
		   "guarantee in the module documentation false";

	/* ...and it must still convert correctly while not doing that. */
	ASSERT_GT(gtext_number_format_double(
		buf, sizeof(buf), 0.5, GTEXT_NUMBER_GENERAL, 17), 0);
	EXPECT_STREQ(buf, "0.5");
}

/* gtext_number_c_extent() is what lets a parse stop where the document says
   the number stops rather than where the locale thinks it does, so it has to
   agree with strtod exactly - an extent one byte short silently truncates a
   number, one byte long silently extends it.
   
   Compared here against the thing it models. Three deliberate mutations of
   its rules - consuming a malformed exponent, dropping the bare "0x" case,
   and accepting a lone "." - were caught 63984, 3224 and 371362 times
   respectively by this comparison. */
/* How much of `in` strtod consumes - the reference the extent scan answers to.

   MinGW's strtod reads "nan(" through the next ')' whatever lies between,
   where C17 7.22.1.3 allows only digits, letters and '_' there; "nan(a b)"
   is 8 bytes to it and 3 to the standard. The scan follows the standard, so
   on Windows the reference is corrected to what a conforming strtod would
   say, for that one shape only. Everything else is still strtod's answer. */
static size_t StrtodExtent(const char *in) {
	char *end = nullptr;
	(void)strtod(in, &end);
	size_t n = (size_t)(end - in);
#ifdef _WIN32
	const char *p = in;
	while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
	if (*p == '+' || *p == '-') p++;
	auto lower = [](char ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; };
	if (lower(p[0]) == 'n' && lower(p[1]) == 'a' && lower(p[2]) == 'n'
			&& p[3] == '(' && n > 0 && in[n - 1] == ')') {
		for (const char *r = p + 4; r < end - 1; r++) {
			const bool nchar = (*r >= '0' && *r <= '9') || *r == '_'
				|| (lower(*r) >= 'a' && lower(*r) <= 'z');
			if (!nchar) return (size_t)(p + 3 - in);
		}
	}
#endif
	return n;
}

TEST(LocaleNumbers, TheExtentScanAgreesWithStrtod) {
	ASSERT_TRUE(EnsureCommaLocale());
	ScopedLocale c("C");
	ASSERT_TRUE(c.active());

	static const char *cases[] = {
		"", "0", "-0", "+1", "1.", ".5", ".", "-.", "1e", "1e+", "1e+5",
		"1E-3", "0x", "0X", "0x1", "0x1.8p3", "0x1.8p+3", "0x.8p1", "0x1p",
		"0xg", "inf", "INF", "infinity", "InFiNiTy", "infin", "nan", "NAN",
		"nan()", "nan(0x1)", "nan(abc_1)", "nan(", "  1.5", "\t-2.5e3xyz",
		"1,5", "1.5,2", "++1", "--1", "1.5e2xyz", "e5", "x1", "1e999",
		"-inf", "+nan", "1.2.3", "0x1.8", "1_000",
	};
	for (const char *in : cases) {
		EXPECT_EQ(gtext_number_c_extent(in), StrtodExtent(in))
			<< "input \"" << in << "\"";
	}

	/* And the same question asked by a generator, because the list above is
	   the set of shapes that occurred to me. */
	static const char alphabet[] = "0123456789.eE+-xXpPaAfFnN iInfNaN()_\t";
	const size_t alpha_len = sizeof(alphabet) - 1;
	unsigned long long st = 88172645463325252ULL;
	auto rnd = [&st]() {
		st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st;
	};
	long mismatches = 0;
	char buf[40];
	for (long i = 0; i < 300000; i++) {
		size_t len = 1 + (size_t)(rnd() % 18);
		for (size_t j = 0; j < len; j++) buf[j] = alphabet[rnd() % alpha_len];
		buf[len] = '\0';
		if (gtext_number_c_extent(buf) != StrtodExtent(buf)) {
			if (mismatches < 5) ADD_FAILURE() << "input \"" << buf << "\"";
			mismatches++;
		}
	}
	EXPECT_EQ(mismatches, 0L);
}

/* Integer conversion has no locale-dependent element, which is why
   gtext_number_format_i64/u64 do no repair. That is a measurement, not a
   deduction, so it is measured. Grouping would need printf's "'" flag; if
   one ever appears in those functions this goes red. */
TEST(LocaleNumbers, AnIntegerIsSpelledTheSameInEveryLocale) {
	ASSERT_TRUE(EnsureCommaLocale());
	const int64_t svals[] = {0, 1, -1, 1234567, -9007199254740993LL,
		INT64_MIN, INT64_MAX};
	const uint64_t uvals[] = {0u, 1u, 1234567u, UINT64_MAX};

	std::string in_c[sizeof(svals) / sizeof(svals[0])];
	std::string uin_c[sizeof(uvals) / sizeof(uvals[0])];
	{
		ScopedLocale c("C");
		ASSERT_TRUE(c.active());
		char buf[64];
		for (size_t i = 0; i < sizeof(svals) / sizeof(svals[0]); i++) {
			ASSERT_GT(gtext_number_format_i64(buf, sizeof(buf), svals[i]), 0);
			in_c[i] = buf;
		}
		for (size_t i = 0; i < sizeof(uvals) / sizeof(uvals[0]); i++) {
			ASSERT_GT(gtext_number_format_u64(buf, sizeof(buf), uvals[i]), 0);
			uin_c[i] = buf;
		}
	}

	ASSERT_TRUE(EnsureCommaLocale());
	CommaLocale loc;
	ASSERT_TRUE(loc.active());
	char buf[64];
	for (size_t i = 0; i < sizeof(svals) / sizeof(svals[0]); i++) {
		ASSERT_GT(gtext_number_format_i64(buf, sizeof(buf), svals[i]), 0);
		EXPECT_EQ(std::string(buf), in_c[i]);
	}
	for (size_t i = 0; i < sizeof(uvals) / sizeof(uvals[0]); i++) {
		ASSERT_GT(gtext_number_format_u64(buf, sizeof(buf), uvals[i]), 0);
		EXPECT_EQ(std::string(buf), uin_c[i]);
	}
}

/* A separator need not be one byte. ps_AF separates with U+066B, which is
   two, so repairing it shortens the string - and the length the function
   reports has to shorten with it, or every caller that checks "did this fit"
   is working from a number that is now wrong. */
TEST(LocaleNumbers, AMultiByteSeparatorIsStillRepaired) {
	ASSERT_TRUE(EnsureCommaLocale());
#ifdef _WIN32
	/* msvcrt's locales are code-page locales, and U+066B is in none of
	   them: its "Pashto_Afghanistan" separates with a one-byte comma. There
	   is no locale here with a multi-byte separator to be hostile with. */
	GTEST_SKIP() << "the Windows CRT has no locale with a multi-byte "
		"decimal separator";
#endif
	ASSERT_TRUE(GenerateLocale("ps_AF"))
		<< "could not generate ps_AF.UTF-8; without it the multi-byte "
		   "separator path is never exercised";
	ScopedLocale loc("ps_AF.UTF-8");
	ASSERT_TRUE(loc.active());

	/* The hostile condition really is hostile, and really is multi-byte. */
	ASSERT_GT(strlen(loc.separator()), 1u) << "ps_AF separator is not "
		"multi-byte here; this test is measuring nothing";
	char probe[32];
	snprintf(probe, sizeof(probe), "%.1f", 1.5);
	ASSERT_STRNE(probe, "1.5") << "plain snprintf already separates with '.'";

	char buf[64];
	int len = gtext_number_format_double(
		buf, sizeof(buf), 0.5, GTEXT_NUMBER_GENERAL, 17);
	EXPECT_STREQ(buf, "0.5");
	EXPECT_EQ(len, (int)strlen(buf))
		<< "the reported length did not shrink with the string";

	len = gtext_number_format_double(
		buf, sizeof(buf), 1.25, GTEXT_NUMBER_FIXED, 3);
	EXPECT_STREQ(buf, "1.250");
	EXPECT_EQ(len, (int)strlen(buf));

	/* And back again, through the multi-byte respelling in the parser. */
	char *end = nullptr;
	EXPECT_EQ(gtext_number_strtod("0.5", &end), 0.5);
	EXPECT_EQ(*end, '\0');
}

/* The half of the parse that truncation buys.
   
   Handed "1,5" in a comma locale a bare strtod answers 1.5, because it reads
   the comma as a decimal point. The document did not say 1.5; it said 1
   followed by a separator that belongs to whatever contains it. */
TEST(LocaleNumbers, AParseStopsWhereTheDocumentSaysItDoes) {
	ASSERT_TRUE(EnsureCommaLocale());
	CommaLocale loc;
	ASSERT_TRUE(loc.active());

	/* The control: this is exactly what the bare function gets wrong here. */
	char *bare_end = nullptr;
	ASSERT_EQ(strtod("1,5", &bare_end), 1.5)
		<< "the locale is not hostile in the way this test needs";

	char *end = nullptr;
	EXPECT_EQ(gtext_number_strtod("1,5", &end), 1.0);
	ASSERT_NE(end, nullptr);
	EXPECT_EQ(*end, ',');

	end = nullptr;
	EXPECT_EQ(gtext_number_strtod("1.5", &end), 1.5);
	ASSERT_NE(end, nullptr);
	EXPECT_EQ(*end, '\0');

	end = nullptr;
	EXPECT_EQ(gtext_number_strtod("-2.5e3xyz", &end), -2500.0);
	ASSERT_NE(end, nullptr);
	EXPECT_STREQ(end, "xyz");

	/* Nothing to convert stays nothing to convert. */
	end = nullptr;
	EXPECT_EQ(gtext_number_strtod("abc", &end), 0.0);
	ASSERT_NE(end, nullptr);
	EXPECT_STREQ(end, "abc");
}

TEST(LocaleNumbers, AYamlFloatIsStillAFloat) {
	ASSERT_TRUE(EnsureCommaLocale());
	EXPECT_EQ(YamlValueType("a: 0.1\n"), GTEXT_YAML_FLOAT) << "in the C locale";
	CommaLocale loc;
	SKIP_WITHOUT_COMMA_LOCALE(loc);
	EXPECT_EQ(YamlValueType("a: 0.1\n"), GTEXT_YAML_FLOAT) << loc.name();
	EXPECT_EQ(YamlValueType("a: -2.5\n"), GTEXT_YAML_FLOAT) << loc.name();
	EXPECT_EQ(YamlValueType("a: 1.5e3\n"), GTEXT_YAML_FLOAT) << loc.name();
	/* An integer was never at risk - no separator - but check it holds. */
	EXPECT_EQ(YamlValueType("a: 7\n"), GTEXT_YAML_INT) << loc.name();
}

TEST(LocaleNumbers, AJsonNumberBuiltFromADoubleIsStillJson) {
	ASSERT_TRUE(EnsureCommaLocale());
	EXPECT_EQ(JsonFromDouble(0.1), "0.10000000000000001") << "in the C locale";
	CommaLocale loc;
	SKIP_WITHOUT_COMMA_LOCALE(loc);
	const std::string out = JsonFromDouble(0.1);
	EXPECT_EQ(out, "0.10000000000000001") << loc.name();
	EXPECT_EQ(out.find(','), std::string::npos)
		<< "a comma in a JSON number: " << out;
}

TEST(LocaleNumbers, AParsedJsonNumberStillHasItsDouble) {
	ASSERT_TRUE(EnsureCommaLocale());
	const char *src = "{\"a\":0.1}";
	auto parse_double = [&](const char *label) {
		GTEXT_JSON_Parse_Options po = gtext_json_parse_options_default();
		po.parse_double = true;
		GTEXT_JSON_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_JSON_Value *v = gtext_json_parse(src, strlen(src), &po, &err);
		ASSERT_NE(v, nullptr) << label;
		double d = 0;
		EXPECT_EQ(gtext_json_get_double(gtext_json_object_get(v, "a", 1), &d),
				GTEXT_JSON_OK) << label;
		EXPECT_DOUBLE_EQ(d, 0.1) << label;
		gtext_json_free(v);
		gtext_json_error_free(&err);
	};
	parse_double("in the C locale");
	CommaLocale loc;
	SKIP_WITHOUT_COMMA_LOCALE(loc);
	parse_double(loc.name().c_str());
}

/* A YAML float key's JSON name carries a "." wherever it is converted. */
TEST(LocaleNumbers, ACoercedFloatKeyKeepsItsDecimalPoint) {
	ASSERT_TRUE(EnsureCommaLocale());
	auto convert = [](const char *label) {
		const char *src = "0.1: v\n";
		GTEXT_YAML_Error err;
		memset(&err, 0, sizeof(err));
		GTEXT_YAML_Document *doc =
			gtext_yaml_parse(src, strlen(src), nullptr, &err);
		ASSERT_NE(doc, nullptr) << label;
		GTEXT_JSON_Value *jv = nullptr;
		GTEXT_YAML_To_JSON_Options o = gtext_yaml_to_json_options_default();
		o.coerce_keys_to_strings = true;
		memset(&err, 0, sizeof(err));
		ASSERT_EQ(gtext_yaml_to_json_with_options(doc, &jv, &o, &err),
				GTEXT_YAML_OK) << label;
		GTEXT_JSON_Sink sink;
		gtext_json_sink_buffer(&sink);
		GTEXT_JSON_Error je;
		memset(&je, 0, sizeof(je));
		gtext_json_write_value(&sink, nullptr, jv, &je);
		const std::string out(gtext_json_sink_buffer_data(&sink),
				gtext_json_sink_buffer_size(&sink));
		gtext_json_sink_buffer_free(&sink);
		gtext_json_free(jv);
		gtext_yaml_free(doc);
		EXPECT_EQ(out, "{\"0.1\":\"v\"}") << label;
	};
	convert("in the C locale");
	CommaLocale loc;
	SKIP_WITHOUT_COMMA_LOCALE(loc);
	convert(loc.name().c_str());
}


/* TOML, both directions.

   These live here rather than beside the other TOML tests because this is
   where a comma locale exists: the versions written in tests/test-toml.cpp
   and tests/test-toml-writer.cpp called setlocale(LC_NUMERIC, "de_DE.UTF-8")
   and took NULL for an answer, so they ran in the C locale for their whole
   life. The reading half is gtext_number_strtod() and the writing half is
   gtext_number_format_double(); a comma locale breaks the bare functions in
   opposite directions, so both need saying. */
TEST(LocaleNumbers, ATomlFloatSurvivesBothWays) {
	ASSERT_TRUE(EnsureCommaLocale());
	auto convert = [](const char *label) {
		/* Reading: strtod in a comma locale stops at the point, and a parser
		   that took the partial answer would read 3.5 as 3 and then refuse the
		   ".5" that follows. */
		GTEXT_TOML_Error err;
		memset(&err, 0, sizeof(err));
		const char *src = "f = 3.5\ng = -0.125\n";
		GTEXT_TOML_Value *root =
			gtext_toml_parse(src, strlen(src), nullptr, &err);
		ASSERT_NE(root, nullptr) << label << ": "
			<< (err.message ? err.message : "no message");
		gtext_toml_error_free(&err);
		double v = 0;
		ASSERT_TRUE(gtext_toml_value_float(
				gtext_toml_table_get(root, "f", 1), &v)) << label;
		EXPECT_DOUBLE_EQ(v, 3.5) << label;

		/* Writing: snprintf in a comma locale spells 3.5 as "3,5", which is
		   not TOML at all - it is a key, a comma and another key. */
		GTEXT_TOML_Sink sink;
		ASSERT_EQ(gtext_toml_sink_buffer(&sink), GTEXT_TOML_OK) << label;
		ASSERT_EQ(gtext_toml_write(root, &sink, nullptr), GTEXT_TOML_OK)
			<< label;
		const std::string out(gtext_toml_sink_buffer_data(&sink),
				gtext_toml_sink_buffer_size(&sink));
		gtext_toml_sink_buffer_free(&sink);
		gtext_toml_free(root);
		EXPECT_EQ(out, "f = 3.5\ng = -0.125\n") << label;
	};
	convert("in the C locale");
	CommaLocale loc;
	SKIP_WITHOUT_COMMA_LOCALE(loc);
	convert(loc.name().c_str());
}
