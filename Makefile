SUITE := ghoti.io
PROJECT := text

BUILD ?= release
# The version of this library. MINOR_VERSION carries the minor and the patch as
# one dotted string; the two are split out below for the places that need three
# separate integers. See CONVENTIONS.md section 4.
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
VERSION_MINOR_ONLY := $(word 1,$(subst ., ,$(MINOR_VERSION)))
VERSION_PATCH_ONLY := $(or $(word 2,$(subst ., ,$(MINOR_VERSION))),0)
# Substituted into the .pc file; an empty Version: field makes every
# pkg-config version constraint fail.
VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)

# Names this build everywhere: the .pc file, the install directory, the soname
# and the symbol token. It defaults to the major version, so an ordinary build
# of 1.x is "-1" and two majors cannot be loaded into one process by mistake.
# Override it for a build that wants its own identity:  make BRANCH=-dev
BRANCH ?= -$(MAJOR_VERSION)

# What the library reports as its version. The branch is appended only when it
# is not the default, so an ordinary build says "1.2.3" and an overridden one
# says "1.2.3-dev". Computed before BUILD=debug rewrites BRANCH below.
ifeq ($(BRANCH),-$(MAJOR_VERSION))
VERSION_STRING := $(VERSION)
else
VERSION_STRING := $(VERSION)$(BRANCH)
endif

# If BUILD is debug, append -debug.
#
# "override" because BRANCH may have come from the command line, and a
# command-line variable otherwise wins over a plain assignment here: without it
# `make BRANCH=-dev BUILD=debug` produced a debug build carrying the release
# token, whose symbols collide with the release build's.
ifeq ($(BUILD),debug)
    override BRANCH := $(BRANCH)-debug
    override VERSION_STRING := $(VERSION_STRING)-debug
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
# The symbol namespace token, from BRANCH, so that the token inside every
# exported symbol is the same one that names the .pc file, the install directory
# and the shared library. See CONVENTIONS.md section 4.
LIBVER_SYMBOL := $(shell echo "ghotiio_$(PROJECT)$(BRANCH)" | sed 's/[.-]/_/g')

BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
ENV_VARS :=

# PC_INSTALL_PATH names where this project's own .pc file is installed.
# PKG_CONFIG_PATH is the environment's and is never assigned here: make exports
# an inherited variable with whatever value the makefile last gave it, so
# overwriting it handed every sub-make a different PKG_CONFIG_PATH from the
# parent's. The sub-make then derived different flags, found the flag stamp
# changed, and rebuilt everything - which check-rebuild reports as a settled
# tree that will not settle. It showed first under MSYS2, whose login shell
# exports PKG_CONFIG_PATH, and happens on Linux whenever the exported value is
# not exactly the install location. cutil made the same change.
PKG_CONFIG_PATH_ENV := $(PKG_CONFIG_PATH)

# `override` on each of those: BUILD may arrive on the command line, and a
# command-line variable beats a plain makefile assignment, so without it
# `make BUILD=debug` skips the rewrite and builds into ./build/debug --
# outside the platform tree, and a different tree from the one plain `make`
# uses. The platform segment exists to keep linux/mac/win builds apart.

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	# Additional Linux-specific variables
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := linux/$(BUILD)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	LIB_EXTENSION := dylib
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	# Additional macOS-specific variables
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := mac/$(BUILD)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	# Windows paths for .pc so gcc invoked by mingw can resolve -I/-L (cygpath for MSYS2)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win32/$(BUILD)

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	# Windows paths for .pc so gcc invoked by mingw can resolve -I/-L (cygpath for MSYS2)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))

endif

# ---------------------------------------------------------------------------
# Installation prefix
#
# Defaults to the system location chosen above. Override it to install
# somewhere else - the suite's bootstrap installs every library into a local
# prefix so that each build resolves its dependencies through pkg-config,
# exactly as a consumer would, rather than through a second code path that
# only in-tree builds exercise. See CONVENTIONS.md section 1.
#
#     make install PREFIX=/path/to/prefix
# ---------------------------------------------------------------------------
ifdef PREFIX
INCLUDE_INSTALL_PATH := $(PREFIX)/include
LIB_INSTALL_PATH := $(PREFIX)/lib
BIN_INSTALL_PATH := $(PREFIX)/bin
PC_INSTALL_PATH := $(PREFIX)/share/pkgconfig
ifeq ($(OS_NAME), Windows)
PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
else
PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
endif
# A non-system prefix has no /etc/ld.so.conf.d, and writing to it would need
# root anyway. Everything built here carries an rpath to the prefix instead.
LDCONF_INSTALL_PATH :=
endif

# Dependencies are looked up along the inherited PKG_CONFIG_PATH as well as the
# install location chosen above, so that exporting PKG_CONFIG_PATH works as the
# errors below say it does. The inherited value comes first: it is an explicit
# request for this build, where the install location may be only a default.
PKG_CONFIG_LOOKUP_PATH := $(if $(PKG_CONFIG_PATH_ENV),$(PKG_CONFIG_PATH_ENV):)$(PC_INSTALL_PATH)


# The optimization level is the one thing that distinguishes the two builds'
# compile flags. `release` is what gets installed and what anything linking
# against this library actually runs, so it is compiled for speed; `debug` is
# compiled for stepping through. -g stays in both, because a release build
# that cannot be read in a debugger is a release build nobody can diagnose,
# and the symbols cost only file size.
#
# Until 2026-09 both were -O0. That was never decided: the production build
# doubled as the debugging build early on and nothing revisited it, so the
# `ifeq ($(BUILD),debug)` block above renamed the artifact and changed nothing
# about how it was compiled. The suite-wide floor is now -O2.
#
# Everything that wants a different level appends its own -O after this one,
# since the last -O on the command line wins: `make coverage` passes
# EXTRA_CFLAGS="--coverage -O0" and EXTRA_CFLAGS is last in CFLAGS, the
# sanitizer build carries -O1 in ASAN_UBSAN_FLAGS, and the fuzzers carry -O1
# in FUZZ_SAN, which does not derive from CFLAGS at all.
ifeq ($(BUILD),debug)
OPT_CFLAGS := -O0
else
OPT_CFLAGS := -O2
endif

CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 -O1 -g $(EXTRA_CXXFLAGS)
CC := cc
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c17 $(OPT_CFLAGS) -g $(EXTRA_CFLAGS)
# Library-specific compile flags (export symbols on Windows, PIC on Linux)
# GTEXT_BUILD enables DLL export on Windows (checked by GTEXT_API macro)
# GTEXT_TEST_BUILD enables export of internal functions for testing (checked by GTEXT_INTERNAL_API macro)
# No -DGTEXT_TEST_BUILD: the shipped library exports its public API and nothing
# else. Tests reach the internals by linking the static archive, which a static
# link can do even for hidden symbols.
ifeq ($(OS_NAME), Windows)
# Everything built here but the library itself links the static archive, so
# the headers must not say dllimport to it: an archive has no __imp_ thunks.
# The library's own objects also get GTEXT_BUILD, which the header tests first.
# See GTEXT_API in macros.h.
CFLAGS += -DGTEXT_STATIC
CXXFLAGS += -DGTEXT_STATIC
endif
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -DGTEXT_BUILD $(EXTRA_CFLAGS)
# -DGHOTIIO_CUTIL_ENABLE_MEMORY_DEBUG
LDFLAGS := -L /usr/lib -lstdc++ -lm $(EXTRA_LDFLAGS)
ifdef PREFIX
# So that a library, a test or an example finds its Ghoti.io dependencies in the
# prefix at run time without LD_LIBRARY_PATH.
LDFLAGS += -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Windows)
# Windows has no rpath: a program finds its DLLs through PATH. Putting the
# prefix's bin/ on it for everything make runs is the equivalent, so that a
# test or an example finds its dependencies without the caller arranging it.
# Without this they die before main() with 0xC0000135 and make reports 127.
export PATH := $(BIN_INSTALL_PATH):$(PATH)
endif
endif

BUILD_DIR := ./build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/objects
FLAGS_STAMP := $(OBJ_DIR)/.flags
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps


# Add OS-specific flags
ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC

else ifeq ($(UNAME_S), Darwin)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows

else
	$(error Unsupported OS: $(UNAME_S))

endif

# The standard include directories for the project.
INCLUDE := -I include/ -I $(GEN_DIR)/

# Goals that compile and link nothing.  A missing sibling library must not stop
# them: `make clean` needs rm, not cutil, and the $(error) below fires while
# this file is being *read*, so it takes out every target rather than the ones
# that need a dependency.  `clean` is where that is least expected and least
# visible, because nobody reads clean's output - four of the suite's nine
# libraries were found failing it this way during a workspace-wide rebuild,
# each reporting that the fix was to run bootstrap.sh, from inside bootstrap.
#
# The two `ifndef SKIP_DEP_CHECK` guards below have been here for as long as
# the errors have.  Nothing ever set the variable, so both were inert, which
# is worse than their being absent: the mechanism is visible in the file and
# does nothing, so reading it tells you the case is handled.
#
# $(or $(MAKECMDGOALS),all) is the load-bearing part.  With no goal named,
# MAKECMDGOALS is empty and filter-out over nothing is also empty, so a bare
# `make` would skip the check the error exists for.  Substituting `all` - a
# goal that is not in this list - is what keeps the default build honest.
DEPLESS_GOALS := clean fuzz-clean docs docs-pdf cloc help
ifeq ($(filter-out $(DEPLESS_GOALS),$(or $(MAKECMDGOALS),all)),)
SKIP_DEP_CHECK := 1
endif

# ghoti.io-cutil, for GCU_Allocator.  text used to declare its own copy of that
# vtable because CONVENTIONS.md described the library as standalone; that was a
# misreading - a dependency inside the suite is fine as long as the graph stays
# a DAG, and cutil is its root. compress, image and model all take the same
# dependency for the same type, and one definition is what lets an allocator
# written for any of them work with all of them.
CUTIL_PC ?= ghoti.io-cutil$(BRANCH)
CUTIL_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(CUTIL_PC) 2>/dev/null)
CUTIL_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(CUTIL_PC) 2>/dev/null)
ifeq ($(strip $(CUTIL_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-cutil was not found by pkg-config. Run ./bootstrap.sh in the parent folder to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback: a second resolution path that only in-tree builds exercise is one that silently rots.)
endif
endif
INCLUDE += $(CUTIL_CFLAGS)
# A link dependency, not just a header one: gtext_allocator_default() returns
# cutil's default allocator rather than reimplementing it.
LDFLAGS += $(CUTIL_LIBS)

# ghoti.io-chron, for YAML's !!timestamp.  The type YAML 1.1 defines is a
# calendar date, a wall-clock reading and an offset, and this library used to
# read it with a parser of its own - a hundred lines in yaml_resolve.c that
# were off-spec in five ways, because they had never been held against an
# outside implementation.  Time is not a text format's business, and chron
# owns it for the same reason cutil owns the allocator: one definition, held
# against the reference implementation, rather than a copy per consumer.
#
# The graph stays a DAG: cutil -> chron -> text.
CHRON_PC ?= ghoti.io-chron$(BRANCH)
CHRON_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(CHRON_PC) 2>/dev/null)
CHRON_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(CHRON_PC) 2>/dev/null)
ifeq ($(strip $(CHRON_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-chron was not found by pkg-config. Run ./bootstrap.sh in the parent folder to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback: a second resolution path that only in-tree builds exercise is one that silently rots.)
endif
endif
INCLUDE += $(CHRON_CFLAGS)
LDFLAGS += $(CHRON_LIBS)

# ghoti.io-unicode, for the Unicode Character Database and the algorithms over
# it.  This library used to generate its own tables: NFC's composition data
# from UnicodeData.txt, and ID_Start/ID_Continue and General_Category=Zs for
# JSON5's identifiers, from DerivedCoreProperties.txt.  regex generated the
# same tables from the same files with a second generator, and font would have
# been the third - three copies of one dataset with three version pins that
# nothing compared.  unicode holds one copy, one pin and one generator, and
# every property here is read from it.
#
# What did not move: IDNA2008's derived property and UTS #46's mapping, in
# src/idna/tables/.  Those are Unicode standards about host names rather than
# character data, and this is their only consumer (unicode's design.md §15
# decision 4).
#
# The graph stays a DAG: cutil -> unicode -> text.
UNICODE_PC ?= ghoti.io-unicode$(BRANCH)
UNICODE_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(UNICODE_PC) 2>/dev/null)
UNICODE_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(UNICODE_PC) 2>/dev/null)
ifeq ($(strip $(UNICODE_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-unicode was not found by pkg-config. Run ./bootstrap.sh in the parent folder to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback: a second resolution path that only in-tree builds exercise is one that silently rots.)
endif
endif
INCLUDE += $(UNICODE_CFLAGS)
LDFLAGS += $(UNICODE_LIBS)

# ghoti.io-regex, for JSONPath match() and search(). Those two functions are
# I-Regexp (RFC 9485), and this is the library that implements it. The call
# is inside src/json/json_path.c; no public header names regex.
#
# regex's own manifest still lists text as optional. That edge is two programs
# in the regex repository - the JSON Schema adapter and its suite runner -
# and it is not a link of either library. This edge is the other one, and it
# is why regex is built first.
#
# The graph stays a DAG: cutil -> unicode -> regex -> text.
REGEX_PC ?= ghoti.io-regex$(BRANCH)
REGEX_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(REGEX_PC) 2>/dev/null)
REGEX_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(REGEX_PC) 2>/dev/null)
ifeq ($(strip $(REGEX_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-regex was not found by pkg-config. Run ./bootstrap.sh in the parent folder to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback: a second resolution path that only in-tree builds exercise is one that silently rots.)
endif
endif
INCLUDE += $(REGEX_CFLAGS)
LDFLAGS += $(REGEX_LIBS)

# The four above, in one place, because every consumer of this list used to
# carry its own copy and they drifted.  When chron arrived, tools/conformance's
# scripts still asked pkg-config for cutil alone, so `make conformance` stopped
# compiling and nothing noticed - it is not a target `make test` runs.  The same
# thing happened again when unicode arrived: six conformance targets and the
# fuzzers all failed to link, in fourteen hand-written dependency lists.  So the
# Makefile hands the list to the scripts the way it hands them PREFIX, and a
# script with no list refuses rather than falling back to a stale one.
DEP_PCS := $(CUTIL_PC) $(CHRON_PC) $(UNICODE_PC) $(REGEX_PC)
DEP_CFLAGS := $(CUTIL_CFLAGS) $(CHRON_CFLAGS) $(UNICODE_CFLAGS) $(REGEX_CFLAGS)
DEP_LIBS := $(CUTIL_LIBS) $(CHRON_LIBS) $(UNICODE_LIBS) $(REGEX_LIBS)

# Automatically collect all .c source files under the src directory.
SOURCES := $(shell find src -type f -name '*.c')

# Convert each source file path to an object file path.
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))


TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest gtest_main`
ifeq ($(OS_NAME), Windows)
# The tests were written against Linux's 8 MiB main-thread stack, and some
# build fixtures with the recursive JSON parser at a max_depth well past the
# default: test-json-to-yaml parses 5000 nested arrays. A PE executable's
# stack is fixed at link time, 2 MiB by MinGW's default, and that fixture
# overflowed it before the code under test ran. Give the tests what they
# assume. The library is not affected; its default max_depth fits either way.
TESTFLAGS += -Wl,--stack,8388608
endif


# The static archive, not -l: a static link resolves hidden symbols, so the
# tests can exercise internals the shared library does not export.
# --whole-archive because anything that registers itself from a constructor is
# otherwise dropped - a plain archive link only pulls in object files that
# something references by name.
TEXTLIBRARY := -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive

# Valgrind configuration for memory checking
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 --suppressions=tools/valgrind.supp

# Sanitizer flags (ASan + UBSan)
#
# -fno-sanitize-recover is what makes the second half of that a gate.  ASan
# aborts on a finding, so `|| exit 1` in the loop below catches it; UBSan by
# default prints a diagnostic and *runs on past the defect*, the process exits
# 0, and the run is reported as clean.  A gate that cannot fail is not a gate.
#
# One list, named once, because the two flags have to agree and a check named
# in only one of them is worse than a check named in neither.  Measured here
# on gcc 14.2, one defect per probe program - a program with two shows only
# whichever aborts first:
#
#   -fsanitize=address,undefined -fno-sanitize-recover=undefined
#       (int)1e30 -> silent, exit 0
#   float-cast-overflow added to -fsanitize= only
#       (int)1e30 -> diagnosed, exit 0
#   float-cast-overflow in both
#       (int)1e30 -> diagnosed, exit 1
#
# The middle row is the trap, and it looks like progress because output
# appears where there was none: -fno-sanitize-recover=undefined does not
# cover a check outside gcc's `undefined` group even when that check is
# explicitly enabled.  float-cast-overflow is in clang's `undefined` and not
# in gcc's, which is how it came to be missing from a build that reads as
# though it asked for everything.
#
# Verify by exit status, never by output.  A firing UBSan gate aborts the
# process before gtest prints anything, so the run contains no
# "[  FAILED  ]" line at all - a summary that counts those reads a firing
# gate as green.
UBSAN_CHECKS := undefined,float-cast-overflow
# -O1, pinned rather than inherited from OPT_CFLAGS, and it must stay last so
# it wins: ASAN_CFLAGS puts these after $(CFLAGS).
#
# Pinned because the level buys the gate nothing and costs it independence.
# Measured on gcc 14.2, one defect per program so that halting at the first
# finding cannot hide a later one: heap-use-after-free, stack-buffer-overflow,
# signed overflow and float-cast-overflow are all caught identically at -O1
# and -O2, with identical reports.  A strict-aliasing violation is caught at
# neither, which is worth saying because it was the argument for inheriting:
# no sanitizer in this toolchain detects one, so running the gate at the
# release level does not buy the aliasing coverage it sounds like it should.
#
# Pinning also keeps this gate and the fuzzers on the same codegen - FUZZ_SAN
# is -O1 too - so a finding reproduces between them, and it stops the gate
# silently changing the next time the release level does.
ASAN_UBSAN_FLAGS := -fsanitize=address,$(UBSAN_CHECKS) \
                    -fno-sanitize-recover=$(UBSAN_CHECKS) \
                    -fno-omit-frame-pointer -g -O1

# Sanitizer build directory
ASAN_BUILD_DIR := $(BUILD_DIR)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.flags
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps

# Sanitizer target names
ASAN_TARGET := $(BASE_NAME_PREFIX)-asan.$(LIB_EXTENSION)
# Soname for the instrumented library: its own filename, since it is only ever
# loaded out of the build tree and never installed.
ifeq ($(UNAME_S),Linux)
	ASAN_LIBRARY_NAME_FLAG := -Wl,-soname,$(ASAN_TARGET)
else
	ASAN_LIBRARY_NAME_FLAG :=
endif
STATIC_TARGET := $(BASE_NAME_PREFIX).a
ASAN_STATIC_TARGET := $(BASE_NAME_PREFIX)-asan.a

# Sanitizer object files and library
ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES))

####################################################################
# ThreadSanitizer
####################################################################

# A separate tree from the ASan one because the two runtimes cannot be linked
# together, and a separate target because `make test` must not depend on a
# sanitizer this library needs only as a standing guard.
#
# ## What this gate is for, since it is not looking for a known bug
#
# This library has **no mutable state outside the caller's objects**: no
# file-scope variable that is not const, no function-local static, no lazily
# built table, no cache. Every byte a call touches is on its stack, in a
# document the caller owns, or from the allocator the caller supplied. That is
# why its concurrency guarantee can be as strong as it is, and it is a property
# of the code rather than of a lock - there is no lock to inspect.
#
# A property like that needs a gate, because the thing that breaks it is an
# ordinary-looking improvement: a memo on a hot path, a table built on first
# use, a cached default. `make test-asan` cannot see any of it - ASan and UBSan
# detect nothing about data races, measured: a mutex removed from a guarded
# cache in a sibling library passed every ASan run and was reported by
# ThreadSanitizer on the first.
#
# ## Why every test, not the threaded ones
#
# Three test files run threads today and the rest do not, so most of this build
# is instrumentation over single-threaded code that can report nothing. A list
# of "the threaded tests" would be cheaper and is the wrong shape: it is a list
# someone has to remember to extend, and the test that matters is the one that
# grows a thread later. The same argument check-headers makes about globbing
# include/.
#
# ## Two things not to copy from the ASan target
#
# There is no LD_PRELOAD of the runtime here. Linking with -fsanitize=thread
# already puts libtsan first in the executable's NEEDED list, so it buys
# nothing - and it breaks every system() call, because the spawned shell
# inherits the preload and dies. Several tests here shell out to an oracle.
#
# LD_PRELOAD *is* cleared, for the same reason the ASan target clears it: a
# desktop-wide preload inherited from the session (Debian sets
# libgtk3-nocsd.so.0) gets ahead of the sanitizer runtime.
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g -O1

TSAN_BUILD_DIR := $(BUILD_DIR)-tsan
TSAN_OBJ_DIR := $(TSAN_BUILD_DIR)/objects
TSAN_FLAGS_STAMP := $(TSAN_OBJ_DIR)/.flags
TSAN_APP_DIR := $(TSAN_BUILD_DIR)/apps

TSAN_TARGET := $(BASE_NAME_PREFIX)-tsan.$(LIB_EXTENSION)
ifeq ($(UNAME_S),Linux)
	TSAN_LIBRARY_NAME_FLAG := -Wl,-soname,$(TSAN_TARGET)
else
	TSAN_LIBRARY_NAME_FLAG :=
endif

TSAN_LIBOBJECTS := $(patsubst src/%.c,$(TSAN_OBJ_DIR)/%.o,$(SOURCES))

####################################################################
# Test discovery
####################################################################

# The static archive, not -l: a static link resolves hidden symbols, so the
# tests can exercise internals the shared library does not export.
# --whole-archive because anything that registers itself from a constructor is
# otherwise dropped - a plain archive link only pulls in object files that
# something references by name.
TEXTLIBRARY := -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive

# Single shell: discover test sources and compute executable name for each (path|name per line).
# test.cpp -> testText; test-yaml-*.cpp -> testYaml*. Avoids hundreds of $(call test-name) / CreateProcess.
# The gates `make test` runs alongside the tests.  A coverage build clears
# this: --coverage links the gcov runtime, which exports mangle_path, and
# check-symbols is right to reject that in a shipping build but it is not a
# defect in an instrumented one.
ALL_TEST_GATES := check-symbols check-allocators check-allocator-callees check-headers \
	check-fuzz-harnesses \
	check-oracle-env \
	check-idna-tables check-idna-oracle check-nfc-oracle check-ucd-pin \
	check-metaschema
TEST_GATES ?= $(ALL_TEST_GATES)

# Four of those gates used to exit 0 when they could not run: three printed
# "skipped (no python3 ...)" and four printed "skipped (no third_party/...)".
# Nothing under third_party/ is tracked - it is gitignored and arrives through
# tools/idna/fetch.sh and tools/metaschema/fetch.sh - so the absent case is not
# a rare machine, it is **every clone**, and `make test` reported success with
# half its gates having compared nothing.
#
# The rule for which way a missing tool should go, from the regex session: a
# gate may skip when the property it checks *cannot exist* in that environment,
# and must fail when the property exists and the gate merely cannot see it. A
# wrong table is wrong whether or not this machine has python3, so exit 0 is a
# false statement about it.
#
# Both macros print the per-gate opt-out rather than offering a global one, so
# that dropping a check is visible in the command someone typed instead of in
# the output of a run that looked like it passed.
#
# The `#` in each banner is escaped as `\#`: this is a variable assignment, and
# make lexes an unescaped `#` as the start of a comment and truncates the rest
# of the value silently. The same text is safe in a recipe line, which is where
# it used to live. (regex hit this first; it is worth the two lines.)
REQUIRE_PYTHON3 = if ! command -v python3 >/dev/null 2>&1; then \
		printf "\033[0;31m\n\#\#\# $@: python3 is missing \#\#\#\033[0m\n" >&2; \
		printf "\nThis gate is a python3 script. Without an interpreter it does not\n" >&2; \
		printf "check less - it checks nothing, and used to say so only by printing\n" >&2; \
		printf "\"skipped\" and exiting 0.\n\n" >&2; \
		printf "If this machine genuinely has no python3, drop the gate for the run,\n" >&2; \
		printf "so the choice is visible in the command:\n\n" >&2; \
		printf "  make test TEST_GATES='\$$(filter-out $@,\$$(ALL_TEST_GATES))'\n\n" >&2; \
		exit 1; \
	fi

# $1 is the directory, $2 the script that fetches it.
REQUIRE_DATA = if [ ! -d "$1" ]; then \
		printf "\033[0;31m\n\#\#\# $@: $1 is not here \#\#\#\033[0m\n" >&2; \
		printf "\nThis gate compares committed output against the data it was\n" >&2; \
		printf "generated from. Nothing under third_party/ is tracked, so a fresh\n" >&2; \
		printf "clone has none of it and this gate used to print \"skipped\" and exit\n" >&2; \
		printf "0 - which says the committed output is right, having read none of it.\n\n" >&2; \
		printf "Fetch it:\n\n  $2\n\n" >&2; \
		printf "Or drop the gate for the run, so the choice is in the command:\n\n" >&2; \
		printf "  make test TEST_GATES='\$$(filter-out $@,\$$(ALL_TEST_GATES))'\n\n" >&2; \
		exit 1; \
	fi

TEST_PAIRS := $(shell find tests -type f -name 'test*.cpp' -o -name 'test-*.cpp' 2>/dev/null | sort | while read f; do \
	if [ "$$f" = "tests/test.cpp" ]; then echo "$$f|testText"; \
	else echo "$$f|$$(basename "$$f" .cpp | sed 's/test-/test/g; s/test_/test/g; s/-\([a-z]\)/\U\1/g; s/_\([a-z]\)/\U\1/g; s/^test\([a-z]\)/test\U\1/')"; fi; done)
TEST_SOURCES := $(foreach pair,$(TEST_PAIRS),$(word 1,$(subst |, ,$(pair))))
TEST_NAMES := $(foreach pair,$(TEST_PAIRS),$(word 2,$(subst |, ,$(pair))))

# Generate list of test executables (no $(call test-name) - use precomputed TEST_NAMES)
TEST_EXECUTABLES := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# ASan test executables
ASAN_TEST_EXECUTABLES := $(patsubst $(APP_DIR)/%,$(ASAN_APP_DIR)/%,$(TEST_EXECUTABLES))

# TSan test executables
TSAN_TEST_EXECUTABLES := $(patsubst $(APP_DIR)/%,$(TSAN_APP_DIR)/%,$(TEST_EXECUTABLES))

# Automatically collect all example .c files, one directory per module.  A new
# module's examples are picked up by adding it to EXAMPLE_MODULES; yaml was
# omitted here for a long time, so its examples were never compiled and nothing
# noticed when they stopped building.
EXAMPLE_MODULES := json csv yaml

JSON_EXAMPLE_SOURCES := $(shell find examples/json -type f -name '*.c' 2>/dev/null)
CSV_EXAMPLE_SOURCES := $(shell find examples/csv -type f -name '*.c' 2>/dev/null)
YAML_EXAMPLE_SOURCES := $(shell find examples/yaml -type f -name '*.c' 2>/dev/null)
EXAMPLE_SOURCES := $(JSON_EXAMPLE_SOURCES) $(CSV_EXAMPLE_SOURCES) $(YAML_EXAMPLE_SOURCES)

# Convert each example source file path to an executable path.
JSON_EXAMPLES := $(patsubst examples/json/%.c,$(APP_DIR)/examples/json/%$(EXE_EXTENSION),$(JSON_EXAMPLE_SOURCES))
CSV_EXAMPLES := $(patsubst examples/csv/%.c,$(APP_DIR)/examples/csv/%$(EXE_EXTENSION),$(CSV_EXAMPLE_SOURCES))
YAML_EXAMPLES := $(patsubst examples/yaml/%.c,$(APP_DIR)/examples/yaml/%$(EXE_EXTENSION),$(YAML_EXAMPLE_SOURCES))
EXAMPLES := $(JSON_EXAMPLES) $(CSV_EXAMPLES) $(YAML_EXAMPLES)


all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build the shared and static libraries

####################################################################
# Dependency Inclusion
####################################################################

# Explicit list of dependency files (no wildcard: same set on all platforms, faster make startup).
TEST_DEPFILES := $(addprefix $(APP_DIR)/,$(addsuffix .d,$(TEST_NAMES)))
# The ASan build needs these as much as the release build does.  Without them
# a header change rebuilds nothing under release-asan, and the stale objects
# disagree with the freshly built ones about struct layout - which shows up as
# an AddressSanitizer report in code that is correct, and can equally hide a
# report in code that is not.
ASAN_TEST_DEPFILES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix .d,$(TEST_NAMES)))
TSAN_TEST_DEPFILES := $(addprefix $(TSAN_APP_DIR)/,$(addsuffix .d,$(TEST_NAMES)))
DEPFILES := $(LIBOBJECTS:.o=.d) $(TEST_DEPFILES) \
	$(ASAN_LIBOBJECTS:.o=.d) $(ASAN_TEST_DEPFILES) \
	$(TSAN_LIBOBJECTS:.o=.d) $(TSAN_TEST_DEPFILES)
-include $(DEPFILES)


####################################################################
# Object Files
####################################################################

# Pattern rule for C source files: compile .c files to .o files, generating dependency files.
####################################################################
# Generated version header
####################################################################

LIBVER_GEN := $(GEN_DIR)/ghoti.io/$(PROJECT)/libver_gen.h
# EVERY rule that compiles a translation unit carries `| $(LIBVER_GEN)`, not
# just the release library's. Each one reaches this generated header -
# macros.h includes namespace.h includes libver.h includes libver_gen.h - so
# a rule without it works only on a tree where something else already
# generated the file. That is the worst shape a build defect takes: it passes
# for everyone who has built before and fails for everyone who has not, and
# under -j it is a race rather than a clean failure.
#
# CONVENTIONS.md section 6 states the rule and section 12 warns that copying
# a Makefile copies its defects. This is that: the release C rule had it and
# the ASan and fuzz rules did not. From a clean tree, asking for a single
# ASan object failed outright:
#
#     include/ghoti.io/text/libver.h:42:10: fatal error:
#     ghoti.io/text/libver_gen.h: No such file or directory
#
# The release C++ rule is corrected for the same reason even though src/
# holds no .cpp file today, so the trap is not left armed for whoever adds
# the first one.

# libver_gen.h is regenerated on every build and rewritten only when its content
# changes, so a variable given on the command line - make MAJOR_VERSION=2, or
# make BRANCH=-dev - takes effect. Keying the rule on the Makefile's timestamp
# alone left the previous token and version baked into the build, and nothing
# said so.
.PHONY: force-libver
force-libver:

$(LIBVER_GEN): force-libver
	@if [ -z "$(LIBVER_SYMBOL)" ]; then \
		printf "### LIBVER_SYMBOL is empty ###\n" >&2; \
		printf "Every exported symbol would lose its version namespace.\n" >&2; \
		exit 1; \
	fi
	@mkdir -p $(@D)
	@printf '%s\n' \
		'// Generated by the Makefile. Do not edit; see CONVENTIONS.md section 4.' \
		'#ifndef GHOTI_IO_GTEXT_LIBVER_GEN_H' \
		'#define GHOTI_IO_GTEXT_LIBVER_GEN_H' \
		'' \
		'/** The symbol namespace for this build, from the Makefile'"'"'s BRANCH. */' \
		'#define GHOTIIO_TEXT_NAME $(LIBVER_SYMBOL)' \
		'' \
		'/** Human-readable version of this build. */' \
		'#define GHOTIIO_TEXT_VERSION "$(VERSION_STRING)"' \
		'' \
		'/** The same version as three integers. */' \
		'#define GHOTIIO_TEXT_VERSION_MAJOR $(MAJOR_VERSION)' \
		'#define GHOTIIO_TEXT_VERSION_MINOR $(VERSION_MINOR_ONLY)' \
		'#define GHOTIIO_TEXT_VERSION_PATCH $(VERSION_PATCH_ONLY)' \
		'' \
		'#endif // GHOTI_IO_GTEXT_LIBVER_GEN_H' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for C++ source files (if any):
$(OBJ_DIR)/%.o: src/%.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@


####################################################################
# Shared Library
####################################################################

$(APP_DIR)/$(STATIC_TARGET): $(LIBOBJECTS)
	@printf "\n### Archiving Text Library ###\n"
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(APP_DIR)/$(TARGET): \
		$(LIBOBJECTS)
	@printf "\n### Compiling Text Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)

ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

####################################################################
# Unit Tests
####################################################################

# Compile each test .cpp directly to executable. Fewer targets = faster make graph.
# Args: $1 = source path, $2 = executable name (from TEST_PAIRS).
define test-executable-rule
# The library is a normal prerequisite, not an order-only one.  The tests link
# $(STATIC_TARGET) with --whole-archive, so a change to the library has to
# relink them; behind `|` it did not, and `make test` would happily run last
# build's binaries against this build's sources.  That produces both phantom
# failures and, worse, phantom passes.
$(APP_DIR)/$2$(EXE_EXTENSION): \
		$1 \
		$(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET)
	@printf "\n### Compiling %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) $$(INCLUDE) -MMD -MP -MF $$(APP_DIR)/$2.d -o $$@ $$< $$(TEXTLIBRARY) $$(LDFLAGS) $$(TESTFLAGS)
endef

# Generate build rules from TEST_PAIRS (one pair = source|name)
$(foreach pair,$(TEST_PAIRS),$(eval $(call test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

# tests/test-headers.c is C, and TEST_PAIRS above globs only test*.cpp and
# test-*.cpp - so this file had never been compiled, not once, since it was
# written.  That is why its YAML smoke functions were defined and never called
# from main(), and why it covered no CSV header at all: nothing was ever in a
# position to notice.
#
# It is kept as C rather than renamed to .cpp because every other test in this
# project is C++, and this is the only place a C consumer compiles against
# these headers.  It links the library but not gtest; success is exit 0.
#
# Appended to TEST_EXECUTABLES here, below the point where
# ASAN_TEST_EXECUTABLES is derived from it, so the ASan build does not try to
# apply the C++ test rule to a C source.
$(APP_DIR)/testHeaders$(EXE_EXTENSION): \
		tests/test-headers.c \
		$(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET)
	@printf "\n### Compiling testHeaders Test ###\n"
	@mkdir -p $(@D)
# -Werror=unused-function overrides the -Wno-error=unused-function in CFLAGS,
# which is set for reasons elsewhere in the tree.  Every smoke function here
# must be reachable from main(); a new one that nobody calls is the defect
# this file already had, so it is a build error here rather than a warning.
	$(CC) $(CFLAGS) -Werror=unused-function $(INCLUDE) -MMD -MP -MF $(APP_DIR)/testHeaders.d -o $@ $< $(TEXTLIBRARY) $(LDFLAGS)

TEST_EXECUTABLES += $(APP_DIR)/testHeaders$(EXE_EXTENSION)

####################################################################
# Examples
####################################################################

# $(TEXTLIBRARY) comes before $(LDFLAGS), as it does in the test rule.  These
# three rules had them the other way round, so -lghoti.io-cutil-0 was offered
# to the linker before the archive that references it and every example failed
# to link with an undefined reference to gcu_allocator_default.  Nothing
# noticed because `make examples` is not part of `make test`; it is a CI step
# now.

# Pattern rule for JSON example executables
# Links the archive, so it depends on the archive: naming only the shared
# library left nothing in the chain that builds the archive, so a clean tree
# could not build the examples at all.
$(APP_DIR)/examples/json/%$(EXE_EXTENSION): examples/json/%.c \
		$(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(TEXTLIBRARY) $(LDFLAGS)

# Pattern rule for CSV example executables
# Links the archive, so it depends on the archive: naming only the shared
# library left nothing in the chain that builds the archive, so a clean tree
# could not build the examples at all.
$(APP_DIR)/examples/csv/%$(EXE_EXTENSION): examples/csv/%.c \
		$(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(TEXTLIBRARY) $(LDFLAGS)

# Pattern rule for YAML example executables
# Links the archive, so it depends on the archive: naming only the shared
# library left nothing in the chain that builds the archive, so a clean tree
# could not build the examples at all.
$(APP_DIR)/examples/yaml/%$(EXE_EXTENSION): examples/yaml/%.c \
		$(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(TEXTLIBRARY) $(LDFLAGS)

####################################################################
# Sanitizer Builds (ASan + UBSan)
####################################################################

# Compile flags for ASan builds (include UBSan for comprehensive checking)
# ASAN_UBSAN_FLAGS comes after $(CFLAGS) and carries its own -O1, so that is
# the level the sanitizer build uses whatever OPT_CFLAGS says.  See the note
# there for why it is pinned rather than inherited.
ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGTEXT_BUILD -DGTEXT_TEST_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_TEXTLIBRARY := -L $(ASAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-asan

# Add PIC on Linux
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

# Pattern rule for ASan-instrumented C object files
$(ASAN_OBJ_DIR)/%.o: src/%.c $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling (ASan+UBSan instrumented): $< ###\n"
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# ASan-instrumented shared library
# The soname has to name this file, not the release library's.  Using
# OS_SPECIFIC_LIBRARY_NAME_FLAG here stamped the instrumented library with the
# release soname, so every ASan test binary recorded a dependency on
# libghoti.io-text-0.so.0 - which is not what this file is called and is not in
# ASAN_APP_DIR.  `make test-asan` died at the first test with "error while
# loading shared libraries" before running anything.
$(ASAN_APP_DIR)/$(ASAN_TARGET): $(ASAN_LIBOBJECTS)
	@printf "\n### Compiling ASan+UBSan-instrumented Shared Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -shared -o $@ $^ $(ASAN_LDFLAGS) $(ASAN_LIBRARY_NAME_FLAG)

# Pattern rule for ASan test executables - uses same TEST_PAIRS
define asan-test-executable-rule
# Same reasoning as the release rule: a normal prerequisite, so a library
# change relinks the ASan binaries.
$(ASAN_APP_DIR)/$2$(EXE_EXTENSION): \
		$1 \
		$(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Compiling ASan+UBSan %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(ASAN_CXXFLAGS) $$(INCLUDE) -MMD -MP -MF $$(ASAN_APP_DIR)/$2.d -o $$@ $$< $$(ASAN_LDFLAGS) $$(TESTFLAGS) $$(ASAN_APP_DIR)/$$(ASAN_TARGET)
endef

# Generate ASAN build rules from TEST_PAIRS
$(foreach pair,$(TEST_PAIRS),$(eval $(call asan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

####################################################################
# ThreadSanitizer Build
####################################################################

TSAN_CFLAGS := $(CFLAGS) $(TSAN_FLAGS) -DGTEXT_BUILD -DGTEXT_TEST_BUILD
TSAN_CXXFLAGS := $(CXXFLAGS) $(TSAN_FLAGS)
TSAN_LDFLAGS := $(LDFLAGS) $(TSAN_FLAGS)

ifeq ($(UNAME_S), Linux)
	TSAN_CFLAGS += -fPIC
endif

$(TSAN_OBJ_DIR)/%.o: src/%.c $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling (TSan instrumented): $< ###\n"
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Same soname reasoning as the ASan library: it names this file, because this
# file is never installed and is only ever loaded out of the build tree.
$(TSAN_APP_DIR)/$(TSAN_TARGET): $(TSAN_LIBOBJECTS)
	@printf "\n### Compiling TSan-instrumented Shared Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) -shared -o $@ $^ $(TSAN_LDFLAGS) $(TSAN_LIBRARY_NAME_FLAG)

define tsan-test-executable-rule
$(TSAN_APP_DIR)/$2$(EXE_EXTENSION): \
		$1 \
		$(TSAN_APP_DIR)/$(TSAN_TARGET)
	@printf "\n### Compiling TSan %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(TSAN_CXXFLAGS) $$(INCLUDE) -MMD -MP -MF $$(TSAN_APP_DIR)/$2.d -o $$@ $$< $$(TSAN_LDFLAGS) $$(TESTFLAGS) $$(TSAN_APP_DIR)/$$(TSAN_TARGET)
endef

$(foreach pair,$(TEST_PAIRS),$(eval $(call tsan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

####################################################################
# Commands
####################################################################

# General commands
.PHONY: clean cloc docs docs-pdf examples help coverage conformance conformance-ini-desktop-entry conformance-ini-editorconfig conformance-ini-systemd conformance-ini-configparser conformance-ini-win32 conformance-roundtrip conformance-fastpath conformance-json conformance-json-to-toml conformance-csv conformance-json-schema conformance-json-schema-all conformance-jsonpath conformance-toml conformance-toml-next conformance-all fuzz fuzz-clean check-symbols check-allocators check-allocator-callees check-windows-cross check-headers check-idna-tables check-idna-oracle check-nfc-oracle check-ucd-pin check-metaschema check-oracle-env check-fuzz-harnesses check-nfc-oracle-strict check-toml-oracle check-toml-1-1-oracle check-ini-oracle check-ini-git-oracle check-ini-editorconfig-oracle check-ini-systemd-oracle check-ini-configparser-oracle check-ini-win32-oracle check-ini-win32-encoding-oracle check-ini-win32-authored-oracle oracle-images oracle-version oracle-clean
# Release build commands
.PHONY: all install test test-quiet test-valgrind test-valgrind-quiet test-watch uninstall watch
# Debug build commands
.PHONY: all-debug install-debug test-debug test-watch-debug uninstall-debug watch-debug
# Sanitizer commands
.PHONY: test-asan test-asan-quiet test-ubsan test-tsan test-tsan-quiet sanitizer-help


watch: ## Watch the file directory for changes and compile the target
	@while true; do \
		make --no-print-directory all; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

test-watch: ## Watch the file directory for changes and run the unit tests
	@while true; do \
		make --no-print-directory all; \
		make --no-print-directory test; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

examples: ## Build all JSON, CSV and YAML examples
examples: $(APP_DIR)/$(TARGET) $(EXAMPLES)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Examples built       ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@printf "JSON examples are available in: $(APP_DIR)/examples/json/\n"
	@printf "CSV examples are available in: $(APP_DIR)/examples/csv/\n"
	@printf "YAML examples are available in: $(APP_DIR)/examples/yaml/\n"
	@printf "\n"
	@printf "\033[0;33mTo run examples:\033[0m\n"
ifeq ($(OS_NAME), Linux)
	@printf "  Linux: Set LD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export LD_LIBRARY_PATH=\"$(APP_DIR):$$LD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/json/json_basic\n"
	@printf "    $(APP_DIR)/examples/csv/csv_basic\n"
else ifeq ($(OS_NAME), Mac)
	@printf "  macOS: Set DYLD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export DYLD_LIBRARY_PATH=\"$(APP_DIR):$$DYLD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/json/json_basic\n"
	@printf "    $(APP_DIR)/examples/csv/csv_basic\n"
else ifeq ($(OS_NAME), Windows)
	@printf "  Windows (MSYS2): The DLL must be in the same directory or in PATH.\n"
	@printf "  Option 1 - Run from the library directory:\n"
	@printf "    cd $(APP_DIR)\n"
	@printf "    ./examples/json/json_basic$(EXE_EXTENSION)\n"
	@printf "    ./examples/csv/csv_basic$(EXE_EXTENSION)\n"
	@printf "  Option 2 - Add library directory to PATH:\n"
	@printf "    export PATH=\"$(APP_DIR):$$PATH\"\n"
	@printf "    $(APP_DIR)/examples/json/json_basic$(EXE_EXTENSION)\n"
	@printf "    $(APP_DIR)/examples/csv/csv_basic$(EXE_EXTENSION)\n"
	@printf "  Option 3 - Copy DLL to example directories:\n"
	@printf "    cp $(APP_DIR)/$(TARGET) $(APP_DIR)/examples/json/\n"
	@printf "    cp $(APP_DIR)/$(TARGET) $(APP_DIR)/examples/csv/\n"
	@printf "    Then run: $(APP_DIR)/examples/json/json_basic$(EXE_EXTENSION)\n"
endif
	@printf "\n"


####################################################################
# Symbol namespace check
####################################################################

# Files whose allocations must all go through GTEXT_Allocator.  A file is
# added here when it has been converted; the check then keeps it converted.
# The list is the contract behind the "allocator" field in the parse options:
# without it, one raw malloc() added later would silently reintroduce the
# bypass the option exists to remove.
# src/allocator.c is deliberately absent: it is the default allocator, so the
# C library calls in it are the implementation rather than a bypass.
ALLOCATOR_CLEAN_SOURCES := \
	src/json/json_dom.c \
	src/json/json_writer.c \
	src/json/json_pointer.c \
	src/json/json_patch.c \
	src/json/json_schema.c \
	src/json/json_uri.c \
	src/json/json_stream.c \
	src/json/json_stream_buffer.c \
	src/json/json_utils.c \
	src/json/json_lexer.c \
	src/json/json_number.c \
	src/json/json_error.c \
	src/json/json_parser.c \
	src/csv/csv_pull_reader.c \
	src/csv/csv_sniff.c \
	src/csv/csv_error.c \
	src/csv/csv_writer.c \
	src/csv/csv_stream.c \
	src/csv/csv_stream_buffer.c \
	src/csv/csv_table.c \
	src/json/json_path.c \
	src/json/json_pull_reader.c \
	src/allocator.c \
	src/text_file_io.c \
	src/text_number.c \
	src/idna/nfc_utf8.c \
	src/yaml/json_to_yaml.c \
	src/yaml/reader.c \
	src/yaml/scanner.c \
	src/yaml/stream.c \
	src/yaml/utf8.c \
	src/yaml/yaml_arena.c \
	src/yaml/yaml_writer.c \
	src/yaml/yaml_context.c \
	src/yaml/yaml_dom.c \
	src/yaml/yaml_node_set.c \
	src/yaml/yaml_parser.c \
	src/yaml/yaml_pull_reader.c \
	src/yaml/yaml_resolve.c \
	src/yaml/yaml_to_json.c \
	src/toml/toml_dom.c \
	src/toml/toml_lexer.c \
	src/toml/toml_parser.c \
	src/toml/toml_file_io.c \
	src/toml/toml_json.c \
	src/toml/toml_writer.c \
	src/ini/ini_core.c \
	src/ini/ini_dom.c \
	src/ini/ini_encoding.c \
	src/ini/ini_parser.c \
	src/ini/ini_value.c \
	src/ini/ini_writer.c \
	src/ini/ini_file_io.c

check-allocator-callees: ## Fail if a converted file calls into an unconverted one that allocates
	@python3 tools/check-allocator-callees.py

# Deliberately outside TEST_GATES, like every gate here that needs something
# this machine may not have. It wants podman and a mingw image, and a gate that
# cannot run on a fresh clone has no business failing `make test`.
#
# It is still a gate and not a report: the library had never been compiled off
# Linux before it existed - five CI jobs, all ubuntu-latest - and the four
# preprocessor conditionals it reaches decide what every exported symbol is
# declared as. A `#if` arm nobody compiles is not small, it is unparsed.
check-windows-cross: ## Compile every source for Windows with mingw-w64
	@tools/cross-windows.sh; \
	status=$$?; \
	if [ $$status -eq 77 ]; then \
		printf "check-windows-cross: skipped (no host mingw-w64, and no container engine or image)\n"; \
		exit 0; \
	fi; \
	exit $$status

check-allocators: ## Fail if a converted file allocates without the allocator
	@raw=$$(grep -nE '(^|[^_[:alnum:]])(malloc|calloc|realloc|free|strdup|strndup)[[:space:]]*\(' \
		$(ALLOCATOR_CLEAN_SOURCES) /dev/null \
		| grep -v 'gtext_allocator_' \
		| grep -v 'allocator-exempt' \
		| grep -vE ':[0-9]+:[[:space:]]*(\*|//|/\*)' || true); \
	if [ -n "$$raw" ]; then \
		printf "\033[0;31m\n### Raw allocation in a file that must use GTEXT_Allocator ###\033[0m\n" >&2; \
		printf "%s\n" "$$raw" >&2; \
		printf "\nGTEXT_JSON_Parse_Options::allocator promises that a caller-supplied\n" >&2; \
		printf "allocator sees every allocation the parse makes. A direct malloc() or\n" >&2; \
		printf "free() here breaks that promise silently - the caller cannot detect it,\n" >&2; \
		printf "and a free() through the wrong allocator corrupts the heap.\n" >&2; \
		printf "Use gtext_allocator_malloc()/_calloc()/_realloc()/_free().\n" >&2; \
		printf "strdup() counts: it allocates from the C library, and the free\n" >&2; \
		printf "beside it will not. Fifteen of them hid here once.\n" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mEvery allocation in the converted files goes through GTEXT_Allocator.\033[0m\n"

check-headers: ## Fail if any installed header is not self-contained
# `make install` copies include/ghoti.io wholesale, so every header under
# include/ ships whether or not anything includes it.  Globbing that directory
# is therefore the authoritative list, and the reason this is a gate rather
# than a test file: tests/test-headers.c was a hand-maintained list of fifteen
# includes, which covered no CSV header at all and did not notice that
# json/json.h had been dead since the great rename.  A list someone has to
# remember to extend is a list that silently stops matching the tree.
#
# Each header is compiled alone, so it must include what it uses; twice, so a
# broken or duplicated include guard shows up as a redefinition; and once as
# C++, because every test in this project is C++ and a header that forgets
# `extern "C"` links against nothing.
#
# The list is every header `install` copies, which is not the same as every
# header under include/.  It globbed include/ alone and so checked 25 of the
# 26 headers that ship: libver_gen.h is generated into $(GEN_DIR) and installed
# from there, so the one header whose content is produced by the build rather
# than written by hand was the one nothing verified.  Same shape as the gate
# itself is for - a hand-kept list that silently stops matching - one level up,
# in a list kept by a glob rather than by a person.
	@mkdir -p $(BUILD_DIR)
	@fail=0; checked=0; \
	for h in $$( { find include -name '*.h' | sed 's|^include/||'; \
			find $(GEN_DIR) -name '*.h' 2>/dev/null \
				| sed 's|^$(GEN_DIR)/||'; } | sort -u); do \
		checked=$$((checked + 1)); \
		printf '#include <%s>\n#include <%s>\nint main(void) { return 0; }\n' "$$h" "$$h" \
			> $(BUILD_DIR)/hdrcheck.c; \
		if ! $(CC) $(CFLAGS) -I include -I $(GEN_DIR) $(CUTIL_CFLAGS) $(CHRON_CFLAGS) \
				-c -o /dev/null $(BUILD_DIR)/hdrcheck.c 2> $(BUILD_DIR)/hdrcheck.log; then \
			printf "\033[0;31m\n### %s is not self-contained (C) ###\033[0m\n" "$$h" >&2; \
			sed 's/^/    /' $(BUILD_DIR)/hdrcheck.log >&2; \
			fail=1; \
		fi; \
		cp $(BUILD_DIR)/hdrcheck.c $(BUILD_DIR)/hdrcheck.cpp; \
		if ! $(CXX) $(CXXFLAGS) -I include -I $(GEN_DIR) $(CUTIL_CFLAGS) $(CHRON_CFLAGS) \
				-c -o /dev/null $(BUILD_DIR)/hdrcheck.cpp 2> $(BUILD_DIR)/hdrcheck.log; then \
			printf "\033[0;31m\n### %s is not self-contained (C++) ###\033[0m\n" "$$h" >&2; \
			sed 's/^/    /' $(BUILD_DIR)/hdrcheck.log >&2; \
			fail=1; \
		fi; \
	done; \
	rm -f $(BUILD_DIR)/hdrcheck.c $(BUILD_DIR)/hdrcheck.cpp $(BUILD_DIR)/hdrcheck.log; \
	if [ "$$fail" -ne 0 ]; then \
		printf "\nA header that does not compile alone works only for callers who\n" >&2; \
		printf "happen to have included its dependencies first.\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mAll %s installed headers compile standalone, twice, as C and C++.\033[0m\n" "$$checked"

check-symbols: ## Fail if any exported symbol lacks the version namespace
check-symbols: $(APP_DIR)/$(TARGET)
ifeq ($(OS_NAME), Linux)
	@leaked=$$(nm -D --defined-only $(APP_DIR)/$(TARGET) \
		| awk '$$2 ~ /^[TDBR]$$/ {print $$3}' \
		| grep -v '^$(LIBVER_SYMBOL)_' | grep -v '^_' || true); \
	if [ -n "$$leaked" ]; then \
		printf "\033[0;31m\n### Exported symbols missing the $(LIBVER_SYMBOL)_ namespace ###\033[0m\n" >&2; \
		printf "%s\n" "$$leaked" >&2; \
		printf "\nEach needs a '#define <name> GHOTIIO_TEXT(<name>)' line in the header\n" >&2; \
		printf "that declares it. See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@unexported=$$(find include -name '*.h' -exec awk '/^#if DOXYGEN/{d=1} d==0 && /^[a-z_][A-Za-z0-9_ ]*\**[[:space:]]*gtext_[a-z0-9_]+[[:space:]]*\(/{print FILENAME": "$$0} /^#endif/{d=0}' {} + \
		| grep -vE 'typedef|static inline' || true); \
	if [ -n "$$unexported" ]; then \
		printf "\033[0;31m\n### Public declarations without GTEXT_API ###\033[0m\n" >&2; \
		printf "%s\n" "$$unexported" >&2; \
		printf "\nThese are hidden in the shared library. The tests link the archive and\n" >&2; \
		printf "would not notice; a consumer gets an undefined reference.\n" >&2; \
		exit 1; \
	fi
	@decl=$$(mktemp); exp=$$(mktemp); \
	grep -rhoE 'GTEXT_API[[:space:]]+[A-Za-z_][A-Za-z0-9_ ]*\**[[:space:]]*\**gtext_[a-z0-9_]+[[:space:]]*\(' include/ \
		| grep -oE 'gtext_[a-z0-9_]+' | sort -u > $$decl; \
	nm -D --defined-only $(APP_DIR)/$(TARGET) \
		| awk '$$2 ~ /^[TDBR]$$/ {print $$3}' \
		| sed 's/^$(LIBVER_SYMBOL)_//' | sort -u > $$exp; \
	missing=$$(comm -23 $$decl $$exp); \
	rm -f $$decl $$exp; \
	if [ -n "$$missing" ]; then \
		printf "\033[0;31m\n### Declared GTEXT_API but not in the shared library ###\033[0m\n" >&2; \
		printf "%s\n" "$$missing" >&2; \
		printf "\nThe header promises these and the shared library does not have them,\n" >&2; \
		printf "so a consumer linking it gets an undefined reference for an API we\n" >&2; \
		printf "document. Usually the definition in src/ is missing the GTEXT_API its\n" >&2; \
		printf "declaration has. The check above reads the header, which is one half;\n" >&2; \
		printf "this one reads the library, which is what a consumer actually links.\n" >&2; \
		exit 1; \
	fi
	@split=$$(nm -D --undefined-only $(APP_DIR)/$(TARGET) \
		| awk '{print $$2}' | grep '^$(LIBVER_SYMBOL)_' || true); \
	if [ -n "$$split" ]; then \
		printf "\033[0;31m\n### Renamed but undefined - a split symbol ###\033[0m\n" >&2; \
		printf "%s\n" "$$split" >&2; \
		printf "\nA translation unit referenced the namespaced name while the one that\n" >&2; \
		printf "defines it did not see the rename - usually an internal header that\n" >&2; \
		printf "declares or defines something without including macros.h first.\n" >&2; \
		exit 1; \
	fi
	@nomacros=$$(find include src -name '*.h' \
		! -name 'libver.h' ! -name 'libver_gen.h' ! -name 'namespace.h' ! -name 'macros.h' \
		-exec grep -L '#include <ghoti.io/text/macros.h>' {} + || true); \
	if [ -n "$$nomacros" ]; then \
		printf "\033[0;31m\n### Headers that do not include macros.h ###\033[0m\n" >&2; \
		printf "%s\n" "$$nomacros" >&2; \
		printf "\nEvery header must include <ghoti.io/text/macros.h> before it declares\n" >&2; \
		printf "anything, so that the renames in namespace.h are already in effect. A\n" >&2; \
		printf "header that skips it can name a type before that type has been renamed,\n" >&2; \
		printf "producing two different types under one spelling.\n" >&2; \
		printf "See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@badguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| awk '$$1 !~ /^GHOTI_IO_GTEXT_/ {print $$1}' || true); \
	if [ -n "$$badguards" ]; then \
		printf "\033[0;31m\n### Include guards with the wrong prefix ###\033[0m\n" >&2; \
		printf "%s\n" "$$badguards" >&2; \
		printf "\nGuards mirror the path: GHOTI_IO_GTEXT_<PATH>_H. A guard without the\n" >&2; \
		printf "library token is one rename away from colliding with another library's.\n" >&2; \
		exit 1; \
	fi
	@dupguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| sort | uniq -d || true); \
	if [ -n "$$dupguards" ]; then \
		printf "\033[0;31m\n### Headers sharing an include guard ###\033[0m\n" >&2; \
		printf "%s\n" "$$dupguards" >&2; \
		printf "\nTwo headers with one guard means whichever is included second is\n" >&2; \
		printf "silently empty. Guards mirror the path: GHOTI_IO_GTEXT_<PATH>_H.\n" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mEvery exported symbol carries the $(LIBVER_SYMBOL)_ namespace.\033[0m\n"
	@printf "\033[0;32mEvery public declaration carries GTEXT_API.\033[0m\n"
	@printf "\033[0;32mEvery GTEXT_API declaration is in the shared library.\033[0m\n"
	@printf "\033[0;32mEvery header includes macros.h.\033[0m\n"
	@printf "\033[0;32mEvery include guard is unique and correctly prefixed.\033[0m\n"
else
	@printf "check-symbols: skipped (Linux only)\n"
endif

check-fuzz-harnesses: ## Fail if a fuzz harness no longer compiles
# **A harness that does not compile is a gate that cannot run, and it reads exactly
# like a gate that ran and found nothing.** Nothing else here builds tests/fuzz/:
# `make test` does not, `conformance-all` does not, and neither does any check-*
# target - so a change to a harness went through 3,061 tests, ASan, seven oracles and
# five gates and was committed with three uses of an undeclared identifier in it. It
# was caught by shipping the tree to a machine whose first step is building the
# harnesses, which is the third time a fresh machine has been this suite's gate.
#
# **-fsyntax-only, not a link.** The defect this exists for is a compile error, and
# building all seven with coverage, ASan and UBSan takes about a minute where parsing
# them takes about a second. A gate cheap enough to be in `make test` catches this on
# the commit that causes it; one that costs a minute gets moved out of the way.
#
# Skips when clang is absent, and that is the right way round by this file's own rule:
# the property is "does clang accept this file", which cannot exist without clang. A
# harness that fails to compile is still wrong on a machine with no clang, but nothing
# there can see it - unlike the four gates above, whose subject is a table that is
# wrong whether or not python3 is installed.
# The clang check is made **in the recipe and not with ifeq**, because FUZZ_CC_OK is
# defined some four hundred lines below this rule: `ifeq ($(FUZZ_CC_OK),)` is evaluated
# while make reads the file, so it saw an empty value and this gate skipped on a machine
# that has clang and had just built all seven harnesses with it. A gate that skips when
# it could run is the failure this file has most of its comments about, and it reached
# that state through variable ordering rather than through logic.
	@if ! command -v $(FUZZ_CXX) > /dev/null 2>&1; then \
		printf "check-fuzz-harnesses: skipped (no $(FUZZ_CXX); the harnesses are not parsed)\n"; \
		exit 0; \
	fi; \
	bad=""; \
	for h in tests/fuzz/*.cpp; do \
		$(FUZZ_CXX) -fsyntax-only -std=c++20 -w $(INCLUDE) "$$h" 2> $(BUILD_DIR)/.fuzz-syntax.log \
			|| { bad="$$bad $$h"; cat $(BUILD_DIR)/.fuzz-syntax.log >&2; }; \
	done; \
	rm -f $(BUILD_DIR)/.fuzz-syntax.log; \
	if [ -n "$$bad" ]; then \
		printf "\033[0;31m\n### Fuzz harnesses that no longer compile ###\033[0m\n" >&2; \
		printf "%s\n" "$$bad" >&2; \
		printf "\nNothing else in this Makefile builds tests/fuzz/, so this is the only\n" >&2; \
		printf "gate that would notice. Build one for real with: make fuzz-<target>\n" >&2; \
		exit 1; \
	fi; \
	printf "check-fuzz-harnesses: %s harnesses parse\n" "$$(ls tests/fuzz/*.cpp | wc -l)"

test: ## Make and run the Unit tests
test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES)
	@printf "\033[0;30;43m\n"
	@printf "############################\n"
	@printf "### Running Text tests   ###\n"
	@printf "############################\n"
	@printf "\033[0m\n\n"
	LD_LIBRARY_PATH="$(APP_DIR)" $(APP_DIR)/testText$(EXE_EXTENSION) --gtest_brief=1

	@printf "\033[0;30;43m\n"
	@printf "############################\n"
	@printf "### Running JSON tests   ###\n"
	@printf "############################\n"
	@printf "\033[0m\n\n"
	LD_LIBRARY_PATH="$(APP_DIR)" $(APP_DIR)/testJson$(EXE_EXTENSION) --gtest_brief=1

	@printf "\033[0;30;43m\n"
	@printf "############################\n"
	@printf "### Running CSV tests    ###\n"
	@printf "############################\n"
	@printf "\033[0m\n\n"
	LD_LIBRARY_PATH="$(APP_DIR)" $(APP_DIR)/testCsv$(EXE_EXTENSION) --gtest_brief=1

	@printf "\n############################\n"
	@printf "### Running All Tests    ###\n"
	@printf "############################\n\n"
# The `|| true` that used to end this line meant `make test` exited 0 with a
# failing suite.  Only the three named runs above could fail the build; the
# other sixty-four were decorative - their failures printed and were discarded.
# Every claim resting on "the suite is green" rested on this loop.
	@failed=""; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\n### Running $$test_name ###\n"; \
		if ! LD_LIBRARY_PATH="$(APP_DIR)" $$test_exe --gtest_brief=1; then \
			failed="$$failed $$test_name"; \
		fi; \
	done; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31m\n### Failing suites:%s ###\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi

# A suite that never started reports no tests at all - a missing shared
# library exits 127 before gtest prints a line. The row said FAIL, but the
# failure count came from the absent "[ FAILED ] n" line and so was zero, and
# a TOTAL of zero failures prints PASS. The run was green with a whole suite
# unexecuted. Anything that exited non-zero counts as at least one failure,
# and the passed tally never goes below zero when it does.
test-quiet: ## Run tests with minimal output (one line per test suite)
test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;36m%-30s %8s %10s %s\033[0m\n" "Test Suite" "Tests" "Time" "Status"; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(APP_DIR)" $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			if [ $$failures -eq 0 ]; then failures=1; fi; \
			if [ $$failures -gt $$num_tests ]; then passed_here=0; \
			else passed_here=$$((num_tests - failures)); fi; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + passed_here)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi

test-valgrind: ## Run all tests under valgrind (Linux only)
test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
# VALGRIND_FLAGS carries --error-exitcode=1, so valgrind already reports a
# memory error as a non-zero status.  This loop simply did not look at it.
	@failed=""; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests under Valgrind ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		if ! LD_LIBRARY_PATH="$(APP_DIR)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1; then \
			failed="$$failed $$test_name"; \
		fi; \
	done; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31m\n### Suites failing under valgrind:%s ###\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-valgrind-quiet: ## Run tests under valgrind with minimal output (Linux only)
test-valgrind-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;35m%-30s %8s %10s %s\033[0m\n" "Test Suite (Valgrind)" "Tests" "Time" "Status"; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(APP_DIR)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		has_leak=$$(echo "$$output" | grep -c "are definitely lost\|are indirectly lost\|are possibly lost" || true); \
		if [ $$exit_code -eq 0 ] && [ $$has_leak -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=0; \
			if [ $$exit_code -ne 0 ] && [ $$failures -eq 0 ]; then failures=1; fi; \
			if [ $$failures -gt $$num_tests ]; then passed_here=0; \
			else passed_here=$$((num_tests - failures)); fi; \
			if [ $$has_leak -gt 0 ]; then \
				total_failed=$$((total_failed + num_tests)); \
				printf "%-30s %8d %8dms \033[0;31mLEAK\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			else \
				total_failed=$$((total_failed + failures)); \
				total_passed=$$((total_passed + passed_here)); \
				printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			fi; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES/LEAKS ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-asan: ## Run all tests with AddressSanitizer + UndefinedBehaviorSanitizer (Linux only)
# LD_PRELOAD is cleared for each run: the ASan runtime has to be first in the
# initial library list, and a desktop-wide preload (Debian sets
# libgtk3-nocsd.so.0 in many sessions) gets ahead of it, at which point ASan
# aborts before the test starts.
test-asan: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m\n"
	@printf "###########################################\n"
	@printf "### Running tests with ASan + UBSan    ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
# allocator_may_return_null=1 makes an oversized request return NULL instead of
# aborting the process.  Without it ASan treats "too big to allocate" as a fatal
# error, which makes every out-of-memory path in this library untestable under
# the sanitizer - the buffer growth tests ask for SIZE_MAX-sized capacities on
# purpose, to reach the overflow branches, and expect the documented
# GTEXT_*_E_OOM back.  Leak detection and every other check are unaffected.
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests (ASan+UBSan) ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		LD_PRELOAD= LD_LIBRARY_PATH="$(ASAN_APP_DIR)" ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\n"
	@printf "###########################################\n"
	@printf "### All tests passed with ASan + UBSan ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-ubsan: ## Alias for test-asan (ASan+UBSan are run together)
test-ubsan: test-asan

test-tsan: ## Run all tests with ThreadSanitizer (Linux only)
# See the ThreadSanitizer block near ASAN_UBSAN_FLAGS for what this gate is
# for, why it builds every test rather than the three that run threads, and
# the two things not to copy here from the ASan target.
test-tsan: $(TSAN_APP_DIR)/$(TSAN_TARGET) $(TSAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m\n"
	@printf "###########################################\n"
	@printf "### Running tests with ThreadSanitizer  ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
# halt_on_error=1 so that the first race fails the gate rather than being
# counted and passed over: TSan's default is to report and carry on, and a
# report with exit status 0 is a gate that cannot fail.
#
# second_deadlock_stack=1 costs some memory and makes a lock-order report name
# both sites, which is the difference between a report that can be acted on and
# one that says only that something is wrong.
#
# allocator_may_return_null=1 for the same reason the ASan target sets it, and
# found the same way: without it testJson dies in
# JsonBufferGrow.HybridSmallIncrementOverflowFallsBackToNeeded, which asks for
# a SIZE_MAX-sized capacity on purpose to reach the overflow branch and expects
# GTEXT_JSON_E_OOM back. TSan treats "too big to allocate" as fatal, so every
# out-of-memory path in this library is untestable under it otherwise. Race
# detection is unaffected; this option governs allocation-size errors only.
	@failed=""; \
	for test_exe in $(TSAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests (TSan) ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		LD_PRELOAD= LD_LIBRARY_PATH="$(TSAN_APP_DIR)" \
			TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:allocator_may_return_null=1 \
			$$test_exe --gtest_brief=1 || failed="$$failed $$test_name"; \
	done; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31m\nThreadSanitizer findings in:%s\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32m\n"
	@printf "###########################################\n"
	@printf "### All tests passed with TSan          ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-tsan-quiet: ## Run TSan tests with minimal output (Linux only)
# **A test can fail under this gate without a race, and the first version of
# this recipe printed nothing when that happened.** It showed only the lines
# around `WARNING: ThreadSanitizer`, so a failure with no report - which is
# what a timing assertion or an exhausted resource looks like - left the
# operator with a test name, an exit status of 1, and no information at all.
# Seen once: testYamlParseScaling, whose DepthCostsLinearTime asserts a time
# ratio, failed in a full run with 0 reports and has not reproduced in 25
# subsequent runs idle, under eight spinning cores, and alongside a -j8
# rebuild. It peaks at 285 MB under TSan against 83 MB plain, which is the
# kind of thing that matters on a loaded box.
#
# So the no-report branch prints the test's own output instead. The cause of
# that one failure is still unknown, and the point of this note is that the
# next occurrence will say which half it was.
test-tsan-quiet: $(TSAN_APP_DIR)/$(TSAN_TARGET) $(TSAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@races=0; failed=""; \
	printf "\n\033[1;33m%-34s %8s %s\033[0m\n" "Test Suite (TSan)" "Tests" "Status"; \
	for test_exe in $(TSAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_PRELOAD= LD_LIBRARY_PATH="$(TSAN_APP_DIR)" \
			TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:allocator_may_return_null=1 \
			$$test_exe --gtest_brief=1 2>&1); \
		code=$$?; \
		num=$$(printf '%s' "$$output" | grep -oE '\[=+\] [0-9]+ tests? from' | grep -oE '[0-9]+' | head -1); \
		this=$$(printf '%s' "$$output" | grep -c 'WARNING: ThreadSanitizer'); \
		races=$$((races + this)); \
		if [ $$code -eq 0 ]; then \
			printf "%-34s %8s \033[0;32mok\033[0m\n" "$$test_name" "$${num:-?}"; \
		else \
			printf "%-34s %8s \033[0;31mFAILED rc=%s (%s race report(s))\033[0m\n" "$$test_name" "$${num:-?}" "$$code" "$$this"; \
			if [ "$$this" -gt 0 ]; then \
				printf '%s\n' "$$output" | grep -A 12 'WARNING: ThreadSanitizer' | head -40; \
			else \
				printf '%s\n' "$$output" | tail -40; \
			fi; \
			failed="$$failed $$test_name"; \
		fi; \
	done; \
	printf "\ntsan: %d ThreadSanitizer report(s)\n" "$$races"; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31mfailing:%s\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mNo races reported.\033[0m\n"
else
	@printf "\033[0;31m\nSanitizer builds are currently only supported on Linux\n\033[0m\n"
	@exit 1
endif

test-asan-quiet: ## Run ASan+UBSan tests with minimal output (Linux only)
test-asan-quiet: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;33m%-30s %8s %10s %s\033[0m\n" "Test Suite (ASan+UBSan)" "Tests" "Time" "Status"; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_PRELOAD= LD_LIBRARY_PATH="$(ASAN_APP_DIR)" ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			if [ $$failures -eq 0 ]; then failures=1; fi; \
			if [ $$failures -gt $$num_tests ]; then passed_here=0; \
			else passed_here=$$((num_tests - failures)); fi; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + passed_here)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

sanitizer-help: ## Show help for sanitizer usage
	@printf "\033[1;36m\n"
	@printf "##############################################\n"
	@printf "### Sanitizer Testing (ASan + UBSan)      ###\n"
	@printf "##############################################\n"
	@printf "\033[0m\n"
	@printf "AddressSanitizer (ASan) detects:\n"
	@printf "  - Memory leaks\n"
	@printf "  - Use-after-free\n"
	@printf "  - Buffer overflows\n"
	@printf "  - Stack/heap corruption\n"
	@printf "\n"
	@printf "UndefinedBehaviorSanitizer (UBSan) detects:\n"
	@printf "  - Integer overflow/underflow\n"
	@printf "  - Null pointer dereference\n"
	@printf "  - Unaligned memory access\n"
	@printf "  - Division by zero\n"
	@printf "\n"
	@printf "ThreadSanitizer (TSan) detects, and the two above do not:\n"
	@printf "  - Data races\n"
	@printf "  - Lock-order inversions\n"
	@printf "\n"
	@printf "  It cannot be linked with ASan, so it has its own tree and target.\n"
	@printf "  This library keeps no mutable global state, so it is a standing\n"
	@printf "  guard on that rather than a hunt: see tests/test-concurrency.cpp\n"
	@printf "  and documentation/modules/Core.md section 7.\n"
	@printf "\n"
	@printf "Usage:\n"
	@printf "  make test-asan         - Run all tests with sanitizers (verbose)\n"
	@printf "  make test-asan-quiet   - Run all tests with sanitizers (summary)\n"
	@printf "  make test-ubsan        - Alias for test-asan\n"
	@printf "  make test-tsan         - Run all tests with ThreadSanitizer\n"
	@printf "  make test-tsan-quiet   - Run all tests with TSan (summary)\n"
	@printf "\n"
	@printf "Sanitizers build separate instrumented libraries in:\n"
	@printf "  $(ASAN_APP_DIR)/\n"
	@printf "  $(TSAN_APP_DIR)/\n"
	@printf "\n"
	@printf "Note: Sanitizer builds are slower but catch more bugs.\n"
	@printf "      Use them before commits or releases.\n"
	@printf "\n"

# Removed duplicated run block and empty target stubs that previously caused
# "overriding recipe" warnings. The YAML test targets are defined above.

clean: ## Remove all contents of the build directories.
# Every tree, not just $(BUILD_DIR).  The sanitizer build is
# $(BUILD_DIR)-asan - a *sibling* of the ordinary one, not a child - so
# `rm -rf $(BUILD_DIR)` left it whole while this target's own help text
# says "directories", plural.  A workspace-wide clean left 55 object files
# here, and that is the stale-object trap with a clean's authority behind
# it: somebody who cleans to rule out a stale object has not ruled out the
# sanitizer one, and `make test-asan` relinks against whatever survived.
#
# The glob rather than a list, because the list is the thing that goes
# stale.  A library grows a tree - -asan here, -tsan and -fuzz elsewhere in
# the suite - and whoever adds it has no reason to think about a target
# five hundred lines away.  $(BUILD_DIR)-* covers every suffixed sibling
# there will ever be, and cannot match the cloned corpora, which are
# siblings of build/linux rather than of build/linux/release.
	-@rm -rvf $(BUILD_DIR) $(BUILD_DIR)-*

# Files will be as follows:
# /usr/local/lib/(SUITE)/
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR).(MINOR)
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR) link to previous
#   lib(SUITE)-(PROJECT)(BRANCH).so link to previous
# Where the dynamic loader configuration fragment goes. Overridable so a
# staged or user-prefix install has somewhere to write it; the default is the
# system location, which is what an ordinary `sudo make install` uses.
LDCONF_INSTALL_PATH ?= /etc/ld.so.conf.d

# Dependencies a consumer of this library needs on its own include path.
PC_REQUIRES := $(DEP_PCS)

# Where this project's own .pc file is installed. Defaults to the directory
# pkg-config is already being told to search, but separate from it so a
# staged install can write somewhere else without also redirecting lookups.
PKGCONFIG_INSTALL_PATH ?= $(PC_INSTALL_PATH)

# $(LDCONF_INSTALL_PATH)/(SUITE)-(PROJECT)(BRANCH).conf will point to $(LIB_INSTALL_PATH)/(SUITE)
# /usr/local/include/(SUITE)/(PROJECT)(BRANCH)
#   *.h copied from ./include/(PROJECT)
# /usr/local/share/pkgconfig
#   (SUITE)-(PROJECT)(BRANCH).pc created

install: ## Install the library globally, requires sudo
# Depends on all: install used to copy whatever happened to be in the build
# directory, so it could install a stale artifact or fail outright on a clean
# tree.
install: all
	# Installing the shared library.
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
# Install the .so file
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	# Installing the ld configuration file.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then mkdir -p $(LDCONF_INSTALL_PATH); fi
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then echo "$(LIB_INSTALL_PATH)/$(SUITE)" > $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
# The .dll goes in bin/, where the loader finds it once that directory is on
# PATH - Windows has no rpath. The import library goes where the .pc's -L
# points, lib/$(SUITE)/, as the .so does on Linux; in lib/ no -L named it.
	@mkdir -p $(BIN_INSTALL_PATH) $(LIB_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)/$(SUITE)/
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)/
endif
	# Installing the headers.
	# Removed first: this directory is owned entirely by this project and
	# branch, and copying over the top of it would leave headers behind that
	# have since been renamed or deleted.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -n "$$(find $(GEN_DIR) -maxdepth 1 -name '*.h' 2>/dev/null)" ]; then \
		mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT); \
		cp $(GEN_DIR)/*.h $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT)/; \
	fi
	@if [ -d $(GEN_DIR)/ghoti.io ]; then \
		cp -r $(GEN_DIR)/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	# Installing the pkg-config files.
	@mkdir -p $(PKGCONFIG_INSTALL_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(PC_LIB_DIR)|$(PC_LIB_DIR)|g; s|(PC_INCLUDE_DIR)|$(PC_INCLUDE_DIR)|g; s|(REQUIRES)|$(PC_REQUIRES)|g' > $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the globally-installed files.  Requires sudo.
	# Deleting the shared library.
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	# Deleting the ld configuration file.
	@rm -f $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	# Deleting the headers.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	# Deleting the pkg-config files.
	@rm -f $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	# Cleaning up (potentially) no longer needed directories.
	@rmdir --ignore-fail-on-non-empty $(INCLUDE_INSTALL_PATH)/$(SUITE)
	@rmdir --ignore-fail-on-non-empty $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build the project in DEBUG mode
	make all BUILD=debug

install-debug: ## Install the DEBUG library globally, requires sudo
	make install BUILD=debug

uninstall-debug: ## Delete the DEBUG globally-installed files.  Requires sudo.
	make uninstall BUILD=debug

test-debug: ## Make and run the Unit tests in DEBUG mode
	make test BUILD=debug

watch-debug: ## Watch the file directory for changes and compile the target in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch the file directory for changes and run the unit tests in DEBUG mode
	make test-watch BUILD=debug

docs: ## Generate the documentation in the ./docs subdirectory
	doxygen

docs-pdf: docs ## Generate the documentation as a pdf, at ./docs/(SUITE)-(PROJECT)(BRANCH).pdf
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include tests Makefile

####################################################################
# Fuzzing (libFuzzer)
####################################################################
# The library sources are recompiled with clang's coverage instrumentation
# and linked into the harness, rather than the harness linking the ordinary
# shared library. That matters: libFuzzer steers its mutations by the
# coverage it observes, and against an uninstrumented library it sees only
# the harness file and degrades into random input generation.
#
# AddressSanitizer and UndefinedBehaviorSanitizer are on, since a parser
# reading one byte past a buffer is exactly the bug being looked for and it
# will not usually crash on its own.
FUZZ_CC ?= clang
FUZZ_CXX ?= clang++
FUZZ_CC_OK := $(shell which $(FUZZ_CC) 2>/dev/null)
# -fno-sanitize-recover=undefined for the same reason as ASAN_UBSAN_FLAGS: a
# fuzzer steers by crashes, and a UBSan finding that only prints is an input
# libFuzzer will never save as an artifact.
FUZZ_SAN := -fsanitize=address,$(UBSAN_CHECKS) \
            -fno-sanitize-recover=$(UBSAN_CHECKS) -fno-omit-frame-pointer -g -O1
FUZZ_LIB_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer-no-link
FUZZ_BIN_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer
# The fuzzers link cutil like everything else, so they need the same rpath the
# test binaries get from LDFLAGS; FUZZ_BIN_FLAGS does not include LDFLAGS.
ifdef PREFIX
FUZZ_RPATH := -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
endif
FUZZ_DIR := $(BUILD_DIR)/fuzz
FUZZ_OBJ_DIR := $(FUZZ_DIR)/objects
FUZZ_FLAGS_STAMP := $(FUZZ_OBJ_DIR)/.flags
FUZZ_APP_DIR := $(FUZZ_DIR)/apps
FUZZ_OBJECTS := $(patsubst src/%.c,$(FUZZ_OBJ_DIR)/%.o,$(SOURCES))
# Included here rather than added to DEPFILES, which is simply-expanded and
# defined long before FUZZ_OBJECTS exists.
-include $(FUZZ_OBJECTS:.o=.d)
FUZZ_CORPUS := tests/fuzz/corpus
# Long enough to be worth running, short enough for a coffee. Override for a
# real campaign: make fuzz FUZZ_TIME=3600
FUZZ_TIME ?= 60

# -MMD -MP -MF for the same reason as the release and ASan rules: without it a
# header change rebuilds nothing here, and the stale objects disagree with the
# fresh ones about struct layout.  That shows up as a fuzzer "finding" -
# a _Bool loaded as 255, a SEGV in free() - in code that is correct.
$(FUZZ_OBJ_DIR)/%.o: src/%.c $(FUZZ_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	@$(FUZZ_CC) $(FUZZ_LIB_FLAGS) -std=c17 -w $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# $1 = harness basename (fuzz_json), $2 = target suffix (json)
define fuzz-rule
fuzz-$2: ## Build the $2 fuzz harness (requires clang)
fuzz-$2: $$(FUZZ_APP_DIR)/$1

$$(FUZZ_APP_DIR)/$1: tests/fuzz/$1.cpp $$(FUZZ_OBJECTS)
	@if [ -z "$$(FUZZ_CC_OK)" ]; then \
		echo "fuzzing requires $$(FUZZ_CXX); install clang or set FUZZ_CC/FUZZ_CXX"; \
		exit 1; \
	fi
	@mkdir -p $$(@D) $$(FUZZ_CORPUS)
	@printf "\n### Building $1 ###\n"
	$$(FUZZ_CXX) $$(FUZZ_BIN_FLAGS) -std=c++20 -w $$(INCLUDE) \
		-o $$@ $$< $$(FUZZ_OBJECTS) $$(DEP_LIBS) $$(FUZZ_RPATH)

fuzz-run-$2: ## Run the $2 fuzzer for $$(FUZZ_TIME) seconds
fuzz-run-$2: $$(FUZZ_APP_DIR)/$1
	@mkdir -p $$(FUZZ_CORPUS)/$2
	@printf "\n### Fuzzing $2 for $$(FUZZ_TIME)s ###\n"
	@$$(FUZZ_APP_DIR)/$1 $$(FUZZ_CORPUS)/$2 \
		-max_total_time=$$(FUZZ_TIME) -print_final_stats=1
endef

$(eval $(call fuzz-rule,fuzz_json,json))
$(eval $(call fuzz-rule,fuzz_yaml,yaml))
$(eval $(call fuzz-rule,fuzz_yaml_writer,yaml-writer))
$(eval $(call fuzz-rule,fuzz_csv,csv))
$(eval $(call fuzz-rule,fuzz_toml,toml))
$(eval $(call fuzz-rule,fuzz_toml_writer,toml-writer))
$(eval $(call fuzz-rule,fuzz_ini,ini))
$(eval $(call fuzz-rule,fuzz_json_writer,json-writer))
$(eval $(call fuzz-rule,fuzz_csv_writer,csv-writer))
$(eval $(call fuzz-rule,fuzz_ini_writer,ini-writer))
$(eval $(call fuzz-rule,fuzz_json_records,json-records))

# Every format with a writer now has a harness for it. YAML and TOML had one
# and JSON, CSV and INI did not, which is the asymmetry that let an object of
# two or more members come out of gtext_json_writer_* as {"a":1,"b":,2} - not
# JSON, from calls that each returned OK - and let the CSV streaming writer
# ignore two options its table writer honours. A corpus of documents reaches a
# parse; it cannot reach a sequence of writer calls.
fuzz: ## Build and run every fuzzer for $(FUZZ_TIME) seconds each
fuzz: fuzz-run-json fuzz-run-yaml fuzz-run-yaml-writer fuzz-run-csv
fuzz: fuzz-run-toml fuzz-run-toml-writer fuzz-run-ini
fuzz: fuzz-run-json-writer fuzz-run-csv-writer fuzz-run-ini-writer
fuzz: fuzz-run-json-records

fuzz-clean: ## Remove the fuzz build (keeps the corpus)
fuzz-clean:
	-@rm -rf $(FUZZ_DIR)

# Every conformance target links the library the build just made, so every one
# of them names it as a prerequisite and tells the script where it is.
#
# Neither used to be true. The nine targets below had no prerequisites at all
# and each script globbed `build/*/release/apps/*.a`, taking whatever was there:
# the absent case was handled and the *stale* case was not, so a score could
# describe code that had already been edited away. It did, twice, on
# 2026-09-28 - a planted defect looked undetectable until the archive was
# rebuilt by hand. The glob also hardcoded `release`, so `BUILD=debug` scored
# the release archive or nothing.
#
# The archive is a normal prerequisite and the shared library an order-only one,
# which is the same shape the example rules use and for the same reason: each
# runner links $(STATIC_TARGET), so a change to the library has to relink it,
# while $(TARGET) is named only so that a conformance target on a clean tree
# leaves the tree in the state `all` would. The runner's own rpath points at
# $(PREFIX) for cutil and chron and does not want this library shared.
CONFORMANCE_LIB := $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
CONFORMANCE_ENV := PREFIX="$(PREFIX)" DEP_PCS="$(DEP_PCS)" \
	ARCHIVE="$(APP_DIR)/$(STATIC_TARGET)"

conformance: ## Score the YAML parser against yaml-test-suite (clones it on first use)
conformance: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run.sh

conformance-roundtrip: ## Round-trip yaml-test-suite through all three YAML writers
conformance-roundtrip: $(CONFORMANCE_LIB)
	@echo "--- the DOM writer, flow style ---"
	@$(CONFORMANCE_ENV) YTS_RT_MIN=100 tools/conformance/run.sh roundtrip
	@echo "--- the DOM writer, block style ---"
	@$(CONFORMANCE_ENV) YTS_RT_BLOCK=1 YTS_RT_MIN=100 tools/conformance/run.sh roundtrip
	@echo "--- the streaming writer ---"
	@$(CONFORMANCE_ENV) YTS_RT_STREAM=1 YTS_RT_MIN=100 tools/conformance/run.sh roundtrip

conformance-fastpath: ## Check the YAML JSON fast path against the general parser
conformance-fastpath: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) YTS_FP_MIN=100 tools/conformance/run.sh fastpath

conformance-json: ## Score the JSON parser against JSONTestSuite (clones it on first use)
conformance-json: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-json.sh

conformance-csv: ## Score the CSV parser against csv-spectrum (clones it on first use)
conformance-csv: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-csv.sh

conformance-jsonpath: ## Score JSONPath against the compliance test suite (clones it on first use)
conformance-jsonpath: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-jsonpath.sh

conformance-toml: ## Score the TOML parser against toml-test (clones it on first use)
conformance-toml: $(CONFORMANCE_LIB)
	@$(ORACLE_ENV) $(CONFORMANCE_ENV) TOML_MIN=100 \
		tools/conformance/run-toml.sh

conformance-toml-next: ## Score the TOML parser against toml-test's 1.1.0 list
# The other arm of GTEXT_TOML_Parse_Options::version, scored the same way and to
# the same floor. Both targets also run the suite's `crossed` mode, and what
# that adds was measured rather than argued: of eight planted defects the two
# manifests' ordinary rows caught five, crossed caught six, and one of its six
# - a lone carriage return admitted at 1.0.0, which is 1.0.0's prose read in
# place of its ABNF - was seen by no other corpus channel at all, because both
# cases for it sit in the 1.1.0 list and in neither 1.0.0 list. Two further
# defects were invisible to the corpus in every mode and are pinned in
# tests/test-toml-version.cpp; that file's header has the table.
conformance-toml-next: $(CONFORMANCE_LIB)
	@$(ORACLE_ENV) $(CONFORMANCE_ENV) TOML_SUITE_VERSION=1.1.0 \
		TOML_MIN=100 tools/conformance/run-toml.sh

UCD_VERSION := $(shell cat tools/idna/UCD_VERSION 2>/dev/null)
UCD_DIR := third_party/ucd/$(UCD_VERSION)
IDNA_TABLES := src/idna/tables
# UTS #46's mapping table is not part of the UCD and is published on its own
# schedule, so it has its own pin. Both pins are 17.0.0. They answer different
# questions; tools/idna/fetch.sh says why at length.
COMMA := ,
IDNA_MAPPING_VERSION := $(shell cat tools/idna/IDNA_MAPPING_VERSION 2>/dev/null)
IDNA_MAPPING_DIR := third_party/idna/$(IDNA_MAPPING_VERSION)
METASCHEMA_DIR := third_party/json-schema
METASCHEMA_SRC := src/json/metaschema

# The JSON5 identifier and whitespace tables used to be generated here and
# committed, and a gate diffed them against the generator's output so that they
# could not drift from the pinned UCD.  They are gone: the lexer asks
# ghoti.io-unicode for ID_Start, ID_Continue and General_Category, so there is
# no committed copy left to drift.
#
# The hazard that replaces it is a version skew.  Two UCD versions are now in
# play - the one this library pins for IDNA's own tables, in
# tools/idna/UCD_VERSION, and the one the unicode library was generated from -
# and a JSON5 document whose names are decided by one while IDNA's validity is
# decided by the other is a library that disagrees with itself about which
# characters exist.
#
# This gate asks the **linked library**, because a pin file says what a
# repository intends and a build can still have resolved an older unicode from
# an older prefix.
check-ucd-pin: ## Fail if the linked unicode library's UCD version is not the one pinned here
check-ucd-pin: $(APP_DIR)/$(TARGET)
	@tmp=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$tmp"' EXIT; \
	printf '#include <stdio.h>\n#include <ghoti.io/unicode/char.h>\nint main(void){printf("%%s\\n",guni_ucd_version());return 0;}\n' > "$$tmp/ask.c"; \
	if ! $(CC) -o "$$tmp/ask" "$$tmp/ask.c" $(UNICODE_CFLAGS) $(UNICODE_LIBS) \
			$(patsubst -L%,-Wl$(COMMA)-rpath$(COMMA)%,$(filter -L%,$(UNICODE_LIBS))) \
			2>"$$tmp/err"; then \
		printf "\033[0;31m\n### Could not ask ghoti.io-unicode for its UCD version ###\033[0m\n" >&2; \
		cat "$$tmp/err" >&2; \
		exit 1; \
	fi; \
	theirs=$$("$$tmp/ask") || exit 1; \
	if [ "$$theirs" != "$(UCD_VERSION)" ]; then \
		printf "\033[0;31m\n### Two UCD versions in one library ###\033[0m\n" >&2; \
		printf "ghoti.io-unicode was generated from UCD %s\n" "$$theirs" >&2; \
		printf "tools/idna/UCD_VERSION pins %s\n" "$(UCD_VERSION)" >&2; \
		printf "\nJSON5 names and JSON5 whitespace are decided by the first; IDNA2008\n" >&2; \
		printf "validity and UTS #46 mapping by the second. Move this library's pin\n" >&2; \
		printf "and regenerate (tools/idna/fetch.sh && tools/idna/gen_tables.py &&\n" >&2; \
		printf "tools/idna/gen_uts46.py), or build against a unicode that matches.\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mOne UCD version: ghoti.io-unicode and tools/idna/UCD_VERSION both say $$theirs.\033[0m\n"

check-idna-tables: ## Fail if the committed IDNA tables are not what the generator produces
	@$(REQUIRE_PYTHON3); \
	if ! python3 tools/idna/test_gen.py >/dev/null 2>&1; then \
		printf "\033[0;31m\n### The IDNA generator's own tests fail ###\033[0m\n" >&2; \
		python3 tools/idna/test_gen.py >&2 || true; \
		exit 1; \
	fi; \
	$(call REQUIRE_DATA,$(UCD_DIR),tools/idna/fetch.sh); \
	tmp=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$tmp"' EXIT; \
	mkdir -p "$$tmp/out"; \
	cp $(IDNA_TABLES)/tables_internal.h "$$tmp/out/"; \
	if ! python3 tools/idna/gen_tables.py --out "$$tmp/out" >/dev/null 2>"$$tmp/err"; then \
		printf "\033[0;31m\n### The IDNA generator failed ###\033[0m\n" >&2; \
		cat "$$tmp/err" >&2; \
		exit 1; \
	fi; \
	$(call REQUIRE_DATA,$(IDNA_MAPPING_DIR),tools/idna/fetch.sh); \
	if ! python3 tools/idna/gen_uts46.py --out "$$tmp/out" >/dev/null 2>"$$tmp/err"; then \
		printf "\033[0;31m\n### The UTS #46 generator failed ###\033[0m\n" >&2; \
		cat "$$tmp/err" >&2; \
		exit 1; \
	fi; \
	if ! diff -ru $(IDNA_TABLES) "$$tmp/out" >"$$tmp/diff" 2>&1; then \
		printf "\033[0;31m\n### The committed IDNA tables are stale ###\033[0m\n" >&2; \
		head -40 "$$tmp/diff" >&2; \
		printf "\nThe table under $(IDNA_TABLES) is committed so that a build needs\n" >&2; \
		printf "neither the network nor Python, which means it can drift from the\n" >&2; \
		printf "generator that is supposed to produce it. Regenerate with:\n" >&2; \
		printf "  tools/idna/gen_tables.py\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mIDNA tables are byte-identical to the generators' output (UCD $(UCD_VERSION), IDNA mapping $(IDNA_MAPPING_VERSION)).\033[0m\n"

# ---------------------------------------------------------------------------
# Oracles
#
# Both references this library compares against used to be "whatever is
# installed": CPython's unicodedata for NFC, and python-idna for the derived
# IDNA property. tools/oracle/ pins them - see containers/IMAGES for what each
# one is and why that version - and every gate below reaches its reference
# through tools/oracle/oracle_run.py, which resolves the pin, prints what
# answered, and fails rather than skipping when GHOTI_ORACLE_REQUIRED=1.
#
# GHOTI_ORACLE_MODE=host runs this machine's own tools instead. It is an escape
# hatch for a machine with no container engine, it says `unpinned` in the line
# it prints, and it names the pin it is not.
ORACLE_ENGINE ?= $(if $(GHOTI_CONTAINER_ENGINE),$(GHOTI_CONTAINER_ENGINE),docker)
# ORACLE_REQUIRED is the fail-closed half, and 1 is the suite's default: an
# unreachable reference is an error naming what is missing, rather than a skip.
# It matters more here than the word "default" suggests. Both oracle gates are
# in TEST_GATES, and four gates in this repository spent their whole lives
# exiting 0 because they answered "is python3 on PATH" instead of "can this gate
# reach its reference". python3 has always been on PATH on this machine; the
# reference it reached was two Unicode releases from the tables it was checking.
#
#   make test ORACLE_MODE=host       this machine's own tools, printed as unpinned
#   make check-idna-oracle ORACLE_REQUIRED=0   decline loudly, exit 0
#
# GHOTI_ORACLE_GATE carries the target name so that an unreachable reference
# prints the same per-gate opt-out as a missing python3 or a missing UCD.
ORACLE_MODE ?= container
ORACLE_REQUIRED ?= 1
# Recursive, not simple: `$@` in a `:=` assignment expands where there is no
# target and lands as the empty string, which is how the first version of this
# printed no opt-out at all. The flag only means anything if it is expanded in
# the recipe.
ORACLE_ENV = GHOTI_ORACLE_MODE=$(ORACLE_MODE) \
	GHOTI_ORACLE_REQUIRED=$(ORACLE_REQUIRED) GHOTI_ORACLE_GATE=$@
ORACLE_RUN = $(ORACLE_ENV) python3 tools/oracle/oracle_run.py
ORACLE_IMAGES := tools/oracle/containers

check-oracle-env: ## Fail if the oracle pin table or the code reading it has rotted
# In TEST_GATES, unlike every gate that consults an oracle, because it needs no
# engine, no reference and no build - only the committed table and the module
# that parses it. It is also the only thing that ties the oracle pins to
# tools/idna/UCD_VERSION: raising the UCD pin without raising the idna pin puts
# the reference back behind the tables it checks, which is the state that gate
# spent today climbing out of.
	@$(REQUIRE_PYTHON3); \
	python3 tools/oracle/check_oracle_env.py

oracle-version: ## Resolve every oracle pin and print what answered
	@$(REQUIRE_PYTHON3); \
	python3 tools/oracle/oracle_env.py

oracle-images: ## Build the oracle images that are made here rather than pulled
# Two references need building rather than pulling. `python-idna` vendors its own
# UCD tables, so the version of the package *is* the version of the data and no
# stock image carries the one this library needs; `toml++` is a single header with
# a compile-time switch, and no image carries that either. The stock CPython
# images are pulled on demand by oracle_env.py.
#
# The tag comes from containers/IMAGES, which is the same place oracle_env.py
# reads it, so an image built here cannot be tagged with a version the pin does
# not name. It used to be derived from the directory's requirements.txt, which
# was a second place the version lived and only worked for a reference installed
# by pip.
	@set -e; \
	for dir in $(ORACLE_IMAGES)/*/; do \
		[ -f "$$dir/Dockerfile" ] || continue; \
		name=$$(basename "$$dir"); \
		image=$$(awk -F'\t' -v n="localhost/ghoti-text-oracle-$$name:" \
			'!/^#/ && NF >= 3 && index($$2, n) == 1 { print $$2 }' \
			$(ORACLE_IMAGES)/IMAGES | head -1); \
		[ -n "$$image" ] || { printf "%s has a Dockerfile and no built-here line in containers/IMAGES, so nothing says what to tag it\n" "$$name" >&2; exit 1; }; \
		printf "\033[0;36mbuilding %s\033[0m\n" "$$image"; \
		$(ORACLE_ENGINE) build -t "$$image" "$$dir"; \
	done; \
	printf "\033[0;32mOracle images built. 'make oracle-version' checks them against IMAGES.\033[0m\n"

oracle-clean: ## Remove the oracle images built here, leaving the pulled ones
	@$(ORACLE_ENGINE) images --format '{{.Repository}}:{{.Tag}}' \
		| grep '^localhost/ghoti-text-oracle-' \
		| xargs -r $(ORACLE_ENGINE) rmi

check-idna-oracle: ## Compare the derived IDNA property against a pinned independent implementation
# The one gate here whose pin changed an answer rather than only making it
# reproducible. python-idna vendors its own generated tables, so the package
# version *is* the data version and no interpreter could have moved it: Debian
# packages 3.10, whose tables are UCD 15.1.0 against this library's 17.0.0, and
# that gap was 925 codepoints this library assigns, the reference does not, and
# both call DISALLOWED - so a DISALLOWED of ours that should have been PVALID
# agreed with the reference's ignorance and passed. Against the pinned 3.19,
# whose tables are 17.0.0, the comparison covers all 299,382 assigned codepoints
# and that bucket has no members.
#
# The host `import idna` check is gone because it asked the wrong question. Any
# idna satisfied it; what this gate needs is one whose tables are the pinned
# UCD, and oracle_env.py is what can tell the difference.
check-idna-oracle:
	@$(REQUIRE_PYTHON3); \
	$(call REQUIRE_DATA,$(UCD_DIR),tools/idna/fetch.sh); \
	$(ORACLE_RUN) idna -- python3 tools/oracle/idna_diff.py

check-nfc-oracle: ## Compare this library's NFC against a pinned CPython's, over every sequence
# CPython's unicodedata is a normaliser written by other people from the same
# annex, and a wrong one here is not visible from outside: a missed composition
# exclusion or an unstable canonical sort produces a normaliser that is right
# about almost every string. The driver links the archive, so this needs a
# build - and that is why only the *reference* half runs in the image.
#
# The gating pin carries UCD 16.0.0 against these tables' 17.0.0, so the skew is
# stated rather than closed. Measured before the conversion: over the 3,412,112
# sequences the host's 15.1.0 could answer, UCD 15.1.0, 16.0.0 and 17.0.0 return
# identical NFC, so moving off the host buys coverage and reproducibility and
# corrects nothing. check-nfc-oracle-strict below is where the skew goes away.
check-nfc-oracle: $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@$(REQUIRE_PYTHON3); \
	$(call REQUIRE_DATA,$(UCD_DIR),tools/idna/fetch.sh); \
	ARCHIVE="$(APP_DIR)/$(STATIC_TARGET)" \
		$(ORACLE_RUN) python -- python3 tools/oracle/nfc_diff.py

check-nfc-oracle-strict: ## The NFC oracle against a UCD-matched CPython, where a disagreement is a defect
# Deliberately outside TEST_GATES. The only pin whose UCD equals this library's
# is a release candidate, and gating on one of those is not something to do -
# but a matched reference is the only configuration in which a disagreement is
# necessarily a defect rather than a finding to read, and it is the only one
# with no skipped-for-age bucket at all. It also compares 184,690 more sequences
# than the gating pin, which is the whole of what the skew costs: the remaining
# 814,730 skips are codepoints 17.0.0 does not assign either, so no reference
# reaches them and this gate cannot pass 81.5% coverage at any version.
#
# When CPython 3.15.0 is released the python pin moves to it and the two gates
# converge; until then this is the one to run by hand after touching NFC.
check-nfc-oracle-strict: $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@$(REQUIRE_PYTHON3); \
	$(call REQUIRE_DATA,$(UCD_DIR),tools/idna/fetch.sh); \
	ARCHIVE="$(APP_DIR)/$(STATIC_TARGET)" \
		GHOTI_ORACLE_ALIAS=python=python-next \
		$(ORACLE_RUN) python -- python3 tools/oracle/nfc_diff.py --strict

check-toml-oracle: ## Compare the TOML reader against a pinned tomllib, over generated documents
# The third thing. The fuzzers check this library against itself - two of its own
# readers, and its writer against its reader - and toml-test is the only place a
# second implementation appears, holding the cases somebody chose.
# tools/oracle/toml_gen.py generates documents nobody chose, valid v1.0.0 by
# construction, with the spelling varying independently of the value.
#
# TOML_ORACLE_COUNT sets how many (3,000 by default) and TOML_ORACLE_SEED where
# to start, so a disagreement is reproducible from the seed the gate prints.
#
# Outside TEST_GATES, like every gate that consults an oracle: it needs a
# container engine and a pull.
check-toml-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-toml-oracle.sh

check-toml-1-1-oracle: ## Compare the v1.1.0 reader against toml++'s unreleased set
# The only second reader there is for the v1.1.0 draft, and it is not a v1.1.0
# implementation: toml++'s `TOML_ENABLE_UNRELEASED_FEATURES` is its own
# cherry-pick from the TOML master branch and issue list, overlapping v1.1.0's
# four relaxations this library implements and adding two v1.1.0 does not have.
# tools/oracle/toml_1_1_diff.py carries that list by name and generates only the
# overlap, and it fails rather than passing if a run did not reach all five
# spellings (four items; the inline-table one is two independent choices).
#
# Needs the built-here image: make oracle-images.
check-toml-1-1-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-toml-1-1-oracle.sh

check-ini-git-oracle: ## Compare the git config reader against git itself
# git config has **one** reference, unlike Desktop Entry's two, and that is the
# weakness this gate is built around rather than a convenience: the same program
# decides whether a file is legal and what its values are, so a clean run cannot
# tell "we agree" from "we are both wrong the same way". What stands in for the
# second reference is the generator's own intent - every rule written down from
# git-config(1) and from measurement before either program is asked - so a rule
# both got wrong fails `intent` while passing `legality`.
#
# There is also no corpus to fall back on, and that is measured rather than
# asserted. Over the 25 git config files on this machine - every `.git/config`
# under $HOME plus /etc/gitconfig - 24 carry a quoted subsection and **zero** carry
# any of: a dotted subsection, a continuation, a quoted value, a valueless key, an
# inline comment, a repeated key, a `;` comment line, CRLF, a preamble entry, or a
# non-ASCII byte. A conformance run over them would score one construct out of
# eleven and print clean, which is the shape this repository keeps meeting.
#
# Outside TEST_GATES, like every gate here that consults an oracle.
check-ini-git-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-ini-git-oracle.sh

check-ini-editorconfig-oracle: ## Compare the EditorConfig reader against both cores
# **Two references that disagree, and neither is an authority** - which is a third
# situation, distinct from both of the gates around it. Desktop Entry has two
# references that disagree and between them decide the answers; git config has one
# that decides everything; EditorConfig has two that disagree *and a normative
# conformance suite that both of them fail*.
#
# So the specification gets the vote. `intent` scores our verdict against
# specification 0.17.2 as tools/oracle/ini_ec_gen.py writes it down; `values-c` and
# `values-py` score only over documents carrying no construct that core is known to
# get wrong; and `divergence-c` and `divergence-py` assert that each known
# departure **is still there**, per axis. That last score is the unusual one and it
# is the point: a core fixed upstream fails it loudly rather than silently
# inflating the values score, and a generator that stopped emitting the
# discriminating document fails it too.
#
# Their agreement is also worth less than the Desktop Entry pin's, because the
# sharing is structural rather than incidental: both cores descend from Python's
# `ConfigParser`, and the one rule they agree on while contradicting the
# specification - a value truncated at a whitespace-preceded `#` or `;` - is
# exactly the inherited behaviour. containers/IMAGES says so where the pin is.
#
# The differential deliberately needs **no glob matcher**: every section name the
# generator emits is a literal filename and the query is one of those names. A
# filepath glob is the subject of 130 of the suite's 202 assertions and is not a
# text library's job, so the glob-bearing documents are conformance-ini-editorconfig's.
#
# Outside TEST_GATES, like every gate here that consults an oracle.
check-ini-editorconfig-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-ini-editorconfig-oracle.sh

check-ini-systemd-oracle: ## Compare the systemd reader against systemd itself
# **The instrument answers a different question than it appears to**, and finding that
# out was most of the work: `systemd-analyze verify` **exits 0 on a syntax error**. It
# warns, skips the offending line, and keeps the file; the exit status reports semantic
# failure, such as a service with no ExecStart=. A gate built on it would have scored
# every syntactically broken document as legal and printed clean.
#
# So the gate reads the **diagnostics** and classifies them into line-grammar faults
# and everything else, and values come back through `Environment=` - the one setting
# that echoes each parsed word verbatim, after unquoting, unescaping and word
# splitting. Two limits of that channel are measured and live in the denominator: it
# rewrites a CR to an LF and truncates a message at 2,097 bytes.
#
# systemd and this module **disagree on purpose**: systemd keeps a file with a bad line
# and drops the line, while this refuses the document. So `grammar` compares the
# *presence* of a fault rather than the recovery. And one score asserts that systemd is
# **still wrong** about a byte-order mark before anything but a section header, which it
# skips too late - a reference fixed upstream fails that loudly rather than quietly
# inflating the others.
#
# Outside TEST_GATES, like every gate here that consults an oracle.
check-ini-systemd-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-ini-systemd-oracle.sh

check-ini-configparser-oracle: ## Compare the configparser reader against CPython's
# **One reference, and it is also the specification** - the weakest position any dialect
# in this module is in. git config has a lone reference too, but `git-config(1)` exists
# to disagree with git; the Python documentation describes what `configparser` does and
# says so. So two things carry the weight instead: the generator's own reading, scored as
# `intent`, and conformance-ini-configparser's 703 real files, 224 of which the reference
# refuses.
#
# The reference reads a **file**. Python's universal-newline translation applies to
# `read(path)` and not to `read_string()`, and of 38 documents probed both ways a lone CR
# is the only thing they disagree about. A file is what an INI document is.
#
# Three axes are excluded from `values` and asserted separately: `configparser` works on
# Python `str`, so its whitespace and its case folding are Unicode's, and a byte-oriented
# reader cannot ask either question of one byte. The `divergence` score is that each
# departure is **still there** - a change to the fold fails loudly rather than quietly
# inflating `values`.
#
# Outside TEST_GATES, like every gate here that consults an oracle.
check-ini-configparser-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-ini-configparser-oracle.sh

conformance-ini-win32: ## Score the Win32 reader over this machine's .ini files
# **Real bytes of the right shape from the wrong provenance**, and the gate prints
# that with every run. This machine has two `.ini` files a Windows application
# wrote, both inside a wine prefix; what it has hundreds of are freedesktop and
# Python `.ini` and `.cfg` files. The profile API reads any of them, so they are a
# valid population for "do we agree with the reference about real bytes" and no
# population at all for "is this format used this way".
#
# Worth having anyway, and the reason is configparser's: of that dialect's three
# defects the **corpus** found the cheapest one - a value beginning with `;`,
# present in 331 of 479 real files and in none of the 102 probes or 88 generator
# axes. This is the same instrument aimed at a weaker population.
#
# The comparison is the differential's own, with `--corpus` instead of generated
# documents: one implementation, two populations.
#
# Outside TEST_GATES, like every gate here that consults an oracle.
conformance-ini-win32: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/conformance/run-ini-win32.sh

check-ini-win32-oracle: ## Compare the Win32 reader against wine's profile API
# **The reference is wine, not Windows**, and the gate prints that with every run.
# Two of this dialect's thirty rules have a second, independent source - Microsoft's
# documentation for `GetPrivateProfileString` states quote stripping and case
# insensitivity - and the other twenty-eight rest on one reimplementation. That is a
# weaker position than any other INI dialect here is in, configparser included: there
# the reference is at least the artifact everyone else reads.
#
# What offsets it is that the reference has **three entry points and two of them
# disagree about the most consequential rule in the format**.
# `GetPrivateProfileString` retrieves `;disabled=1`; `GetPrivateProfileSection` does
# not list it. So this is a two-reference differential built from one implementation,
# scored both ways, with the disagreement asserted by the `divergence` score rather
# than resolved by picking a side quietly. The dialect follows the enumeration API.
#
# **There is no REFUSES table, because this dialect refuses nothing.** The key charset
# is open, the empty key and empty section name are both spellable, a line with no
# separator is a valueless entry, and an unclosed header is an ordinary line. `intent`
# checks exactly that: a byte sequence this reader will not parse is a defect, and no
# reference can report one.
#
# Outside TEST_GATES, like every gate here that consults an oracle.
check-ini-win32-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-ini-win32-oracle.sh

check-ini-win32-encoding-oracle: ## Compare the UTF-16 decode against the profile API
# **A UTF-16 `.ini` is a question the reference answers**, which is why this gate
# exists and why the UTF-16 decode is not simply asserted in a header. Given the same
# document as UTF-8 and as UTF-16LE-with-a-mark, `GetPrivateProfileSectionNames` and
# `GetPrivateProfileString` return the same sections and the same values - so every
# generated document can be asked in three encodings and scored on all three.
#
# **Without a mark the reference reads nothing**: no sections, every lookup MISSING,
# because the first line reads as a section name beginning with a NUL and the API
# hands back C strings. That is the corroboration for having no content heuristic,
# and it is a better reason than the one it replaces: not "this machine has no
# BOM-less UTF-16 file to calibrate a guess against" but "the reference does not
# guess, and a reader that did would be reading a document it does not read".
#
# Not in `conformance-all` and not in `make test`, for the reason every gate needing
# the pinned image is not: it needs that image. Its ASCII-only population is stated
# with the number - the A entry points transcode to the host code page, so a non-ASCII
# document is the channel narrowing rather than a disagreement, and those are counted
# out loud rather than dropped.
check-ini-win32-encoding-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) \
		tools/oracle/run-ini-win32-encoding-oracle.sh

check-ini-win32-authored-oracle: ## Score against files the profile API itself wrote
# **The population whose provenance is right by construction**, and the only answer
# this host can give to what `conformance-ini-win32` states about itself: that its 701
# files are real bytes of the right shape from the wrong provenance. Two `.ini` files
# here were written by a Windows application. Nothing can change that; what can be
# changed is who writes the population, and `WritePrivateProfileString` is the other
# half of the same reference.
#
# It is also the only gate that exercises the reference as a **writer**; every other
# ask here is about what a lookup returns. What it confirmed on its first run was
# already measured by a one-off probe and already written down - the authoring call
# strips a value's leading and trailing whitespace, so `"  padded  "` reaches the file
# as `padded`. The gain is that the rule is now under a gate rather than in a
# sentence.
#
# Not in `conformance-all`, for the same reason as the other two: it needs the pinned
# image.
check-ini-win32-authored-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) \
		tools/oracle/run-ini-win32-authored-oracle.sh

check-ini-oracle: ## Compare the Desktop Entry reader against both of its references
# The gate the corpus cannot be. Every `.desktop` file on this machine is already
# valid, so conformance-ini-desktop-entry scores acceptance and preservation and
# reaches no refusal at all: no real file here carries a duplicate key, a `;`
# comment, a BOM, CRLF or a non-ASCII name. This generates them.
#
# Two references, in one image, over the same bytes in one pass, because they
# answer different halves of one question and disagree about the answer: GKeyFile
# says what a value *is* and accepts documents section 3.2 forbids;
# desktop-file-validate says whether a document is *legal* and refuses them. A
# differential against either alone would agree with that one's blind spot.
#
# Four scores with four denominators, plus the generator's own intent as a third
# reading, plus a zero-axis check: an axis the generator stopped emitting fails
# the gate rather than quietly shrinking the population.
#
# Outside TEST_GATES, like every gate here that consults an oracle.
# Needs the built-here image: make oracle-images.
check-ini-oracle: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(ORACLE_ENV) $(CONFORMANCE_ENV) tools/oracle/run-ini-oracle.sh

check-metaschema: ## Fail if the embedded meta-schemas are not what json-schema.org publishes
# The nineteen documents under $(METASCHEMA_SRC) - 2020-12's nine, 2019-09's
# seven, and one each for draft-07, draft-06 and draft-04 - are somebody else's,
# embedded so that a schema which validates another schema needs no resolver
# and no socket. That makes this file the one place in the repository where a
# silent edit would change what "a valid schema" means in any of those
# dialects, with nothing to compare against. Regenerating from the published
# documents and diffing is the comparison; it is also a content check, since
# the bytes are verbatim and any difference at all is a difference from what is
# published.
	@$(REQUIRE_PYTHON3); \
	$(call REQUIRE_DATA,$(METASCHEMA_DIR),tools/metaschema/fetch.sh); \
	tmp=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$tmp"' EXIT; \
	if ! python3 tools/metaschema/gen_metaschema.py --out "$$tmp" >/dev/null 2>"$$tmp/err"; then \
		printf "\033[0;31m\n### The meta-schema generator failed ###\033[0m\n" >&2; \
		cat "$$tmp/err" >&2; \
		exit 1; \
	fi; \
	if ! diff -u $(METASCHEMA_SRC)/metaschema_docs.c "$$tmp/metaschema_docs.c" >"$$tmp/diff" 2>&1; then \
		printf "\033[0;31m\n### The embedded meta-schemas are not what is published ###\033[0m\n" >&2; \
		head -40 "$$tmp/diff" >&2; \
		printf "\nEither the committed file was edited, or the fetched documents are\n" >&2; \
		printf "not the published ones. Refetch and regenerate with:\n" >&2; \
		printf "  tools/metaschema/fetch.sh && tools/metaschema/gen_metaschema.py\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mEmbedded meta-schemas are byte-identical to what json-schema.org publishes.\033[0m\n"

conformance-json-schema: ## Score the schema engine against JSON-Schema-Test-Suite (clones it on first use)
conformance-json-schema: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-json-schema.sh

# Every draft this engine reads, each at its own floor.
#
# The target above scores one directory - 2020-12 unless JSS_DRAFT says
# otherwise - so until this existed the three older drafts were measured by
# hand and nothing failed when one of them lost an assertion. That was not
# hypothetical: draft-07 and draft-06 each sat eight assertions short of the
# other two for as long as there was no target that would have said so, and the
# defects behind them were things only those two directories ask about.
#
# One `make` per draft rather than a loop, so that a failure names the draft in
# the line that fails.
JSS_DRAFTS := draft2020-12 draft2019-09 draft7 draft6 draft4

conformance-json-schema-all: ## Score the schema engine against every draft it reads
conformance-json-schema-all: $(CONFORMANCE_LIB)
	@fail=0; \
	for draft in $(JSS_DRAFTS); do \
		printf "\033[0;36m### JSON-Schema-Test-Suite: $$draft ###\033[0m\n"; \
		JSS_DRAFT=$$draft $(CONFORMANCE_ENV) \
			tools/conformance/run-json-schema.sh || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then \
		printf "\033[0;31m\n### A draft did not meet its floor ###\033[0m\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mEvery draft met its floor.\033[0m\n"

conformance-json-to-toml: ## Score gtext_json_to_toml() over JSONTestSuite's documents
conformance-json-to-toml: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-json-to-toml.sh

conformance-ini-desktop-entry: ## Score the INI reader over this machine's Desktop Entry files
# The one conformance target whose corpus is neither fetched nor pinned, because
# no Desktop Entry test suite exists to fetch. What exists is a large population
# of real files on any Linux system, so the denominator is derived at run time
# and printed, and an empty corpus fails rather than scoring 0 of 0.
#
# It scores acceptance and preservation - parse, byte-identical rewrite, and the
# generic dialect's inherited parity - and not refusal, because every file in the
# corpus is already valid. The refusals are in tests/test-ini.cpp.
conformance-ini-desktop-entry: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-ini-desktop-entry.sh

conformance-ini-editorconfig: ## Score the INI reader against editorconfig-core-test
# The only INI target whose number is a **pass count against a normative suite**
# rather than an agreement with a reference: EditorConfig 0.17.2 says a conforming
# core "must pass the tests in the core-tests repository", so the floor is all of
# them and the suite commit is pinned in tools/conformance/EDITORCONFIG_SUITE_COMMIT.
#
# 34 of the suite's 202 assertions test the grammar. The other 168 test a filepath
# glob matcher (130), file discovery (24), value semantics (10) and a command line
# (3), none of which is a text library's job - quoting 202 and scoring a sixth of
# it would be the wrong number.
#
# **Both reference cores score 33 of these 34**, so this target is the one place
# in the module where agreeing with the reference implementations everywhere would
# be a failure. They are used as differential oracles instead, by
# check-ini-editorconfig-oracle.
#
# The script runs a **control** first - the harness's own glob matcher and section
# merge against a throwaway Python parser - which must score 34 of 34 before the
# library is asked anything, because the harness has to resolve properties itself
# and a harness that has never been shown to work cannot fail for the right reason.
conformance-ini-editorconfig: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-ini-editorconfig.sh

conformance-ini-systemd: ## Score the INI reader over this machine's systemd units
# Acceptance and preservation only, because **this machine has no systemd**: the unit
# files are shipped by other packages, PID 1 is `init`, and there is no
# `systemd-analyze` here to say whether one is valid. The Desktop Entry corpus comes
# with a validator and this one does not.
#
# The gate prints **which constructs the corpus does not contain**, and those zeros are
# the finding rather than a footnote: a `;` comment, CRLF, a BOM and every escape appear
# in no unit file on this machine, so a clean run says nothing about them.
# check-ini-systemd-oracle is what does.
conformance-ini-systemd: $(CONFORMANCE_LIB)
	@$(CONFORMANCE_ENV) tools/conformance/run-ini-systemd.sh

conformance-ini-configparser: ## Score the INI reader against configparser over this machine's .cfg and .ini files
# **The only INI corpus gate whose reference is installed**, and that changes what a
# corpus can be asked. `configparser` is in the standard library of the python3 that
# scores this, so every file gets all three questions - do we accept what it accepts,
# does every value agree, and is the rewrite byte for byte - where the Desktop Entry
# corpus can only be asked legality and the systemd corpus only acceptance.
#
# **A `.cfg` extension does not mean the file is one of these documents**, and the gate
# measures that rather than filtering it away: 224 of the 703 files here are not
# configparser documents at all - most `lit.cfg` files are Python scripts - and they are
# the only **refusals** a real corpus of this format offers. They stay in the
# denominator, because a reader that accepted them would be wrong in the one direction
# no generated document tests.
#
# 78 files are excluded from the values score for a measured reason - `configparser`
# strips Unicode whitespace and this reader strips ASCII whitespace - and the gate
# **asserts that every excluded file really does differ**, so an exclusion cannot widen
# quietly.
conformance-ini-configparser: $(CONFORMANCE_LIB)
	@$(REQUIRE_PYTHON3); \
	$(CONFORMANCE_ENV) tools/conformance/run-ini-configparser.sh

conformance-all: ## Score every parser against its external corpus
# **conformance-ini-win32 is deliberately not here.** Every gate in this list runs
# with what the machine already has - a local corpus, an installed reference, or a
# pinned suite checkout - and none of them needs a container. The Win32 corpus gate
# does: its reference is wine inside the pinned image, by design rather than by
# accident, because pinning the host's wine is not something this library can do.
# Adding it would quietly change this target's contract from "needs this machine" to
# "needs podman", so it sits with the oracle gates instead, where that requirement is
# already the rule. `make conformance-ini-win32` runs it.
conformance-all: conformance conformance-fastpath conformance-json conformance-csv conformance-json-schema-all conformance-jsonpath conformance-toml conformance-toml-next conformance-json-to-toml conformance-ini-desktop-entry conformance-ini-editorconfig conformance-ini-systemd conformance-ini-configparser

coverage: ## Build instrumented, run the tests, and report line coverage
# Cleans first because the object files would otherwise be reused without the
# instrumentation, then cleans and rebuilds at the end: leaving the
# instrumented objects behind would have a later `make` silently link them,
# and leaving the tree cleaned would break any sibling project that links
# this one. The cost is one extra build; coverage is not run often.
	@$(MAKE) --no-print-directory clean > /dev/null
# The instrumented build, the report, and the restoration of the tree are one
# shell command so that the cleanup runs whatever fails.  Letting a failure
# stop the recipe leaves the --coverage objects in build/, and the next
# ordinary `make` links them into a library that needs the gcov runtime; every
# later build then fails with undefined references to __gcov_init until
# somebody works out why.  That is exactly what the cleanup exists to prevent,
# so it must not itself be skipped by the failure it is there to survive.
#
# TEST_GATES is cleared because --coverage links the gcov runtime, which
# exports mangle_path.  check-symbols is right to reject that in a shipping
# build, but it is not a defect in an instrumented one, and it made this
# target fail before it ever produced a report.
#
# COVERAGE_MIN, when set, makes the report fail below that percentage.  CI
# passes one so that coverage can only be argued upward; a local run without it
# just prints the numbers.
	@status=0; \
	$(MAKE) --no-print-directory test TEST_GATES= \
		EXTRA_CFLAGS="--coverage -O0" \
		EXTRA_LDFLAGS="--coverage" > /dev/null || status=$$?; \
	if [ $$status -eq 0 ]; then \
		COVERAGE_MIN=$(COVERAGE_MIN) tools/coverage.sh $(OBJ_DIR) || status=$$?; \
	else \
		printf "coverage: the instrumented test run failed; no report\n" >&2; \
	fi; \
	$(MAKE) --no-print-directory clean > /dev/null; \
	$(MAKE) --no-print-directory all > /dev/null; \
	exit $$status

help: ## Display this help
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' Makefile | sort | sed 's/\([^:]*\):.*## \(.*\)/\1:\2/' | awk -F: '{printf "%-15s %s\n", $$1, $$2}' | sed "s/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g"


####################################################################
# Flag stamps
####################################################################
# Each build tree carries the flag string it was built with. The stamp is
# rewritten only when that string differs -- written to a scratch file,
# compared, moved into place only on a difference -- so its mtime moves on a
# flag change and on nothing else. The object rules above depend on it.
#
# This replaces listing `Makefile` as a prerequisite, which was too broad (a
# comment-only edit recompiled everything) and too narrow (a command-line
# override such as `make EXTRA_CFLAGS=-O2` changes no file's mtime and so was
# invisible).
#
# These rules sit at the end of the file for two reasons. A rule's target
# expands when make reads the line, so a stamp rule above its own OBJ_DIR
# definition has an empty target: not an error, just a rule that silently does
# not exist. And the first target in a makefile is the default goal, so a stamp
# rule above `all:` makes a bare `make` build the stamp and nothing else.
.PHONY: force-flags

$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CFLAGS) $(CXXFLAGS) $(LDFLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(ASAN_CFLAGS) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(TSAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(TSAN_CFLAGS) $(TSAN_CXXFLAGS) $(TSAN_LDFLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(FUZZ_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(FUZZ_SAN) $(FUZZ_LIB_FLAGS) $(FUZZ_BIN_FLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@
