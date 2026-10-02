/**
 * @file test-file-io-errors.cpp
 * @brief Every failure a *_parse_file() or *_write_file() can report, for
 *        every format that has them.
 *
 * The happy paths, the size limits and the atomic-replace property are in
 * tests/test-file-io.cpp and tests/yaml/test-yaml-file-io.cpp. What was
 * missing was the other half: the five per-format mapping functions that turn
 * a shared gtext_file_status into that format's code and message. Between
 * them they have about forty arms, and `make coverage` reported the
 * *_file_io.c files between 24% and 81% because almost none of those arms had
 * ever run - two of the five formats, TOML and INI, had no file tests at all.
 *
 * That gap hid a defect of exactly the shape the gap invites. GTEXT_FILE_E_READ
 * had **no producer anywhere in the library**: gtext_file_read_all() passed
 * GTEXT_FILE_E_OPEN for every failure cutil reported, so the five
 * `case GTEXT_FILE_E_READ:` arms and their five "could not be read" messages
 * were unreachable, and a file that opened and then would not read - a
 * directory is the easy way to make one - was reported as "could not open the
 * file". Nothing failed; the answer was simply wrong, in a message nobody had
 * ever seen because no test asked for one.
 *
 * ## Why this is one parameterized suite and not five hand-written ones
 *
 * The five mappers are copies of one another with different spellings. Written
 * out by hand, per format, the formats nobody was thinking about are the ones
 * that end up missing a case - which is precisely how TOML and INI came to
 * have no file tests. A table of formats and one body per property means
 * adding a format is one row, and a format cannot be quietly left out of a
 * property: it would not compile.
 *
 * ## The messages are asserted, not just the codes
 *
 * Every one of GTEXT_FILE_E_NOT_FOUND, _E_ACCESS, _E_OPEN and _E_READ maps to
 * the *same* public code in all five formats (GTEXT_JSON_E_INVALID and its
 * equivalents), because they are one thing to act on: the path is wrong. So a
 * test that asserts only the code cannot tell the four arms apart, and would
 * have passed before this change and after it. The message is where the
 * distinction lives, so the message is what is asserted.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <gtest/gtest.h>

extern "C" {
#include <ghoti.io/text/csv.h>
#include <ghoti.io/text/ini.h>
#include <ghoti.io/text/json.h>
#include <ghoti.io/text/toml.h>
#include <ghoti.io/text/yaml.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
}

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ostream>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// An allocator that fails on demand
//
// tests/test-allocator.cpp has a richer one, but its `fail_after` counts
// *allowed* allocations and 0 disables the failure, so it cannot express "fail
// the very first one" - which is the only budget that reaches the file layer's
// own allocations. gtext_file_write_atomic() makes exactly one (the
// destination's directory name) and gcu_file_read() makes its first before any
// parsing starts, so a budget of zero is what both of those paths need.
// ---------------------------------------------------------------------------

struct Budget {
	size_t allowed = 0;  ///< Serve this many, then fail every later one.
	size_t served = 0;
	size_t live = 0;     ///< Outstanding blocks, so a leak shows up here too.
};

struct BlockHeader {
	size_t size;
};

void * budget_malloc(void * ctx, size_t size) {
	auto * b = static_cast<Budget *>(ctx);
	if (b->served >= b->allowed) {
		return nullptr;
	}
	void * raw = std::malloc(sizeof(BlockHeader) + (size ? size : 1));
	if (!raw) {
		return nullptr;
	}
	static_cast<BlockHeader *>(raw)->size = size;
	b->served++;
	b->live++;
	return static_cast<char *>(raw) + sizeof(BlockHeader);
}

void * budget_calloc(void * ctx, size_t nitems, size_t size) {
	if (nitems && size > SIZE_MAX / nitems) {
		return nullptr;
	}
	const size_t total = nitems * size;
	void * p = budget_malloc(ctx, total ? total : 1);
	if (p) {
		std::memset(p, 0, total ? total : 1);
	}
	return p;
}

void budget_free(void * ctx, void * ptr) {
	if (!ptr) {
		return;
	}
	auto * b = static_cast<Budget *>(ctx);
	auto * h = reinterpret_cast<BlockHeader *>(
	    static_cast<char *>(ptr) - sizeof(BlockHeader));
	b->live--;
	std::free(h);
}

void * budget_realloc(void * ctx, void * ptr, size_t size) {
	if (!ptr) {
		return budget_malloc(ctx, size);
	}
	auto * h = reinterpret_cast<BlockHeader *>(
	    static_cast<char *>(ptr) - sizeof(BlockHeader));
	const size_t old = h->size;
	void * fresh = budget_malloc(ctx, size);
	if (!fresh) {
		return nullptr;
	}
	std::memcpy(fresh, ptr, old < size ? old : size);
	budget_free(ctx, ptr);
	return fresh;
}

GTEXT_Allocator make_allocator(Budget * b) {
	GTEXT_Allocator a;
	a.ctx = b;
	a.malloc_fn = budget_malloc;
	a.calloc_fn = budget_calloc;
	a.realloc_fn = budget_realloc;
	a.free_fn = budget_free;
	return a;
}

// ---------------------------------------------------------------------------
// Scratch paths
// ---------------------------------------------------------------------------

/** A file in the build tree, removed when the test ends. */
class TempFile {
public:
	explicit TempFile(const std::string & name)
	    : path_("build/test-file-io-errors-" + name) {
		remove(path_.c_str());
	}
	~TempFile() {
#ifndef _WIN32
		// A test that took the permissions away has to put them back, or the
		// removal fails and the next run of the suite reads a stale file.
		chmod(path_.c_str(), 0600);
#endif
		remove(path_.c_str());
	}
	const char * c_str() const { return path_.c_str(); }
	const std::string & str() const { return path_; }

	void write(const std::string & contents) const {
		std::ofstream out(path_, std::ios::binary | std::ios::trunc);
		out << contents;
	}
	std::string read() const {
		std::ifstream in(path_, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)),
		    std::istreambuf_iterator<char>());
	}

private:
	std::string path_;
};

/** An empty directory in the build tree, removed when the test ends. */
class TempDir {
public:
	explicit TempDir(const std::string & name)
	    : path_("build/test-file-io-errors-" + name) {
#ifdef _WIN32
		_mkdir(path_.c_str());
#else
		mkdir(path_.c_str(), 0700);
#endif
	}
	~TempDir() {
#ifdef _WIN32
		_rmdir(path_.c_str());
#else
		rmdir(path_.c_str());
#endif
	}
	const char * c_str() const { return path_.c_str(); }

private:
	std::string path_;
};

// ---------------------------------------------------------------------------
// One format's file entry points, behind a uniform shape
// ---------------------------------------------------------------------------

/**
 * How a format answers.
 *
 * `code` is that format's status as an int - the five enums are separate types
 * but each has OK at 0 - and `message` is err.message where the entry point
 * offers one. TOML's and INI's *_write_file() do not take an error struct at
 * all, so no message comes back from their writes; that is a difference in the
 * public API, not in what this suite measures, and the write properties here
 * are about the code. The one message those two can carry is an adapter's own,
 * describing a document it failed to *build* - which is a broken test rather
 * than a finding, so it is worth having in the failure output.
 */
struct Answer {
	int code = 0;
	std::string message;
};

struct Format {
	const char * name;
	const char * document; ///< A tiny valid document in this format.

	int ok;
	int e_invalid; ///< Where NOT_FOUND, ACCESS, OPEN and READ all land.
	int e_write;
	int e_oom;
	int e_limit;

	const char * msg_not_found;
	const char * msg_access;
	const char * msg_read;

	/**
	 * Parse @p path.  @p alloc may be NULL; @p max_bytes of 0 means "whatever
	 * this format's options default to".
	 */
	Answer (*parse)(
	    const char * path, const GTEXT_Allocator * alloc, size_t max_bytes);

	/**
	 * Parse @p from (which holds `document`) and write it to @p to through
	 * @p alloc.  The parse always uses the default allocator, so that a budget
	 * of zero reaches the write and nothing else.
	 */
	Answer (*write)(
	    const char * from, const char * to, const GTEXT_Allocator * alloc);

	/**
	 * Build a document this format's *writer* refuses, and try to write it to
	 * @p to - which is an ordinary writable path, so the serializer is the only
	 * thing that can say no.
	 *
	 * Every one of the five `*_write_file()` functions carries a comment to the
	 * effect that "an unrepresentable value must not come back as a disk
	 * error", and the branch that keeps that promise had never run in any of
	 * them: the writers were only ever handed documents they had just parsed,
	 * and a parse cannot produce a document its own writer rejects. Each of
	 * these reaches the refusal by a different door, because the formats refuse
	 * different things.
	 */
	Answer (*write_refused)(const char * to);
	int e_refused; ///< What `write_refused` must report.
};

// --- CSV -------------------------------------------------------------------

Answer csv_parse(
    const char * path, const GTEXT_Allocator * alloc, size_t max_bytes) {
	GTEXT_CSV_Parse_Options opts = gtext_csv_parse_options_default();
	opts.allocator = alloc;
	if (max_bytes) opts.max_total_bytes = max_bytes;
	GTEXT_CSV_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_CSV_Table * t = gtext_csv_parse_file(path, &opts, &err);
	Answer a;
	a.code = t ? GTEXT_CSV_OK : (int)err.code;
	if (!t && err.message) a.message = err.message;
	gtext_csv_free_table(t);
	gtext_csv_error_free(&err);
	return a;
}

Answer csv_write(
    const char * from, const char * to, const GTEXT_Allocator * alloc) {
	GTEXT_CSV_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_CSV_Table * t = gtext_csv_parse_file(from, nullptr, &err);
	Answer a;
	if (!t) {
		a.code = (int)err.code;
		a.message = err.message ? err.message : "the source would not parse";
		gtext_csv_error_free(&err);
		return a;
	}
	GTEXT_CSV_Write_Options wopts = gtext_csv_write_options_default();
	wopts.allocator = alloc;
	a.code = (int)gtext_csv_write_file(to, t, &wopts, &err);
	if (err.message) a.message = err.message;
	gtext_csv_free_table(t);
	gtext_csv_error_free(&err);
	return a;
}

// --- CSV: the document its writer will not write --------------------------

Answer csv_write_refused(const char * to) {
	// GTEXT_CSV_QUOTE_NONE and a field holding the delimiter. Quoting is the
	// only thing that carries a comma through a CSV field - neither escape mode
	// touches anything but the quote character - so this is a document that
	// cannot exist, and the writer says so instead of emitting two fields.
	GTEXT_CSV_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_CSV_Table * t =
	    gtext_csv_parse_table("\"a,b\"\n", 6, nullptr, &err);
	Answer a;
	if (!t) {
		a.code = (int)err.code;
		a.message = "the field could not be built";
		gtext_csv_error_free(&err);
		return a;
	}
	GTEXT_CSV_Write_Options wopts = gtext_csv_write_options_default();
	wopts.quoting = GTEXT_CSV_QUOTE_NONE;
	a.code = (int)gtext_csv_write_file(to, t, &wopts, &err);
	if (err.message) a.message = err.message;
	gtext_csv_free_table(t);
	gtext_csv_error_free(&err);
	return a;
}

// --- JSON ------------------------------------------------------------------

Answer json_parse(
    const char * path, const GTEXT_Allocator * alloc, size_t max_bytes) {
	GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();
	opts.allocator = alloc;
	if (max_bytes) opts.max_total_bytes = max_bytes;
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse_file(path, &opts, &err);
	Answer a;
	a.code = v ? GTEXT_JSON_OK : (int)err.code;
	if (!v && err.message) a.message = err.message;
	gtext_json_free(v);
	gtext_json_error_free(&err);
	return a;
}

Answer json_write(
    const char * from, const char * to, const GTEXT_Allocator * alloc) {
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse_file(from, nullptr, &err);
	Answer a;
	if (!v) {
		a.code = (int)err.code;
		a.message = err.message ? err.message : "the source would not parse";
		gtext_json_error_free(&err);
		return a;
	}
	GTEXT_JSON_Write_Options wopts = gtext_json_write_options_default();
	wopts.allocator = alloc;
	a.code = (int)gtext_json_write_file(to, v, &wopts, &err);
	if (err.message) a.message = err.message;
	gtext_json_free(v);
	gtext_json_error_free(&err);
	return a;
}

// --- JSON: the options combination its writer will not honour -------------

Answer json_write_refused(const char * to) {
	// One record per line and a value printed across lines are contradictory
	// requests, and the writer refuses the pair rather than picking one.
	GTEXT_JSON_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_JSON_Value * v = gtext_json_parse("{\"a\":1}", 7, nullptr, &err);
	Answer a;
	if (!v) {
		a.code = (int)err.code;
		a.message = "the value could not be built";
		gtext_json_error_free(&err);
		return a;
	}
	GTEXT_JSON_Write_Options wopts = gtext_json_write_options_default();
	wopts.records = GTEXT_JSON_RECORDS_LINE;
	wopts.pretty = true;
	a.code = (int)gtext_json_write_file(to, v, &wopts, &err);
	if (err.message) a.message = err.message;
	gtext_json_free(v);
	gtext_json_error_free(&err);
	return a;
}

// --- YAML ------------------------------------------------------------------

Answer yaml_parse(
    const char * path, const GTEXT_Allocator * alloc, size_t max_bytes) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
	opts.allocator = alloc;
	if (max_bytes) opts.max_total_bytes = max_bytes;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * d = gtext_yaml_parse_file(path, &opts, &err);
	Answer a;
	a.code = d ? GTEXT_YAML_OK : (int)err.code;
	if (!d && err.message) a.message = err.message;
	gtext_yaml_free(d);
	gtext_yaml_error_free(&err);
	return a;
}

Answer yaml_write(
    const char * from, const char * to, const GTEXT_Allocator * alloc) {
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * d = gtext_yaml_parse_file(from, nullptr, &err);
	Answer a;
	if (!d) {
		a.code = (int)err.code;
		a.message = err.message ? err.message : "the source would not parse";
		gtext_yaml_error_free(&err);
		return a;
	}
	GTEXT_YAML_Write_Options wopts = gtext_yaml_write_options_default();
	wopts.allocator = alloc;
	a.code = (int)gtext_yaml_write_file(to, d, &wopts, &err);
	if (err.message) a.message = err.message;
	gtext_yaml_free(d);
	gtext_yaml_error_free(&err);
	return a;
}

// --- YAML: the tag its writer cannot spell -------------------------------

Answer yaml_write_refused(const char * to) {
	// A "!!" shorthand names a tag in the yaml.org,2002 namespace, and only the
	// defined ones can be written that way; the parser refuses an undefined one
	// too, so this document has to be built rather than parsed. A tag has one
	// spelling and no fallback, so writing an approximation of it would be
	// worse than refusing.
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc = gtext_yaml_document_new(nullptr, &err);
	Answer a;
	if (!doc) {
		a.code = (int)err.code;
		a.message = "the document could not be built";
		gtext_yaml_error_free(&err);
		return a;
	}
	GTEXT_YAML_Node * root =
	    gtext_yaml_node_new_scalar(doc, "x", "!!nosuchtag", nullptr);
	if (!root || !gtext_yaml_document_set_root(doc, root)) {
		gtext_yaml_free(doc);
		a.code = -1;
		a.message = "the tagged scalar could not be built";
		return a;
	}
	a.code = (int)gtext_yaml_write_file(to, doc, nullptr, &err);
	if (err.message) a.message = err.message;
	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
	return a;
}

// --- TOML ------------------------------------------------------------------

Answer toml_parse(
    const char * path, const GTEXT_Allocator * alloc, size_t max_bytes) {
	GTEXT_TOML_Parse_Options opts = gtext_toml_parse_options_default();
	opts.allocator = alloc;
	if (max_bytes) opts.max_total_bytes = max_bytes;
	GTEXT_TOML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_TOML_Value * v = gtext_toml_parse_file(path, &opts, &err);
	Answer a;
	a.code = v ? GTEXT_TOML_OK : (int)err.code;
	if (!v && err.message) a.message = err.message;
	gtext_toml_free(v);
	gtext_toml_error_free(&err);
	return a;
}

Answer toml_write(
    const char * from, const char * to, const GTEXT_Allocator * alloc) {
	GTEXT_TOML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_TOML_Value * v = gtext_toml_parse_file(from, nullptr, &err);
	Answer a;
	if (!v) {
		a.code = (int)err.code;
		a.message = err.message ? err.message : "the source would not parse";
		gtext_toml_error_free(&err);
		return a;
	}
	GTEXT_TOML_Write_Options wopts = gtext_toml_write_options_default();
	wopts.allocator = alloc;
	// No error struct on this entry point, so there is no message to carry.
	a.code = (int)gtext_toml_write_file(v, to, &wopts);
	gtext_toml_free(v);
	gtext_toml_error_free(&err);
	return a;
}

// --- TOML: the root its writer will not accept ---------------------------

Answer toml_write_refused(const char * to) {
	// A TOML document is a table. A parse can only ever produce one, so a
	// scalar root has to be constructed - and the writer refuses it rather than
	// emitting a file that is not TOML.
	GTEXT_TOML_Value * v = gtext_toml_new_integer(nullptr, 42);
	Answer a;
	if (!v) {
		a.code = -1;
		a.message = "the integer could not be built";
		return a;
	}
	GTEXT_TOML_Write_Options wopts = gtext_toml_write_options_default();
	a.code = (int)gtext_toml_write_file(v, to, &wopts);
	gtext_toml_free(v);
	return a;
}

// --- INI -------------------------------------------------------------------

Answer ini_parse(
    const char * path, const GTEXT_Allocator * alloc, size_t max_bytes) {
	GTEXT_INI_Parse_Options opts = gtext_ini_parse_options_default();
	opts.allocator = alloc;
	if (max_bytes) opts.max_total_bytes = max_bytes;
	GTEXT_INI_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_INI_Document * d = gtext_ini_parse_file(path, &opts, &err);
	Answer a;
	a.code = d ? GTEXT_INI_OK : (int)err.code;
	if (!d && err.message) a.message = err.message;
	gtext_ini_free(d);
	gtext_ini_error_free(&err);
	return a;
}

Answer ini_write(
    const char * from, const char * to, const GTEXT_Allocator * alloc) {
	GTEXT_INI_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_INI_Document * d = gtext_ini_parse_file(from, nullptr, &err);
	Answer a;
	if (!d) {
		a.code = (int)err.code;
		a.message = err.message ? err.message : "the source would not parse";
		gtext_ini_error_free(&err);
		return a;
	}
	GTEXT_INI_Write_Options wopts = gtext_ini_write_options_default();
	wopts.allocator = alloc;
	// No error struct on this entry point, so there is no message to carry.
	a.code = (int)gtext_ini_write_file(d, to, &wopts);
	gtext_ini_free(d);
	gtext_ini_error_free(&err);
	return a;
}

// --- INI: the value its writer cannot represent --------------------------

Answer ini_write_refused(const char * to) {
	// An INI entry is one line, so a value holding a newline is two - and the
	// DOM takes it, because a value is bytes and the limit belongs to the
	// serialization rather than to the model. gtext_ini_group_set() does
	// enforce the dialect's *key* rules, which is why a bad key cannot be used
	// here: it is refused on the way in.
	GTEXT_INI_Document * doc = gtext_ini_new(nullptr);
	Answer a;
	if (!doc) {
		a.code = -1;
		a.message = "the document could not be built";
		return a;
	}
	GTEXT_INI_Group * group = nullptr;
	const GTEXT_INI_Status added =
	    gtext_ini_document_add_group(doc, "s", &group);
	if (added != GTEXT_INI_OK || !group
	    || gtext_ini_group_set(group, "a", "x\ny", 3) != GTEXT_INI_OK) {
		gtext_ini_free(doc);
		a.code = -1;
		a.message = "the two-line value could not be built";
		return a;
	}
	GTEXT_INI_Write_Options wopts = gtext_ini_write_options_default();
	a.code = (int)gtext_ini_write_file(doc, to, &wopts);
	gtext_ini_free(doc);
	return a;
}

// ---------------------------------------------------------------------------

const Format kFormats[] = {
    {"Csv", "a,b\n1,2\n", GTEXT_CSV_OK, GTEXT_CSV_E_INVALID, GTEXT_CSV_E_WRITE,
        GTEXT_CSV_E_OOM, GTEXT_CSV_E_LIMIT, "No such file or directory",
        "Permission denied opening file", "Failed to read file", csv_parse,
        csv_write, csv_write_refused, GTEXT_CSV_E_UNQUOTABLE_FIELD},
    {"Json", "{\"a\":1}\n", GTEXT_JSON_OK, GTEXT_JSON_E_INVALID,
        GTEXT_JSON_E_WRITE, GTEXT_JSON_E_OOM, GTEXT_JSON_E_LIMIT,
        "No such file or directory", "Permission denied opening file",
        "Failed to read file", json_parse, json_write, json_write_refused,
        GTEXT_JSON_E_INVALID},
    {"Yaml", "a: 1\n", GTEXT_YAML_OK, GTEXT_YAML_E_INVALID, GTEXT_YAML_E_WRITE,
        GTEXT_YAML_E_OOM, GTEXT_YAML_E_LIMIT, "No such file or directory",
        "Permission denied opening file", "Failed to read file contents",
        yaml_parse, yaml_write, yaml_write_refused, GTEXT_YAML_E_INVALID},
    {"Toml", "a = 1\n", GTEXT_TOML_OK, GTEXT_TOML_E_INVALID,
        GTEXT_TOML_E_WRITE, GTEXT_TOML_E_OOM, GTEXT_TOML_E_LIMIT,
        "no such file or directory", "permission denied opening the file",
        "the file could not be read to the end", toml_parse, toml_write,
        toml_write_refused, GTEXT_TOML_E_INVALID},
    {"Ini", "[s]\na=1\n", GTEXT_INI_OK, GTEXT_INI_E_INVALID, GTEXT_INI_E_WRITE,
        GTEXT_INI_E_OOM, GTEXT_INI_E_LIMIT, "no such file or directory",
        "permission denied opening the file",
        "the file could not be read to the end", ini_parse, ini_write,
        ini_write_refused, GTEXT_INI_E_UNREPRESENTABLE},
};

/**
 * Print a format as its name.
 *
 * Without this gtest falls back to dumping the parameter's 96 bytes, so a
 * failure read "where GetParam() = 96-byte object <A2-2D 5B-CA ...>" - four
 * lines of pointers in place of the one word that says which format broke.
 */
void PrintTo(const Format & format, std::ostream * os) {
	*os << format.name;
}

class FileIoError : public ::testing::TestWithParam<Format> {
protected:
	const Format & f() const { return GetParam(); }

	/** A scratch file holding this format's document. */
	std::string slug(const char * what) const {
		return std::string(f().name) + "-" + what;
	}
};

std::string format_name(const ::testing::TestParamInfo<Format> & info) {
	return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(Formats, FileIoError,
    ::testing::ValuesIn(kFormats), format_name);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

TEST_P(FileIoError, TheDocumentItselfStillParses) {
	// The control for every test below. Each of them asserts that a particular
	// way of breaking the read is reported in a particular way, and a document
	// this suite could not parse at all would make all of them pass for the
	// wrong reason - most loudly the OOM test, where "returned a failure" is
	// the whole assertion.
	TempFile in(slug("good"));
	in.write(f().document);
	const Answer a = f().parse(in.c_str(), nullptr, 0);
	EXPECT_EQ(a.code, f().ok) << a.message;
}

TEST_P(FileIoError, AMissingPathSaysThereIsNothingThere) {
	// cutil tells ENOENT apart from a disk that will not read, and this library
	// used to throw that away: every failure here arrived as "could not open
	// the file", which is true of all of them and therefore says nothing.
	const Answer a =
	    f().parse("build/test-file-io-errors-absent-on-purpose", nullptr, 0);
	EXPECT_EQ(a.code, f().e_invalid);
	EXPECT_EQ(a.message, f().msg_not_found);
}

TEST_P(FileIoError, AnUnreadablePathSaysPermissionDenied) {
#ifdef _WIN32
	GTEST_SKIP() << "no POSIX permission bits";
#else
	if (geteuid() == 0) {
		// Root reads it anyway, so the property under test does not exist here
		// rather than merely being unobservable - which is the one case a skip
		// is honest. CI containers often run as root.
		GTEST_SKIP() << "running as root; the mode bits do not apply";
	}
	TempFile in(slug("noperm"));
	in.write(f().document);
	ASSERT_EQ(chmod(in.c_str(), 0), 0) << strerror(errno);

	const Answer a = f().parse(in.c_str(), nullptr, 0);
	EXPECT_EQ(a.code, f().e_invalid);
	EXPECT_EQ(a.message, f().msg_access);
#endif
}

TEST_P(FileIoError, ADirectoryOpensAndThenWillNotRead) {
	// The test that could not have passed before this change, and the reason
	// GTEXT_FILE_E_READ now has a producer. fopen() on a directory succeeds on
	// POSIX and the first fread() sets the stream's error flag, so this is a
	// read that failed after the open succeeded - which the library had no way
	// to say, and said "could not open the file" instead.
#ifdef _WIN32
	GTEST_SKIP() << "fopen() refuses a directory outright here, so this is an "
	                "open failure rather than a read failure";
#else
	TempDir dir(slug("isadir"));
	const Answer a = f().parse(dir.c_str(), nullptr, 0);
	EXPECT_EQ(a.code, f().e_invalid);
	EXPECT_EQ(a.message, f().msg_read);
#endif
}

TEST_P(FileIoError, AFailedAllocationWhileReadingIsOutOfMemory) {
	// The buffer the file is read into is the single largest allocation a
	// *_parse_file() makes, and it comes from the caller's allocator. A budget
	// of zero fails it before any parsing starts, which is the only way to
	// reach the GTEXT_FILE_E_OOM arm of each mapper.
	TempFile in(slug("oom-read"));
	in.write(f().document);

	Budget budget;
	budget.allowed = 0;
	const GTEXT_Allocator alloc = make_allocator(&budget);

	const Answer a = f().parse(in.c_str(), &alloc, 0);
	EXPECT_EQ(a.code, f().e_oom);
	EXPECT_EQ(budget.live, 0u) << "the failed read kept a block";
}

TEST_P(FileIoError, AFileLargerThanTheLimitIsRefused) {
	// CSV, JSON and YAML had this; TOML and INI had no file test at all, so
	// their GTEXT_FILE_E_LIMIT arms had never run. The limit reaches the reader
	// rather than being applied to bytes already in memory, which is what makes
	// it a limit - and the only way to see that from out here is that the
	// answer is E_LIMIT and not a parse error about a truncated document.
	TempFile in(slug("big"));
	std::string big;
	while (big.size() < 4096) {
		big += f().document;
	}
	in.write(big);

	const Answer a = f().parse(in.c_str(), nullptr, 64);
	EXPECT_EQ(a.code, f().e_limit) << a.message;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

TEST_P(FileIoError, AWriteIntoAMissingDirectoryCannotMakeItsTemporary) {
	// gcu_file_temp_create() calls mkstemp() and reports every reason it can
	// fail as one flat GCU_FILE_ERR_IO, a missing directory included, so this
	// is the GTEXT_FILE_E_OPEN arm of each mapper rather than the NOT_FOUND
	// one. Said here because the two read the same from outside and the
	// difference is in which cutil call answered.
	TempFile in(slug("w-src-nodir"));
	in.write(f().document);

	const Answer a = f().write(in.c_str(),
	    "build/test-file-io-errors-no-such-dir/out", nullptr);
	EXPECT_NE(a.code, f().ok);
}

TEST_P(FileIoError, AWriteOntoADirectoryFailsWhenItCommits) {
	// The temporary is created and written in full; the rename that publishes
	// it is what fails, because the destination is a directory. That is the
	// GTEXT_FILE_E_WRITE arm, and the only failure in the whole write path that
	// happens after the bytes were accepted.
	TempFile in(slug("w-src-ondir"));
	in.write(f().document);
	TempDir dest(slug("w-dest-dir"));

	const Answer a = f().write(in.c_str(), dest.c_str(), nullptr);
	EXPECT_EQ(a.code, f().e_write) << a.message;
}

TEST_P(FileIoError, AFailedAllocationWhileWritingIsOutOfMemory) {
	// gtext_file_write_atomic() allocates exactly once - the destination's
	// directory name, so the temporary can be placed beside it - and takes it
	// from the write options' allocator. A budget of zero fails that one, which
	// is the GTEXT_FILE_E_OOM arm on the writing side.
	TempFile in(slug("oom-write-src"));
	in.write(f().document);
	TempFile out(slug("oom-write-dest"));

	Budget budget;
	budget.allowed = 0;
	const GTEXT_Allocator alloc = make_allocator(&budget);

	const Answer a = f().write(in.c_str(), out.c_str(), &alloc);
	EXPECT_EQ(a.code, f().e_oom) << a.message;
	EXPECT_EQ(budget.live, 0u) << "the failed write kept a block";
	EXPECT_EQ(out.read(), "") << "a write that allocated nothing wrote bytes";
}

TEST_P(FileIoError, AFailedWriteLeavesAnExistingFileExactlyAsItWas) {
	// The atomic-replace promise, asserted for all five formats rather than
	// for the two that happened to have a test. The destination holds bytes
	// that are not valid in this format, so "unchanged" is not something the
	// writer could have produced by accident.
	TempFile in(slug("w-src-atomic"));
	in.write(f().document);

	TempFile dest(slug("w-dest-atomic"));
	const std::string sentinel = "\x01 not a document in any of these formats\n";
	dest.write(sentinel);

	Budget budget;
	budget.allowed = 0;
	const GTEXT_Allocator alloc = make_allocator(&budget);
	const Answer a = f().write(in.c_str(), dest.c_str(), &alloc);
	EXPECT_NE(a.code, f().ok);
	EXPECT_EQ(dest.read(), sentinel);
}

TEST_P(FileIoError, TheDocumentSurvivesAWriteAndAReadBack) {
	// The other half of every failure above, and the line that showed this
	// suite was needed: `return GTEXT_INI_OK` at the end of
	// gtext_ini_write_file() had never executed, so nothing in the library had
	// ever written an INI file successfully.
	TempFile in(slug("rt-src"));
	in.write(f().document);
	TempFile out(slug("rt-dest"));

	const Answer written = f().write(in.c_str(), out.c_str(), nullptr);
	ASSERT_EQ(written.code, f().ok) << written.message;
	EXPECT_FALSE(out.read().empty()) << "the write reported success and wrote "
	                                    "nothing";

	const Answer reread = f().parse(out.c_str(), nullptr, 0);
	EXPECT_EQ(reread.code, f().ok) << reread.message;
}

TEST_P(FileIoError, AWriterThatRefusesSaysSoRatherThanBlamingTheDisk) {
	// Each of the five *_write_file() functions prefers the serializer's own
	// status over the file layer's, because the file layer only knows that the
	// callback said no - and "the disk failed" is the wrong thing to tell
	// someone whose document cannot be written down. The destination is an
	// ordinary writable path, so nothing but the serializer can refuse.
	TempFile out(slug("refused"));

	const Answer a = f().write_refused(out.c_str());
	EXPECT_EQ(a.code, f().e_refused) << a.message;
	// And no half-written file: the temporary is abandoned rather than renamed.
	EXPECT_EQ(out.read(), "")
	    << "a refused document still put bytes at the destination";
}

// ---------------------------------------------------------------------------
// What YAML has that the others do not
//
// Three lines of src/yaml/yaml_file_io.c belong to YAML alone, because YAML is
// the only format here that remembers which line ending its file used so that
// writing it back does not change every line in the diff.
// ---------------------------------------------------------------------------

TEST(YamlFileIoDetail, AnEmptyFileHasNoLineEndingToRemember) {
	// Nothing to detect in zero bytes, and a document with no content is still
	// a document: it writes as "---", not as nothing, because nothing is a
	// different stream (no documents at all).
	TempFile in("yaml-empty");
	in.write("");

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc = gtext_yaml_parse_file(in.c_str(), nullptr, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "unknown");

	// trailing_newline on, so that *which* newline was remembered is visible
	// in the bytes. A file with none to offer must fall back to the writer's
	// default rather than to whatever the last file happened to use: with the
	// detection returning "\r" here instead of nothing, this reads "---\r".
	GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
	opts.trailing_newline = true;
	opts.newline = nullptr;

	TempFile out("yaml-empty-out");
	EXPECT_EQ(gtext_yaml_write_file(out.c_str(), doc, &opts, &err),
	    GTEXT_YAML_OK);
	EXPECT_EQ(out.read(), "---\n");

	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
}

TEST(YamlFileIoDetail, ALoneCarriageReturnIsTheFilesLineEnding) {
	// Classic-Mac line endings. YamlFileIO.PreserveLineEndingsOnWrite covers
	// "\r\n"; a bare "\r" takes the other branch of the same two-character
	// lookahead, and taking the wrong one would turn every line ending in the
	// file into "\r\n" on the first save.
	TempFile in("yaml-cr");
	in.write("a: 1\rb: 2\r");

	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	GTEXT_YAML_Document * doc = gtext_yaml_parse_file(in.c_str(), nullptr, &err);
	ASSERT_NE(doc, nullptr) << (err.message ? err.message : "unknown");

	GTEXT_YAML_Write_Options opts = gtext_yaml_write_options_default();
	opts.pretty = true;
	opts.newline = nullptr; // Take it from the document.

	TempFile out("yaml-cr-out");
	ASSERT_EQ(gtext_yaml_write_file(out.c_str(), doc, &opts, &err),
	    GTEXT_YAML_OK);

	const std::string written = out.read();
	EXPECT_NE(written.find('\r'), std::string::npos);
	EXPECT_EQ(written.find('\n'), std::string::npos)
	    << "a file written with CR line endings grew an LF";

	gtext_yaml_free(doc);
	gtext_yaml_error_free(&err);
}

TEST(YamlFileIoDetail, AMalformedFileIsRefusedByTheMultiDocumentReaderToo) {
	// gtext_yaml_parse_file_all() reports a parse failure on its own path - it
	// has read the file by then, so the shared layer has nothing more to say -
	// and must leave the caller's out-parameters alone.
	TempFile in("yaml-bad-all");
	in.write("a: [1, 2\n");

	GTEXT_YAML_Document ** docs = nullptr;
	size_t count = 99;
	GTEXT_YAML_Error err;
	memset(&err, 0, sizeof(err));
	EXPECT_NE(
	    gtext_yaml_parse_file_all(in.c_str(), nullptr, &docs, &count, &err),
	    GTEXT_YAML_OK);
	EXPECT_EQ(docs, nullptr);
	EXPECT_EQ(count, 99u) << "a refused read wrote to the count anyway";
	EXPECT_NE(err.code, GTEXT_YAML_OK);
	gtext_yaml_error_free(&err);
}

// ---------------------------------------------------------------------------
// Arms with no producer
//
// After this suite, `make coverage` has the six *_file_io.c files between 86%
// and 97%, and every line it still reports is one of these. They are listed
// because a defensive branch described as a load-bearing one is a false
// statement about the code: none is counted as covered, and none is dressed up
// as something a test could reach.
//
//   - **The null-argument guards.** gtext_file_read_all()'s
//     `!path || !out_data || !out_len` and gtext_file_write_atomic()'s
//     `!path || !emit`. Every caller checks its own path first and passes the
//     addresses of its own locals for the rest, so the only way in is a new
//     caller that forgets - which is what the guard is for.
//   - **Both gcu_path_dirname() failures.** The first can only fail on an
//     argument cutil rejects, and the path is already known not to be NULL;
//     the second has the arguments the first one just succeeded with.
//   - **gtext_file_fwrite()'s `len == 0`.** It changes nothing for a real
//     zero-length write - `fwrite(bytes, 1, 0, f)` returns 0 and the
//     comparison holds - and exists for the one case where it would matter,
//     a NULL pointer with a length of zero, which is undefined to pass to
//     fwrite(). No writer here emits one.
//   - **The `default:` arm of each format's mapper.** gtext_file_status has
//     eight values, seven of them have a `case` above it, and the eighth is
//     GTEXT_FILE_OK, which no caller maps because it checks for it first.
//   - **gtext_file_map()'s GCU_FILE_ERR_EXISTS, _NOT_EMPTY and _INVALID arm,
//     and its trailing return.** cutil can return all three, but not from the
//     four calls this library makes: nothing here creates a file that must not
//     already exist, removes a directory, or passes an argument cutil rejects.
//     They are named anyway, and that switch has no `default:`, so that cutil
//     growing a tenth code is a -Wswitch error rather than a silent
//     reclassification - which is what happened to the four it has already
//     grown. The trailing return is the price of having no `default:`.
//
// What *is* reachable, and is asserted below, is the public null check each
// format does before any of that.
// ---------------------------------------------------------------------------

TEST_P(FileIoError, ANullPathIsRefusedBeforeAnythingIsOpened) {
	const Answer a = f().parse(nullptr, nullptr, 0);
	EXPECT_EQ(a.code, f().e_invalid);
	// Each format's own message, not the shared layer's: this refusal never
	// reaches gtext_file_read_all().
	EXPECT_NE(a.message, f().msg_not_found);
	EXPECT_FALSE(a.message.empty());
}

} // namespace
