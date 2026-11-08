/**
 * @file
 *
 * libFuzzer harness for the INI writer.
 *
 * fuzz_ini.cpp parses, so `gtext_ini_write()` is reached only with documents a
 * parse produced - and that is the bound that has mattered most here, because
 * a parse has already refused everything the writer would have to refuse. The
 * group names and keys reachable only through `gtext_ini_new()`,
 * `gtext_ini_document_add_group()` and `gtext_ini_group_set()` are the ones
 * worth asking about: a name holding `]`, a newline, a NUL or bytes that are
 * not UTF-8, and a value holding a line break.
 *
 * The property:
 *
 *     if the writer says OK, the bytes it wrote must parse under the same
 *     dialect, and must hold the same groups, keys and values.
 *
 * Which is the round trip stated over documents nobody chose. The DOM API's own
 * refusals are the other half of what is under test: `gtext_ini_group_set()`
 * enforces the dialect's key rules, so a name the writer could not round-trip
 * should be refused on the way in rather than written and lost - and this
 * harness fails either way round, because it only asserts the round trip for
 * what the DOM accepted.
 *
 * Seven dialects, from the first byte, because a key or a group name legal in
 * one is not legal in all of them and the writer's quoting and escaping differ.
 *
 * The property, the shadow of what the DOM accepted and the comparison of what was read back are tests/ini_writer_model.h, which is also what
 * tests/test-ini-sweep.cpp asks about a table of boundary strings, so what the writer owes is written once. This file is the decoder from the
 * input's bytes to the steps and nothing else: a finding of the fuzzer is a defect of the library, or a rule the model must learn, and the
 * sweep learns it with it.
 *
 * Build with: make fuzz-ini-writer      Run: make fuzz-run-ini-writer
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../ini_writer_model.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) return 0;

  ini_model::Config cfg;
  cfg.dialect = data[0] % ini_model::dialects().size();
  const uint8_t wflags = data[1];
  cfg.crlf = (wflags & 0x01) != 0;
  cfg.emit_comments = (wflags & 0x02) != 0;
  cfg.normalize = (wflags & 0x04) != 0;

  // Even operations add a group, odd ones set a key in the current group; each draws a name or key of up to seven bytes (and a value of up to seven for a set).
  std::vector<ini_model::Step> steps;
  size_t i = 2;
  while (i < size) {
    const uint8_t op = data[i++];
    const size_t len = (i < size) ? (data[i++] % 8u) : 0u;
    const size_t have = (size - i < len) ? (size - i) : len;
    const std::string drawn((const char *)(data + i), have);
    i += have;
    if ((op & 1) == 0) {
      steps.push_back({ini_model::Step::AddGroup, drawn, std::string()});
    }
    else {
      const size_t vlen = (i < size) ? (data[i++] % 8u) : 0u;
      const size_t vhave = (size - i < vlen) ? (size - i) : vlen;
      steps.push_back({ini_model::Step::Set, drawn, std::string((const char *)(data + i), vhave)});
      i += vhave;
    }
  }

  const ini_model::Result r = ini_model::check(cfg, steps);
  if (!r.failure.empty()) {
    std::fprintf(stderr, "%s\n%s\ndialect %s, crlf %d, emit_comments %d, normalize %d\noutput was (%zu bytes): %s\n", r.failure.c_str(), r.detail.c_str(),
        ini_model::dialects()[cfg.dialect].name, cfg.crlf, cfg.emit_comments, cfg.normalize, r.out.size(), ini_model::show(r.out).c_str());
    std::abort();
  }
  return 0;
}
