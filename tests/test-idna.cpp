/**
 * @file
 *
 * Internationalized host names, RFC 5890 to 5893.
 *
 * These go at the internal function rather than through JSON Schema's
 * `format`, because the rules being checked are IDNA's and not the schema
 * engine's, and a failure here should say so.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/enums.h>

#include "../src/idna/idna_internal.h"
#include "../src/idna/tables/tables_internal.h"
}

static bool host(const std::string & s) {
	return gtext_idna_hostname_valid(s.data(), s.size(), 0) != 0;
}

static bool idn(const std::string & s) {
	return gtext_idna_hostname_valid(s.data(), s.size(), 1) != 0;
}

TEST(Idna, AsciiHostNames) {
	EXPECT_TRUE(host("www.example.com"));
	EXPECT_TRUE(host("host-name"));
	EXPECT_TRUE(host("1host"));
	EXPECT_FALSE(host(""));
	EXPECT_FALSE(host("-hello"));
	EXPECT_FALSE(host("hello-"));
	EXPECT_FALSE(host("a..b"));
	// The rooted form is an empty root label, which is an empty label.
	EXPECT_FALSE(host("example.com."));
	EXPECT_FALSE(host(".example"));
	EXPECT_FALSE(host("not_an_underscore"));
	EXPECT_FALSE(host(std::string(64, 'a')));
	EXPECT_TRUE(host(std::string(63, 'a')));
}

TEST(Idna, ALabelsAreDecodedAndChecked) {
	// 2020-12 defines `hostname` to include Punycode-produced names, so an
	// A-label is decoded even in the ASCII-only mode. Accepting `xn--` and
	// any trailing base-36 is not checking the thing the keyword names.
	EXPECT_TRUE(host("xn--ihqwcrb4cv8a8dqg056pqjye"));
	EXPECT_TRUE(host("xn--nxasmq6b"));
	// The body is case-insensitive, and so is the prefix.
	EXPECT_TRUE(host("xn--NXASMQ6B"));
	EXPECT_FALSE(host("xn--X"));
	// RFC 5891 section 4.4 requires the round trip: this decodes to something
	// whose own encoding is different, so it was never an A-label.
	EXPECT_FALSE(host("xn---9uc"));
	// Decoding to pure ASCII means the label should not have been encoded.
	EXPECT_FALSE(host("xn--example-"));
	// A label that decodes to a disallowed codepoint is not a name.
	EXPECT_FALSE(host("xn--7a"));
	// Reserved by the ACE prefix: a hyphen in the third and fourth positions.
	EXPECT_FALSE(host("XN--aa---o47jg78q"));
}

TEST(Idna, UnicodeLabelsOnlyWhereAllowed) {
	EXPECT_TRUE(idn("\xEC\x8B\xA4\xEB\xA1\x80.\xED\x85\x8C\xEC\x8A\xA4\xED\x8A\xB8"));
	// The same name is not a `hostname`, which is ASCII.
	EXPECT_FALSE(host("\xEC\x8B\xA4\xEB\xA1\x80"));
	// U+302E HANGUL SINGLE DOT TONE MARK is a DISALLOWED exception.
	EXPECT_FALSE(idn("\xE3\x80\xAE\xEC\x8B\xA4\xEB\xA1\x80"));
	// Leading combining marks.
	EXPECT_FALSE(idn("\xE0\xA4\x83hello"));
	EXPECT_FALSE(idn("\xCC\x80hello"));
}

TEST(Idna, Rfc5892Exceptions) {
	// PVALID exceptions: sharp s, final sigma, tibetan tsheg, ideographic zero.
	EXPECT_TRUE(idn("\xC3\x9F\xCF\x82\xE0\xBC\x8B\xE3\x80\x87"));
	// DISALLOWED exceptions: tatweel and nko lajanyalan.
	EXPECT_FALSE(idn("\xD9\x80\xDF\xBA"));
}

TEST(Idna, ContextualRules) {
	// MIDDLE DOT needs an 'l' on both sides (rule A.3).
	EXPECT_TRUE(idn("l\xC2\xB7l"));
	EXPECT_FALSE(idn("a\xC2\xB7l"));
	EXPECT_FALSE(idn("l\xC2\xB7""a"));
	EXPECT_FALSE(idn("\xC2\xB7l"));
	EXPECT_FALSE(idn("l\xC2\xB7"));

	// GREEK KERAIA must be followed by Greek (rule A.4).
	EXPECT_TRUE(idn("\xCE\xB1\xCD\xB5\xCE\xB2"));
	EXPECT_FALSE(idn("\xCE\xB1\xCD\xB5S"));
	EXPECT_FALSE(idn("\xCE\xB1\xCD\xB5"));

	// HEBREW GERESH and GERSHAYIM must be preceded by Hebrew (A.5, A.6).
	EXPECT_TRUE(idn("\xD7\x90\xD7\xB3\xD7\x91"));
	EXPECT_FALSE(idn("A\xD7\xB3\xD7\x91"));
	EXPECT_TRUE(idn("\xD7\x90\xD7\xB4\xD7\x91"));
	EXPECT_FALSE(idn("A\xD7\xB4\xD7\x91"));

	// KATAKANA MIDDLE DOT needs Hiragana, Katakana or Han somewhere in the
	// label - anywhere, which is why it is the one rule that reads the whole
	// label rather than its neighbours (A.7).
	EXPECT_TRUE(idn("\xE3\x83\xBB\xE3\x81\x81"));
	EXPECT_TRUE(idn("\xE3\x83\xBB\xE4\xB8\x88"));
	EXPECT_FALSE(idn("def\xE3\x83\xBB""abc"));
	EXPECT_FALSE(idn("\xE3\x83\xBB"));

	// The two blocks of Arabic-Indic digits may not mix in one label, and may
	// appear in different labels of the same name (A.8, A.9).
	EXPECT_FALSE(idn("\xD8\xA8\xD9\xA0\xDB\xB0"));
	EXPECT_TRUE(idn("\xD8\xA8\xD9\xA0\xD8\xA8"));
	EXPECT_TRUE(idn("\xD8\xA8\xD9\xA0.\xD8\xA8\xDB\xB0"));
}

TEST(Idna, ZeroWidthJoinersNeedTheirContext) {
	// ZWJ must follow a virama (rule A.2).
	EXPECT_TRUE(idn("\xE0\xA4\x95\xE0\xA5\x8D\xE2\x80\x8D\xE0\xA4\xB7"));
	EXPECT_FALSE(idn("\xE0\xA4\x95\xE2\x80\x8D\xE0\xA4\xB7"));
	EXPECT_FALSE(idn("\xE2\x80\x8D\xE0\xA4\xB7"));

	// ZWNJ passes with a virama before it, or with the joining-type pattern
	// around it (rule A.1) - the second is what makes this Arabic name valid.
	EXPECT_TRUE(idn("\xE0\xA4\x95\xE0\xA5\x8D\xE2\x80\x8C\xE0\xA4\xB7"));
	EXPECT_TRUE(idn("\xD8\xA8\xD9\x8A\xE2\x80\x8C\xD8\xA8\xD9\x8A"));
	// Every occurrence has to pass, not just the first.
	EXPECT_FALSE(idn("\xE0\xA4\x95\xE0\xA5\x8D\xE2\x80\x8Cx\xE2\x80\x8Cy"));
}

TEST(Idna, TheBidiRuleIsAboutTheWholeName) {
	// One right-to-left label makes the name a bidi domain name, and then
	// every label is judged by RFC 5893 - including the ones that would have
	// been fine on their own.
	EXPECT_FALSE(idn("0a.\xD7\x90"));
	EXPECT_FALSE(idn("0\xD8\xA7"));
	EXPECT_FALSE(idn("a\xD7\x90"));
	// A right-to-left label may not mix the two kinds of digit.
	EXPECT_FALSE(idn("\xD7\x90""0\xD9\xA0"));
	// Without any right-to-left character the rule does not apply.
	EXPECT_TRUE(idn("0a.example"));
}

TEST(Idna, Lengths) {
	// The 63-octet limit is on the A-label form, so a U-label is measured by
	// what it would encode to rather than by its own length.
	// Punycode is compact for a repeated character, so the threshold is not
	// where the character count would suggest: 57 of these encode to exactly
	// 63 octets and 58 to 64.
	std::string u57;
	std::string u58;
	for (int i = 0; i < 58; i++) {
		// LATIN SMALL LETTER U WITH DIAERESIS
		if (i < 57) {
			u57 += "\xC3\xBC";
		}
		u58 += "\xC3\xBC";
	}
	EXPECT_TRUE(idn(u57));
	EXPECT_FALSE(idn(u58));

	std::string long_name;
	for (int i = 0; i < 5; i++) {
		long_name += std::string(63, 'a') + ".";
	}
	long_name += "a";
	EXPECT_FALSE(host(long_name));
}

TEST(Idna, LabelSeparators) {
	// UTS #46 section 4.5: three other stops separate labels, which the
	// mapping step delivers by turning them into FULL STOP - and only for a
	// name that admits Unicode at all, because a plain `hostname` is not
	// mapped.
	EXPECT_TRUE(idn("a\xE3\x80\x82""b"));
	EXPECT_TRUE(idn("a\xEF\xBC\x8E""b"));
	EXPECT_TRUE(idn("a\xEF\xBD\xA1""b"));
	EXPECT_FALSE(idn("\xE3\x80\x82"));
	EXPECT_FALSE(idn("\xE3\x80\x82""example"));
	EXPECT_FALSE(idn("example\xE3\x80\x82"));
	// For a plain `hostname` that is one label with a character no label may
	// contain, which is invalid for a different and truer reason.
	EXPECT_FALSE(host("example\xEF\xBC\x8E""com"));
}

TEST(Idna, MalformedUtf8IsNotAName) {
	EXPECT_FALSE(idn(std::string("a\xC3", 2)));
	EXPECT_FALSE(idn(std::string("\xED\xA0\x80", 3)));  // a surrogate
	EXPECT_FALSE(idn(std::string("\xC0\xAF", 2)));      // overlong
	EXPECT_FALSE(idn(std::string("a\0b", 3)));
}

int main(int argc, char ** argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

// ===========================================================================
// UTS #46: what a name becomes before IDNA2008 judges it
// ===========================================================================
//
// Two specifications, layered rather than merged. UTS #46 says what a name
// *becomes* - fullwidth forms folded to their ASCII counterparts, invisible
// characters dropped, the result normalised - and RFC 5892 still says what is
// valid. JSON Schema defines `idn-hostname` by RFC 5890, so the validity rule
// has to stay IDNA2008's; the mapping is what makes `１２３` and `123` the
// same name rather than one valid name and one rejected string.
//
// Only for `idn-hostname`. Mapping a plain `hostname` would be answering a
// question nobody asked.

TEST(Idna, MappedToAscii) {
	// Fullwidth digits, fullwidth letters, and a fullwidth ASCII string that
	// becomes an ordinary LDH label.
	EXPECT_TRUE(idn("\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93"));  // １２３
	EXPECT_TRUE(idn("\xEF\xBD\x81\xEF\xBD\x82"));              // ａｂ
	// The mapping is a case fold too, so a fullwidth capital arrives as
	// lower case rather than as a capital that then has to be allowed.
	EXPECT_TRUE(idn("\xEF\xBC\xA1\xEF\xBC\xA2"));              // ＡＢ

	// A plain `hostname` gets none of it: this is one label containing a
	// character no label may contain.
	EXPECT_FALSE(host("\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93"));
}

TEST(Idna, MappingTable17FoldsNewCapitals) {
	// IdnaMappingTable 16.0.0 does not list these, so the mapping step left
	// them alone and RFC 5892 then called each one DISALLOWED. 17.0.0 maps
	// each capital to its small letter, which is PVALID. A plain hostname is
	// still not mapped.
	EXPECT_TRUE(idn("\xEA\x9F\x8E"));              // U+A7CE
	EXPECT_TRUE(idn("\xEA\x9F\x92"));              // U+A7D2
	EXPECT_TRUE(idn("\xEA\x9F\x94"));              // U+A7D4
	EXPECT_TRUE(idn("\xEA\x9F\xB1"));              // U+A7F1 -> U+0073
	EXPECT_TRUE(idn("\xF0\x96\xBA\xA0"));          // U+16EA0
	EXPECT_TRUE(idn("\xF0\x96\xBA\xB8"));          // U+16EB8
	EXPECT_FALSE(host("\xEA\x9F\xB1"));
}

TEST(Idna, IgnoredCharactersAreRemoved) {
	// U+200B ZERO WIDTH SPACE is `ignored`: it leaves, and what is left is
	// judged. `a<ZWSP>b` is the name `ab`.
	EXPECT_TRUE(idn("a\xE2\x80\x8B""b"));
	// U+00AD SOFT HYPHEN and U+FEFF ZERO WIDTH NO-BREAK SPACE likewise.
	EXPECT_TRUE(idn("a\xC2\xAD""b"));
	EXPECT_TRUE(idn("a\xEF\xBB\xBF""b"));
	// A name that is nothing but ignored characters is not a name.
	EXPECT_FALSE(idn("\xE2\x80\x8B"));
	// And the removal happens before the length is measured, so this is 63
	// characters and not 64.
	EXPECT_TRUE(idn(std::string(63, 'a') + "\xE2\x80\x8B"));
	EXPECT_FALSE(idn(std::string(64, 'a') + "\xE2\x80\x8B"));
}

TEST(Idna, MappingRunsBeforeTheLengthLimit) {
	// 63 fullwidth `a`, which is 189 bytes of UTF-8 and 63 characters once
	// mapped. A validator that measured first would call this too long.
	std::string wide;
	for (int i = 0; i < 63; i++) {
		wide += "\xEF\xBD\x81";
	}
	EXPECT_TRUE(idn(wide));
	EXPECT_FALSE(idn(wide + "\xEF\xBD\x81"));
}

TEST(Idna, AnAcePrefixTheMappingProduced) {
	// `ｘｎ--nxasmq6b` is not an A-label when it arrives - `ｘ` and `ｎ` are
	// fullwidth - and is one after the mapping step. The Punycode has to be
	// decoded and checked, which means the mapping cannot be something done
	// after the label kind is decided.
	EXPECT_TRUE(idn("\xEF\xBD\x98\xEF\xBD\x8E--nxasmq6b"));
	// The same thing with payload that does not decode is still refused, so
	// this is not passing merely because the prefix went unnoticed.
	EXPECT_FALSE(idn("\xEF\xBD\x98\xEF\xBD\x8E--nxasmq6b\x81"));
}

TEST(Idna, TheThreeOtherStopsAreMappedNotSpecialCased) {
	// These used to be a hard-coded list of separators. They are separators
	// because the mapping step turns all three into FULL STOP, which is where
	// UTS #46 actually puts the rule - and the behaviour is unchanged.
	EXPECT_TRUE(idn("a\xE3\x80\x82""b"));
	EXPECT_TRUE(idn("a\xEF\xBC\x8E""b"));
	EXPECT_TRUE(idn("a\xEF\xBD\xA1""b"));
	EXPECT_FALSE(idn("\xE3\x80\x82"));
	EXPECT_FALSE(idn("example\xEF\xBC\x8E"));
}

TEST(Idna, DeviationCharactersAreLeftAlone) {
	// Nontransitional processing, which is what every current browser does.
	// Transitional processing folds sharp s to `ss` and final sigma to
	// sigma, which would make two different names into one - and this
	// library carries no entry for them precisely because nothing happens.
	//
	// `faß` stays `faß`, a valid name in its own right, and is not the same
	// name as `fass`.
	EXPECT_TRUE(idn("fa\xC3\x9F"));
	EXPECT_TRUE(idn("fass"));
	// Greek final sigma, likewise valid and not folded to sigma.
	EXPECT_TRUE(idn("\xCF\x83\xCF\x8C\xCE\xBB\xCE\xBF\xCF\x82"));
}

TEST(Idna, NamesAreNormalisedToNfc) {
	// `é` written as one character and as `e` plus a combining acute are the
	// same name. A validator that treated them as different names would be
	// the hole spoofing walks through, and one that rejected the decomposed
	// form would reject a name every browser accepts.
	const std::string composed = "caf\xC3\xA9";          // café, U+00E9
	const std::string decomposed = "cafe\xCC\x81";       // e + U+0301
	EXPECT_TRUE(idn(composed));
	EXPECT_TRUE(idn(decomposed));

	// Both encode to the same A-label, which is the property that matters:
	// the 63-octet limit is measured on the encoded form, so the two must
	// agree about length as well as about validity.
	std::string long_composed;
	std::string long_decomposed;
	for (int i = 0; i < 20; i++) {
		long_composed += "\xC3\xA9";
		long_decomposed += "e\xCC\x81";
	}
	EXPECT_EQ(idn(long_composed), idn(long_decomposed));

	// A combining mark with nothing to combine with is still a leading
	// combining mark after normalisation, which RFC 5892 refuses.
	EXPECT_FALSE(idn("\xCC\x81""abc"));
}

TEST(Idna, AnALabelMustDecodeToNfc) {
	// UTS #46 section 4.1, criterion 1. A U-label arrives normalised because
	// the mapping step normalised the whole name; an A-label does not,
	// because nothing looked inside its Punycode. It is refused rather than
	// normalised - an A-label is a spelling of one exact U-label, and one
	// that decodes to a different string than it claims is not that label.
	//
	// xn--e-xbb is `e` + U+0301, which is not NFC: it is `é` spelled the
	// long way, and the A-label for `é` is xn--9ca.
	EXPECT_FALSE(idn("xn--e-xbb"));
	EXPECT_TRUE(idn("xn--9ca"));
	// Not every A-label with a hyphen in its payload is suspect: xn--e-uga
	// decodes to `óe`, which is already NFC and is accepted. Without this the
	// test above would pass for an implementation that refused anything it
	// found hard to decode.
	EXPECT_TRUE(idn("xn--e-uga"));
}

TEST(Idna, EveryAdmittedCodepointHasABidiClassRule5893Names) {
	// RFC 5893's rule is an allow-list: a label is valid only if every
	// character's Bidi_Class is one of eleven, and anything else refuses the
	// label. This asserts the two RFCs agree - that no codepoint RFC 5892
	// admits into a label carries a class the bidi rule has no arm for.
	//
	// It is here because of what the migration to ghoti.io-unicode found.
	// Script, Joining_Type, Bidi_Class and the virama test used to come from
	// tables generated here that held the UCD narrowed to the values these
	// rules read, with everything else folded into one OTHER sentinel. Against
	// the full property, the narrow Bidi_Class table disagreed about 812,943
	// codepoints - always in the same direction, OTHER where the UCD has a
	// real class, never a different class - and not one of them was a
	// codepoint IDNA lets into a label. 810,895 were unassigned, which
	// DerivedBidiClass.txt gives default classes by block, and the remaining
	// 2,048 were exactly the surrogates U+D800..U+DFFF.
	//
	// That is why the substitution changed no answer. The reason is a property
	// of the two RFCs over one UCD version, though, not a theorem, so it is
	// checked rather than remembered: a future UCD that admitted a character
	// with an exotic class would make the bidi rule refuse a name it should
	// accept, and nothing else here would notice.
	size_t admitted = 0;
	size_t outside = 0;
	uint32_t first_outside = 0;
	for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
		if (cp >= 0xD800 && cp <= 0xDFFF) {
			continue; // not a scalar value; no UTF-8 decoder yields one
		}
		size_t lo = 0;
		size_t hi = gtext_idna_derived_count;
		uint32_t property = GTEXT_IDNA_DISALLOWED;
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;
			if (cp < gtext_idna_derived[mid].lo) {
				hi = mid;
			}
			else if (cp > gtext_idna_derived[mid].hi) {
				lo = mid + 1;
			}
			else {
				property = gtext_idna_derived[mid].value;
				break;
			}
		}
		if (property == GTEXT_IDNA_DISALLOWED) {
			continue;
		}
		admitted++;
		switch (guni_bidi_class(cp)) {
		case GUNI_BIDI_L:
		case GUNI_BIDI_R:
		case GUNI_BIDI_AL:
		case GUNI_BIDI_AN:
		case GUNI_BIDI_EN:
		case GUNI_BIDI_ES:
		case GUNI_BIDI_CS:
		case GUNI_BIDI_ET:
		case GUNI_BIDI_ON:
		case GUNI_BIDI_BN:
		case GUNI_BIDI_NSM:
			break;
		default:
			if (outside++ == 0) {
				first_outside = cp;
			}
			break;
		}
	}
	// The count is asserted as well as the property: a derived table that
	// admitted nothing would satisfy the loop above without testing anything.
	EXPECT_GT(admitted, 100000u);
	EXPECT_EQ(outside, 0u)
	    << "U+" << std::hex << first_outside
	    << " is admitted into a label but its Bidi_Class is not one RFC 5893"
	       " names";
}
