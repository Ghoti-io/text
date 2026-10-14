/**
 * @file
 *
 * Shared by the markup tests: an allocator that checks the library's use of it
 * and can be told to fail one allocation.
 *
 * "Checks" is the point. A counting allocator can say how much is live; this
 * one also knows *which* blocks are, so a free of a pointer it never handed out
 * (or a second free of one it did) is reported rather than corrupting the heap
 * quietly, and a realloc is held to the contract the library relies on - a
 * failed one leaves the old block valid.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_MARKUP_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_MARKUP_TEST_SUPPORT_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/markup.h>

struct FaultAllocator {
  GTEXT_Allocator alloc;
  std::set<void *> live;
  size_t calls = 0;     // allocation attempts since arm()
  size_t fail_on = 0;   // 1-based: fail the Nth attempt since arm(); 0 = never
  bool failed = false;  // an attempt was failed since arm()
  size_t bad_frees = 0; // frees of a pointer this allocator does not hold

  FaultAllocator() {
    alloc.ctx = this;
    alloc.malloc_fn = &FaultAllocator::do_malloc;
    alloc.calloc_fn = &FaultAllocator::do_calloc;
    alloc.realloc_fn = &FaultAllocator::do_realloc;
    alloc.free_fn = &FaultAllocator::do_free;
  }

  // Fail the n-th allocation attempt from now (1 = the very next), once.
  void arm(size_t n) {
    calls = 0;
    fail_on = n;
    failed = false;
  }
  void disarm() {
    fail_on = 0;
  }

 private:
  bool should_fail() {
    ++calls;
    if (fail_on != 0 && calls == fail_on) {
      failed = true;
      return true;
    }
    return false;
  }
  static void * do_malloc(void * ctx, size_t size) {
    auto * self = static_cast<FaultAllocator *>(ctx);
    if (self->should_fail()) {
      return nullptr;
    }
    void * p = std::malloc(size ? size : 1);
    if (p) {
      self->live.insert(p);
    }
    return p;
  }
  static void * do_calloc(void * ctx, size_t nitems, size_t size) {
    auto * self = static_cast<FaultAllocator *>(ctx);
    if (self->should_fail()) {
      return nullptr;
    }
    if (size != 0 && nitems > SIZE_MAX / size) {
      return nullptr;
    }
    void * p = std::calloc(nitems ? nitems : 1, size ? size : 1);
    if (p) {
      self->live.insert(p);
    }
    return p;
  }
  static void * do_realloc(void * ctx, void * ptr, size_t size) {
    auto * self = static_cast<FaultAllocator *>(ctx);
    if (self->should_fail()) {
      return nullptr;
    }
    if (ptr && !self->live.count(ptr)) {
      self->bad_frees++;
      return nullptr;
    }
    void * p = std::realloc(ptr, size ? size : 1);
    if (p) {
      self->live.erase(ptr);
      self->live.insert(p);
    }
    return p;
  }
  static void do_free(void * ctx, void * ptr) {
    auto * self = static_cast<FaultAllocator *>(ctx);
    if (!ptr) {
      return;
    }
    if (self->live.erase(ptr) == 0) {
      self->bad_frees++;
      return;
    }
    std::free(ptr);
  }
};

// A node's subtree, or a whole document, as the library's own dump writes it.
inline std::string dump_to_string(const GTEXT_Markup_Node * node) {
#ifdef _WIN32
  // open_memstream is POSIX 2008 and MinGW has none: the dump goes to a file in the temporary directory and is read back.
  char dir[MAX_PATH + 1];
  char name[MAX_PATH + 1];
  std::string out;
  if (GetTempPathA(sizeof dir, dir) == 0 || GetTempFileNameA(dir, "gtx", 0, name) == 0) {
    return out;
  }
  if (FILE * f = std::fopen(name, "wb")) {
    gtext_markup_dump(node, f);
    std::fclose(f);
  }
  if (FILE * f = std::fopen(name, "rb")) {
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
      out.append(chunk, n);
    }
    std::fclose(f);
  }
  std::remove(name);
  return out;
#else
  char * buffer = nullptr;
  size_t size = 0;
  FILE * f = open_memstream(&buffer, &size);
  gtext_markup_dump(node, f);
  fclose(f);
  std::string out(buffer ? buffer : "", size);
  std::free(buffer);
  return out;
#endif
}

#endif // GHOTI_IO_GTEXT_TESTS_MARKUP_TEST_SUPPORT_H
