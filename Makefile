# Copied from runtime-debug's Makefile, which was copied from runtime-heap's,
# per CONVENTIONS.md section 12. PROJECT, the dependency block (cutil and
# runtime-core), the gates, the planted-defect builds and the flags changed;
# the text dependency, the fuzz and DAP targets are gone (there is no loader
# for the IR or for compiled code to fuzz; documentation/design.md says so).
# The direction gate stays dropped (the headers are flat and all `free`). The
# edge gate is an allowlist of cutil, runtime-core and this library (AD-2).
# Tests are built -fno-omit-frame-pointer: the read-back test walks the frame
# chain from a helper into the frame of the compiled code that called it.
#
# The test-executable rules keep section 6's shape: the static archive is a
# normal prerequisite of every test because the recipe links it, and the .so
# is order-only because check-symbols wants it and the tests do not link it.

SUITE := ghoti.io
PROJECT := runtime-jit

BUILD ?= release
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
VERSION_MINOR_ONLY := $(word 1,$(subst ., ,$(MINOR_VERSION)))
VERSION_PATCH_ONLY := $(or $(word 2,$(subst ., ,$(MINOR_VERSION))),0)
VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)

# Names this build everywhere: the .pc file, the install directory, the soname
# and the symbol token. Defaults to the major version. Override for a build
# that wants its own identity: make BRANCH=-dev
BRANCH ?= -$(MAJOR_VERSION)

ifeq ($(BRANCH),-$(MAJOR_VERSION))
VERSION_STRING := $(VERSION)
else
VERSION_STRING := $(VERSION)$(BRANCH)
endif

# override: BRANCH may have come from the command line, and a command-line
# variable otherwise beats a plain assignment. Without it, `make BRANCH=-dev
# BUILD=debug` produced a debug build carrying the release token.
ifeq ($(BUILD),debug)
    override BRANCH := $(BRANCH)-debug
    override VERSION_STRING := $(VERSION_STRING)-debug
endif

# Decided here, before the platform rewrite below. `BUILD := linux/$(BUILD)`
# is a plain assignment, so a command-line BUILD=debug stays "debug" and an
# environment BUILD=debug becomes "linux/debug". Testing it up here is true
# in both cases. Release is -O2 because that is what ships. -O3 is not the
# default because nothing here has measured a figure that would justify it,
# and the armed poll and the frame encoding are the places to measure it when
# something does (AD-26).
ifeq ($(BUILD),debug)
OPT_CFLAGS := -O0
else
OPT_CFLAGS := -O2
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
LIBVER_SYMBOL := $(shell echo "ghotiio_$(PROJECT)$(BRANCH)" | sed 's/[.-]/_/g')
BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
STATIC_TARGET := $(BASE_NAME_PREFIX).a
ENV_VARS :=

# Do not assign PKG_CONFIG_PATH. Make exports an inherited variable with
# whatever value the makefile last gave it, so overwriting it handed every
# sub-make a different path from the parent's.
PKG_CONFIG_PATH_ENV := $(PKG_CONFIG_PATH)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
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
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := mac/$(BUILD)

# The MINGW64 branch below has been cross-built and run under wine, with a
# uname and cygpath that imitate MSYS2 (tools/xwin/m1-run.sh in the workspace);
# that includes GRJIT_API's dllexport/dllimport switching, which the probe
# consumes. It has not run on a Windows machine, and the MINGW32 branch has not
# run at all. See notes/suite/WINDOWS-TODO.md.
else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	PC_INSTALL_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win32/$(BUILD)

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	PC_INSTALL_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))
endif

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
LDCONF_INSTALL_PATH :=
endif

PKG_CONFIG_LOOKUP_PATH := $(if $(PKG_CONFIG_PATH_ENV),$(PKG_CONFIG_PATH_ENV):)$(PC_INSTALL_PATH)

CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 -O1 -g $(EXTRA_CXXFLAGS)
# Frame pointers in the tests: a native helper finds the frame of the compiled
# code that called it by following the saved-rbp chain.
CXXFLAGS += -fno-omit-frame-pointer
CC := cc
# -Wstrict-aliasing=1 and -fstrict-aliasing, named rather than inherited.
# -Wall sets the aliasing warning to level 3, which is silent on the probe
# check-aliasing compiles. An explicit level beats -Wall from either side.
# -fstrict-aliasing is off below -O2 unless named, so a debug or coverage
# tree would otherwise have the warning armed and the assumption off.
# The probe is a pointer parameter stored through a second variable: that
# shape is reported at level 1 and silent at 0, 2, and 3. Do not simplify it
# to `*(int *)&local`, which fires at every level from 1 up and certifies
# nothing. check-aliasing is the measurement.
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wno-error=unused-function -Wfatal-errors -std=c17 -pthread $(OPT_CFLAGS) -g $(EXTRA_CFLAGS)
ifeq ($(OS_NAME), Windows)
CFLAGS += -DGRJIT_STATIC
CXXFLAGS += -DGRJIT_STATIC
endif
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -DGRJIT_BUILD $(EXTRA_CFLAGS)
LDFLAGS := -L /usr/lib -lstdc++ -lm -pthread $(EXTRA_LDFLAGS)
ifdef PREFIX
LDFLAGS += -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Windows)
export PATH := $(BIN_INSTALL_PATH):$(PATH)
endif
endif

BUILD_DIR := ./build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/objects
FLAGS_STAMP := $(OBJ_DIR)/.flags
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps

ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC
endif

INCLUDE := -I include/ -I $(GEN_DIR)/

DEPLESS_GOALS := docs docs-pdf clean cloc help
ifeq ($(filter-out $(DEPLESS_GOALS),$(or $(MAKECMDGOALS),all)),)
SKIP_DEP_CHECK := 1
endif

# The name carries $(BRANCH). ghoti.io-cutil never matches the installed file.
CUTIL_PC ?= ghoti.io-cutil$(BRANCH)
CUTIL_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(CUTIL_PC) 2>/dev/null)
CUTIL_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(CUTIL_PC) 2>/dev/null)
ifeq ($(strip $(CUTIL_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-cutil was not found by pkg-config. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(CUTIL_CFLAGS)

# runtime-core: the page provider, the code-metadata format and the layout
# descriptor this library emits against. Found the same way, by pkg-config alone and with the same
# branch suffix, and a hard error when it is missing.
RTCORE_PC ?= ghoti.io-runtime-core$(BRANCH)
RTCORE_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RTCORE_PC) 2>/dev/null)
RTCORE_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RTCORE_PC) 2>/dev/null)
ifeq ($(strip $(RTCORE_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-core was not found by pkg-config. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(RTCORE_CFLAGS)

SOURCES := $(shell find src -type f -name '*.c')
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest`

# coverage clears this: --coverage links the gcov runtime, whose mangle_path
# check-symbols is right to reject in a shipping library.
TEST_GATES ?= check-symbols check-aliasing check-stamps check-labels \
	check-edges check-gates check-planted examples

VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1 --suppressions=tests/valgrind.supp

TEST_HELPER_SRC := $(wildcard tests/test_helpers.cpp)
TEST_HELPER_OBJ := $(patsubst tests/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_HELPER_SRC))

# The archive, not the shared library: a static link resolves hidden symbols.
# --whole-archive because a constructor-registered object would otherwise be
# dropped. The archive is a normal prerequisite of every test, so a clean
# tree builds it; the .so is order-only because check-symbols wants it and
# the tests do not link it.
CORELIBRARY := -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive $(RTCORE_LIBS) $(CUTIL_LIBS)

# Empty. color wraps __mingw_fprintf for a test helper of its own; this
# library has no such helper, and a --wrap with no __wrap_ definition is an
# undefined-reference link error on the platform where it applies.
TEST_LDFLAGS :=

TEST_PAIRS := $(shell find tests -type f -name 'test_*.cpp' 2>/dev/null | sort | grep -v test_helpers | while read f; do \
	echo "$$f|$$(basename "$$f" .cpp | sed 's/test_/test/; s/^test\([a-z]\)/test\U\1/')"; done)
TEST_SOURCES := $(foreach pair,$(TEST_PAIRS),$(word 1,$(subst |, ,$(pair))))
TEST_NAMES := $(foreach pair,$(TEST_PAIRS),$(word 2,$(subst |, ,$(pair))))
TEST_EXECUTABLES := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

EXAMPLE_SOURCES := $(shell find examples -type f -name '*.c' 2>/dev/null)
EXAMPLES := $(patsubst examples/%.c,$(APP_DIR)/examples/%$(EXE_EXTENSION),$(EXAMPLE_SOURCES))

all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build shared + static libraries

TEST_DEPFILES := $(foreach pair,$(TEST_PAIRS),$(OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
DEPFILES := $(LIBOBJECTS:.o=.d) $(TEST_HELPER_OBJ:.o=.d) $(TEST_DEPFILES)
-include $(DEPFILES)

LIBVER_GEN := $(GEN_DIR)/ghoti.io/$(PROJECT)/libver_gen.h

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
		'/*' \
		' * SPDX-License-Identifier: LGPL-3.0-only' \
		' *' \
		' * Copyright (C) 2026 Corey Pennycuff' \
		' *' \
		' * This file is part of Ghoti.io Runtime-jit.' \
		' *' \
		' * Ghoti.io Runtime-jit is free software: you can redistribute it and/or' \
		' * modify it under the terms of the GNU Lesser General Public License version' \
		' * 3 as published by the Free Software Foundation.' \
		' *' \
		' * Ghoti.io Runtime-jit is distributed in the hope that it will be useful,' \
		' * but WITHOUT ANY WARRANTY; without even the implied warranty of' \
		' * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser' \
		' * General Public License for more details.' \
		' *' \
		' * You should have received a copy of the GNU Lesser General Public License' \
		' * along with this program.  If not, see <https://www.gnu.org/licenses/>.' \
		' */' \
		'' \
		'// Generated by the Makefile. Do not edit; see CONVENTIONS.md section 4.' \
		'#ifndef GHOTI_IO_GRJIT_LIBVER_GEN_H' \
		'#define GHOTI_IO_GRJIT_LIBVER_GEN_H' \
		'' \
		'/** The symbol namespace for this build, from the Makefile'"'"'s BRANCH. */' \
		'#define GHOTIIO_RUNTIME_JIT_NAME $(LIBVER_SYMBOL)' \
		'' \
		'/** Human-readable version of this build. */' \
		'#define GHOTIIO_RUNTIME_JIT_VERSION "$(VERSION_STRING)"' \
		'' \
		'/** The same version as three integers. */' \
		'#define GHOTIIO_RUNTIME_JIT_VERSION_MAJOR $(MAJOR_VERSION)' \
		'#define GHOTIIO_RUNTIME_JIT_VERSION_MINOR $(VERSION_MINOR_ONLY)' \
		'#define GHOTIIO_RUNTIME_JIT_VERSION_PATCH $(VERSION_PATCH_ONLY)' \
		'' \
		'#endif // GHOTI_IO_GRJIT_LIBVER_GEN_H' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(APP_DIR)/$(TARGET): $(LIBOBJECTS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -Wl,--as-needed $(RTCORE_LIBS) $(CUTIL_LIBS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)
ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

$(APP_DIR)/$(STATIC_TARGET): $(LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

ifneq ($(TEST_HELPER_SRC),)
$(TEST_HELPER_OBJ): $(TEST_HELPER_SRC) $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@
endif

$(OBJ_DIR)/tests/%.o: tests/%.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define test-executable-rule
TEST_OBJ_$1 := $(OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(APP_DIR)/$2$(EXE_EXTENSION): $$(TEST_OBJ_$1) $(TEST_HELPER_OBJ) \
		$(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) -o $$@ $$(TEST_OBJ_$1) $(TEST_HELPER_OBJ) $(LDFLAGS) $(TEST_LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

$(APP_DIR)/examples/%$(EXE_EXTENSION): examples/%.c $(APP_DIR)/$(STATIC_TARGET) \
		$(FLAGS_STAMP) | $(APP_DIR)/$(TARGET) $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS)

.PHONY: clean cloc docs docs-pdf examples coverage check-symbols check-stamps check-aliasing
.PHONY: check-labels check-edges check-gates check-planted bench test-tsan
.PHONY: all install test test-quiet test-asan test-valgrind test-valgrind-quiet test-watch uninstall watch
.PHONY: all-debug install-debug test-debug test-valgrind-debug test-watch-debug uninstall-debug watch-debug

watch: ## Watch sources and rebuild
	@while true; do \
		make --no-print-directory all; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile; \
		done

test-watch: ## Watch sources and rerun the tests
	@while true; do \
		make --no-print-directory test; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile; \
		done

TEST_LD_PATH := $(APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)

# Builds each example and runs it: an example that is only built can rot into
# something that compiles and does nothing it claims. It is a test gate for
# the same reason (a failing example fails `make test`).
examples: $(APP_DIR)/$(TARGET) $(EXAMPLES) ## Build the examples and run each
	@ran=0; skipped=0; for e in $(EXAMPLES); do \
		printf '\n### Example %s ###\n\n' "$$(basename $$e $(EXE_EXTENSION))"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$e; rc=$$?; \
		if [ $$rc -eq 77 ]; then skipped=$$((skipped + 1)); \
		elif [ $$rc -ne 0 ]; then exit 1; \
		else ran=$$((ran + 1)); fi; \
	done; \
	printf '\nexamples: %s ran, %s skipped (exit status 77: no native backend on this target)\n' "$$ran" "$$skipped"

# clang accepts -Wstrict-aliasing and implements nothing, so under clang the
# probe can never be reported and the gate would fail for a reason that says
# nothing about the code. It is skipped there, by name, and only there: the gcc
# job is the instrument (CI runs both compilers), and gcc without the warning
# armed still fails below.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
check-aliasing: ## Skipped under clang, which does not implement -Wstrict-aliasing
	@printf 'check-aliasing: skipped under clang, which does not implement -Wstrict-aliasing; the gcc build is the gate\n'
else
check-aliasing: ## Fail if -Wstrict-aliasing is not armed at level 1
	@mkdir -p $(BUILD_DIR)
	@printf 'int grjit_alias_probe(float * f);\nint grjit_alias_probe(float * f) { int * i = (int *)f; *i = 7; return *i; }\n' > $(BUILD_DIR)/alias_probe.c
	@printf 'int grjit_alias_clean(int * i);\nint grjit_alias_clean(int * i) { *i = 7; return *i; }\n' > $(BUILD_DIR)/alias_clean.c
	@probe=$$($(CC) $(CFLAGS) -Wno-error -c $(BUILD_DIR)/alias_probe.c -o $(BUILD_DIR)/alias_probe.o 2>&1); \
	ctl=$$($(CC) $(CFLAGS) -Wno-error -c $(BUILD_DIR)/alias_clean.c -o $(BUILD_DIR)/alias_clean.o 2>&1); ctlrc=$$?; \
	if [ $$ctlrc -ne 0 ]; then \
		printf 'check-aliasing: the control file did not compile, so this gate is measuring nothing:\n%s\n' "$$ctl" >&2; \
		exit 1; \
	fi; \
	if printf '%s' "$$ctl" | grep -q 'strict-aliasing'; then \
		printf 'check-aliasing: the control file drew a strict-aliasing diagnostic, so the probe proves nothing:\n%s\n' "$$ctl" >&2; \
		exit 1; \
	fi; \
	if printf '%s' "$$probe" | grep -q 'strict-aliasing'; then \
		printf 'check-aliasing: the planted violation is reported\n'; \
		exit 0; \
	fi; \
	qout=$$($(CC) -Q --help=warnings $(CFLAGS) 2>/dev/null); qrc=$$?; \
	level=$$(printf '%s' "$$qout" | awk '/-Wstrict-aliasing=</{print $$2}'); \
	if [ $$qrc -ne 0 ] || [ -z "$$level" ]; then \
		printf 'check-aliasing: no diagnostic, and %s reports no -Wstrict-aliasing level. That is a compiler which accepts the option and implements nothing.\n' "$(CC)" >&2; \
	elif [ "$$level" = 1 ]; then \
		printf 'check-aliasing: level 1 is set and the planted store still drew no diagnostic.\n' >&2; \
	else \
		printf 'check-aliasing: CFLAGS resolves to -Wstrict-aliasing=%s; only level 1 reports this probe.\n' "$$level" >&2; \
	fi; \
	exit 1
endif

check-stamps: ## Fail if a compile rule names no flags stamp, or a stamp omits a variable
	@python3 tools/check-stamps.py

####################################################################
# Layering gates (AD-2, AD-3, AD-14)
#
# Each script takes a root directory, so check-gates can run the real script
# against tests/gates fixtures: a planted defect that must fail and a control
# that must pass. A gate that has never been seen to fail is a gate that may
# measure nothing, and each one also fails on an empty population.
####################################################################

check-labels: ## Fail if a public header has no (or the wrong) stable/free label
	@tools/check-labels.sh .

# After the shared library is built, because the link line it reads is that
# file's NEEDED list: the manifest and the #include lines can both be clean
# while the .so links something forbidden.
check-edges: $(APP_DIR)/$(TARGET) ## Fail on a forbidden #include or NEEDED edge (AD-2)
	@tools/check-edges.sh --includes .
	@tools/check-edges.sh --links $(APP_DIR)

check-gates: ## Prove each gate fails on its planted defect and passes its control
	@CC="$(CC)" tools/check-gates.sh

####################################################################
# Benchmarks (AD-26)
####################################################################

BENCH_SOURCES := $(shell find bench -type f -name '*.c' 2>/dev/null | sort)
BENCH_EXECUTABLES := $(patsubst bench/%.c,$(APP_DIR)/bench/%$(EXE_EXTENSION),$(BENCH_SOURCES))
-include $(patsubst bench/%.c,$(APP_DIR)/bench/%.d,$(BENCH_SOURCES))

$(APP_DIR)/bench/%$(EXE_EXTENSION): bench/%.c $(APP_DIR)/$(STATIC_TARGET) \
		$(FLAGS_STAMP) | $(APP_DIR)/$(TARGET) $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -MMD -MP -MF $(@D)/$(*F).d -o $@ $< $(LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS)

bench: $(BENCH_EXECUTABLES) ## Run the benchmark harness (prints a calibration result first)
ifeq ($(strip $(BENCH_EXECUTABLES)),)
	@printf 'bench: no benchmark sources under bench/, so this measures nothing\n' >&2; exit 1
endif
	@for b in $(BENCH_EXECUTABLES); do \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$b; rc=$$?; \
		if [ $$rc -ne 0 ] && [ $$rc -ne 77 ]; then exit 1; fi; \
	done

####################################################################
# Symbols
####################################################################

check-symbols: $(APP_DIR)/$(TARGET) ## Fail if any exported symbol lacks the version namespace
ifeq ($(OS_NAME), Linux)
	@leaked=$$(nm -D --defined-only $(APP_DIR)/$(TARGET) \
		| awk '$$2 ~ /^[TDBR]$$/ {print $$3}' \
		| grep -v '^$(LIBVER_SYMBOL)_' | grep -v '^_' || true); \
	if [ -n "$$leaked" ]; then \
		printf '### Exported symbols missing the $(LIBVER_SYMBOL)_ namespace ###\n%s\n' "$$leaked" >&2; \
		exit 1; \
	fi
	@unexported=$$(find include -name '*.h' -exec awk '/^#if DOXYGEN/{d=1} d==0 && !/GRJIT_API/ && /^[A-Za-z_][A-Za-z0-9_ ]*\**[[:space:]]*grjit_[a-z0-9_]+[[:space:]]*\(/{print FILENAME": "$$0} /^#endif/{d=0}' {} + \
		| grep -vE 'typedef|static inline' || true); \
	if [ -n "$$unexported" ]; then \
		printf '### Public declarations without GRJIT_API ###\n%s\n' "$$unexported" >&2; \
		exit 1; \
	fi
	@missing=$$(grep -h 'GRJIT_API' include/ghoti.io/runtime-jit/*.h \
		| grep -oE 'grjit_[a-z0-9_]+\(' | tr -d '(' | sort -u \
		| while read -r f; do \
			nm -D --defined-only $(APP_DIR)/$(TARGET) | awk '{print $$3}' \
				| grep -qx "$(LIBVER_SYMBOL)_$$f" || echo "$$f"; \
		done); \
	if [ -n "$$missing" ]; then \
		printf '### Declared GRJIT_API functions the shared library does not export ###\n%s\n' "$$missing" >&2; \
		printf '(a definition whose translation unit never saw its declaration is hidden by -fvisibility=hidden, and the tests link the archive, so they cannot see it)\n' >&2; \
		exit 1; \
	fi
	@split=$$(nm -D --undefined-only $(APP_DIR)/$(TARGET) \
		| awk '{print $$2}' | grep '^$(LIBVER_SYMBOL)_' || true); \
	if [ -n "$$split" ]; then \
		printf '### Renamed but undefined - a split symbol ###\n%s\n' "$$split" >&2; \
		exit 1; \
	fi
	@nomacros=$$(find include src -name '*.h' \
		! -name 'libver.h' ! -name 'libver_gen.h' ! -name 'namespace.h' ! -name 'macros.h' \
		-exec grep -L '#include <ghoti.io/runtime-jit/macros.h>' {} + || true); \
	if [ -n "$$nomacros" ]; then \
		printf '### Headers that do not include macros.h ###\n%s\n' "$$nomacros" >&2; \
		exit 1; \
	fi
	@badguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| awk '$$1 !~ /^GHOTI_IO_GRJIT_/ {print $$1}' || true); \
	if [ -n "$$badguards" ]; then \
		printf '### Include guards with the wrong prefix ###\n%s\n' "$$badguards" >&2; \
		exit 1; \
	fi
	@dupguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| sort | uniq -d || true); \
	if [ -n "$$dupguards" ]; then \
		printf '### Headers sharing an include guard ###\n%s\n' "$$dupguards" >&2; \
		exit 1; \
	fi
	@printf 'Every exported symbol carries the %s_ namespace.\n' "$(LIBVER_SYMBOL)"
else
	@printf 'check-symbols: skipped (Linux only)\n'
endif

test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(BENCH_EXECUTABLES) $(TEST_GATES) ## Build and run the tests
	@for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf '\n### Running %s ###\n\n' "$$test_name"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1 || exit 1; \
	done
	@if [ -z "$(strip $(BENCH_EXECUTABLES))" ]; then \
		printf 'test: no benchmark harness under bench/ (AD-26 requires one)\n' >&2; exit 1; \
	fi
	@for b in $(BENCH_EXECUTABLES); do \
		printf '\n### Benchmark smoke %s ###\n\n' "$$(basename $$b $(EXE_EXTENSION))"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$b --smoke; rc=$$?; \
		if [ $$rc -ne 0 ] && [ $$rc -ne 77 ]; then exit 1; fi; \
	done

test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Run tests, one line per suite
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; any_failed=0; \
	printf '\n%-30s %8s %10s %s\n' "Test Suite" "Tests" "Time" "Status"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf '%-30s %8d %8dms PASS\n' "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			any_failed=1; \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			[ "$$failures" -eq 0 ] && failures=1; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf '%-30s %8d %8dms FAIL (exit %d)\n' "$$test_name" "$$num_tests" "$$time_ms" "$$exit_code"; \
			failed_suites="$$failed_suites\n=== $$test_name FAILURES ===\n$$output\n"; \
		fi; \
	done; \
	if [ $$any_failed -eq 0 ]; then \
		printf '%-30s %8d %6dms PASS\n\n' "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf '%-30s %8d %6dms FAIL (%d failed)\n' "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf '%s\n' "$$failed_suites"; \
		exit 1; \
	fi

test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Run the tests under Valgrind
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TEST_EXECUTABLES); do \
		printf '\n### Valgrind %s ###\n\n' "$$(basename $$test_exe)"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 || exit 1; \
	done
else
	@printf 'Valgrind is only available on Linux\n' >&2; exit 1
endif

test-valgrind-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Valgrind, one line per suite
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_failed=0; total_time=0; failed_suites=""; \
	printf '\n%-30s %8s %10s %s\n' "Test Suite (Valgrind)" "Tests" "Time" "Status"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		has_leak=$$(echo "$$output" | grep -c "are definitely lost\|are indirectly lost\|are possibly lost" || true); \
		if [ $$exit_code -eq 0 ] && [ $$has_leak -eq 0 ]; then \
			printf '%-30s %8d %8dms PASS\n' "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			total_failed=$$((total_failed + 1)); \
			printf '%-30s %8d %8dms FAIL\n' "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n=== $$test_name ===\n$$output\n"; \
		fi; \
	done; \
	if [ $$total_failed -eq 0 ]; then \
		printf '%-30s %8d %6dms PASS\n\n' "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf '%-30s %8d %6dms FAIL (%d suites)\n' "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf '%s\n' "$$failed_suites"; \
		exit 1; \
	fi
else
	@printf 'Valgrind is only available on Linux\n' >&2; exit 1
endif

####################################################################
# ASan + UBSan, in their own tree
####################################################################

UBSAN_CHECKS := undefined,float-cast-overflow
ASAN_UBSAN_FLAGS := -fsanitize=address,$(UBSAN_CHECKS) -fno-sanitize-recover=$(UBSAN_CHECKS) -fno-omit-frame-pointer -g -O1
# clang's `undefined` group includes `enum`, which GCC's does not: it reports a
# load of an enum value outside the enumerators. The tests pass such values on
# purpose (static_cast<Kind>(99)) to prove the C API refuses an unknown kind,
# which is undefined in C++ but is exactly the input under test, so the check
# is switched off under clang and the compilers sanitize the same set.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
ASAN_UBSAN_FLAGS += -fno-sanitize=enum
endif
COV_BUILD_DIR := ./build/$(BUILD)-cov
ASAN_BUILD_DIR := ./build/$(BUILD)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.flags
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps
ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES))
ASAN_DEPFILES := $(ASAN_LIBOBJECTS:.o=.d) \
    $(foreach pair,$(TEST_PAIRS),$(ASAN_OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
-include $(ASAN_DEPFILES)
ASAN_ARCHIVE := $(ASAN_APP_DIR)/$(STATIC_TARGET)
ASAN_CORELIBRARY := -Wl,--whole-archive $(ASAN_ARCHIVE) -Wl,--no-whole-archive $(RTCORE_LIBS) $(CUTIL_LIBS)
ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGRJIT_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

$(ASAN_OBJ_DIR)/%.o: src/%.c $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_ARCHIVE): $(ASAN_LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(ASAN_OBJ_DIR)/tests/%.o: tests/%.cpp $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define asan-test-executable-rule
ASAN_TEST_OBJ_$1 := $(ASAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(ASAN_APP_DIR)/$2$(EXE_EXTENSION): $$(ASAN_TEST_OBJ_$1) $(ASAN_ARCHIVE)
	@mkdir -p $$(@D)
	$(CXX) $(ASAN_CXXFLAGS) -o $$@ $$(ASAN_TEST_OBJ_$1) $(ASAN_LDFLAGS) $(ASAN_CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call asan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

ASAN_TEST_EXECUTABLES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))
ASAN_RUNTIME := $(shell $(CC) -print-file-name=libasan.so 2>/dev/null)
# How the sanitized test programs are started. GCC's runtime is a shared
# libasan.so, which has to be preloaded because the libraries under test are
# loaded by a program that was not linked against it. clang links its own
# runtime statically into every program it builds with -fsanitize=address, so
# there is nothing to preload - and preloading libasan.so beside it, or
# anything else (this workstation's desktop sets LD_PRELOAD), makes the runtime
# abort with "ASan runtime does not come first". So with clang the preload is
# emptied instead of set.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
ASAN_PRELOAD = LD_PRELOAD=
else
ASAN_PRELOAD = LD_PRELOAD="$(ASAN_RUNTIME)$${LD_PRELOAD:+:$$LD_PRELOAD}"
endif

test-asan: $(ASAN_TEST_EXECUTABLES) ## Build with ASan+UBSan and run the tests
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		printf '\n### ASan+UBSan %s ###\n\n' "$$(basename $$test_exe)"; \
		$(ASAN_PRELOAD) \
		LD_LIBRARY_PATH="$(ASAN_APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
			$$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf '\nASan+UBSan suite clean.\n'

####################################################################
# ThreadSanitizer, in its own tree
####################################################################

# Not combinable with ASan, so its own tree. -O1 keeps the instrumented run
# fast enough to be worth running.
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g -O1
TSAN_BUILD_DIR := ./build/$(BUILD)-tsan
TSAN_OBJ_DIR := $(TSAN_BUILD_DIR)/objects
TSAN_FLAGS_STAMP := $(TSAN_OBJ_DIR)/.flags
TSAN_APP_DIR := $(TSAN_BUILD_DIR)/apps
TSAN_LIBOBJECTS := $(patsubst src/%.c,$(TSAN_OBJ_DIR)/%.o,$(SOURCES))
TSAN_DEPFILES := $(TSAN_LIBOBJECTS:.o=.d) \
    $(foreach pair,$(TEST_PAIRS),$(TSAN_OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
-include $(TSAN_DEPFILES)
TSAN_ARCHIVE := $(TSAN_APP_DIR)/$(STATIC_TARGET)
TSAN_CORELIBRARY := -Wl,--whole-archive $(TSAN_ARCHIVE) -Wl,--no-whole-archive $(RTCORE_LIBS) $(CUTIL_LIBS)
TSAN_CFLAGS := $(CFLAGS) $(TSAN_FLAGS) -DGRJIT_BUILD
TSAN_CXXFLAGS := $(CXXFLAGS) $(TSAN_FLAGS)
TSAN_LDFLAGS := $(LDFLAGS) $(TSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	TSAN_CFLAGS += -fPIC
endif

$(TSAN_OBJ_DIR)/%.o: src/%.c $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_ARCHIVE): $(TSAN_LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(TSAN_OBJ_DIR)/tests/%.o: tests/%.cpp $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define tsan-test-executable-rule
TSAN_TEST_OBJ_$1 := $(TSAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(TSAN_APP_DIR)/$2$(EXE_EXTENSION): $$(TSAN_TEST_OBJ_$1) $(TSAN_ARCHIVE)
	@mkdir -p $$(@D)
	$(CXX) $(TSAN_CXXFLAGS) -o $$@ $$(TSAN_TEST_OBJ_$1) $(TSAN_LDFLAGS) $(TSAN_CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call tsan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

TSAN_TEST_EXECUTABLES := $(addprefix $(TSAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# No LD_PRELOAD, and the environment's is emptied: -fsanitize=thread at link
# time already puts libtsan first in the executable's NEEDED list, and a
# preloaded runtime is inherited by every process a test spawns.
# halt_on_error stops at the first race rather than repeating it per test.
TSAN_RUN_ENV := LD_PRELOAD= LD_LIBRARY_PATH="$(TSAN_APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
	TSAN_OPTIONS="halt_on_error=1:history_size=7:second_deadlock_stack=1"

test-tsan: $(TSAN_TEST_EXECUTABLES) ## Build with TSan and run the tests (Linux only)
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TSAN_TEST_EXECUTABLES); do \
		printf '\n### TSan %s ###\n\n' "$$(basename $$test_exe)"; \
		$(TSAN_RUN_ENV) $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf '\nTSan suite clean.\n'
else
	@printf 'ThreadSanitizer builds are only supported on Linux\n' >&2; exit 1
endif

####################################################################
# Planted defects (AD-16): a verification that has never been seen to fail
# may measure nothing
####################################################################

# Each defect is compiled into the library objects of a tree of its own, by
# the macro GRJIT_TEST_PLANT_BUG (1: SHR and SAR swapped, 4: the same in the
# arm64 emitter, 2: every stack-map slot offset off by 8, 3: one live REF left
# out of a stack map, and the Windows x86-64 flavour's three: 5: a callee-saved
# register (rsi) used for the parameter loads, 6: no outgoing area, so a
# callee's shadow space and the stack arguments land on the frame's slots, 7:
# the unwind table written and never registered), and the test
# that is meant to catch it is built and run there. The test must FAIL, and
# say what it saw; the same test in the ordinary tree passes (it is part of
# `make test`). The ordinary tree never sees the macro.
PLANT_DEFECTS := 1:testDifferential 2:testReadback 3:testReadback \
	4:testDifferential:Differential.GeneratedFunctionsRunTheSameWhenArm64CodeIsSimulatedAndEvaluated \
	5:testWin64:Win64Registers.* 6:testWin64:Win64Shape.* 7:testWin64:Win64Memory.*

# Defects 5 to 7 are in the Windows x86-64 flavour of the emitter, which is
# emitted on every host, so testWin64's structural tests (what is encoded, the
# bytes of the prologue, where the unwind table is written and when it is
# registered) catch them here without running anything; the catch by running
# the code, a callee-saved register changed, a live slot overwritten by a
# callee's shadow space, a stack walk that finds no frame, is the Windows
# build's, under wine (tools/xwin/m1-controls.sh in the workspace).
#
# The tests that catch defects 1 to 4 run generated code, and the library plants
# them by building a tree of its own with make, a shell script and LD_LIBRARY_PATH,
# none of which a Windows build has: it says so and stops, by name, as
# check-symbols does, and the Windows catch of 5 to 7 is the workspace's.
ifeq ($(OS_NAME), Linux)
check-planted: $(APP_DIR)/$(STATIC_TARGET) ## Prove the differential and the read-back fail on a planted backend defect
	@tools/check-planted.sh "$(MAKE)" "$(PLANT_DEFECTS)" "$(LIB_INSTALL_PATH)/$(SUITE)" "$(APP_DIR)" "$(BUILD_DIR)"
else
check-planted: ## Skipped off Linux: the planted trees are built by a shell script
	@printf 'check-planted: skipped on this target (it builds one tree per defect with make and sh and runs the tests with LD_LIBRARY_PATH; the backend is here, and the Windows catches of defects 5 to 7 are run by tools/xwin/m1-controls.sh in the workspace)\n'
endif

####################################################################
# Install
####################################################################

LDCONF_INSTALL_PATH ?= /etc/ld.so.conf.d
PC_REQUIRES := $(RTCORE_PC) $(CUTIL_PC)
PKGCONFIG_INSTALL_PATH ?= $(PC_INSTALL_PATH)

install: all ## Install the library
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then mkdir -p $(LDCONF_INSTALL_PATH); fi
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then echo "$(LIB_INSTALL_PATH)/$(SUITE)" > $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
	@mkdir -p $(BIN_INSTALL_PATH) $(LIB_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)/$(SUITE)/
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)/
endif
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -d $(GEN_DIR)/ghoti.io ]; then \
		cp -r $(GEN_DIR)/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@mkdir -p $(PKGCONFIG_INSTALL_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(PC_LIB_DIR)|$(PC_LIB_DIR)|g; s|(PC_INCLUDE_DIR)|$(PC_INCLUDE_DIR)|g; s|(REQUIRES)|$(PC_REQUIRES)|g' > $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the installed files
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then rm -f $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@rm -f $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build in DEBUG mode
	make all BUILD=debug

install-debug: ## Install the DEBUG library
	make install BUILD=debug

uninstall-debug: ## Uninstall the DEBUG library
	make uninstall BUILD=debug

test-debug: ## Run the tests in DEBUG mode
	make test BUILD=debug

test-valgrind-debug: ## Valgrind in DEBUG mode
	make test-valgrind BUILD=debug

watch-debug: ## Watch and rebuild in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch and test in DEBUG mode
	make test-watch BUILD=debug

docs: ## Generate documentation under docs/
	doxygen

docs-pdf: docs ## Generate the PDF manual
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include tests Makefile

coverage: ## Build instrumented, run the tests, and report line coverage
	@rm -rf $(COV_BUILD_DIR)/objects/*.gcda \
		$(COV_BUILD_DIR)/objects/*/*.gcda 2> /dev/null || true
	@status=0; \
	$(MAKE) --no-print-directory test TEST_GATES= \
		BUILD_DIR=$(COV_BUILD_DIR) \
		EXTRA_CFLAGS="--coverage -O0 -fprofile-update=atomic" \
		EXTRA_LDFLAGS="--coverage" > /dev/null || status=$$?; \
	if [ $$status -eq 0 ]; then \
		tools/coverage.sh $(COV_BUILD_DIR)/objects || status=$$?; \
	else \
		printf 'coverage: the instrumented test run failed; no report\n' >&2; \
	fi; \
	exit $$status

clean: ## Remove the build directories
	-@rm -rf ./build

help: ## Display this help
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' Makefile | sort | sed 's/\([^:]*\):.*## \(.*\)/\1:\2/' | awk -F: '{printf "%-22s %s\n", $$1, $$2}'

####################################################################
# Flag stamps. At the end so they are not the default goal, and so the
# directories they name have already been assigned.
####################################################################

.PHONY: force-flags

$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(LIB_CFLAGS) $(CFLAGS) $(CXXFLAGS) $(LDFLAGS) $(INCLUDE) $(CORELIBRARY) $(RTCORE_LIBS) $(CUTIL_LIBS) $(TESTFLAGS) $(TEST_LDFLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(TSAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(TSAN_CFLAGS) $(TSAN_CXXFLAGS) $(TSAN_LDFLAGS) $(INCLUDE) $(TSAN_CORELIBRARY) $(RTCORE_LIBS) $(CUTIL_LIBS) $(TESTFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(ASAN_CFLAGS) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(INCLUDE) $(ASAN_CORELIBRARY) $(RTCORE_LIBS) $(CUTIL_LIBS) $(TESTFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@
