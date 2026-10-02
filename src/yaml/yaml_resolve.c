/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file yaml_resolve.c
 * @brief Tag and implicit type resolver for YAML documents.
 *
 * Resolves explicit tags and applies schema-based implicit typing to
 * scalar nodes after parsing.
 */

#include <ghoti.io/text/macros.h>
#include "yaml_internal.h"
#include "../text_number_internal.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool str_eq_len(const char *a, size_t len, const char *b) {
	size_t blen = strlen(b);
	if (len != blen) return false;
	return memcmp(a, b, len) == 0;
}

static const char *tag_suffix(const char *tag) {
	static const char yaml_prefix[] = "tag:yaml.org,2002:";
	if (!tag) return NULL;
	if (strncmp(tag, "!!", 2) == 0) return tag + 2;
	if (strncmp(tag, yaml_prefix, sizeof(yaml_prefix) - 1) == 0) {
		return tag + (sizeof(yaml_prefix) - 1);
	}
	return NULL;
}

/* The tags the spec defines in the "tag:yaml.org,2002:" namespace: the
   failsafe, JSON and core schemas plus the types YAML carries alongside
   them.  "value" and "yaml" are named by the 1.1 type repository but no
   schema here resolves them, and both reference implementations refuse
   them, so they are not on this list. */
/* The types the "tag:yaml.org,2002:" namespace names.  The writer asks this
   too - a tag in that namespace naming no type the spec defines is a
   malformed document, and writing one produces something this library then
   refuses to read - so the list lives in one place rather than two. */
GTEXT_INTERNAL_API bool gtext_yaml_tag_is_defined_standard(const char *suffix) {
	static const char *defined[] = {
		"str",
		"bool",
		"int",
		"float",
		"null",
		"seq",
		"map",
		"set",
		"omap",
		"pairs",
		"binary",
		"timestamp",
		"merge"
	};

	for (size_t i = 0; i < sizeof(defined) / sizeof(defined[0]); i++) {
		if (strcmp(suffix, defined[i]) == 0) return true;
	}
	return false;
}

/* True for a tag that is not in the "tag:yaml.org,2002:" namespace at all:
   a local tag ("!foo"), or a global one under somebody else's prefix.  These
   are the tags an application defines for itself, and whether to accept them
   is a policy question - see allow_nonstandard_tags.  Whether a tag inside
   the YAML namespace names a type the spec actually defines is a separate
   question, and not a matter of policy; see enforce_tag_policy(). */
static bool is_standard_tag(const char *tag) {
	const char *suffix = NULL;

	if (!tag) return true;
	if (strcmp(tag, "!") == 0 || strcmp(tag, "!!") == 0) return true;

	suffix = tag_suffix(tag);
	if (!suffix) return false;
	if (suffix[0] == '\0') return true;

	return gtext_yaml_tag_is_defined_standard(suffix);
}

/**
 * @brief Is this a named tag shorthand whose handle was never declared?
 *
 * A shorthand is a handle and a suffix. The primary handle "!" and the
 * secondary "!!" are always available, but "!name!" exists only where a %TAG
 * directive put it, and a shorthand using one that was never declared is an
 * error (6.8.2.2).  It resolved to itself instead and the document was
 * accepted - which is how a handle declared in the first document of a
 * stream appeared to carry into the rest of them: each document does get its
 * own table, and the later ones simply never complained (suite case QLJ7).
 */
static bool tag_handle_undeclared(
	const GTEXT_YAML_Document *doc,
	const char *tag
) {
	const char *second = NULL;
	size_t hlen = 0;

	if (!tag || tag[0] != '!') return false;
	/* "!!x" is the secondary handle and "!x" the primary; a named handle is
	   "!" name "!" with the name not empty. */
	second = strchr(tag + 1, '!');
	if (!second || second == tag + 1) return false;
	hlen = (size_t)(second - tag) + 1;

	if (!doc) return true;
	for (size_t i = 0; i < doc->tag_handle_count; i++) {
		const char *handle = doc->tag_handles[i].handle;
		if (!handle) continue;
		/* Comparing hlen characters is enough: a handle is "!" name "!"
		   with no further "!" in it, so one whose first hlen characters
		   match - hlen reaching to and including the shorthand's second
		   "!" - has no more characters to differ in. */
		if (strncmp(handle, tag, hlen) == 0) {
			return false;
		}
	}
	return true;
}

static GTEXT_YAML_Status enforce_tag_policy(
	const GTEXT_YAML_Document *doc,
	const char *tag,
	bool verbatim,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error
) {
	/* The handle rule is a rule about a *shorthand*, and a verbatim tag is
	   not one: "!<!a!>" is the tag "!a!" written out in full, and no %TAG
	   declares anything for it (5.3, 6.8.2.2).  It was being held to the rule
	   anyway, because by the time it arrived here nothing said which spelling
	   it had come from. */
	if (!verbatim && tag_handle_undeclared(doc, tag)) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Tag shorthand uses a handle no %TAG declared";
		}
		return GTEXT_YAML_E_INVALID;
	}
	/* The "tag:yaml.org,2002:" namespace belongs to the spec, so a tag in
	   it that names no type the spec defines - "!!bogus" - is a malformed
	   document, not a matter of taste.  A %TAG directive that re-points
	   "!!" somewhere else has already been expanded by the time the tag
	   arrives here, so this only sees tags genuinely in the namespace. */
	const char *suffix = tag_suffix(tag);
	if (suffix && suffix[0] != '\0' && !gtext_yaml_tag_is_defined_standard(suffix)) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Unknown tag in the tag:yaml.org,2002 namespace";
		}
		return GTEXT_YAML_E_INVALID;
	}

	/* Application-defined tags are valid YAML - the spec's own examples use
	   them - so they are accepted unless the caller has asked for a document
	   that sticks to the tags this library resolves. */
	if (!opts || opts->allow_nonstandard_tags) return GTEXT_YAML_OK;
	if (is_standard_tag(tag)) return GTEXT_YAML_OK;
	if (error) {
		error->code = GTEXT_YAML_E_INVALID;
		error->message = "Non-standard tag not allowed by parse options";
	}
	return GTEXT_YAML_E_INVALID;
}

static const GTEXT_YAML_Node *deref_alias(const GTEXT_YAML_Node *node);

static const char *implicit_tag_suffix(GTEXT_YAML_Node_Type type) {
	switch (type) {
		case GTEXT_YAML_STRING:
			return "str";
		case GTEXT_YAML_BOOL:
			return "bool";
		case GTEXT_YAML_INT:
			return "int";
		case GTEXT_YAML_FLOAT:
			return "float";
		case GTEXT_YAML_NULL:
			return "null";
		default:
			return NULL;
	}
}

static const char *scalar_tag_id(const GTEXT_YAML_Node *node) {
	if (!node) return NULL;
	const char *tag = node->as.scalar.tag;
	if (tag) {
		const char *suffix = tag_suffix(tag);
		return suffix ? suffix : tag;
	}
	return implicit_tag_suffix(node->type);
}

static bool node_is_null(const GTEXT_YAML_Node *node) {
	const GTEXT_YAML_Node *resolved = deref_alias(node);
	if (!resolved) return false;
	if (resolved->type == GTEXT_YAML_NULL) return true;
	return false;
}

/**
 * Read a YAML 1.1 `!!timestamp`.
 *
 * The grammar is chron's, which is the type repository's regular expression -
 * and not, as the hundred lines this replaced were, an approximation of it.
 * That parser refused four spellings YAML permits (a one-digit hour, a
 * one-digit offset hour, more than one space before the time, and whitespace
 * before the zone) and accepted one it does not (a time with no seconds); it
 * had never been compared against another implementation, so nothing said so.
 *
 * @param value The scalar text.
 * @param len Its length.
 * @param out Receives the timestamp; untouched on failure.
 * @return Whether the text is a YAML 1.1 timestamp.
 */
static bool parse_timestamp(
	const char *value,
	size_t len,
	yaml_node_scalar *out
) {
	if (!value || !out) return false;
	GCHRON_YamlValue parsed;
	GCHRON_ParseInfo info;
	/* NULL options are gchron_parse_options_yaml(): truncate a long fraction
	   and accept `:60`, which is what the grammar permits and YAML declines
	   to rule on either way. */
	if (gchron_parse_yaml_timestamp(value, len, NULL, &parsed, &info, NULL)
			!= GCHRON_OK) {
		return false;
	}
	out->has_timestamp = true;
	out->timestamp = parsed;
	out->timestamp_leap_second = info.leap_second;
	return true;
}

/**
 * Write a `!!timestamp` back out in its canonical spelling.
 *
 * chron writes the two-digit, `T`-separated form with the shortest fraction
 * that loses nothing, which is the spelling the type repository's own
 * canonical example uses and the one every YAML 1.1 reader accepts. The
 * relaxed input spellings deliberately do not survive.
 *
 * @param doc The document, for its arena.
 * @param scalar The scalar carrying the timestamp.
 * @return The normalised text, or NULL when the arena is exhausted.
 */
static const char *format_timestamp(
	GTEXT_YAML_Document *doc,
	const yaml_node_scalar *scalar
) {
	if (!doc || !scalar || !scalar->has_timestamp) return NULL;
	if (scalar->timestamp_leap_second) {
		/* The one reading whose canonical spelling would say something
		   different from what the document said. chron holds a `:60` as `:59`
		   of the same minute - there is nowhere else to put it - so writing
		   the value back out would move a log line one second earlier and
		   nothing in the document would record that it had happened.
		   Normalisation here changes the spelling, never the value, so this
		   scalar keeps the text it arrived with. The parsed value still says
		   `:59`, and gtext_yaml_node_timestamp_is_leap_second() is what
		   reconciles the two. */
		return scalar->value;
	}
	char buf[GCHRON_YAML_TIMESTAMP_MAX];
	size_t len = 0;
	if (gchron_write_yaml_timestamp(&scalar->timestamp, NULL, buf, sizeof(buf),
			&len) != GCHRON_OK) {
		return NULL;
	}
	char *out = (char *)yaml_context_alloc(doc->ctx, len + 1, 1);
	if (!out) return NULL;
	memcpy(out, buf, len);
	out[len] = '\0';
	return out;
}

/* The scratch copy is transient - it never escapes the caller - but it still
   comes from the caller's allocator, because "every allocation the parse makes"
   is what the option promises and an arena caller wants the scratch accounted
   too. */
static char *strip_underscores(const char *s, size_t len, bool allow,
	const GTEXT_Allocator *alloc) {
	if (!allow) {
		char *copy = (char *)gtext_allocator_malloc(alloc, len + 1);
		if (!copy) return NULL;
		memcpy(copy, s, len);
		copy[len] = '\0';
		return copy;
	}

	char *buf = (char *)gtext_allocator_malloc(alloc, len + 1);
	if (!buf) return NULL;
	size_t out = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] != '_') {
			buf[out++] = s[i];
		}
	}
	buf[out] = '\0';
	return buf;
}

/**
 * @brief Match one of an enumerated set of spellings, exactly.
 *
 * The resolution tables in chapter 10 are lists of spellings, not
 * case-insensitive words: the core schema's bool row is
 * "true | True | TRUE | false | False | FALSE" and nothing else, so "tRue"
 * is a string. Matching case-insensitively resolved a whole family of
 * spellings the spec leaves alone, and silently - a key written "nULL"
 * became a null rather than the string somebody typed.
 *
 * The 1.1 tables are enumerations too ("y|Y|yes|Yes|YES|..."), so "yEs" is
 * not a 1.1 boolean either. One helper serves both.
 */
static bool str_eq_any(const char *s, size_t len, const char *const *set) {
	for (size_t i = 0; set[i]; i++) {
		if (str_eq_len(s, len, set[i])) return true;
	}
	return false;
}

static bool parse_bool_value(
	const char *s,
	size_t len,
	bool json_only,
	bool yaml_1_1,
	bool *out
) {
	/* 10.3.2, the core schema's bool row - and 10.2.2's, which is the same
	   two spellings the JSON schema allows. */
	static const char *const true_12[] = {"true", "True", "TRUE", NULL};
	static const char *const false_12[] = {"false", "False", "FALSE", NULL};
	/* The YAML 1.1 bool type, in full. */
	static const char *const true_11[] = {
		"y", "Y", "yes", "Yes", "YES", "on", "On", "ON", NULL};
	static const char *const false_11[] = {
		"n", "N", "no", "No", "NO", "off", "Off", "OFF", NULL};

	if (json_only) {
		if (str_eq_len(s, len, "true")) { *out = true; return true; }
		if (str_eq_len(s, len, "false")) { *out = false; return true; }
		return false;
	}
	if (str_eq_any(s, len, true_12)) { *out = true; return true; }
	if (str_eq_any(s, len, false_12)) { *out = false; return true; }
	if (yaml_1_1) {
		if (str_eq_any(s, len, true_11)) { *out = true; return true; }
		if (str_eq_any(s, len, false_11)) { *out = false; return true; }
	}
	return false;
}

static bool parse_null_value(const char *s, size_t len, bool json_only) {
	/* The empty node resolves to null (10.3.2, and the "Empty" row of the
	   core schema's resolution table), which is what "a:" with no value and
	   "&anchor" with no node both stand for. */
	static const char *const null_spellings[] = {"null", "Null", "NULL", NULL};

	if (len == 0) return true;
	if (json_only) {
		return str_eq_len(s, len, "null");
	}
	if (len == 1 && s[0] == '~') return true;
	return str_eq_any(s, len, null_spellings);
}

static bool is_base64_space(unsigned char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int base64_value(unsigned char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

GTEXT_INTERNAL_API bool gtext_yaml_base64_decode(
	GTEXT_YAML_Document *doc,
	const char *value,
	size_t len,
	const unsigned char **out_data,
	size_t *out_len
) {
	if (!doc || !value || !out_data || !out_len) return false;

	char *filtered = (char *)gtext_allocator_malloc(doc->ctx->alloc, len + 1);
	if (!filtered) return false;
	size_t count = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)value[i];
		if (is_base64_space(c)) continue;
		if (c == '=' || base64_value(c) >= 0) {
			filtered[count++] = (char)c;
			continue;
		}
		gtext_allocator_free(doc->ctx->alloc, filtered);
		return false;
	}
	filtered[count] = '\0';

	if ((count % 4) != 0) {
		gtext_allocator_free(doc->ctx->alloc, filtered);
		return false;
	}

	if (count == 0) {
		/* Base64 of no bytes is the empty string, so "!!binary" with an empty
		   value - or with nothing but white space in it - is an empty byte
		   string.  PyYAML, whose type repository defines !!binary, reads both
		   as b''.  Refusing them made the writer's own output unreadable,
		   since an empty binary node is written '!!binary ""'.

		   The padding test below is why this has to return before it: with no
		   characters, filtered[count - 2] reads off the front of the buffer. */
		unsigned char *empty =
			(unsigned char *)yaml_context_alloc(doc->ctx, 1, 1);
		gtext_allocator_free(doc->ctx->alloc, filtered);
		if (!empty) return false;
		empty[0] = '\0';
		*out_data = empty;
		*out_len = 0;
		return true;
	}

	size_t padding = 0;
	if (filtered[count - 1] == '=') padding++;
	if (filtered[count - 2] == '=') padding++;
	if (padding > 2) {
		gtext_allocator_free(doc->ctx->alloc, filtered);
		return false;
	}

	size_t decoded_len = (count / 4) * 3;
	if (padding > 0) decoded_len -= padding;
	unsigned char *decoded = (unsigned char *)yaml_context_alloc(doc->ctx, decoded_len, 1);
	if (!decoded) {
		gtext_allocator_free(doc->ctx->alloc, filtered);
		return false;
	}

	size_t out = 0;
	for (size_t i = 0; i < count; i += 4) {
		char c0 = filtered[i];
		char c1 = filtered[i + 1];
		char c2 = filtered[i + 2];
		char c3 = filtered[i + 3];

		if (c0 == '=' || c1 == '=') {
			gtext_allocator_free(doc->ctx->alloc, filtered);
			return false;
		}

		int v0 = base64_value((unsigned char)c0);
		int v1 = base64_value((unsigned char)c1);
		if (v0 < 0 || v1 < 0) {
			gtext_allocator_free(doc->ctx->alloc, filtered);
			return false;
		}

		if (c2 == '=') {
			if (c3 != '=' || i + 4 != count) {
				gtext_allocator_free(doc->ctx->alloc, filtered);
				return false;
			}
			decoded[out++] = (unsigned char)((v0 << 2) | (v1 >> 4));
			break;
		}

		int v2 = base64_value((unsigned char)c2);
		if (v2 < 0) {
			gtext_allocator_free(doc->ctx->alloc, filtered);
			return false;
		}

		if (c3 == '=') {
			if (i + 4 != count) {
				gtext_allocator_free(doc->ctx->alloc, filtered);
				return false;
			}
			decoded[out++] = (unsigned char)((v0 << 2) | (v1 >> 4));
			decoded[out++] = (unsigned char)(((v1 & 0x0F) << 4) | (v2 >> 2));
			break;
		}

		int v3 = base64_value((unsigned char)c3);
		if (v3 < 0) {
			gtext_allocator_free(doc->ctx->alloc, filtered);
			return false;
		}

		decoded[out++] = (unsigned char)((v0 << 2) | (v1 >> 4));
		decoded[out++] = (unsigned char)(((v1 & 0x0F) << 4) | (v2 >> 2));
		decoded[out++] = (unsigned char)(((v2 & 0x03) << 6) | v3);
	}

	gtext_allocator_free(doc->ctx->alloc, filtered);
	*out_data = decoded;
	*out_len = decoded_len;
	return true;
}

static bool yaml_use_1_1(const GTEXT_YAML_Document *doc, const GTEXT_YAML_Parse_Options *opts) {
	if (opts && opts->yaml_1_1) return true;
	if (doc && doc->yaml_version_major == 1 && doc->yaml_version_minor == 1) return true;
	return false;
}

static bool has_disallowed_leading_zero(const char *s, size_t len, bool allow_underscore, const GTEXT_Allocator *alloc) {
	if (!s || len == 0) return false;
	char *clean = strip_underscores(s, len, allow_underscore, alloc);
	if (!clean) return false;
	const char *p = clean;
	if (*p == '+' || *p == '-') p++;
	bool result = false;
	if (p[0] == '0' && p[1] != '\0') {
		if (p[1] == 'x' || p[1] == 'X' || p[1] == 'o' || p[1] == 'O' || p[1] == 'b' || p[1] == 'B') {
			result = false;
		} else if (p[1] >= '0' && p[1] <= '9') {
			result = true;
		}
	}
	gtext_allocator_free(alloc, clean);
	return result;
}

/* YAML 1.1's two sexagesimal rows, read literally off the 1.1 type
   repository:

     int    [-+]? [1-9] [0-9_]* ( : [0-5]? [0-9] )+
     float  [-+]? [0-9] [0-9_]* ( : [0-5]? [0-9] )+ \. [0-9_]*

   Every clause there is load-bearing, and this walk had none of them except
   "a colon is present".  It split on ":", accepted a segment of any length
   and any value, stripped underscores from the whole text before looking at
   it, and took a dot anywhere in the final segment as the fraction.  So
   "0:30" was the integer 30, "1:99" was 159, "01:30" was 90 and "1:3_0" was
   90 - four texts that PyYAML, which is a 1.1 implementation, reads as the
   strings they are, and that the rows above do not admit.

   None of those is a round trip failure: the writer emits the digits it was
   given, the reader resolves them to the same wrong number, and the two agree
   all the way.  That is why the fuzzer found the trailing-colon case and not
   these - a self-consistent wrong answer needs a reader that disagrees, and
   there was none.  It is also why they are the worse half: a document said
   "0:30" and this library answered 30 with nothing anywhere to notice.

   The segment rules in particular are not pedantry.  "[0-5]? [0-9]" is what
   makes a sexagesimal a clock reading: the row exists so that "15:01:42" is a
   number of seconds, and a group of "99" or of three digits means the text
   was never that. Leading zeros are what separate the int row from the float
   row - "0:30" is in neither, "0:30.5" is in the second - and 1.1 puts
   underscores in the first segment and the fraction only, which is the one
   clause a whole-text strip cannot express.

   No allocation: the walk is bounded by @p len, so there is no copy to make
   and no failure that is not the text's own.  @p allow_underscore is the
   dialect's, and with it off an underscore is simply not a digit. */
static bool parse_sexagesimal_value(
	const char *s,
	size_t len,
	bool allow_underscore,
	double *out,
	bool *out_is_int
) {
	if (!s || len == 0 || !out) return false;

	size_t i = 0;
	bool neg = false;
	if (s[i] == '+' || s[i] == '-') {
		neg = (s[i] == '-');
		i++;
	}

	/* The first segment: a digit, then digits and underscores.  Its leading
	   digit is what the two rows disagree about, so it is kept. */
	if (i >= len || s[i] < '0' || s[i] > '9') return false;
	const bool first_is_zero = (s[i] == '0');
	double total = (double)(s[i] - '0');
	i++;
	while (i < len) {
		if (s[i] == '_') {
			if (!allow_underscore) return false;
			i++;
			continue;
		}
		if (s[i] < '0' || s[i] > '9') break;
		total = total * 10.0 + (double)(s[i] - '0');
		i++;
	}

	/* One or more ":" groups, each one or two digits worth at most 59.  A
	   colon with nothing after it, a third digit, or a group past 59 all
	   leave the row - and a colon with an underscore after it stops the digit
	   run here and is caught as trailing text below. */
	size_t groups = 0;
	while (i < len && s[i] == ':') {
		i++;
		unsigned segment = 0;
		size_t digits = 0;
		while (i < len && digits < 2 && s[i] >= '0' && s[i] <= '9') {
			segment = segment * 10u + (unsigned)(s[i] - '0');
			digits++;
			i++;
		}
		if (digits == 0) return false;
		if (i < len && s[i] >= '0' && s[i] <= '9') return false;
		if (segment > 59u) return false;
		total = total * 60.0 + (double)segment;
		groups++;
	}
	if (groups == 0) return false;

	/* The fraction is the float row's, and its digits are optional: "1:5." is
	   a float of 65.  Which row this is has to be settled by the dot and not
	   by what follows it. */
	bool is_float = false;
	if (i < len && s[i] == '.') {
		is_float = true;
		i++;
		double scale = 1.0;
		while (i < len) {
			if (s[i] == '_') {
				if (!allow_underscore) return false;
				i++;
				continue;
			}
			if (s[i] < '0' || s[i] > '9') break;
			scale *= 10.0;
			total += (double)(s[i] - '0') / scale;
			i++;
		}
	}

	/* Anything left over was never part of either row. */
	if (i != len) return false;
	/* And the clause the two rows differ on: the int row starts "[1-9]". */
	if (!is_float && first_is_zero) return false;

	if (neg) total = -total;
	*out = total;
	if (out_is_int) *out_is_int = !is_float;
	return true;
}

/* 2^63 is exactly representable as a double and is the first one that does
   not fit, so "< limit" is the comparison and "<= INT64_MAX" would not be -
   (double)INT64_MAX rounds *up* to 2^63. */
GTEXT_INTERNAL_API bool gtext_yaml_double_fits_int64(double f) {
	static const double limit = 9223372036854775808.0;  /* 2^63 */
	return isfinite(f) && f >= -limit && f < limit;
}

/* Whether @p c is a digit of @p base, which is 2, 8, 10 or 16 here. */
static bool digit_in_base(char c, int base) {
	int value;
	if (c >= '0' && c <= '9') value = c - '0';
	else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
	else return false;
	return value < base;
}

static bool parse_int_value(
	const char *s,
	size_t len,
	bool allow_underscore,
	bool allow_base_prefix,
	bool allow_binary_and_upper_prefix,
	bool allow_yaml_1_1_octal,
	int64_t *out,
	const GTEXT_Allocator *alloc
) {
	if (!s || len == 0 || !out) return false;

	char *clean = strip_underscores(s, len, allow_underscore, alloc);
	if (!clean) return false;

	const char *p = clean;
	bool neg = false;
	if (*p == '+' || *p == '-') {
		neg = (*p == '-');
		p++;
	}
	if (*p == '\0') { gtext_allocator_free(alloc, clean); return false; }

	/* 10.3.2 spells the two prefixed int rows "0o [0-7]+" and
	   "0x [0-9a-fA-F]+" - lower case, and no binary row at all.  "0B"/"0O"/
	   "0X" and base 2 are YAML 1.1, so they need the 1.1 schema. */
	int base = 10;
	if (allow_base_prefix && p[0] == '0' && p[1] != '\0') {
		char prefix = p[1];
		if (allow_binary_and_upper_prefix) {
			if (prefix == 'B') prefix = 'b';
			else if (prefix == 'O') prefix = 'o';
			else if (prefix == 'X') prefix = 'x';
		}
		switch (prefix) {
			case 'b':
				if (!allow_binary_and_upper_prefix) break;
				base = 2;
				p += 2;
				break;
			case 'o':
				base = 8;
				p += 2;
				break;
			case 'x':
				base = 16;
				p += 2;
				break;
			default:
				break;
		}
	}
	/* Nothing consumed the prefix, so it is not a number the schema knows:
	   strtoll would read the leading "0" of "0b101" and stop, and the
	   trailing-character check below turns that into a string. */

	if (allow_yaml_1_1_octal && p[0] == '0' && p[1] != '\0') {
		bool octal = true;
		for (size_t i = 1; p[i] != '\0'; i++) {
			if (p[i] < '0' || p[i] > '7') {
				octal = false;
				break;
			}
		}
		if (octal) {
			if (!digit_in_base(*p, 8)) { gtext_allocator_free(alloc, clean); return false; }
			errno = 0;
			char *end = NULL;
			long long parsed = strtoll(p, &end, 8);
			if (errno == ERANGE || end == p || (end && *end != '\0')) {
				gtext_allocator_free(alloc, clean);
				return false;
			}
			if (neg) parsed = -parsed;
			*out = (int64_t)parsed;
			gtext_allocator_free(alloc, clean);
			return true;
		}
	}

	if (*p == '\0') { gtext_allocator_free(alloc, clean); return false; }

	/* strtoll() skips leading white space and would take a sign of its own,
	   and this has already taken the sign and any base prefix - so without
	   this "+\n1" reads as 1 and "+ +1" as 1 too.  10.3.2's integer row is
	   "[-+]? [0-9]+": one optional sign, then digits, and nothing between
	   them.  The caller above keeps white space out of the classifier
	   entirely; this keeps the helper honest on its own. */
	if (!digit_in_base(*p, base)) { gtext_allocator_free(alloc, clean); return false; }

	errno = 0;
	char *end = NULL;
	long long parsed = strtoll(p, &end, base);
	if (errno == ERANGE || end == p || (end && *end != '\0')) {
		gtext_allocator_free(alloc, clean);
		return false;
	}

	if (neg) parsed = -parsed;
	*out = (int64_t)parsed;
	gtext_allocator_free(alloc, clean);
	return true;
}

static bool parse_float_value(
	const char *s,
	size_t len,
	bool allow_underscore,
	double *out,
	const GTEXT_Allocator *alloc
) {
	if (!s || len == 0 || !out) return false;

	char *clean = strip_underscores(s, len, allow_underscore, alloc);
	if (!clean) return false;

	bool has_dot = strchr(clean, '.') != NULL;
	bool has_exp = strchr(clean, 'e') != NULL || strchr(clean, 'E') != NULL;

	/* 10.3.2 again, and enumerations again: the float row is
	   "[-+]? ( .inf | .Inf | .INF )" and the not-a-number row
	   ".nan | .NaN | .NAN". ".Nan" and ".INf" are strings. */
	static const char *const inf_spellings[] = {
		".inf", ".Inf", ".INF",
		"+.inf", "+.Inf", "+.INF",
		"-.inf", "-.Inf", "-.INF", NULL};
	static const char *const nan_spellings[] = {".nan", ".NaN", ".NAN", NULL};

	size_t clean_len = strlen(clean);
	if (str_eq_any(clean, clean_len, inf_spellings)) {
		*out = clean[0] == '-' ? -INFINITY : INFINITY;
		gtext_allocator_free(alloc, clean);
		return true;
	}

	if (str_eq_any(clean, clean_len, nan_spellings)) {
		*out = NAN;
		gtext_allocator_free(alloc, clean);
		return true;
	}

	if (!has_dot && !has_exp) {
		gtext_allocator_free(alloc, clean);
		return false;
	}

	/* strtod() skips leading white space, and 10.3.2's float row has none:
	   it is "[-+]? ( \. [0-9]+ | [0-9]+ ( \. [0-9]* )? ) ( [eE] [-+]? [0-9]+ )?".
	   One optional sign, then a digit or a ".", and nothing before them.  The
	   same hole parse_int_value() had, and reachable the same way. */
	{
		const char *first = clean;
		if (*first == '+' || *first == '-') first++;
		if (*first != '.' && !(*first >= '0' && *first <= '9')) {
			gtext_allocator_free(alloc, clean);
			return false;
		}
	}

	errno = 0;
	char *end = NULL;
	/* Not strtod: it reads LC_NUMERIC, and where the separator is a comma it
	   stops at the "." in "0.1", leaves *end pointing at it, and the test
	   below then calls a perfectly good float a string. */
	double parsed = gtext_number_strtod(clean, &end);
	if (errno == ERANGE || end == clean || (end && *end != '\0')) {
		gtext_allocator_free(alloc, clean);
		return false;
	}

	*out = parsed;
	gtext_allocator_free(alloc, clean);
	return true;
}

static const GTEXT_YAML_Node *deref_alias(const GTEXT_YAML_Node *node) {
	if (!node) return NULL;
	if (node->type == GTEXT_YAML_ALIAS && node->as.alias.target) {
		return node->as.alias.target;
	}
	return node;
}

static bool is_scalar_key_type(GTEXT_YAML_Node_Type type) {
	switch (type) {
		case GTEXT_YAML_STRING:
		case GTEXT_YAML_BOOL:
		case GTEXT_YAML_INT:
		case GTEXT_YAML_FLOAT:
		case GTEXT_YAML_NULL:
			return true;
		default:
			return false;
	}
}

static GTEXT_YAML_Status validate_mapping_key(
	const GTEXT_YAML_Node *key,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error
) {
	const GTEXT_YAML_Node *resolved = NULL;

	/* Nothing to check when neither option restricts anything: complex keys
	   allowed and strings not required.
	   
	   The first clause used to be inverted - "!allow_complex_keys &&
	   !require_string_keys" - which returned OK precisely when complex keys
	   were the thing being refused. allow_complex_keys = false therefore did
	   nothing on its own, and did nothing useful together with
	   require_string_keys either, since the string check refuses everything
	   the complex check would have. The option had never once had an effect.
	   
	   It survived because the wrong clause shares a condition with a right
	   one: setting both options behaves correctly, and both of the built-in
	   option sets that touch these set both. */
	if (!opts || (opts->allow_complex_keys && !opts->require_string_keys)) {
		return GTEXT_YAML_OK;
	}

	resolved = deref_alias(key);
	if (!resolved) return GTEXT_YAML_OK;

	if (!opts->allow_complex_keys && !is_scalar_key_type(resolved->type)) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Mapping keys must be scalars";
		}
		return GTEXT_YAML_E_INVALID;
	}

	if (opts->require_string_keys && resolved->type != GTEXT_YAML_STRING) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Mapping keys must be strings";
		}
		return GTEXT_YAML_E_INVALID;
	}

	return GTEXT_YAML_OK;
}

static bool scalar_equal(const GTEXT_YAML_Node *a, const GTEXT_YAML_Node *b) {
	const char *tag_a = scalar_tag_id(a);
	const char *tag_b = scalar_tag_id(b);
	if (tag_a || tag_b) {
		if (!tag_a || !tag_b) return false;
		if (strcmp(tag_a, tag_b) != 0) return false;
	}
	if (a->type != b->type) return false;
	if (a->type == GTEXT_YAML_NULL) return true;
	if (a->type == GTEXT_YAML_BOOL) return a->as.scalar.bool_value == b->as.scalar.bool_value;
	if (a->type == GTEXT_YAML_INT) return a->as.scalar.int_value == b->as.scalar.int_value;
	if (a->type == GTEXT_YAML_FLOAT) {
		if (isnan(a->as.scalar.float_value) && isnan(b->as.scalar.float_value)) return true;
		return a->as.scalar.float_value == b->as.scalar.float_value;
	}
	if (a->type == GTEXT_YAML_STRING) {
		if (a->as.scalar.length != b->as.scalar.length) return false;
		return memcmp(a->as.scalar.value, b->as.scalar.value, a->as.scalar.length) == 0;
	}
	return false;
}

/* One frame of nodes_equal()'s comparison, which used to be a C stack frame.
 *
 * @a and @b are the pair being compared, normalised through deref_alias()
 * once the frame leaves YAML_EQ_ENTER.  @i walks @a's children or pairs; @j
 * walks @b's pairs looking for one that matches pair @i, because a mapping's
 * pairs are unordered and the match is a search rather than a zip.
 *
 * @guarded says this frame's (@a, @b) counts as an assumption that the two are
 * equal.  See nodes_equal(). */
typedef struct {
	const GTEXT_YAML_Node *a;
	const GTEXT_YAML_Node *b;
	size_t depth;
	size_t i;
	size_t j;
	unsigned char stage;
	bool guarded;
} yaml_eq_frame;

enum {
	YAML_EQ_ENTER = 0,     /* normalise, decide the arm, set the cursors */
	YAML_EQ_SEQ,           /* children, in order, all of them */
	YAML_EQ_MAP_NEXT,      /* on to a's next pair */
	YAML_EQ_MAP_TRY,       /* try b's pair j as the match for a's pair i */
	YAML_EQ_MAP_KEY_DONE,  /* the keys' answer is in */
	YAML_EQ_MAP_VALUE_DONE /* the values' answer is in */
};

/* Is (@a, @b) already being compared further up @frames?
 *
 * Only guarded frames count, which is every collection frame that got past
 * YAML_EQ_ENTER: a scalar frame answers immediately and never has anything
 * below it. */
static bool eq_pair_on_path(
	const yaml_eq_frame *frames,
	size_t count,
	const GTEXT_YAML_Node *a,
	const GTEXT_YAML_Node *b
) {
	for (size_t k = 0; k < count; k++) {
		if (frames[k].guarded && frames[k].a == a && frames[k].b == b) {
			return true;
		}
	}
	return false;
}

/* Structural equality, with its stack on the heap and cycles terminated.
 *
 * This was the fifth recursive walk over a DOM in this library and the last
 * one on the path `max_depth = SIZE_MAX` opens.  resolve_node(),
 * gtext_yaml_node_clone() and write_node() were each converted after a crash,
 * and this one had not crashed yet - its frames are smaller, so it took a
 * deeper document to reach the end of the stack, which is the whole reason it
 * outlived them.  Two documents found it, and they are *different* failures:
 *
 *     ? [[[ ... a hundred thousand of them ... ]]]
 *     : 0
 *     ? [[[ ... the same again ... ]]]
 *     : 1
 *
 * is too deep.  This one is not deep at all and does not terminate:
 *
 *     ? &x [*x]
 *     : 1
 *     ? &y [*y]
 *     : 2
 *
 * deref_alias() resolves each alias to its own enclosing sequence, so the pair
 * (x, y) recurs and the comparison asks the same question for ever.  A bigger
 * stack does not help the second one, and that is how the two were told apart:
 * a gigabyte of stack overflowed in the same place.  Both documents are legal
 * YAML and both parse on their own.
 *
 * So: the frames are on the heap, and **a pair already being compared is
 * answered "equal"**.  That is the coinductive reading of structural equality
 * - the greatest fixpoint - and it is the right one for a graph that may have
 * cycles: assuming a == b and then proving a == b from that assumption is a
 * proof of bisimilarity, and it makes `&x [*x]` properly equal to `&y [*y]`,
 * which they are.  The assumption lives exactly as long as the frame that made
 * it, which is what makes it safe under the mapping arm's backtracking - an
 * assumption made while trying a candidate pair that then fails goes away with
 * the frame that tried it.
 *
 * The path is searched linearly, and only when deref_alias() actually moved a
 * pointer, because an alias is the only way back round to a node already being
 * compared.  A parsed document cannot contain itself - apply_merge_keys()
 * refuses the merge that would have done it - and a built one cannot either,
 * because every DOM constructor that *adds* a child (sequence_append,
 * sequence_insert, mapping_set) returns a new node rather than mutating one,
 * so a node's children are fixed when it is created and the only in-place
 * operations remove.  So an ordinary deep document pays nothing for the check,
 * and one with k aliases on a path pays k comparisons per step.
 *
 * @p max_depth keeps its old meaning: a bound at which the answer is "not
 * equal" rather than an error.  It is unreachable from a parsed document,
 * since the parser has already refused anything deeper, and it is 0 - no
 * bound - at the one door that has no options to read.  It is no longer what
 * stands between this function and the end of the stack.
 *
 * @p out_failed - optional - says the comparison could not be made at all,
 * which here means the frame stack could not grow.  It is not an answer, and a
 * caller that reads it as one gets the dangerous reading in one of the two
 * directions: "not equal" lets a duplicate key through at the omap door and
 * hides one at the dupkey door. */
static bool nodes_equal(
	const GTEXT_YAML_Node *a,
	const GTEXT_YAML_Node *b,
	size_t depth,
	size_t max_depth,
	bool *out_failed
) {
	if (out_failed) *out_failed = false;

	/* Enough for any document that was parsed with the default limit, so the
	   common case never allocates.  No allocator is threaded in: this is a
	   predicate with no document in hand whose scratch is transient, the same
	   reason gtext_yaml_plain_text_resolves_to_non_string_as() gives. */
	yaml_eq_frame inline_frames[320];
	yaml_eq_frame *frames = inline_frames;
	size_t capacity = sizeof(inline_frames) / sizeof(inline_frames[0]);
	size_t count = 0;
	bool failed = false;
	/* The result of the frame that most recently popped. */
	bool answer = false;

	frames[0].a = a;
	frames[0].b = b;
	frames[0].depth = depth;
	frames[0].i = 0;
	frames[0].j = 0;
	frames[0].stage = YAML_EQ_ENTER;
	frames[0].guarded = false;
	count = 1;

	/* Writes to the frame being left must happen before the push, because the
	   push can move the array. */
	#define YAML_EQ_PUSH(na, nb, nd)                                           \
		do {                                                                   \
			if (count == capacity) {                                           \
				size_t new_capacity = capacity * 2;                            \
				yaml_eq_frame *grown;                                          \
				if (frames == inline_frames) {                                 \
					grown = (yaml_eq_frame *)gtext_allocator_malloc(           \
						NULL, new_capacity * sizeof(*grown));                  \
					if (grown) memcpy(grown, frames, count * sizeof(*grown));  \
				}                                                              \
				else {                                                         \
					grown = (yaml_eq_frame *)gtext_allocator_realloc(          \
						NULL, frames, new_capacity * sizeof(*grown));          \
				}                                                              \
				if (!grown) { failed = true; goto done; }                      \
				frames = grown;                                                \
				capacity = new_capacity;                                       \
			}                                                                  \
			frames[count].a = (na);                                            \
			frames[count].b = (nb);                                            \
			frames[count].depth = (nd);                                        \
			frames[count].i = 0;                                               \
			frames[count].j = 0;                                               \
			frames[count].stage = YAML_EQ_ENTER;                               \
			frames[count].guarded = false;                                     \
			count++;                                                           \
		} while (0)

	while (count > 0) {
		yaml_eq_frame *f = &frames[count - 1];

		switch (f->stage) {
			case YAML_EQ_ENTER: {
				const GTEXT_YAML_Node *x = deref_alias(f->a);
				const GTEXT_YAML_Node *y = deref_alias(f->b);
				const bool moved = (x != f->a) || (y != f->b);
				f->a = x;
				f->b = y;

				if (x == y) { answer = true; count--; continue; }
				if (!x || !y) { answer = false; count--; continue; }
				if (max_depth > 0 && f->depth >= max_depth) {
					answer = false; count--; continue;
				}
				if (x->type != y->type) { answer = false; count--; continue; }

				switch (x->type) {
					case GTEXT_YAML_STRING:
					case GTEXT_YAML_BOOL:
					case GTEXT_YAML_INT:
					case GTEXT_YAML_FLOAT:
					case GTEXT_YAML_NULL:
						answer = scalar_equal(x, y); count--; continue;

					case GTEXT_YAML_SEQUENCE:
					case GTEXT_YAML_OMAP:
					case GTEXT_YAML_PAIRS:
						if (x->as.sequence.count != y->as.sequence.count) {
							answer = false; count--; continue;
						}
						if (moved && eq_pair_on_path(frames, count - 1, x, y)) {
							answer = true; count--; continue;
						}
						f->guarded = true;
						f->stage = YAML_EQ_SEQ;
						continue;

					case GTEXT_YAML_MAPPING:
					case GTEXT_YAML_SET:
						if (x->as.mapping.count != y->as.mapping.count) {
							answer = false; count--; continue;
						}
						if (moved && eq_pair_on_path(frames, count - 1, x, y)) {
							answer = true; count--; continue;
						}
						f->guarded = true;
						f->stage = YAML_EQ_MAP_NEXT;
						continue;

					default:
						answer = false; count--; continue;
				}
			}

			case YAML_EQ_SEQ:
				/* Every child, in order.  On re-entry @answer is the previous
				   child's verdict, and one "no" ends the whole question. */
				if (f->i > 0 && !answer) { count--; continue; }
				if (f->i >= f->a->as.sequence.count) {
					answer = true; count--; continue;
				}
				{
					const GTEXT_YAML_Node *ca = f->a->as.sequence.children[f->i];
					const GTEXT_YAML_Node *cb = f->b->as.sequence.children[f->i];
					const size_t nd = f->depth + 1;
					f->i++;
					YAML_EQ_PUSH(ca, cb, nd);
				}
				continue;

			case YAML_EQ_MAP_NEXT:
				if (f->i >= f->a->as.mapping.count) {
					answer = true; count--; continue;
				}
				f->j = 0;
				f->stage = YAML_EQ_MAP_TRY;
				continue;

			case YAML_EQ_MAP_TRY:
				/* No pair of @b matched @a's pair @i, so the two mappings hold
				   different pairs. */
				if (f->j >= f->b->as.mapping.count) {
					answer = false; count--; continue;
				}
				{
					const GTEXT_YAML_Node *ka = f->a->as.mapping.pairs[f->i].key;
					const GTEXT_YAML_Node *kb = f->b->as.mapping.pairs[f->j].key;
					const size_t nd = f->depth + 1;
					f->stage = YAML_EQ_MAP_KEY_DONE;
					YAML_EQ_PUSH(ka, kb, nd);
				}
				continue;

			case YAML_EQ_MAP_KEY_DONE:
				if (!answer) { f->j++; f->stage = YAML_EQ_MAP_TRY; continue; }
				{
					const GTEXT_YAML_Node *va = f->a->as.mapping.pairs[f->i].value;
					const GTEXT_YAML_Node *vb = f->b->as.mapping.pairs[f->j].value;
					const size_t nd = f->depth + 1;
					f->stage = YAML_EQ_MAP_VALUE_DONE;
					YAML_EQ_PUSH(va, vb, nd);
				}
				continue;

			case YAML_EQ_MAP_VALUE_DONE:
				/* The keys matched and the values did not, so this candidate
				   is not the match; another pair of @b may still be. */
				if (!answer) { f->j++; f->stage = YAML_EQ_MAP_TRY; continue; }
				f->i++;
				f->stage = YAML_EQ_MAP_NEXT;
				continue;

			default:
				answer = false; count--; continue;
		}
	}

done:
	#undef YAML_EQ_PUSH
	if (frames != inline_frames) gtext_allocator_free(NULL, frames);
	if (failed) {
		if (out_failed) *out_failed = true;
		return false;
	}
	return answer;
}

typedef struct {
	GTEXT_YAML_Node *key;
	GTEXT_YAML_Node *value;
	const char *key_tag;
	const char *value_tag;
	bool from_merge;
} yaml_merge_pair;

typedef struct {
	GTEXT_YAML_Node *old_node;
	GTEXT_YAML_Node *new_node;
} yaml_merge_replacement;

/* The outcome of a reachability question that can itself run out of memory.
   "No" and "could not tell" are different answers and a caller must not read
   one as the other, which a bool return would force it to. */
typedef enum {
	YAML_REACH_NO = 0,
	YAML_REACH_YES,
	YAML_REACH_OOM
} yaml_reach_result;

/* Does any node in @p roots contain @p target, at any depth, counting a root
   that *is* the target?
 *
 * Containment only: an alias is a leaf here, exactly as it is in
 * update_alias_targets() below, because the graph whose cycles matter is the
 * one those walks follow.  An alias to an ancestor is ordinary YAML and is not
 * a cycle in this graph; a mapping that contains itself is.
 *
 * The seen set is shared across all of @p roots, so the whole question costs
 * one visit per reachable node rather than one per root. */
static yaml_reach_result nodes_reach_node(
	GTEXT_YAML_Node *const *roots,
	size_t root_count,
	const GTEXT_YAML_Node *target,
	const GTEXT_Allocator *alloc
) {
	if (!target || root_count == 0) return YAML_REACH_NO;

	GTEXT_YAML_Node **stack = NULL;
	size_t count = 0, capacity = 0;
	GTEXT_YAML_Node_Set seen;
	gtext_yaml_node_set_init(&seen, alloc);
	yaml_reach_result result = YAML_REACH_NO;
	bool oom = false;

	#define YAML_REACH_PUSH(n)                                                 \
		do {                                                                   \
			GTEXT_YAML_Node *push_node = (n);                                  \
			if (push_node) {                                                   \
				if (push_node == target) { result = YAML_REACH_YES; goto done; } \
				if (count == capacity) {                                        \
					size_t new_capacity = capacity == 0 ? 32 : capacity * 2;     \
					GTEXT_YAML_Node **items = (GTEXT_YAML_Node **)              \
						gtext_allocator_realloc(alloc, stack,                    \
							new_capacity * sizeof(*items));                       \
					if (!items) { oom = true; goto done; }                       \
					stack = items;                                              \
					capacity = new_capacity;                                    \
				}                                                               \
				stack[count++] = push_node;                                     \
			}                                                                   \
		} while (0)

	for (size_t i = 0; i < root_count; i++) YAML_REACH_PUSH(roots[i]);

	while (count > 0) {
		GTEXT_YAML_Node *node = stack[--count];

		if (node->type == GTEXT_YAML_ALIAS) continue;

		/* Only a collection can be arrived at twice, and only a collection has
		   children to enumerate, so the table holds collections alone. */
		if (!gtext_yaml_node_set_add(&seen, node, &oom)) {
			if (oom) goto done;
			continue;
		}

		switch (node->type) {
			case GTEXT_YAML_SEQUENCE:
			case GTEXT_YAML_OMAP:
			case GTEXT_YAML_PAIRS:
				for (size_t i = 0; i < node->as.sequence.count; i++) {
					YAML_REACH_PUSH(node->as.sequence.children[i]);
				}
				break;
			case GTEXT_YAML_MAPPING:
			case GTEXT_YAML_SET:
				for (size_t i = 0; i < node->as.mapping.count; i++) {
					YAML_REACH_PUSH(node->as.mapping.pairs[i].key);
					YAML_REACH_PUSH(node->as.mapping.pairs[i].value);
				}
				break;
			default:
				break;
		}
	}

done:
	#undef YAML_REACH_PUSH
	if (oom) result = YAML_REACH_OOM;
	gtext_allocator_free(alloc, stack);
	gtext_yaml_node_set_free(&seen);
	return result;
}

static bool is_merge_key(const GTEXT_YAML_Node *key) {
	key = deref_alias(key);
	if (!key) return false;
	if (key->type != GTEXT_YAML_STRING) return false;

	/* An explicit "!!merge" tag says so whatever the style. */
	const char *suffix = tag_suffix(key->as.scalar.tag);
	if (suffix && strcmp(suffix, "merge") == 0) return true;

	/* Otherwise the key is a merge key because its *contents* resolve to
	   tag:yaml.org,2002:merge - and only a plain scalar is resolved by its
	   contents (10.3.2).  '"<<"' is the two-character string, which is what
	   both references say, and taking it for a merge key was the usual two
	   faults at once: '{"<<": 1}' was refused for a merge value that is not a
	   mapping, and '{"<<": {a: 1}}' was *merged* - the key vanished and its
	   contents were spliced into the mapping around it, with nothing
	   reported. */
	if (key->as.scalar.scalar_style != GTEXT_YAML_SCALAR_STYLE_PLAIN) {
		return false;
	}
	if (key->as.scalar.value && strcmp(key->as.scalar.value, "<<") == 0) return true;

	return false;
}

static bool merge_pairs_grow(
	yaml_merge_pair **pairs,
	const size_t *count,
	size_t *capacity,
	const GTEXT_Allocator *alloc
) {
	if (*count < *capacity) return true;
	size_t new_cap = *capacity == 0 ? 8 : *capacity * 2;
	yaml_merge_pair *new_pairs = (yaml_merge_pair *)gtext_allocator_realloc(alloc,
		*pairs, new_cap * sizeof(yaml_merge_pair)
	);
	if (!new_pairs) return false;
	*pairs = new_pairs;
	*capacity = new_cap;
	return true;
}

/* The index of the pair whose key equals @p key, or -1.  @p out_failed says
   the comparison could not be made, which is not the same as "no match": a
   caller that treats it as one silently drops a merge override. */
static long merge_pairs_find(
	yaml_merge_pair *pairs,
	size_t count,
	const GTEXT_YAML_Node *key,
	size_t max_depth,
	bool *out_failed
) {
	for (size_t i = 0; i < count; i++) {
		bool failed = false;
		if (nodes_equal(key, pairs[i].key, 0, max_depth, &failed)) return (long)i;
		if (failed) {
			if (out_failed) *out_failed = true;
			return -1;
		}
	}
	return -1;
}

static bool merge_pairs_add_or_replace(
	yaml_merge_pair **pairs,
	size_t *count,
	size_t *capacity,
	const GTEXT_YAML_Node *key,
	GTEXT_YAML_Node *value,
	const char *key_tag,
	const char *value_tag,
	size_t max_depth,
	bool from_merge,
	const GTEXT_Allocator *alloc
) {
	bool find_failed = false;
	long idx = merge_pairs_find(*pairs, *count, key, max_depth, &find_failed);
	/* Reported as a failure to add, which every caller already turns into
	   E_OOM - and a stack that would not grow is what the failure is. */
	if (find_failed) return false;
	if (idx >= 0) {
		yaml_merge_pair *existing = &(*pairs)[(size_t)idx];
		/* The winning pair is taken whole.  This used to overwrite the value and
		   *both tags* and leave the losing key node in place, so a pair could
		   carry one source's key with another source's key_tag.  The two keys
		   are nodes_equal, which is why nothing visible went wrong - but
		   nodes_equal stops at max_depth, so deep enough keys are only equal as
		   far as it looked, and from_merge then described half the pair.  It
		   also decides whether the cycle check in apply_merge_keys() walks this
		   key, which has to be the key that is actually stored. */
		if (from_merge || existing->from_merge) {
			existing->key = (GTEXT_YAML_Node *)key;
			existing->value = value;
			existing->key_tag = key_tag;
			existing->value_tag = value_tag;
			existing->from_merge = from_merge;
			return true;
		}
	}
	if (!merge_pairs_grow(pairs, count, capacity, alloc)) return false;
	(*pairs)[*count].key = (GTEXT_YAML_Node *)key;
	(*pairs)[*count].value = value;
	(*pairs)[*count].key_tag = key_tag;
	(*pairs)[*count].value_tag = value_tag;
	(*pairs)[*count].from_merge = from_merge;
	(*count)++;
	return true;
}

static GTEXT_YAML_Status merge_from_mapping(
	yaml_merge_pair **pairs,
	size_t *count,
	size_t *capacity,
	const GTEXT_YAML_Node *source,
	size_t max_depth,
	GTEXT_YAML_Error *error,
	const GTEXT_Allocator *alloc
) {
	if (!source || source->type != GTEXT_YAML_MAPPING) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Merge source is not a mapping";
		}
		return GTEXT_YAML_E_INVALID;
	}

	for (size_t i = 0; i < source->as.mapping.count; i++) {
		const yaml_mapping_pair *pair = &source->as.mapping.pairs[i];
		if (!pair->key || is_merge_key(pair->key)) continue;
		if (!merge_pairs_add_or_replace(
			pairs,
			count,
			capacity,
			pair->key,
			pair->value,
			pair->key_tag,
			pair->value_tag,
			max_depth,
			true,
			alloc
		)) {
			if (error) {
				error->code = GTEXT_YAML_E_OOM;
				error->message = "Out of memory merging mapping";
			}
			return GTEXT_YAML_E_OOM;
		}
	}

	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status apply_merge_keys(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Node **out_node,
	bool *out_replaced,
	GTEXT_YAML_Error *error
) {
	*out_node = node;
	*out_replaced = false;
	if (!node || node->type != GTEXT_YAML_MAPPING) return GTEXT_YAML_OK;

	bool has_merge = false;
	for (size_t i = 0; i < node->as.mapping.count; i++) {
		yaml_mapping_pair *pair = &node->as.mapping.pairs[i];
		if (pair->key && is_merge_key(pair->key)) {
			has_merge = true;
			break;
		}
	}

	if (!has_merge) return GTEXT_YAML_OK;
	if (opts && !opts->allow_merge_keys) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Merge keys are disabled by parse options";
		}
		return GTEXT_YAML_E_INVALID;
	}

	doc->has_merge_keys = true;

	yaml_merge_pair *merged_pairs = NULL;
	size_t merged_count = 0;
	size_t merged_capacity = 0;
	GTEXT_YAML_Status st = GTEXT_YAML_OK;

	for (size_t i = 0; i < node->as.mapping.count; i++) {
		yaml_mapping_pair *pair = &node->as.mapping.pairs[i];
		if (!pair->key || !is_merge_key(pair->key)) continue;
		const GTEXT_YAML_Node *value = deref_alias(pair->value);
		if (!value) continue;

		if (value->type == GTEXT_YAML_MAPPING) {
			st = merge_from_mapping(
				&merged_pairs,
				&merged_count,
				&merged_capacity,
				value,
				opts ? opts->max_depth : 0,
				error,
				doc->ctx->alloc
			);
			if (st != GTEXT_YAML_OK) break;
		} else if (value->type == GTEXT_YAML_SEQUENCE) {
			for (size_t j = 0; j < value->as.sequence.count; j++) {
				const GTEXT_YAML_Node *item = deref_alias(value->as.sequence.children[j]);
				st = merge_from_mapping(
					&merged_pairs,
					&merged_count,
					&merged_capacity,
					item,
					opts ? opts->max_depth : 0,
					error,
					doc->ctx->alloc
				);
				if (st != GTEXT_YAML_OK) break;
			}
			if (st != GTEXT_YAML_OK) break;
		} else {
			if (error) {
				error->code = GTEXT_YAML_E_INVALID;
				error->message = "Merge value must be mapping or sequence of mappings";
			}
			st = GTEXT_YAML_E_INVALID;
			break;
		}
	}

	if (st != GTEXT_YAML_OK) {
		gtext_allocator_free(doc->ctx->alloc, merged_pairs);
		return st;
	}

	for (size_t i = 0; i < node->as.mapping.count; i++) {
		yaml_mapping_pair *pair = &node->as.mapping.pairs[i];
		if (!pair->key || is_merge_key(pair->key)) continue;
		if (!merge_pairs_add_or_replace(
			&merged_pairs,
			&merged_count,
			&merged_capacity,
			pair->key,
			pair->value,
			pair->key_tag,
			pair->value_tag,
			opts ? opts->max_depth : 0,
			false,
			doc->ctx->alloc
		)) {
			gtext_allocator_free(doc->ctx->alloc, merged_pairs);
			if (error) {
				error->code = GTEXT_YAML_E_OOM;
				error->message = "Out of memory merging mapping";
			}
			return GTEXT_YAML_E_OOM;
		}
	}

	/* A merge must not make the mapping contain itself.
	 *
	 * The spliced pairs are shared by pointer with the merge source, so if the
	 * source is this mapping's own ancestor - which a merge value that is an
	 * alias to an enclosing anchor reaches in one step - the document
	 * afterwards has a cycle in it.  Twenty-two bytes did this:
	 *
	 *     &O
	 *     :: - <:
	 *     <<
	 *     - <::
	 *     *O
	 *
	 * and the process died in update_alias_targets() below, which was a
	 * recursion.  That walk is iterative and cycle-safe now, and the writer
	 * and the accessors are not: a cyclic DOM handed back to a caller is a
	 * segmentation fault in their code, so the place to stop it is here,
	 * before the document exists.
	 *
	 * The question asked is the exact one: does any edge this splice is about
	 * to add lead back to the mapping the edge starts from.  The pairs the
	 * document wrote itself are already this mapping's children, so in a graph
	 * that is still acyclic they cannot reach it, and only the pairs that came
	 * from a merge are walked.  Nothing here rejects an *alias* to an ancestor
	 * - that is ordinary YAML, and it is not containment. */
	for (size_t i = 0; i < merged_count; i++) {
		if (!merged_pairs[i].from_merge) continue;
		GTEXT_YAML_Node *ends[2] = {merged_pairs[i].key, merged_pairs[i].value};
		yaml_reach_result reach = nodes_reach_node(ends, 2, node, doc->ctx->alloc);
		if (reach == YAML_REACH_NO) continue;
		gtext_allocator_free(doc->ctx->alloc, merged_pairs);
		if (error) {
			if (reach == YAML_REACH_OOM) {
				error->code = GTEXT_YAML_E_OOM;
				error->message = "Out of memory checking a merge for a cycle";
			}
			else {
				error->code = GTEXT_YAML_E_INVALID;
				error->message = "Merge key would make a mapping contain itself";
			}
		}
		return reach == YAML_REACH_OOM ? GTEXT_YAML_E_OOM : GTEXT_YAML_E_INVALID;
	}
	if (merged_count <= node->as.mapping.count) {
		for (size_t i = 0; i < merged_count; i++) {
			node->as.mapping.pairs[i].key = merged_pairs[i].key;
			node->as.mapping.pairs[i].value = merged_pairs[i].value;
			node->as.mapping.pairs[i].key_tag = merged_pairs[i].key_tag;
			node->as.mapping.pairs[i].value_tag = merged_pairs[i].value_tag;
		}
		node->as.mapping.count = merged_count;
		gtext_allocator_free(doc->ctx->alloc, merged_pairs);
		*out_node = node;
		*out_replaced = false;
		return GTEXT_YAML_OK;
	}

	GTEXT_YAML_Node *merged = yaml_node_new_mapping(
		doc->ctx,
		merged_count,
		node->as.mapping.tag,
		node->as.mapping.anchor
	);
	if (!merged) {
		gtext_allocator_free(doc->ctx->alloc, merged_pairs);
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory creating merged mapping";
		}
		return GTEXT_YAML_E_OOM;
	}

	for (size_t i = 0; i < merged_count; i++) {
		merged->as.mapping.pairs[i].key = merged_pairs[i].key;
		merged->as.mapping.pairs[i].value = merged_pairs[i].value;
		merged->as.mapping.pairs[i].key_tag = merged_pairs[i].key_tag;
		merged->as.mapping.pairs[i].value_tag = merged_pairs[i].value_tag;
	}
	merged->as.mapping.count = merged_count;

	gtext_allocator_free(doc->ctx->alloc, merged_pairs);
	*out_node = merged;
	*out_replaced = true;
	return GTEXT_YAML_OK;
}

/* Repoint every alias whose target was replaced by a merge.
 *
 * This was a recursion over the whole document, and 22 bytes of YAML ended the
 * process:
 *
 *     &O
 *     :: - <:
 *     <<
 *     - <::
 *     *O
 *
 * with stock options - allow_merge_keys defaults to true - in every schema and
 * both dialects.  resolve_node() below was converted from recursion to a heap
 * stack for exactly this reason, and says so in its own comment; this walk is
 * called from the same function thirty lines further down and stayed a
 * recursion over the same tree.
 *
 * **But depth is only half of it, and the smaller half.** A merge key whose
 * value dereferences to one of its own ancestors splices that ancestor's pairs
 * into a descendant, and the pairs are copied by pointer - so the document
 * afterwards contains a cycle.  This walk was not running out of stack because
 * the document was deep.  It was not terminating.  A one-gigabyte stack
 * overflows in the same place, which is how that was established and is the
 * only way to tell the two apart from a trace that just repeats.
 *
 * So both: the stack is on the heap, and the walk carries the set of nodes it
 * has already visited.  The set is the part that matters, and a depth cap
 * would not have done instead - deep documents are supported here deliberately
 * (see resolve_node), so any cap large enough to keep them working is also
 * large enough to spend a long time going round a cycle.
 *
 * Repointing is idempotent, so a global set rather than a path is right: a DAG
 * built by ordinary aliases is walked once per node instead of once per path
 * into it, which is a side benefit and not the reason.
 *
 * apply_merge_keys() now refuses the merge that made the cycle, so this walk
 * should no longer be able to meet one.  The set stays: it is what the walk
 * costs in a DAG anyway, and "should no longer be able to" is a claim about
 * another function thirteen hundred lines up, which is not the sort of claim a
 * walk over a caller's data should rest its termination on. */
static GTEXT_YAML_Status update_alias_targets(
	GTEXT_YAML_Node *root,
	yaml_merge_replacement *replacements,
	size_t replacement_count,
	const GTEXT_Allocator *alloc
) {
	if (!root) return GTEXT_YAML_OK;

	GTEXT_YAML_Node **stack = NULL;
	size_t count = 0, capacity = 0;
	GTEXT_YAML_Node_Set seen;
	gtext_yaml_node_set_init(&seen, alloc);
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	bool oom = false;

	#define YAML_UAT_PUSH(n)                                                   \
		do {                                                                   \
			GTEXT_YAML_Node *push_node = (n);                                  \
			if (push_node) {                                                   \
				if (count == capacity) {                                        \
					size_t new_capacity = capacity == 0 ? 32 : capacity * 2;     \
					GTEXT_YAML_Node **items = (GTEXT_YAML_Node **)              \
						gtext_allocator_realloc(alloc, stack,                    \
							new_capacity * sizeof(*items));                       \
					if (!items) { oom = true; goto done; }                       \
					stack = items;                                              \
					capacity = new_capacity;                                    \
				}                                                               \
				stack[count++] = push_node;                                     \
			}                                                                   \
		} while (0)

	YAML_UAT_PUSH(root);

	while (count > 0) {
		GTEXT_YAML_Node *node = stack[--count];

		if (node->type == GTEXT_YAML_ALIAS) {
			/* A leaf for this walk: an alias's target is not descended into,
			   which is what keeps an ordinary alias to an ancestor from being
			   a cycle at all. */
			if (node->as.alias.target) {
				for (size_t i = 0; i < replacement_count; i++) {
					if (node->as.alias.target == replacements[i].old_node) {
						node->as.alias.target = replacements[i].new_node;
						break;
					}
				}
			}
			continue;
		}

		/* Only collections can be revisited, and only they are worth the
		   table's memory. */
		if (!gtext_yaml_node_set_add(&seen, node, &oom)) {
			if (oom) goto done;
			continue;
		}

		switch (node->type) {
			case GTEXT_YAML_SEQUENCE:
			case GTEXT_YAML_OMAP:
			case GTEXT_YAML_PAIRS:
				for (size_t i = 0; i < node->as.sequence.count; i++) {
					YAML_UAT_PUSH(node->as.sequence.children[i]);
				}
				break;
			case GTEXT_YAML_MAPPING:
			case GTEXT_YAML_SET:
				for (size_t i = 0; i < node->as.mapping.count; i++) {
					YAML_UAT_PUSH(node->as.mapping.pairs[i].key);
					YAML_UAT_PUSH(node->as.mapping.pairs[i].value);
				}
				break;
			default:
				break;
		}
	}

done:
	#undef YAML_UAT_PUSH
	if (oom) status = GTEXT_YAML_E_OOM;
	gtext_allocator_free(alloc, stack);
	gtext_yaml_node_set_free(&seen);
	return status;
}

static void mapping_remove_pair(GTEXT_YAML_Node *node, size_t index) {
	if (!node || (node->type != GTEXT_YAML_MAPPING && node->type != GTEXT_YAML_SET)) return;
	if (index >= node->as.mapping.count) return;
	for (size_t i = index; i + 1 < node->as.mapping.count; i++) {
		node->as.mapping.pairs[i] = node->as.mapping.pairs[i + 1];
	}
	if (node->as.mapping.count > 0) {
		node->as.mapping.count--;
	}
}

GTEXT_INTERNAL_API GTEXT_YAML_Status gtext_yaml_emit_warning(
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Warning_Code code,
	const char *message,
	GTEXT_YAML_Error *error
) {
	if (!opts) return GTEXT_YAML_OK;
	if (opts->warning_mask & GTEXT_YAML_WARNING_MASK(code)) return GTEXT_YAML_OK;

	GTEXT_YAML_Warning warning;
	warning.code = code;
	warning.message = message;
	warning.offset = 0;
	warning.line = 0;
	warning.col = 0;

	if (opts->warning_callback) {
		opts->warning_callback(&warning, opts->warning_user_data);
	}

	if (opts->warnings_as_errors) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = message;
		}
		return GTEXT_YAML_E_INVALID;
	}

	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status warn_yaml_1_1_scalars(
	const char *value,
	size_t len,
	bool json_only,
	bool allow_underscore,
	bool allow_base_prefix,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error,
	const GTEXT_Allocator *alloc
) {
	if (!opts) return GTEXT_YAML_OK;
	if (opts->yaml_1_1) return GTEXT_YAML_OK;
	if (json_only) return GTEXT_YAML_OK;

	bool dummy = false;
	bool yaml_1_1 = true;
	if (parse_bool_value(value, len, json_only, yaml_1_1, &dummy)) {
		if (!parse_bool_value(value, len, json_only, false, &dummy)) {
			GTEXT_YAML_Status st = gtext_yaml_emit_warning(
				opts,
				GTEXT_YAML_WARNING_YAML11_BOOL,
				"YAML 1.1 boolean value in YAML 1.2 mode",
				error
			);
			if (st != GTEXT_YAML_OK) return st;
		}
	}

	double sexa = 0.0;
	bool sexa_is_int = false;
	if (parse_sexagesimal_value(value, len, allow_underscore, &sexa, &sexa_is_int)) {
		GTEXT_YAML_Status st = gtext_yaml_emit_warning(
			opts,
			GTEXT_YAML_WARNING_YAML11_SEXAGESIMAL,
			"YAML 1.1 sexagesimal value in YAML 1.2 mode",
			error
		);
		if (st != GTEXT_YAML_OK) return st;
	}

	if (has_disallowed_leading_zero(value, len, allow_underscore, alloc)) {
		int64_t out = 0;
		if (parse_int_value(value, len, allow_underscore, allow_base_prefix,
				true, true, &out, alloc)) {
			GTEXT_YAML_Status st = gtext_yaml_emit_warning(
				opts,
				GTEXT_YAML_WARNING_YAML11_OCTAL,
				"YAML 1.1 octal value in YAML 1.2 mode",
				error
			);
			if (st != GTEXT_YAML_OK) return st;
		}
	}

	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status apply_dupkey_policy(
	const GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error
) {
	if (!doc || !node || !opts) return GTEXT_YAML_OK;
	if (node->type != GTEXT_YAML_MAPPING && node->type != GTEXT_YAML_SET) return GTEXT_YAML_OK;
	if (node->as.mapping.count < 2) return GTEXT_YAML_OK;

	for (size_t i = 0; i < node->as.mapping.count; i++) {
		for (size_t j = i + 1; j < node->as.mapping.count; j++) {
			bool failed = false;
			const bool same = nodes_equal(
				node->as.mapping.pairs[i].key, node->as.mapping.pairs[j].key,
				0, opts->max_depth, &failed
			);
			if (failed) {
				if (error) {
					error->code = GTEXT_YAML_E_OOM;
					error->message = "Out of memory comparing mapping keys";
				}
				return GTEXT_YAML_E_OOM;
			}
			if (!same) {
				continue;
			}
			switch (opts->dupkeys) {
				case GTEXT_YAML_DUPKEY_ERROR:
					if (error) {
						error->code = GTEXT_YAML_E_DUPKEY;
						error->message = "Duplicate mapping key";
					}
					return GTEXT_YAML_E_DUPKEY;
				case GTEXT_YAML_DUPKEY_FIRST_WINS:
					{
						GTEXT_YAML_Status warn = gtext_yaml_emit_warning(
							opts,
							GTEXT_YAML_WARNING_DUPLICATE_KEY,
							"Duplicate mapping key (first wins)",
							error
						);
						if (warn != GTEXT_YAML_OK) return warn;
					}
					mapping_remove_pair(node, j);
					j--;
					break;
				case GTEXT_YAML_DUPKEY_LAST_WINS:
					{
						GTEXT_YAML_Status warn = gtext_yaml_emit_warning(
							opts,
							GTEXT_YAML_WARNING_DUPLICATE_KEY,
							"Duplicate mapping key (last wins)",
							error
						);
						if (warn != GTEXT_YAML_OK) return warn;
					}
					mapping_remove_pair(node, i);
					if (i > 0) i--;
					j = i;
					break;
				case GTEXT_YAML_DUPKEY_KEEP_ALL:
					/* Both pairs stay.  The caller asked for the document
					   rather than for a mapping that can be looked up in. */
					break;
			}
		}
	}

	return GTEXT_YAML_OK;
}

/** The value of one hex digit, or -1 if it is not one. */
static int yaml_hex_value(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/**
 * @brief Decode the URI escapes in a shorthand tag's suffix.
 *
 * A suffix is made of ns-tag-chars, which exclude "!", "," and the flow
 * indicators; those are written as "%" and two hex digits and are part of the
 * tag rather than of its spelling (5.6, 6.8.2.2).  Spec example 6.26 writes
 * "!e!tag%21", and the tag it names ends in "!".
 *
 * A "%" that is not followed by two hex digits is not an escape and is copied
 * through: refusing it here would turn a tag this library has no opinion
 * about into a parse error.
 *
 * Writes at most @p len bytes and returns how many, since decoding only ever
 * shortens.
 */
static size_t tag_percent_decode(char *out, const char *in, size_t len) {
	size_t w = 0;
	for (size_t i = 0; i < len; i++) {
		if (in[i] == '%' && i + 2 < len) {
			int hi = yaml_hex_value(in[i + 1]);
			int lo = yaml_hex_value(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out[w++] = (char)((hi << 4) | lo);
				i += 2;
				continue;
			}
		}
		out[w++] = in[i];
	}
	return w;
}

/* This library spells a tag in the standard namespace "!!str" wherever it
   keeps one - the DOM constructors, the tag policy, tag_suffix(), every
   caller of gtext_yaml_node_tag().  A tag that arrives spelled as the URI
   has to be put into that spelling too: "!<tag:yaml.org,2002:str>" and
   "!!str" name one tag, and a DOM that held both answered two different
   ways when asked what tag a node carried. */
static const char *normalize_standard_tag(
	GTEXT_YAML_Document *doc,
	const char *tag
) {
	static const char yaml_prefix[] = "tag:yaml.org,2002:";
	if (!doc || !tag) return tag;
	if (strncmp(tag, yaml_prefix, sizeof(yaml_prefix) - 1) != 0) return tag;
	const char *suffix = tag + (sizeof(yaml_prefix) - 1);
	if (!*suffix) return tag;
	/* Only where the shorthand reads back as the same tag: 6.8.2.2 keeps
	   "!" and the flow indicators out of a suffix, so a URI carrying one of
	   them has no shorthand spelling and keeps the one it came with. */
	for (const unsigned char *c = (const unsigned char *)suffix; *c; c++) {
		if (*c == '!' || *c == ',' || *c == '['
				|| *c == ']' || *c == '{' || *c == '}') {
			return tag;
		}
	}
	size_t len = strlen(suffix);
	char *out = (char *)yaml_context_alloc(doc->ctx, len + 3, 1);
	if (!out) return tag;
	out[0] = '!';
	out[1] = '!';
	memcpy(out + 2, suffix, len + 1);
	return out;
}

static const char *resolve_tag_handle(
	GTEXT_YAML_Document *doc,
	const char *tag
) {
	if (!doc || !tag) return tag;
	/* A verbatim tag arrives still wrapped in its brackets, because that is
	   the only thing that distinguishes it: "!<!a!>" is the tag "!a!" exactly
	   as written (5.3), and a URI beginning "!" is otherwise indistinguishable
	   from a shorthand.  The brackets come off here and nothing else happens
	   to it - no handle to expand, no escapes to decode. */
	const size_t len = strlen(tag);
	if (len >= 2 && tag[0] == '<' && tag[len - 1] == '>') {
		char *bare = (char *)yaml_context_alloc(doc->ctx, len - 1, 1);
		if (!bare) return tag;
		memcpy(bare, tag + 1, len - 2);
		bare[len - 2] = '\0';
		return normalize_standard_tag(doc, bare);
	}
	/* Anything else not beginning "!" is not a shorthand and has neither a
	   handle to expand nor escapes to decode. */
	if (tag[0] != '!') return normalize_standard_tag(doc, tag);
	/* "!!" is a handle like any other and may be redefined: %TAG !! makes
	   the secondary handle mean something else for that document, and then
	   "!!int" is that tag rather than tag:yaml.org,2002:int (6.8.2.2, spec
	   example 6.19).  This used to return early on any tag beginning "!!",
	   so the directive had no effect and "!!int 1 - 3" was still checked as
	   an integer and refused.
	   Nothing changes when %TAG !! is absent: the loop below finds no
	   handle matching, the tag comes back as written, and tag_suffix()
	   reads "!!int" as the standard shorthand as before. */
	const size_t tag_len = strlen(tag);
	const char *best_prefix = NULL;
	size_t best_len = 0;

	for (size_t i = 0; doc->tag_handles && i < doc->tag_handle_count; i++) {
		const char *handle = doc->tag_handles[i].handle;
		const char *prefix = doc->tag_handles[i].prefix;
		if (!handle || !prefix) continue;
		size_t hlen = strlen(handle);
		if (hlen == 0 || hlen > tag_len) continue;
		if (strncmp(tag, handle, hlen) != 0) continue;
		if (hlen > best_len) {
			best_len = hlen;
			best_prefix = prefix;
		}
	}

	/* With no %TAG for this handle the tag keeps the spelling it was written
	   with - "!!int" stays "!!int", which is what tag_suffix() reads - and so
	   do its escapes.  Decoding them would be wrong there rather than merely
	   unhelpful: the result still begins with "!", so a "%21" would decode
	   into the very character that makes a shorthand named, and "!local%21"
	   would become "!local!", a handle no %TAG ever declared.  6.8.2.2 keeps
	   "!" out of a suffix for exactly that reason.  Where a %TAG prefix
	   applies the result is a URI and there is no handle left to confuse. */
	if (!best_prefix) return tag;

	const size_t handle_len = best_len;
	const char *prefix = best_prefix;
	const size_t prefix_len = strlen(best_prefix);
	const char *suffix = tag + handle_len;
	const size_t suffix_len = tag_len - handle_len;

	char *resolved = (char *)yaml_context_alloc(doc->ctx, prefix_len + suffix_len + 1, 1);
	if (!resolved) return tag;
	memcpy(resolved, prefix, prefix_len);
	size_t written = tag_percent_decode(resolved + prefix_len, suffix, suffix_len);
	resolved[prefix_len + written] = '\0';
	return normalize_standard_tag(doc, resolved);
}

static const GTEXT_YAML_Custom_Tag *find_custom_tag(
	const GTEXT_YAML_Parse_Options *opts,
	const char *tag
) {
	if (!opts || !tag || !opts->enable_custom_tags) return NULL;
	if (!opts->custom_tags || opts->custom_tag_count == 0) return NULL;
	for (size_t i = 0; i < opts->custom_tag_count; i++) {
		if (!opts->custom_tags[i].tag) continue;
		if (strcmp(opts->custom_tags[i].tag, tag) == 0) {
			return &opts->custom_tags[i];
		}
	}
	return NULL;
}

static GTEXT_YAML_Status apply_custom_tag_constructor(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	const char *tag,
	GTEXT_YAML_Error *error
) {
	const GTEXT_YAML_Custom_Tag *handler = find_custom_tag(opts, tag);
	if (!handler || !handler->construct) return GTEXT_YAML_OK;
	return handler->construct(doc, node, tag, handler->user, error);
}

static GTEXT_YAML_Status resolve_scalar(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error
) {
	const GTEXT_Allocator *alloc = doc ? doc->ctx->alloc : NULL;
	const char *value = NULL;
	size_t len = 0;
	const char *tag = NULL;
	const char *resolved_tag = NULL;
	GTEXT_YAML_Status tag_status = GTEXT_YAML_OK;

	if (!node || node->type == GTEXT_YAML_ALIAS) return GTEXT_YAML_OK;

	value = node->as.scalar.value;
	len = node->as.scalar.length;
	tag = node->as.scalar.tag;
	/* Read before resolving: the brackets are what say the tag was written
	   verbatim, and resolve_tag_handle() takes them off. */
	const bool tag_was_verbatim = tag && tag[0] == '<';
	resolved_tag = tag ? resolve_tag_handle(doc, tag) : NULL;
	if (resolved_tag && resolved_tag != tag) {
		node->as.scalar.tag = resolved_tag;
		tag = resolved_tag;
	}

	tag_status = enforce_tag_policy(doc, tag, tag_was_verbatim, opts, error);
	if (tag_status != GTEXT_YAML_OK) return tag_status;
	if (!opts || !opts->resolve_tags) return GTEXT_YAML_OK;

	const char *suffix = tag_suffix(tag);
	if (suffix) {
		if (strcmp(suffix, "str") == 0) {
			node->type = GTEXT_YAML_STRING;
			node->as.scalar.type = GTEXT_YAML_STRING;
			return GTEXT_YAML_OK;
		}
		if (strcmp(suffix, "bool") == 0) {
			bool out = false;
			bool yaml_1_1 = yaml_use_1_1(doc, opts);
			if (!parse_bool_value(value, len, false, yaml_1_1, &out)) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid boolean scalar for explicit tag";
				}
				return GTEXT_YAML_E_INVALID;
			}
			node->type = GTEXT_YAML_BOOL;
			node->as.scalar.type = GTEXT_YAML_BOOL;
			node->as.scalar.bool_value = out;
			return GTEXT_YAML_OK;
		}
		if (strcmp(suffix, "int") == 0) {
			int64_t out = 0;
			bool yaml_1_1 = yaml_use_1_1(doc, opts);
			if (yaml_1_1) {
				double sexa = 0.0;
				bool is_int = false;
				if (parse_sexagesimal_value(value, len, true, &sexa, &is_int)) {
					/* A fraction makes it a float and not this tag's type;
					   a whole number past int64_t has nothing to convert to,
					   and "!!int 99999999999999999999999" is already refused
					   for the same reason - strtoll() says ERANGE and
					   parse_int_value() gives up.  Converting it anyway is
					   undefined, and what it gave was INT64_MIN. */
					if (!is_int || !gtext_yaml_double_fits_int64(sexa)) {
						if (error) {
							error->code = GTEXT_YAML_E_INVALID;
							error->message = "Invalid integer scalar for explicit tag";
						}
						return GTEXT_YAML_E_INVALID;
					}
					out = (int64_t)sexa;
					node->type = GTEXT_YAML_INT;
					node->as.scalar.type = GTEXT_YAML_INT;
					node->as.scalar.int_value = out;
					return GTEXT_YAML_OK;
				}
			}
			/* An explicit "!!int" names the tag whose syntax 10.3.2 defines,
			   so the content has to be in it.  Leaving this lenient while
			   implicit resolution follows the version would make "0b101" a
			   string and "!!int 0b101" a five. */
			if (!parse_int_value(value, len, yaml_1_1, true, yaml_1_1,
					yaml_1_1, &out, alloc)) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid integer scalar for explicit tag";
				}
				return GTEXT_YAML_E_INVALID;
			}
			node->type = GTEXT_YAML_INT;
			node->as.scalar.type = GTEXT_YAML_INT;
			node->as.scalar.int_value = out;
			return GTEXT_YAML_OK;
		}
		if (strcmp(suffix, "float") == 0) {
			double out = 0.0;
			bool yaml_1_1 = yaml_use_1_1(doc, opts);
			if (yaml_1_1) {
				double sexa = 0.0;
				bool is_int = false;
				if (parse_sexagesimal_value(value, len, true, &sexa, &is_int)) {
					out = sexa;
					node->type = GTEXT_YAML_FLOAT;
					node->as.scalar.type = GTEXT_YAML_FLOAT;
					node->as.scalar.float_value = out;
					return GTEXT_YAML_OK;
				}
			}
			if (!parse_float_value(value, len, yaml_use_1_1(doc, opts), &out, alloc)) {
				/* 10.3.2's float row is
				   "[-+]? ( \. [0-9]+ | [0-9]+ ( \. [0-9]* )? ) ..." - the
				   fraction is optional, so "12" is in it.  Implicit
				   resolution still answers *int* for that text, because the
				   int row is tried first; an explicit "!!float 12" is asking
				   for the other reading and is a float of 12.  Both
				   references agree, and this refused it. */
				int64_t as_int = 0;
				if (parse_int_value(value, len, yaml_use_1_1(doc, opts), true,
						yaml_use_1_1(doc, opts), yaml_use_1_1(doc, opts),
						&as_int, alloc)) {
					out = (double)as_int;
				}
				else {
					if (error) {
						error->code = GTEXT_YAML_E_INVALID;
						error->message = "Invalid float scalar for explicit tag";
					}
					return GTEXT_YAML_E_INVALID;
				}
			}
			node->type = GTEXT_YAML_FLOAT;
			node->as.scalar.type = GTEXT_YAML_FLOAT;
			node->as.scalar.float_value = out;
			return GTEXT_YAML_OK;
		}
		if (strcmp(suffix, "null") == 0) {
			/* An explicit tag names the type whose syntax 10.3.2 defines, and
			   the null row is "~ | null | Null | NULL | <empty>" and nothing
			   else - the same reasoning the "!!int" branch already carries.
			   This took any content at all, so "!!null x" was a null and the
			   "x" went nowhere; js-yaml refuses it. */
			if (!parse_null_value(value, len, false)) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid null scalar for explicit tag";
				}
				return GTEXT_YAML_E_INVALID;
			}
			node->type = GTEXT_YAML_NULL;
			node->as.scalar.type = GTEXT_YAML_NULL;
			return GTEXT_YAML_OK;
		}
		if (strcmp(suffix, "timestamp") == 0) {
			yaml_node_scalar snapshot = node->as.scalar;
			if (!parse_timestamp(value, len, &snapshot)) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid timestamp scalar";
				}
				return GTEXT_YAML_E_INVALID;
			}
			const char *formatted = format_timestamp(doc, &snapshot);
			if (!formatted) {
				if (error) {
					error->code = GTEXT_YAML_E_OOM;
					error->message = "Out of memory normalizing timestamp";
				}
				return GTEXT_YAML_E_OOM;
			}
			node->as.scalar = snapshot;
			node->as.scalar.value = formatted;
			node->as.scalar.length = strlen(formatted);
			return GTEXT_YAML_OK;
		}
		if (strcmp(suffix, "binary") == 0) {
			const unsigned char *data = NULL;
			size_t data_len = 0;
			if (!gtext_yaml_base64_decode(doc, value, len, &data, &data_len)) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid base64 binary scalar";
				}
				return GTEXT_YAML_E_INVALID;
			}
			/* The decoded bytes are what "!!binary" means, and they are kept
			   here for gtext_yaml_node_as_binary().  The scalar's own text is
			   left exactly as it was written.
			
			   It used to be replaced with a canonical re-encoding, which threw
			   away the line breaks the author had put in: base64 in a literal
			   block scalar is written in short lines on purpose, and

			       generic: !!binary |
			         R0lGODlhDAAMAIQAAP//9/X17unp5WZmZgAAAOfn515eXvPz7Y6OjuDg4J+fn5
			         OTk6enp56enmlpaWNjY6Ojo4SEhP...

			   came back as one unbroken run.  Re-encoding also silently
			   rewrites input that decodes to the same bytes but was not
			   spelled the same way, which is not this parser's business to
			   do.  Suite case 565N. */
			node->as.scalar.has_binary = true;
			node->as.scalar.binary_data = data;
			node->as.scalar.binary_len = data_len;
			node->type = GTEXT_YAML_STRING;
			node->as.scalar.type = GTEXT_YAML_STRING;
			return GTEXT_YAML_OK;
		}

		if (opts && opts->enable_custom_tags) {
			GTEXT_YAML_Status custom = apply_custom_tag_constructor(
				doc,
				node,
				opts,
				tag,
				error
			);
			if (custom != GTEXT_YAML_OK) return custom;
		}
		return GTEXT_YAML_OK;
	}

	if (tag && tag[0] != '\0') {
		GTEXT_YAML_Status custom = apply_custom_tag_constructor(doc, node, opts, tag, error);
		if (custom != GTEXT_YAML_OK) return custom;
		return GTEXT_YAML_OK;
	}

	/* Only a plain scalar is resolved by its contents. Every other style
	 * carries the non-specific tag "!", which for a scalar resolves to
	 * tag:yaml.org,2002:str (10.3.2) - that is the whole point of quoting.
	 * The style was not being consulted at all, so 'a: "12"' came back as
	 * the integer 12 and 'a: "null"' as null. */
	if (node->as.scalar.scalar_style != GTEXT_YAML_SCALAR_STYLE_PLAIN) {
		node->type = GTEXT_YAML_STRING;
		node->as.scalar.type = GTEXT_YAML_STRING;
		return GTEXT_YAML_OK;
	}

	if (opts->schema == GTEXT_YAML_SCHEMA_FAILSAFE) {
		return GTEXT_YAML_OK;
	}

	bool json_only = opts->schema == GTEXT_YAML_SCHEMA_JSON;
	bool yaml_1_1 = yaml_use_1_1(doc, opts) && opts->schema == GTEXT_YAML_SCHEMA_CORE;
	/* "1_000" and "0b101" are YAML 1.1 int forms.  The 1.2 core schema has
	   three int rows and none of them admits either: "[-+]? [0-9]+",
	   "0o [0-7]+" and "0x [0-9a-fA-F]+".  These were on for every core-schema
	   parse, so a 1.2 document resolved them silently - and unlike the 1.1
	   bool, octal and sexagesimal forms, which do warn, nothing said so.
	   The uppercase "0O"/"0X" spellings are 1.1's too; 1.2 writes the
	   prefix in lower case. */
	bool allow_underscore = yaml_1_1;
	bool allow_base_prefix = opts->schema == GTEXT_YAML_SCHEMA_CORE;
	bool allow_binary_and_upper_prefix = yaml_1_1;

	GTEXT_YAML_Status warn = warn_yaml_1_1_scalars(
		value,
		len,
		json_only,
		allow_underscore,
		allow_base_prefix,
		opts,
		error,
		alloc
	);
	if (warn != GTEXT_YAML_OK) return warn;

	if (parse_null_value(value, len, json_only)) {
		node->type = GTEXT_YAML_NULL;
		node->as.scalar.type = GTEXT_YAML_NULL;
		return GTEXT_YAML_OK;
	}

	bool bool_out = false;
	if (parse_bool_value(value, len, json_only, yaml_1_1, &bool_out)) {
		node->type = GTEXT_YAML_BOOL;
		node->as.scalar.type = GTEXT_YAML_BOOL;
		node->as.scalar.bool_value = bool_out;
		return GTEXT_YAML_OK;
	}

	if (yaml_1_1) {
		double sexa = 0.0;
		bool is_int = false;
		/* A whole sexagesimal past int64_t is not an int this library can
		   hold, and converting it is undefined - INT64_MIN, in practice.
		   Left to the rows below, which have no sexagesimal among them, so
		   it stays the string it was written as.  That is what a decimal
		   too large for the type already resolves to. */
		if (parse_sexagesimal_value(value, len, allow_underscore, &sexa, &is_int)
				&& (!is_int || gtext_yaml_double_fits_int64(sexa))) {
			if (is_int) {
				node->type = GTEXT_YAML_INT;
				node->as.scalar.type = GTEXT_YAML_INT;
				node->as.scalar.int_value = (int64_t)sexa;
			} else {
				node->type = GTEXT_YAML_FLOAT;
				node->as.scalar.type = GTEXT_YAML_FLOAT;
				node->as.scalar.float_value = sexa;
			}
			return GTEXT_YAML_OK;
		}
	}

	int64_t int_out = 0;
	if (!yaml_1_1 && has_disallowed_leading_zero(value, len, allow_underscore, alloc)) {
		return GTEXT_YAML_OK;
	}
	if (parse_int_value(value, len, allow_underscore, allow_base_prefix,
			allow_binary_and_upper_prefix, yaml_1_1, &int_out, alloc)) {
		node->type = GTEXT_YAML_INT;
		node->as.scalar.type = GTEXT_YAML_INT;
		node->as.scalar.int_value = int_out;
		return GTEXT_YAML_OK;
	}

	double float_out = 0.0;
	if (parse_float_value(value, len, allow_underscore, &float_out, alloc)) {
		node->type = GTEXT_YAML_FLOAT;
		node->as.scalar.type = GTEXT_YAML_FLOAT;
		node->as.scalar.float_value = float_out;
		return GTEXT_YAML_OK;
	}

	return GTEXT_YAML_OK;
}

/* Would this text, written as a plain scalar, come back as something other
   than a string?
 *
 * The writer asks, because quoting is not only a matter of style: only a
 * plain scalar is resolved by its contents (10.3.2), so a string node whose
 * text happens to spell a number has to be written in quotes or it comes back
 * as the number.  gtext_yaml_node_new_scalar() makes a string of whatever it
 * is given, so a caller who builds the string "1" and writes it was getting
 * the integer 1 back.
 *
 * The 1.2 core schema is the one that matters here: the writer emits 1.2, and
 * a document written under it is read back under it.  These are the same
 * predicates resolve_scalar() uses a few lines above, not a second copy of
 * the tables. */
/**
 * @brief Whether @p value holds white space anywhere.
 *
 * Not one row of the 10.3.2 resolution table contains any: the null and
 * boolean rows are enumerations of whole words, the integer row is
 * "[-+]? [0-9]+" and its two prefixed forms, and the float rows are the same
 * shape.  So text carrying white space resolves to a string wherever it
 * stands, and the question does not have to be asked of each row separately.
 *
 * This began as a test of the two *ends* only, which was the half the writer
 * fuzzer found first: a plain scalar's content has white space at neither end
 * (ns-plain begins and ends with an ns-char, 7.3.3), so only a quoted scalar
 * could spell " 3" and quoted is string.  That left the middle, and the
 * middle was reachable: strtoll() skips leading white space, and this code
 * consumes the sign itself and hands strtoll what follows - so "+\n1" and
 * "+ 1" were the integer 1, and "0x\n10" was 16.
 */
static bool plain_text_has_space(const char *value, size_t len) {
	if (!value) return false;
	for (size_t i = 0; i < len; i++) {
		/* C's whole white-space set, not YAML's s-white.  The vertical tab
		   and the form feed are not even c-printable (5.1), so no parsed
		   scalar holds one - but the DOM API takes any char *, and strtod()
		   and strtoll() skip them exactly as they skip a space.  A scalar
		   of "\v6662." was answering "the float 6662". */
		switch (value[i]) {
			case ' ': case '\t': case '\n': case '\r': case '\v': case '\f':
				return true;
			/* And a NUL, for the mirror-image reason: white space makes the
			   C conversions read *past* where 10.3.2 ends a row, and a NUL
			   makes them stop *before* the text does.  strtoll() was handed
			   "42\0x" and answered 42, so the DOM API built an integer out
			   of a value the parser reads as the string it is - a quoted
			   "42\0" parses to a string, because no row of 10.3.2 has a NUL
			   in it and a NUL is not c-printable either.

			   The bool and null rows compare with lengths and were right
			   already; only the two that hand their text to the C library
			   were wrong, which is why "true\0" was a string while "42\0"
			   was a number. */
			case '\0':
				return true;
			default:
				break;
		}
	}
	return false;
}

GTEXT_INTERNAL_API GTEXT_YAML_Node_Type gtext_yaml_plain_text_classify_as(
	const char *value,
	size_t len,
	GTEXT_YAML_Schema schema,
	bool yaml_1_1,
	bool *bool_out,
	int64_t *int_out,
	double *float_out,
	const GTEXT_Allocator *alloc
) {
	bool b = false;
	int64_t i = 0;
	double f = 0.0;
	GTEXT_YAML_Node_Type type = GTEXT_YAML_STRING;

	/* The same flags resolve_scalar_by_text() derives, derived the same way.
	   They were spelled out a second time here with the 1.2 core answers
	   hard-coded, on the reasoning that "a document written here is not read
	   back in 1.1 mode" - which is a statement about the caller, not about
	   the library, and the writer fuzzer falsified it. */
	const bool json_only = (schema == GTEXT_YAML_SCHEMA_JSON);
	const bool v11 = yaml_1_1 && (schema == GTEXT_YAML_SCHEMA_CORE);
	const bool allow_underscore = v11;
	const bool allow_base_prefix = (schema == GTEXT_YAML_SCHEMA_CORE);
	const bool allow_binary_and_upper_prefix = v11;

	if (!value) {
		type = GTEXT_YAML_STRING;
	}
	/* The failsafe schema resolves nothing: every scalar is a string, which
	   is what asking for it means. */
	else if (schema == GTEXT_YAML_SCHEMA_FAILSAFE) {
		type = GTEXT_YAML_STRING;
	}
	else if (len == 0) {
		type = GTEXT_YAML_NULL;   /* 7.2's empty node */
	}
	else if (plain_text_has_space(value, len)) {
		type = GTEXT_YAML_STRING;
	}
	else if (parse_null_value(value, len, json_only)) {
		type = GTEXT_YAML_NULL;
	}
	else if (parse_bool_value(value, len, json_only, v11, &b)) {
		type = GTEXT_YAML_BOOL;
	}
	else if (v11 && parse_sexagesimal_value(value, len, allow_underscore, &f,
			&b)) {
		/* b is reused as "is an integer" here, the way the resolver reads it,
		   and is put back below before anything else can see it. */
		if (b && gtext_yaml_double_fits_int64(f)) {
			type = GTEXT_YAML_INT;
			i = (int64_t)f;
		}
		else if (!b) {
			type = GTEXT_YAML_FLOAT;
		}
		b = false;
	}
	else if (!v11 && has_disallowed_leading_zero(value, len, allow_underscore, alloc)) {
		type = GTEXT_YAML_STRING;
	}
	else if (parse_int_value(value, len, allow_underscore, allow_base_prefix,
			allow_binary_and_upper_prefix, v11, &i, alloc)) {
		type = GTEXT_YAML_INT;
	}
	else if (parse_float_value(value, len, allow_underscore, &f, alloc)) {
		type = GTEXT_YAML_FLOAT;
	}

	if (bool_out) *bool_out = b;
	if (int_out) *int_out = i;
	if (float_out) *float_out = f;
	return type;
}

GTEXT_INTERNAL_API GTEXT_YAML_Node_Type gtext_yaml_plain_text_classify(
	const char *value,
	size_t len,
	bool *bool_out,
	int64_t *int_out,
	double *float_out,
	const GTEXT_Allocator *alloc
) {
	/* The 1.2 core schema, which is what the DOM constructors document and
	   what this used to be the only spelling of. */
	return gtext_yaml_plain_text_classify_as(value, len,
		GTEXT_YAML_SCHEMA_CORE, false, bool_out, int_out, float_out, alloc);
}

GTEXT_INTERNAL_API GTEXT_YAML_Node_Type gtext_yaml_plain_text_type(
	const char *value,
	size_t len
) {
	/* No allocator: this is the type-only probe, used where no document is in
	   hand. Its scratch buffer is transient and never reaches a caller. */
	return gtext_yaml_plain_text_classify(value, len, NULL, NULL, NULL, NULL);
}

GTEXT_INTERNAL_API bool gtext_yaml_omap_can_take(
	const GTEXT_YAML_Node *omap,
	const GTEXT_YAML_Node *entry
) {
	/* !!omap is an ordered *mapping*: its entries are single-pair mappings
	   and its keys are unique.  The resolver holds a parsed one to both rules
	   - see the is_omap arm of resolve_collection_tags() - and the DOM
	   constructors held it to neither, so gtext_yaml_sequence_append() built
	   an omap with the key "a" twice, the writer emitted
	   '!!omap [{a: 1}, {a: 2}]', and this library refused to read back what
	   it had just written.

	   The comparison is nodes_equal(), which is the one the resolver uses, so
	   the two doors cannot drift apart on what "the same key" means. */
	const GTEXT_YAML_Node *item = deref_alias(entry);
	if (!item || item->type != GTEXT_YAML_MAPPING
			|| item->as.mapping.count != 1) {
		return false;
	}
	if (!omap) return true;
	const GTEXT_YAML_Node *key = item->as.mapping.pairs[0].key;
	for (size_t i = 0; i < omap->as.sequence.count; i++) {
		const GTEXT_YAML_Node *prev = deref_alias(omap->as.sequence.children[i]);
		if (!prev || prev->type != GTEXT_YAML_MAPPING
				|| prev->as.mapping.count != 1) {
			continue;
		}
		/* A comparison that could not be made refuses the entry.  This door
		   returns a bool and has nowhere to report a failure, and of the two
		   wrong answers available "no, it cannot take it" is the one that does
		   not put a duplicate key into an omap. */
		bool failed = false;
		if (nodes_equal(key, prev->as.mapping.pairs[0].key, 0, 0, &failed)) {
			return false;
		}
		if (failed) return false;
	}
	return true;
}

GTEXT_INTERNAL_API bool gtext_yaml_plain_text_resolves_to_non_string(
	const char *value,
	size_t len
) {
	return gtext_yaml_plain_text_type(value, len) != GTEXT_YAML_STRING;
}

GTEXT_INTERNAL_API bool gtext_yaml_plain_text_resolves_to_non_string_as(
	const char *value,
	size_t len,
	GTEXT_YAML_Schema schema,
	bool yaml_1_1
) {
	/* No allocator: a predicate with no document in hand, whose scratch is
	   transient. */
	return gtext_yaml_plain_text_classify_as(value, len, schema, yaml_1_1,
		NULL, NULL, NULL, NULL) != GTEXT_YAML_STRING;
}

GTEXT_INTERNAL_API const char *gtext_yaml_null_spelling_for(
	GTEXT_YAML_Schema schema
) {
	/* "~" is a null in 1.1 and in the 1.2 core schema and in neither of the
	   other two.  The JSON schema has exactly one null spelling and it is the
	   word; the failsafe schema has none at all, so a null cannot survive a
	   round trip through it whatever is written - the word is what a reader
	   of that document would expect to see, and is the honest thing to put
	   there. */
	return (schema == GTEXT_YAML_SCHEMA_CORE) ? "~" : "null";
}

/* Out of memory, said the same way in each of the places the walk below can
   run out of it. */
static GTEXT_YAML_Status resolve_oom(GTEXT_YAML_Error *error) {
	if (error) {
		error->code = GTEXT_YAML_E_OOM;
		error->message = "Out of memory resolving the document";
	}
	return GTEXT_YAML_E_OOM;
}

/* The work a collection needs before its children are resolved, and the work
   it needs after. Splitting them out is what lets the walk below be a loop:
   the "after" half is the reason a plain worklist will not do here, since a
   merge key can replace the mapping it was found in and a custom tag
   constructor has to see finished children. */
static GTEXT_YAML_Status resolve_enter_sequence(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error
) {
	/* Read before resolving: the brackets are what say the tag
	   was written verbatim, and resolve_tag_handle() takes
	   them off. */
	const bool was_verbatim = node->as.sequence.tag
		&& node->as.sequence.tag[0] == '<';
	if (node->as.sequence.tag) {
		node->as.sequence.tag = resolve_tag_handle(doc, node->as.sequence.tag);
	}
	{
		const char *tag = node->as.sequence.tag;
		GTEXT_YAML_Status tag_status = enforce_tag_policy(doc, tag, was_verbatim, opts, error);
		if (tag_status != GTEXT_YAML_OK) return tag_status;
	}
	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status resolve_exit_sequence(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error
) {
	{
		const char *seq_suffix = tag_suffix(node->as.sequence.tag);
		bool is_omap = seq_suffix && strcmp(seq_suffix, "omap") == 0;
		bool is_pairs = seq_suffix && strcmp(seq_suffix, "pairs") == 0;
		if (node->type == GTEXT_YAML_OMAP) {
			is_omap = true;
			is_pairs = false;
		} else if (node->type == GTEXT_YAML_PAIRS) {
			is_pairs = true;
			is_omap = false;
		}
		if (is_omap) {
			for (size_t i = 0; i < node->as.sequence.count; i++) {
				const GTEXT_YAML_Node *item = deref_alias(node->as.sequence.children[i]);
				if (!item || item->type != GTEXT_YAML_MAPPING || item->as.mapping.count != 1) {
					if (error) {
						error->code = GTEXT_YAML_E_INVALID;
						error->message = "omap entries must be single-pair mappings";
					}
					return GTEXT_YAML_E_INVALID;
				}
				const GTEXT_YAML_Node *key = item->as.mapping.pairs[0].key;
				for (size_t j = 0; j < i; j++) {
					const GTEXT_YAML_Node *prev = deref_alias(node->as.sequence.children[j]);
					if (!prev || prev->type != GTEXT_YAML_MAPPING) continue;
					bool failed = false;
					const bool same = nodes_equal(
						key, prev->as.mapping.pairs[0].key, 0,
						opts ? opts->max_depth : 0, &failed
					);
					if (failed) {
						if (error) {
							error->code = GTEXT_YAML_E_OOM;
							error->message = "Out of memory comparing omap keys";
						}
						return GTEXT_YAML_E_OOM;
					}
					if (same) {
						if (error) {
							error->code = GTEXT_YAML_E_DUPKEY;
							error->message = "omap keys must be unique";
						}
						return GTEXT_YAML_E_DUPKEY;
					}
				}
			}
			node->type = GTEXT_YAML_OMAP;
			node->as.sequence.type = GTEXT_YAML_OMAP;
		} else if (is_pairs) {
			for (size_t i = 0; i < node->as.sequence.count; i++) {
				const GTEXT_YAML_Node *item = deref_alias(node->as.sequence.children[i]);
				if (!item || item->type != GTEXT_YAML_MAPPING || item->as.mapping.count != 1) {
					if (error) {
						error->code = GTEXT_YAML_E_INVALID;
						error->message = "pairs entries must be single-pair mappings";
					}
					return GTEXT_YAML_E_INVALID;
				}
			}
			node->type = GTEXT_YAML_PAIRS;
			node->as.sequence.type = GTEXT_YAML_PAIRS;
		}
	}
	{
		const char *tag = node->as.sequence.tag;
		GTEXT_YAML_Status custom = apply_custom_tag_constructor(
			doc,
			node,
			opts,
			tag,
			error
		);
		if (custom != GTEXT_YAML_OK) return custom;
	}
	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status resolve_enter_mapping(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	GTEXT_YAML_Error *error
) {
	/* Read before resolving: the brackets are what say the tag
	   was written verbatim, and resolve_tag_handle() takes
	   them off. */
	const bool was_verbatim = node->as.mapping.tag
		&& node->as.mapping.tag[0] == '<';
	if (node->as.mapping.tag) {
		node->as.mapping.tag = resolve_tag_handle(doc, node->as.mapping.tag);
	}
	{
		const char *tag = node->as.mapping.tag;
		GTEXT_YAML_Status tag_status = enforce_tag_policy(doc, tag, was_verbatim, opts, error);
		if (tag_status != GTEXT_YAML_OK) return tag_status;
	}
	return GTEXT_YAML_OK;
}

/* node_ptr is the slot the mapping sits in, because apply_merge_keys() can
   hand back a different node and whoever is holding this one has to be told. */
static GTEXT_YAML_Status resolve_exit_mapping(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node **node_ptr,
	GTEXT_YAML_Node *node,
	const GTEXT_YAML_Parse_Options *opts,
	yaml_merge_replacement **replacements,
	size_t *replacement_count,
	size_t *replacement_capacity,
	GTEXT_YAML_Error *error
) {
	{
		GTEXT_YAML_Node *merged_node = node;
		bool replaced = false;
		GTEXT_YAML_Status st = apply_merge_keys(
			doc,
			node,
			opts,
			&merged_node,
			&replaced,
			error
		);
		if (st != GTEXT_YAML_OK) return st;
		if (replaced) {
			if (*replacement_count >= *replacement_capacity) {
				size_t new_cap = *replacement_capacity == 0 ? 4 : *replacement_capacity * 2;
				yaml_merge_replacement *new_items = (yaml_merge_replacement *)gtext_allocator_realloc(doc->ctx->alloc, 
					*replacements,
					new_cap * sizeof(yaml_merge_replacement)
				);
				if (!new_items) {
					if (error) {
						error->code = GTEXT_YAML_E_OOM;
						error->message = "Out of memory tracking merge replacements";
					}
					return GTEXT_YAML_E_OOM;
				}
				*replacements = new_items;
				*replacement_capacity = new_cap;
			}
			(*replacements)[*replacement_count].old_node = node;
			(*replacements)[*replacement_count].new_node = merged_node;
			(*replacement_count)++;
			*node_ptr = merged_node;
			node = merged_node;
		}
	}
	{
		const char *map_suffix = tag_suffix(node->as.mapping.tag);
		bool is_set = map_suffix && strcmp(map_suffix, "set") == 0;
		if (node->type == GTEXT_YAML_SET) {
			is_set = true;
		}
		if (is_set) {
			for (size_t i = 0; i < node->as.mapping.count; i++) {
				if (!node_is_null(node->as.mapping.pairs[i].value)) {
					if (error) {
						error->code = GTEXT_YAML_E_INVALID;
						error->message = "set values must be null";
					}
					return GTEXT_YAML_E_INVALID;
				}
			}
			node->type = GTEXT_YAML_SET;
			node->as.mapping.type = GTEXT_YAML_SET;
		}
	}
	{
		const char *tag = node->as.mapping.tag;
		GTEXT_YAML_Status custom = apply_custom_tag_constructor(
			doc,
			node,
			opts,
			tag,
			error
		);
		if (custom != GTEXT_YAML_OK) return custom;
	}
	return apply_dupkey_policy(doc, node, opts, error);
}

/* One node part-way through being resolved.
 *
 * stage is where in the node's own work we are: 0 before its children, 1
 * among them, 2 after them. sub is the extra state a mapping pair needs,
 * because validate_mapping_key() runs between the key and the value rather
 * than before or after both.
 *
 * node_ptr is the slot this node sits in rather than a copy of the pointer,
 * so a merge-key replacement lands where the parent will read it. The
 * recursion used a local and copied it back; pointing at the slot does the
 * same thing with one less step. */
typedef struct {
	GTEXT_YAML_Node **node_ptr;
	GTEXT_YAML_Node *node;
	size_t index;
	unsigned char stage;
	unsigned char sub;
} resolve_frame;

typedef struct {
	resolve_frame *items;
	size_t count;
	size_t capacity;
	/* Carried so resolve_stack_push() reaches it without a signature change,
	   and set at the declaration so no push can precede it. */
	const GTEXT_Allocator *alloc;
} resolve_stack;

static bool resolve_stack_push(resolve_stack *stack, GTEXT_YAML_Node **node_ptr) {
	if (stack->count == stack->capacity) {
		size_t new_capacity = stack->capacity == 0 ? 32 : stack->capacity * 2;
		resolve_frame *items = (resolve_frame *)gtext_allocator_realloc(stack->alloc,
			stack->items, new_capacity * sizeof(resolve_frame)
		);
		if (!items) return false;
		stack->items = items;
		stack->capacity = new_capacity;
	}
	stack->items[stack->count].node_ptr = node_ptr;
	stack->items[stack->count].node = *node_ptr;
	stack->items[stack->count].index = 0;
	stack->items[stack->count].stage = 0;
	stack->items[stack->count].sub = 0;
	stack->count++;
	return true;
}

/* Resolve a node and everything under it.
 *
 * This was a recursion, at about 344 bytes of C stack a level, so a document
 * nested past about 24000 levels ended the process instead of being resolved.
 * The parser's max_depth normally keeps a parsed document well short of that
 * - but SIZE_MAX is the documented way to say "no depth limit", and a caller
 * who says it is asking for a deep document, not for a segmentation fault.
 * The stack is on the heap now, so depth costs memory rather than a frame.
 *
 * The loop always works on the top frame and never holds a pointer to it
 * across a push, since a push can move the array. */
static GTEXT_YAML_Status resolve_node(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Node **node_ptr,
	const GTEXT_YAML_Parse_Options *opts,
	yaml_merge_replacement **replacements,
	size_t *replacement_count,
	size_t *replacement_capacity,
	GTEXT_YAML_Error *error
) {
	resolve_stack stack = {NULL, 0, 0, doc->ctx->alloc};
	GTEXT_YAML_Status status = GTEXT_YAML_OK;

	if (!node_ptr || !*node_ptr) return GTEXT_YAML_OK;
	if (!resolve_stack_push(&stack, node_ptr)) return resolve_oom(error);

	while (status == GTEXT_YAML_OK && stack.count > 0) {
		resolve_frame *frame = &stack.items[stack.count - 1];
		GTEXT_YAML_Node *node = frame->node;
		GTEXT_YAML_Node **slot = NULL;

		switch (node->type) {
			case GTEXT_YAML_STRING:
			case GTEXT_YAML_BOOL:
			case GTEXT_YAML_INT:
			case GTEXT_YAML_FLOAT:
			case GTEXT_YAML_NULL:
				status = resolve_scalar(doc, node, opts, error);
				stack.count--;
				break;

			case GTEXT_YAML_SEQUENCE:
			case GTEXT_YAML_OMAP:
			case GTEXT_YAML_PAIRS:
				if (frame->stage == 0) {
					status = resolve_enter_sequence(doc, node, opts, error);
					frame->stage = 1;
					break;
				}
				if (frame->stage == 1) {
					if (frame->index >= node->as.sequence.count) {
						frame->stage = 2;
						break;
					}
					slot = &node->as.sequence.children[frame->index++];
					/* A NULL child resolved to OK in the recursion, by its
					   own first line, and is skipped here for the same
					   reason. */
					if (*slot && !resolve_stack_push(&stack, slot)) {
						status = resolve_oom(error);
					}
					break;
				}
				status = resolve_exit_sequence(doc, node, opts, error);
				stack.count--;
				break;

			case GTEXT_YAML_MAPPING:
			case GTEXT_YAML_SET:
				if (frame->stage == 0) {
					status = resolve_enter_mapping(doc, node, opts, error);
					frame->stage = 1;
					break;
				}
				if (frame->stage == 1) {
					if (frame->index >= node->as.mapping.count) {
						frame->stage = 2;
						break;
					}
					if (frame->sub == 0) {
						frame->sub = 1;
						slot = &node->as.mapping.pairs[frame->index].key;
						if (*slot && !resolve_stack_push(&stack, slot)) {
							status = resolve_oom(error);
						}
						break;
					}
					if (frame->sub == 1) {
						/* Asked of the slot rather than of what was pushed,
						   because the recursion validated a NULL key too -
						   it reached validate_mapping_key() by way of a
						   resolve that returned OK without doing anything. */
						status = validate_mapping_key(
							node->as.mapping.pairs[frame->index].key,
							opts,
							error
						);
						if (status != GTEXT_YAML_OK) break;
						frame->sub = 2;
						slot = &node->as.mapping.pairs[frame->index].value;
						if (*slot && !resolve_stack_push(&stack, slot)) {
							status = resolve_oom(error);
						}
						break;
					}
					frame->index++;
					frame->sub = 0;
					break;
				}
				status = resolve_exit_mapping(
					doc,
					frame->node_ptr,
					node,
					opts,
					replacements,
					replacement_count,
					replacement_capacity,
					error
				);
				stack.count--;
				break;

			case GTEXT_YAML_ALIAS:
			default:
				stack.count--;
				break;
		}
	}

	gtext_allocator_free(doc->ctx->alloc, stack.items);
	return status;
}
GTEXT_INTERNAL_API GTEXT_YAML_Status yaml_resolve_document(
	GTEXT_YAML_Document *doc,
	GTEXT_YAML_Error *error
) {
	if (!doc) return GTEXT_YAML_E_INVALID;
	const GTEXT_YAML_Parse_Options *opts = &doc->options;

	yaml_merge_replacement *replacements = NULL;
	size_t replacement_count = 0;
	size_t replacement_capacity = 0;

	GTEXT_YAML_Node *root = doc->root;
	GTEXT_YAML_Status st = resolve_node(
		doc,
		&root,
		opts,
		&replacements,
		&replacement_count,
		&replacement_capacity,
		error
	);
	if (st != GTEXT_YAML_OK) {
		gtext_allocator_free(doc->ctx->alloc, replacements);
		return st;
	}

	doc->root = root;
	if (replacement_count > 0) {
		GTEXT_YAML_Status walk = update_alias_targets(
			doc->root, replacements, replacement_count, doc->ctx->alloc
		);
		if (walk != GTEXT_YAML_OK) {
			gtext_allocator_free(doc->ctx->alloc, replacements);
			if (error) {
				error->code = GTEXT_YAML_E_OOM;
				error->message = "Out of memory repointing merged aliases";
			}
			return walk;
		}
	}
	gtext_allocator_free(doc->ctx->alloc, replacements);
	return GTEXT_YAML_OK;
}
