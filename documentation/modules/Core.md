@page core_module Core

# Core

The Core module provides the foundational utilities and infrastructure for the Ghoti.io Text library. This includes cross-compiler macros, version information API, and platform compatibility features.

---

## 1. Overview

The Core module consists of:

- **Cross-compiler macros** (`macros.h`) - Platform-independent utilities for common C programming patterns
- **Version API** (`text.h`) - Runtime access to library version information
- **Platform compatibility** - Compiler arms for GCC, Clang and MSVC; section
  4 below says which of those are exercised and which are only written

This module is automatically included when you use any part of the text library, as it provides the base macros and utilities used throughout the library.

---

## 2. Cross-Compiler Macros

The `macros.h` header provides macros that work consistently across different compilers and platforms.

### 2.1 Unused Parameter Suppression

**`GTEXT_MAYBE_UNUSED(X)`**

Marks a function parameter as potentially unused, suppressing compiler warnings. Useful for callback functions or functions with parameters reserved for future use.

**Compiler Support:**
- GCC/Clang: Uses `__attribute__((unused))`
- MSVC: Uses `(void)(X)` cast
- Other compilers: No-op (parameter remains as-is)

**Example:**
```c
#include <ghoti.io/text/macros.h>

void callback(int GTEXT_MAYBE_UNUSED(user_data)) {
    // user_data may not be used in all callback implementations
}
```

### 2.2 Deprecated Function Marking

**`GTEXT_DEPRECATED`**

Marks a function as deprecated, generating compiler warnings when the function is used. Helps with API migration and deprecation workflows.

**Compiler Support:**
- GCC/Clang: Uses `__attribute__((deprecated))`
- MSVC: Uses `__declspec(deprecated)`
- Other compilers: No-op

**Example:**
```c
GTEXT_DEPRECATED
void old_function(void);

// Usage will generate a deprecation warning:
// old_function();  // Warning: 'old_function' is deprecated
```

### 2.3 API Export/Import

**`GTEXT_API`**

Marks functions for export from shared libraries. Automatically handles:
- Windows DLL export/import (`__declspec(dllexport)` / `__declspec(dllimport)`)
- Unix symbol visibility (`__attribute__((visibility("default")))`)
- C++ linkage (`extern "C"`)

**Usage:**
- When building the library (`GTEXT_BUILD` defined): Functions are exported
- When using the library: Functions are imported (Windows) or visible (Unix)
- When using the library's **static archive on Windows** (`GTEXT_STATIC`
  defined): neither, just `extern "C"`

That third state is not optional. `__declspec(dllimport)` makes the compiler
reference `__imp_<symbol>` thunks, which only a DLL's import library provides;
an archive has none, so a consumer that links `libghoti.io-text-0.a` on
Windows without `-DGTEXT_STATIC` fails at link time with undefined `__imp_`
symbols. The test suite links the archive, which is why the Makefile defines
it for everything it builds but the library itself. `GTEXT_API_DATA` takes the
same three states.

**Example:**
```c
GTEXT_API void public_function(void);
GTEXT_API int process_data(const char *data);
```

### 2.4 Internal API for Testing

**`GTEXT_INTERNAL_API`**

Marks internal functions that should only be exported when building for testing. These functions are not part of the public API but may be needed for unit testing.

**Behavior:**
- When `GTEXT_TEST_BUILD` is defined: Same as `GTEXT_API`
- Otherwise: No export marking (function remains internal)

**Example:**
```c
GTEXT_INTERNAL_API void internal_helper_function(void);
```

### 2.5 Array Size Helper

**`GTEXT_ARRAY_SIZE(a)`**

Calculates the number of elements in a statically-allocated array at compile time. This is a compile-time constant and safe to use in constant expressions.

**Important:** Only works with actual arrays, not pointers. Passing a pointer will give incorrect results.

**Example:**
```c
int numbers[10];
size_t count = GTEXT_ARRAY_SIZE(numbers);  // count = 10

// Safe in constant expressions:
#define MAX_ITEMS 100
int items[MAX_ITEMS];
static_assert(GTEXT_ARRAY_SIZE(items) == MAX_ITEMS);
```

### 2.6 Bit Manipulation

**`GTEXT_BIT(x)`**

Creates a bitmask with a single bit set at the specified position (0-based).

**Example:**
```c
unsigned int flags = 0;
flags |= GTEXT_BIT(0);  // Set bit 0: flags = 0x01
flags |= GTEXT_BIT(3);  // Set bit 3: flags = 0x09
flags |= GTEXT_BIT(7);  // Set bit 7: flags = 0x89

// Check if bit is set:
if (flags & GTEXT_BIT(3)) {
    // Bit 3 is set
}
```

---

## 3. Version Information API

The `text.h` header provides runtime access to library version information.

### 3.1 Version Constants

The library defines compile-time version constants:

```c
#define GTEXT_VERSION_MAJOR 0
#define GTEXT_VERSION_MINOR 0
#define GTEXT_VERSION_PATCH 0
```

These can be used for compile-time version checks.

### 3.2 Version Functions

**`gtext_version_major()`**

Returns the major version number as a `uint32_t`.

**`gtext_version_minor()`**

Returns the minor version number as a `uint32_t`.

**`gtext_version_patch()`**

Returns the patch version number as a `uint32_t`.

**`gtext_version_string()`**

Returns a string representation of the version (e.g., `"0.0.0"`). The string is statically allocated and remains valid for the lifetime of the program.

**Example:**
```c
#include <ghoti.io/text/text.h>
#include <stdio.h>

int main(void) {
    printf("Ghoti.io Text Library Version: %s\n", gtext_version_string());
    printf("Version: %u.%u.%u\n",
           gtext_version_major(),
           gtext_version_minor(),
           gtext_version_patch());
    
    // Version checking:
    if (gtext_version_major() > 0 || gtext_version_minor() >= 1) {
        // Use newer API features
    }
    
    return 0;
}
```

---

## 4. Cross-Platform Considerations

This section separates what is **built and tested** from what the source
merely has an arm for. A `#if defined(_MSC_VER)` branch is not evidence that
the compiler it names can build this library: the arms you do not take are
never parsed.

### 4.1 Compilers

**Exercised**, every commit or every fuzz run:

- **GCC** - the build every target uses, at `-O2` with `-Werror` and the full
  warning set, on Linux and on Windows through MinGW-w64.
- **Clang** - builds the library sources for the fuzz harnesses and for the
  coverage target.

**Written but not exercised:**

- **MSVC** - `macros.h` has `_MSC_VER` arms for `GTEXT_MAYBE_UNUSED` and
  `GTEXT_DEPRECATED`, and `GTEXT_API` uses `__declspec`, which MSVC
  understands. Nothing here compiles the library with it and there is no
  MSVC project or CMake file, so treat it as unmeasured rather than
  supported. The [YAML page](@ref format_yaml) says the same thing under
  "Not implemented".
- **Other C99 compilers** - the macros degrade to portable spellings, and the
  library itself is C17.

### 4.2 Platforms

**Exercised:**

- **Linux** - the primary target; every test, sanitizer and conformance run.
- **Windows** - MinGW-w64 under MSYS2, natively: as of 2026-09-23 the whole
  suite builds and passes there. Windows is a third build in the Makefile, not a variation on
  the Linux one - it links the tests against the static archive, fixes the
  executable stack at 8 MiB, and puts the prefix's `bin/` on `PATH` because
  Windows has no rpath.

**Expected to work, not verified here:**

- **macOS and the BSDs** - the Makefile has a Darwin block and the code has
  no Linux-only syscalls, but no run is made on either.
- **Cygwin** - `macros.h` tests `__CYGWIN__` alongside `_WIN32`; untested.
- **Embedded targets** - the library needs a hosted C library: it opens,
  reads and renames files through ghoti.io-cutil.

### 4.3 C++ Compatibility

All headers are C++ compatible and use `extern "C"` linkage when included from C++ code. This allows the library to be used from C++ projects without issues.

---

## 5. Usage Examples

### 5.1 Basic Usage

```c
#include <ghoti.io/text/text.h>
#include <ghoti.io/text/macros.h>
#include <stdio.h>

GTEXT_API void example_function(int GTEXT_MAYBE_UNUSED(param)) {
    printf("Library version: %s\n", gtext_version_string());
}

int main(void) {
    example_function(42);
    return 0;
}
```

### 5.2 Version Checking

```c
#include <ghoti.io/text/text.h>

bool supports_feature(void) {
    // Check if library version supports a specific feature
    return gtext_version_major() > 0 ||
           (gtext_version_major() == 0 && gtext_version_minor() >= 1);
}
```

### 5.3 Flag Management

```c
#include <ghoti.io/text/macros.h>

#define FLAG_OPTION1 GTEXT_BIT(0)
#define FLAG_OPTION2 GTEXT_BIT(1)
#define FLAG_OPTION3 GTEXT_BIT(2)

void process_with_flags(unsigned int flags) {
    if (flags & FLAG_OPTION1) {
        // Handle option 1
    }
    if (flags & FLAG_OPTION2) {
        // Handle option 2
    }
    if (flags & FLAG_OPTION3) {
        // Handle option 3
    }
}
```

---

## 6. Best Practices

1. **Always use `GTEXT_API`** for public functions that should be exported from shared libraries.

2. **Use `GTEXT_MAYBE_UNUSED`** instead of `(void)param` casts for better cross-compiler compatibility.

3. **Prefer `GTEXT_ARRAY_SIZE`** over manual `sizeof` calculations for array size, as it's more readable and less error-prone.

4. **Check version at runtime** if your code depends on specific library features or bug fixes.

5. **Use `GTEXT_DEPRECATED`** when deprecating functions to help users migrate to new APIs.

---

@anchor core-thread-safety
## 7. Thread Safety

The rule is the same for JSON, CSV and YAML, and it is the usual one for a C
parser: **no object in this library is thread-safe, and no two threads may
touch the same object at once, but distinct objects share nothing and may be
used concurrently.**

Concretely:

| Used by two threads at once | Safe |
|---|---|
| The same DOM, table, parser, stream, reader or writer | **no** |
| Two DOMs, tables, streams or writers created separately | yes |
| The same DOM read-only from both, with no writer anywhere | yes |
| `gtext_*_parse()` on separate inputs into separate results | yes |
| The version accessors and the `*_options_default()` functions | yes |

This holds because the library keeps no mutable global state. Every parse
writes only into the context, arena or table it was given, and the only
file-scope variables in the sources are `const` tables - no function-local
`static`, no lazily built table, no cache. It was checked by searching for
non-const file-scope variables rather than assumed - two things that turned up
are worth naming, because they are easy to get wrong:

- **`gtext_version_string()` returns a compile-time constant.** There is no
  buffer for two threads to share.
- **Number formatting does not change the locale.** A double is formatted
  with whatever separator the locale gives, and that one byte is rewritten as
  `.`. A parse measures how far the number runs under C rules and hands
  `strtod` a copy spelled the way the current locale reads, so `"1,5"` in a
  German locale parses as `1`, stopping at the comma. Integer conversion has
  no locale-dependent element. No `setlocale`, `uselocale`, `newlocale` or
  `localeconv` appears in the library.

**A caller-supplied `GTEXT_Allocator` must be thread-safe if the objects using
it are touched from more than one thread.** The library adds no locking of its
own. The default stdlib allocator is thread-safe.

Each of the four "yes" rows above is run from eight threads by
`tests/test-concurrency.cpp`, which is also what `make test-tsan` is pointed
at. The two halves answer different questions and both are needed: under the
ordinary build those tests catch a torn or wrong *answer*, which is what a
user of a racy build would see, and under ThreadSanitizer they catch the race
whether or not it lost. `make test-asan` sees neither - ASan and UBSan detect
nothing about data races at all. Measured here rather than asserted: an
unlocked "build the defaults once and copy them" cache planted in
`gtext_json_parse_options_default()` passed ten out of ten plain runs and ten
out of ten ASan+UBSan runs, and ThreadSanitizer reported it on the first run,
naming both accesses.

The table's first row - the same object from two threads at once - is the
documented prohibition and is deliberately *not* tested. A test for it would be
a race on purpose: it would pass the plain build most of the time and fail
`make test-tsan` by design, which is a gate reporting a defect that is not one.

The guarantee itself is still not *enforced*. There is no internal locking to
disable and no thread-safe build variant; a caller that needs shared access
provides its own mutual exclusion. What the gate adds is that the property the
guarantee rests on - no mutable global state - can no longer be broken quietly
by an ordinary-looking optimisation.

## 8. Related Documentation

- [JSON Module](@ref json_module) - JSON parsing and serialization
- [CSV Module](@ref csv_module) - CSV reading and writing
- [Function Index](@ref text_functions_index) - Complete API reference
- [Main Documentation](@ref index) - Library overview
