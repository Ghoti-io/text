/**
 * @file
 *
 * libFuzzer harness for the markup tree.
 *
 * Nothing here parses. The input is a program for building and rearranging a
 * tree - create, insert, remove, replace, free, clone, set an attribute,
 * change data, give a template its content - and the properties checked are
 * the ones the model test (tests/test-markup-model.cpp) checks against a
 * reference, stated without needing one:
 *
 *     after every operation the tree's invariants hold; and
 *     an operation that returns anything but OK has changed nothing.
 *
 * "Changed nothing" is the whole of the contract for a refusal, and it is
 * checked by snapshot - the library's own dump of every top-level node - so a
 * link left pointing at a freed neighbour, a half-built clone left on the
 * detached list, or a node counted and never made all show up as a snapshot
 * that differs.
 *
 * An allocation failure is part of the program. The op that arms one says how
 * many allocations to let through first, so the fuzzer decides where the
 * failure falls and the coverage-guided search can steer towards the error
 * paths the model test only reaches by sweeping.
 *
 * Document options come from the first bytes, so one harness covers the
 * limits at every value, as the conventions ask.
 *
 * Build with: make fuzz-markup-tree      Run: make fuzz-run-markup-tree
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <ghoti.io/text/markup.h>
}

#include "../../src/markup/markup_internal.h"
#include "../markup_test_support.h"

namespace {

using RNode = GTEXT_Markup_Node;

[[noreturn]] void fail(const char * what, const std::string & detail) {
  // (The prefix is what tools/markup/plant-fuzz.py looks for, as the other harnesses' reports begin "fuzz_<name>: ".)
  std::fprintf(stderr, "fuzz_markup_tree: %s\n%s\n", what, detail.c_str());
  std::abort();
}

struct Reader {
  const uint8_t * p;
  size_t n;
  size_t i = 0;
  bool done() const {
    return i >= n;
  }
  uint8_t byte() {
    return i < n ? p[i++] : 0;
  }
};

// Every node reachable from the root or the detached list, template contents
// included - what the document owns.
std::vector<RNode *> collect(GTEXT_Markup_Document * doc) {
  std::vector<RNode *> out;
  std::vector<RNode *> tops;
  tops.push_back(gtext_markup_document_root(doc));
  for (RNode * d = doc->detached; d; d = d->next) {
    tops.push_back(d);
  }
  for (RNode * top : tops) {
    RNode * cur = top;
    for (;;) {
      out.push_back(cur);
      RNode * c = markup_ext_first(cur);
      if (c) {
        cur = c;
        continue;
      }
      for (;;) {
        if (cur == top) {
          goto next_top;
        }
        RNode * n = markup_ext_next(cur);
        if (n) {
          cur = n;
          break;
        }
        cur = cur->parent;
      }
    }
  next_top:;
  }
  return out;
}

std::string snapshot(GTEXT_Markup_Document * doc) {
  std::vector<std::string> parts;
  parts.push_back(dump_to_string(gtext_markup_document_root(doc)));
  for (RNode * d = doc->detached; d; d = d->next) {
    parts.push_back(dump_to_string(d));
  }
  std::sort(parts.begin() + 1, parts.end());
  std::string s;
  for (const std::string & p : parts) {
    s += p;
    s += '\x01';
  }
  s += std::to_string(gtext_markup_document_node_count(doc));
  return s;
}

const char * kNames[] = {"a", "b", "c", "template", "x", "svg"};
const char * kNamespaces[] = {nullptr, "urn:a", "urn:b", GTEXT_MARKUP_NS_HTML};
const char * kPrefixes[] = {nullptr, "p", "q"};
const char * kTexts[] = {"", "t", "hello", "caf\xC3\xA9", "x\ny", "<&>"};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  Reader in{data, size};

  FaultAllocator fa;
  GTEXT_Markup_Options opts = gtext_markup_options_default();
  const uint8_t o1 = in.byte();
  const uint8_t o2 = in.byte();
  static const size_t depths[] = {0, 2, 3, 5, 8, 256};
  static const size_t node_caps[] = {0, 8, 16, 40, 0, 0, 200, 0};
  static const size_t attr_caps[] = {0, 1, 2, 4, 1024, 0};
  opts.max_depth = depths[o1 % 6];
  opts.max_nodes = node_caps[(o1 / 6) % 8];
  opts.max_attributes = attr_caps[o2 % 6];
  opts.allocator = &fa.alloc;

  GTEXT_Markup_Document * doc = nullptr;
  if (gtext_markup_document_new(&opts, &doc) != GTEXT_MARKUP_OK) {
    return 0;
  }

  size_t ops = 0;
  while (!in.done() && ops++ < 400) {
    const uint8_t op = in.byte() % 16;
    std::vector<RNode *> live = collect(doc);
    auto operand = [&]() -> RNode * {
      return live[in.byte() % live.size()];
    };
    const std::string before = snapshot(doc);
    GTEXT_Markup_Status status = GTEXT_MARKUP_OK;
    bool mutating = true;
    RNode * out = nullptr;

    switch (op) {
      case 0: {
        const uint8_t a = in.byte();
        const uint8_t b = in.byte();
        status = gtext_markup_create_element(doc, kNamespaces[a % 4],
            kPrefixes[(a / 4) % 3], kNames[b % 6], &out);
        break;
      }
      case 1: {
        const char * t = kTexts[in.byte() % 6];
        status = gtext_markup_create_text(doc, t, strlen(t), &out);
        break;
      }
      case 2: {
        const uint8_t k = in.byte();
        const char * t = kTexts[(k / 2) % 6];
        status = (k & 1) ? gtext_markup_create_cdata(doc, t, strlen(t), &out)
                         : gtext_markup_create_comment(doc, t, strlen(t), &out);
        break;
      }
      case 3: {
        const uint8_t k = in.byte();
        const char * t = kTexts[(k / 2) % 6];
        status = (k & 1)
            ? gtext_markup_create_processing_instruction(doc, kNames[k % 6], t,
                  strlen(t), &out)
            : gtext_markup_create_doctype(doc, kTexts[k % 6],
                  (k & 2) ? "pub" : nullptr, (k & 4) ? "sys" : nullptr, &out);
        break;
      }
      case 4:
        status = gtext_markup_create_fragment(doc, &out);
        break;
      case 5: {
        RNode * p = operand();
        RNode * c = operand();
        status = gtext_markup_append_child(p, c);
        break;
      }
      case 6: {
        RNode * p = operand();
        RNode * c = operand();
        RNode * r = (in.byte() & 1) ? operand() : nullptr;
        status = gtext_markup_insert_before(p, c, r);
        break;
      }
      case 7:
        status = gtext_markup_remove(operand());
        break;
      case 8: {
        RNode * o = operand();
        RNode * n = operand();
        status = gtext_markup_replace(o, n);
        break;
      }
      case 9:
        status = gtext_markup_node_free(operand());
        break;
      case 10: {
        RNode * src = operand();
        const bool deep = in.byte() & 1;
        status = gtext_markup_clone(doc, src, deep, &out);
        break;
      }
      case 11: {
        RNode * e = operand();
        const uint8_t a = in.byte();
        const char * v = kTexts[in.byte() % 6];
        status = gtext_markup_element_set_attribute(e, kNamespaces[a % 4],
            kPrefixes[(a / 4) % 3], kNames[(a / 12) % 6], v, strlen(v));
        break;
      }
      case 12: {
        RNode * e = operand();
        const uint8_t a = in.byte();
        gtext_markup_element_remove_attribute(e, kNamespaces[a % 4],
            kNames[(a / 4) % 6]);
        mutating = false;  // returns a bool, and "nothing to remove" is not an error
        break;
      }
      case 13: {
        RNode * n = operand();
        const char * t = kTexts[in.byte() % 6];
        status = gtext_markup_node_set_data(n, t, strlen(t));
        break;
      }
      case 14:
        status = gtext_markup_element_ensure_template_content(operand(), nullptr);
        break;
      default: {
        // Arm a failure for the next operation: let N allocations through, then
        // fail one. This operation itself does nothing else, and is not checked
        // - and, above all, does not disarm what it just armed. (It did, once,
        // so no failure ever fired, and a planted defect on a failure path
        // went unfound for the length of a run: the harness was green because
        // it could not reach the thing it was written to check.)
        fa.arm(1 + in.byte() % 12);
        continue;
      }
    }

    // An armed failure that was not consumed must not leak into the checks
    // below, which allocate.
    const bool failed = fa.failed;
    fa.disarm();

    const char * problem = markup_check_invariants(doc);
    if (problem) {
      fail(problem, "op " + std::to_string(op) + "\nbefore:\n" + before);
    }
    if (mutating && status != GTEXT_MARKUP_OK) {
      const std::string after = snapshot(doc);
      if (after != before) {
        fail("a refused operation changed the tree",
            "op " + std::to_string(op) + " returned "
            + gtext_markup_status_string(status) + "\nbefore:\n" + before
            + "\nafter:\n" + after);
      }
    }
    if (status == GTEXT_MARKUP_E_OOM && !failed) {
      fail("OOM reported with no allocation failed", std::to_string(op));
    }
  }

  gtext_markup_document_free(doc);
  if (!fa.live.empty() || fa.bad_frees != 0) {
    fail("the document leaked or double-freed", std::to_string(fa.live.size())
        + " blocks, " + std::to_string(fa.bad_frees) + " bad frees");
  }
  return 0;
}
