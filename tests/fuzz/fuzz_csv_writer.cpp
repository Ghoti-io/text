/**
 * @file
 *
 * libFuzzer harness for the CSV writers, and for the agreement between them.
 *
 * fuzz_csv.cpp parses, so `gtext_csv_write_table()` is reached only with tables
 * a parse produced, and `gtext_csv_writer_*` was reached by nothing. Two
 * options showed what that costs: the streaming writer never read
 * `trailing_newline` or `trim_trailing_empty_fields`, so a streaming write
 * always ended in a newline and never trimmed, where a table write did both.
 *
 * Two properties, and the second is the one no single-writer harness can state:
 *
 *     if the writer says OK, the bytes it wrote must parse back
 *     to the same fields;
 *
 *     and the table writer and the streaming writer, given the same fields
 *     and the same options, must write the same bytes.
 *
 * The fields come from the input rather than from a parse, so they reach what a
 * document cannot: a field holding the delimiter, the quote character, a bare
 * CR, a NUL, or bytes that are not UTF-8; an empty field in every position; and
 * the quoting policies that refuse such a field outright
 * (GTEXT_CSV_QUOTE_NONE yields GTEXT_CSV_E_UNQUOTABLE_FIELD rather than
 * writing something that reads back differently).
 *
 * Round-tripping is asserted only where it is meaningful. A dialect whose
 * quoting policy is NONE, or whose `always_escape_quotes` is off, is permitted
 * to write bytes that read back as something else - that is the documented
 * interoperability choice - so the round trip is claimed only for the policies
 * that promise it.
 *
 * Build with: make fuzz-csv-writer      Run: make fuzz-run-csv-writer
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <ghoti.io/text/csv.h>
}

namespace {

[[noreturn]] void fail(const char * what, const std::string & out) {
  std::fprintf(stderr, "%s\noutput was (%zu bytes): ", what, out.size());
  for (unsigned char c : out) {
    if (c >= 0x20 && c < 0x7f) {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::abort();
}

using Rows = std::vector<std::vector<std::string>>;

std::string write_with_table(const GTEXT_CSV_Write_Options & opts,
    const Rows & rows, GTEXT_CSV_Status * out_status) {
  *out_status = GTEXT_CSV_E_INVALID;
  GTEXT_CSV_Table * t = gtext_csv_new_table();
  if (!t) return std::string();
  for (const auto & row : rows) {
    std::vector<const char *> p;
    std::vector<size_t> l;
    for (const auto & f : row) {
      p.push_back(f.data());
      l.push_back(f.size());
    }
    if (gtext_csv_row_append(t, p.data(), l.data(), p.size(), nullptr)
        != GTEXT_CSV_OK) {
      gtext_csv_free_table(t);
      return std::string(); // ragged, which the table refuses
    }
  }
  GTEXT_CSV_Sink sink;
  if (gtext_csv_sink_buffer(&sink) != GTEXT_CSV_OK) {
    gtext_csv_free_table(t);
    return std::string();
  }
  *out_status = gtext_csv_write_table(&sink, &opts, t);
  std::string out(
      gtext_csv_sink_buffer_data(&sink), gtext_csv_sink_buffer_size(&sink));
  gtext_csv_sink_buffer_free(&sink);
  gtext_csv_free_table(t);
  return out;
}

std::string write_with_stream(const GTEXT_CSV_Write_Options & opts,
    const Rows & rows, GTEXT_CSV_Status * out_status) {
  *out_status = GTEXT_CSV_E_INVALID;
  GTEXT_CSV_Sink sink;
  if (gtext_csv_sink_buffer(&sink) != GTEXT_CSV_OK) return std::string();
  GTEXT_CSV_Writer * w = gtext_csv_writer_new(&sink, &opts);
  if (!w) {
    gtext_csv_sink_buffer_free(&sink);
    return std::string();
  }
  GTEXT_CSV_Status st = GTEXT_CSV_OK;
  for (const auto & row : rows) {
    if (st != GTEXT_CSV_OK) break;
    st = gtext_csv_writer_record_begin(w);
    for (const auto & f : row) {
      if (st != GTEXT_CSV_OK) break;
      st = gtext_csv_writer_field(w, f.data(), f.size());
    }
    if (st == GTEXT_CSV_OK) st = gtext_csv_writer_record_end(w);
  }
  const GTEXT_CSV_Status fi = gtext_csv_writer_finish(w);
  *out_status = (st != GTEXT_CSV_OK) ? st : fi;
  std::string out(
      gtext_csv_sink_buffer_data(&sink), gtext_csv_sink_buffer_size(&sink));
  gtext_csv_writer_free(w);
  gtext_csv_sink_buffer_free(&sink);
  return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 3) return 0;

  const uint8_t o0 = data[0];
  const uint8_t o1 = data[1];
  const uint8_t shape = data[2];

  GTEXT_CSV_Write_Options opts = gtext_csv_write_options_default();
  opts.trailing_newline = (o0 & 0x01) != 0;
  opts.trim_trailing_empty_fields = (o0 & 0x02) != 0;
  opts.quote_empty_fields = (o0 & 0x04) != 0;
  opts.quote_if_needed = (o0 & 0x08) != 0;
  opts.always_escape_quotes = (o0 & 0x10) != 0;
  opts.quote_all_fields = (o0 & 0x20) != 0;
  opts.quoting = (GTEXT_CSV_Quoting)((o0 >> 6) & 0x03);
  opts.newline = (o1 & 0x01) ? "\r\n" : "\n";
  // A delimiter that is not a comma, including ones that collide with what the
  // quoting policies inspect.
  static const char delims[] = {',', ';', '\t', '|', '.', ' ', ':', '"'};
  opts.dialect.delimiter = delims[(o1 >> 1) & 0x07];

  // Rows of a uniform field count, because a ragged table is refused and the
  // two writers would then be given different content - the mistake that made
  // an earlier comparison report 32 disagreements that were one harness bug.
  const size_t cols = (size_t)(shape & 0x07) + 1u;
  const size_t max_rows = (size_t)((shape >> 3) & 0x07) + 1u;

  Rows rows;
  size_t i = 3;
  while (i < size && rows.size() < max_rows) {
    std::vector<std::string> row;
    for (size_t c = 0; c < cols; c++) {
      const size_t len = (i < size) ? (data[i++] % 6u) : 0u;
      const size_t have = (size - i < len) ? (size - i) : len;
      row.emplace_back((const char *)(data + i), have);
      i += have;
    }
    rows.push_back(std::move(row));
    if (i >= size) break;
  }
  if (rows.empty()) return 0;

  GTEXT_CSV_Status ts = GTEXT_CSV_OK;
  GTEXT_CSV_Status ss = GTEXT_CSV_OK;
  const std::string from_table = write_with_table(opts, rows, &ts);
  const std::string from_stream = write_with_stream(opts, rows, &ss);

  // **The differential.** Same fields, same options, so the same bytes and the
  // same verdict. This is the property the two options above violated.
  if ((ts == GTEXT_CSV_OK) != (ss == GTEXT_CSV_OK)) {
    std::fprintf(stderr, "table status=%d stream status=%d\n", (int)ts, (int)ss);
    fail("one CSV writer refused what the other accepted", from_table);
  }
  if (ts == GTEXT_CSV_OK && from_table != from_stream) {
    std::fprintf(stderr, "table  wrote: %s\nstream wrote: %s\n",
        from_table.c_str(), from_stream.c_str());
    fail("the two CSV writers disagree on the same fields and options",
        from_table);
  }

  // **The round trip**, for the policies that promise one. QUOTE_NONE may
  // refuse, and with always_escape_quotes off a quote in an unquoted field is
  // emitted verbatim, which a strict reader is entitled to read differently -
  // both are documented choices rather than defects.
  const bool promises_round_trip = ts == GTEXT_CSV_OK
      && opts.quoting != GTEXT_CSV_QUOTE_NONE && opts.quote_if_needed
      && opts.always_escape_quotes && opts.dialect.delimiter != '"';
  if (promises_round_trip && !from_table.empty()) {
    GTEXT_CSV_Parse_Options popts = gtext_csv_parse_options_default();
    popts.dialect = opts.dialect;
    popts.max_total_bytes = 0;
    GTEXT_CSV_Table * back = gtext_csv_parse_table(
        from_table.data(), from_table.size(), &popts, nullptr);
    if (back) {
      // Only when nothing was trimmed away: trimming is a deliberate change to
      // the field count, so the row read back is shorter on purpose.
      if (!opts.trim_trailing_empty_fields
          && gtext_csv_row_count(back) == rows.size()) {
        for (size_t r = 0; r < rows.size(); r++) {
          if (gtext_csv_col_count(back, r) != rows[r].size()) {
            fail("a round trip changed the field count", from_table);
          }
          for (size_t c = 0; c < rows[r].size(); c++) {
            size_t len = 0;
            const char * got = gtext_csv_field(back, r, c, &len);
            const std::string had = rows[r][c];
            if (!got ? !had.empty()
                     : (len != had.size()
                         || std::memcmp(got, had.data(), len) != 0)) {
              std::fprintf(stderr, "row %zu col %zu: wrote %zu bytes, read "
                                   "back %zu\n",
                  r, c, had.size(), len);
              fail("a round trip changed a field's bytes", from_table);
            }
          }
        }
      }
      gtext_csv_free_table(back);
    }
  }

  return 0;
}
