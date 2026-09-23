# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Native (Linux) build: libtickle.a + examples + unit tests + lint. Lives here rather than the
# repo root so Linux is treated as one platform among others, exactly like
# platform/freertos/Makefile - see the repo root Makefile's own comment (it's now just a thin
# forwarding shim to this one). Unlike platform/freertos/Makefile this still just needs a plain
# host C compiler - no separate toolchain, no bare-metal linker script.

ROOT = ../..

INCLUDE = $(ROOT)/include
SRC = $(ROOT)/src

# Toolchain: plain `?=` so these can be overridden from the environment or command line,
# e.g. `make CC=clang` or `CC=clang make`.
CC ?= gcc
AR ?= ar

# BUILD_TYPE picks the default optimization/debug-info level and keeps each mode's objects
# in their own obj/<type>/ tree, so switching between them doesn't need a `make clean` in
# between (each mode's own object cache just sits there until you ask for the other one).
BUILD_TYPE ?= debug
ifeq ($(BUILD_TYPE),debug)
    CFLAGS ?= -O0 -g -Wall -Wextra
else ifeq ($(BUILD_TYPE),release)
    CFLAGS ?= -O2 -DNDEBUG -Wall -Wextra
else
    $(error Unknown BUILD_TYPE '$(BUILD_TYPE)': expected 'debug' or 'release')
endif
OBJ = obj/$(BUILD_TYPE)

LDFLAGS ?=
LDLIBS ?=

# CPPFLAGS/LDFLAGS/LDLIBS additions below use `override` so the project's own required
# flags (include paths, library search path, the library itself) always survive even if
# the user passes their own CPPFLAGS/LDFLAGS/LDLIBS on the command line.
override CPPFLAGS += -I$(INCLUDE) -I$(SRC)
override LDFLAGS += -L.
override LDLIBS += -ltickle -lm

# Auto-generate per-object .d dependency files so editing a header triggers a rebuild of
# everything that includes it, instead of silently reusing stale .o files.
DEPFLAGS = -MMD -MP

# Platform detection (still overridable: `make PLATFORM=linux`). There is no native build for any
# host besides Linux - the only other supported platform (FreeRTOS) is cross-built separately via
# platform/freertos/Makefile, never through this one.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    PLATFORM ?= linux
else
    $(error Unsupported host platform '$(UNAME_S)' - this Makefile only builds natively for Linux; see platform/freertos/Makefile for the FreeRTOS cross-build)
endif

# HAL source file based on platform
HAL_SRC = $(SRC)/hal_$(PLATFORM).c

# Automatically find all .c files in src directory, excluding hal_*.c files
SRC_FILES = $(filter-out $(SRC)/hal_%.c, $(wildcard $(SRC)/*.c))
# Add platform-specific HAL file
SRC_FILES += $(HAL_SRC)

# REPO_ROOT-relative object paths (via $(abspath) + stripping that prefix), not a plain
# `$(OBJ)/$(src).o` pattern: SRC/EXAMPLE_BINS/COMMON_SRCS/TEST_DIR below all reach outside this
# directory with a leading `$(ROOT)/` (== `../../`), and naively prepending $(OBJ)/ to those lets
# the `..` components normalize straight back out of $(OBJ)/ entirely (e.g.
# `obj/../../src/tickle.c.o` resolves to a path *outside* obj/) - silently escaping `make clean`
# and letting stale .o files survive a supposedly-clean rebuild. Same fix as
# platform/freertos/Makefile's own REPO_ROOT/OBJS, applied here via the obj_for/dir_for helpers.
REPO_ROOT = $(abspath $(ROOT))
obj_for = $(patsubst $(REPO_ROOT)/%.c,$(OBJ)/%.o,$(abspath $(1)))
dir_for = $(patsubst $(REPO_ROOT)/%,$(OBJ)/%,$(abspath $(1)))

OBJS = $(call obj_for,$(SRC_FILES))

# Register each example binary as "<binary-name>:<its driver directory>:<its generated-codec
# directory>". Every .c file in EITHER directory (the driver's own, and its protocol's generated
# codec - e.g. examples/set_bool/SetBool.c, flattened out of the driver's own directory per
# tools/typesupport/PLAN.md's M5 milestone, and shared verbatim by examples/freertos/'s drivers
# for the same protocol too, via their own ROLE_INCLUDES/ROLE_SRCS in platform/freertos/Makefile)
# is treated as a shared source compiled into that binary too. Adding a new example binary is
# then a one-line addition here instead of a hand-written target + object list.
EXAMPLE_BINS := client:$(ROOT)/examples/linux/set_bool:$(ROOT)/examples/set_bool \
                server:$(ROOT)/examples/linux/set_bool:$(ROOT)/examples/set_bool \
                publisher:$(ROOT)/examples/linux/uint64:$(ROOT)/examples/uint64 \
                subscriber:$(ROOT)/examples/linux/uint64:$(ROOT)/examples/uint64 \
                ping:$(ROOT)/examples/linux/ping_pong:$(ROOT)/examples/ping_pong \
                pong:$(ROOT)/examples/linux/ping_pong:$(ROOT)/examples/ping_pong \
                perf_client:$(ROOT)/examples/linux/perf:$(ROOT)/examples/perf \
                perf_server:$(ROOT)/examples/linux/perf:$(ROOT)/examples/perf

# Every example binary links this too (see examples/linux/common/cli_opts.h) - unlike EXAMPLE_BINS'
# per-directory _SHARED files below, it lives outside every example's own directory, so it's
# added to each one's object list explicitly instead of being picked up by that directory's
# wildcard.
COMMON_SRCS = $(ROOT)/examples/linux/common/cli_opts.c
COMMON_OBJS = $(call obj_for,$(COMMON_SRCS))

# A driver #includes its protocol's generated header by bare name (e.g. "SetBool.h") without
# its own -I - historically because that header sat right alongside it, and unchanged now that
# flattening (above) moved it out: every driver's own directory search still finds nothing there
# now, so the compiler falls through to CPPFLAGS' own -I list, which needs each codec directory
# added once here (harmless as an unused -I for any driver that doesn't need a given one).
override CPPFLAGS += $(patsubst %,-I%,$(sort $(foreach bin,$(EXAMPLE_BINS),$(word 3,$(subst :, ,$(bin))))))

# Directories the object rule below needs to exist first, derived from EXAMPLE_BINS so a
# newly-registered example directory doesn't also need a manual entry here. Order-only
# prerequisites (see `|` below) so make doesn't try to relink everything just because a
# sibling .o's mkdir touched the directory's mtime, and so -j doesn't race multiple
# `mkdir -p` calls against a per-file rule.
OBJ_DIRS = $(call dir_for,$(SRC)) $(call dir_for,$(ROOT)/examples/linux/common) \
           $(sort $(call dir_for,$(foreach bin,$(EXAMPLE_BINS),$(word 2,$(subst :, ,$(bin))) $(word 3,$(subst :, ,$(bin))))))

# Unit tests: each tests/test_*.c #includes ../src/tickle.c directly (whitebox, to reach its
# static functions) and provides its own mock HAL (tests/test_mock.h), so it's linked against
# encoding.c/log.c only - never hal_linux.c or libtickle.a, and never runs real network I/O.
TEST_DIR = $(ROOT)/tests
TEST_SRCS = $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(OBJ)/tests/%,$(TEST_SRCS))

.PHONY: all library examples set_bool uint64 ping_pong perf test lint clean fuzz sanitize

all:
	$(MAKE) library
	$(MAKE) examples

library: libtickle.a

examples: set_bool uint64 ping_pong perf

set_bool: client server

uint64: publisher subscriber

ping_pong: ping pong

perf: perf_client perf_server

# Generic rule: mirrors every source file's REPO_ROOT-relative path under $(OBJ)/, so
# ../../src/tickle.c becomes obj/<type>/src/tickle.o and
# ../../examples/set_bool/SetBool.c becomes obj/<type>/examples/set_bool/SetBool.o.
$(OBJ)/%.o: $(REPO_ROOT)/%.c | $(OBJ_DIRS)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIRS):
	mkdir -p $@

libtickle.a: $(OBJS)
	$(AR) crv $@ $^

# Generates one target per EXAMPLE_BINS entry: <name>_MAIN is that binary's own source,
# <name>_SHARED is every other .c file living alongside it in the same directory (minus any
# OTHER registered binary's main file - two binaries can share a directory, e.g. client.c
# and server.c both sit in examples/linux/set_bool/), and the link recipe compiles+links exactly
# those two sets against the library.
EXAMPLE_MAIN_FILES := $(foreach bin,$(EXAMPLE_BINS),$(word 2,$(subst :, ,$(bin)))/$(word 1,$(subst :, ,$(bin))).c)

ALL_EXAMPLE_OBJS :=
define EXAMPLE_RULE
$(1)_MAIN := $(2)/$(1).c
$(1)_SHARED := $$(filter-out $(EXAMPLE_MAIN_FILES),$$(wildcard $(2)/*.c) $$(wildcard $(3)/*.c))
$(1)_OBJS := $$(call obj_for,$$($(1)_MAIN) $$($(1)_SHARED)) $(COMMON_OBJS)
ALL_EXAMPLE_OBJS += $$($(1)_OBJS)

$(1): $$($(1)_OBJS) libtickle.a
	$$(CC) $$(CFLAGS) $$(LDFLAGS) -o $$@ $$($(1)_OBJS) $$(LDLIBS)
endef
$(foreach bin,$(EXAMPLE_BINS),$(eval $(call EXAMPLE_RULE,$(word 1,$(subst :, ,$(bin))),$(word 2,$(subst :, ,$(bin))),$(word 3,$(subst :, ,$(bin))))))

ALL_OBJS = $(OBJS) $(ALL_EXAMPLE_OBJS)

# Pull in the auto-generated per-object dependency files (headers each .o actually used),
# so changing a header rebuilds everything that includes it. Silently ignored on a clean tree.
-include $(ALL_OBJS:.o=.d)

test: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
	    echo "-- $$bin --"; \
	    ./$$bin || exit 1; \
	done

$(OBJ)/tests/%: $(TEST_DIR)/%.c $(SRC)/tickle.c $(SRC)/encoding.c $(SRC)/log.c | $(OBJ)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(SRC)/encoding.c $(SRC)/log.c -lm

$(OBJ)/tests:
	mkdir -p $@

# Coverage-guided fuzzing of the packet parser - the untrusted-input boundary. Needs clang.
# `make fuzz` builds and runs it; pass FUZZ_ARGS (e.g. '-max_total_time=60', or a corpus dir).
FUZZ_CC ?= clang
fuzz: $(OBJ)/fuzz/fuzz_process_packet
	./$< $(FUZZ_ARGS)

$(OBJ)/fuzz/fuzz_process_packet: $(TEST_DIR)/fuzz_process_packet.c $(SRC)/tickle.c $(SRC)/encoding.c $(SRC)/log.c \
                                 | $(OBJ)/fuzz
	$(FUZZ_CC) $(CPPFLAGS) -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	    -o $@ $< $(SRC)/encoding.c $(SRC)/log.c -lm

$(OBJ)/fuzz:
	mkdir -p $@

# Re-run the unit tests under AddressSanitizer + UndefinedBehaviorSanitizer. Catches
# use-after-free / out-of-bounds / UB the plain run misses (e.g. process_update()'s malloc path).
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer'

# Excludes, and why each needs its own lint pass instead (see platform/freertos/Makefile's own
# `lint` target) rather than just being swept in here with everything else:
#   - third_party/*: vendored submodules (FreeRTOS-Kernel, lwIP) aren't our code to format/lint.
#   - platform/*: cross-compiled for RISC-V, needs -DTT_PLATFORM_FREERTOS and lwIP/FreeRTOS
#     include paths this host build knows nothing about, and FreeRTOSConfig.h's macro names
#     (configUSE_PREEMPTION, ...) are FreeRTOS's own API contract, not ours to rename to fit our
#     naming-convention checks. (This directory - platform/linux/ - is excluded by the same
#     pattern too, but has no .[ch] files of its own to lint anyway.)
#   - examples/freertos/*: same reason as platform/* - these are cross-compiled FreeRTOS role
#     drivers that happen to live under examples/ (mirroring examples/linux/) rather than
#     platform/freertos/ itself.
#   - hal_freertos.*: same reason again - cross-compiled code that happens to live in include/src
#     rather than platform/ (mirroring hal_linux.* placement).
# Which files `lint` below covers: everything tracked in git, minus the trees that are genuinely
# out of scope for THIS platform's lint pass.
#
# Tracked-in-git rather than found-on-disk (2026-09-23) because "whatever happens to be in the
# working tree" varies per developer, and that is precisely how this target drifted into being
# unrunnable: a colcon build under build/ or rmw_tickle/build/ left CMake's own compiler-probe
# sources lying around, and the target dutifully linted them. Excluding each such directory as it
# turns up is whack-a-mole - the next generated tree reintroduces the problem. Nothing under either
# build tree is tracked, so selecting from the index fixes the whole class at once, and it matches
# what CI effectively checks: files that are in the repository.
#
# The exclusions that remain are scope decisions, not artifact filtering. platform/ and
# examples/freertos/ (and hal_freertos.*) are cross-compiled and have their own lint pass -
# `make -C platform/freertos lint`, see CONTRIBUTING.md - and clang-tidy cannot parse them without
# that target's own flags. third_party/ isn't ours.
LINT_EXCLUDE_RE = ^(third_party|platform|examples/freertos)/|hal_freertos\.

# clang-tidy additionally skips two trees that clang-format still covers. Neither compiles without
# include paths this Makefile cannot describe, so clang-tidy stops at 'file not found' before
# reaching anything real:
#
#   rmw_tickle/  - ROS 2 packages needing a sourced ROS environment. CI lints them from their own
#                  colcon-built compile database (check-all.yml has a separate step for exactly
#                  this). Locally:
#                      source /opt/ros/<distro>/setup.bash
#                      cd rmw_tickle && colcon build --packages-select rmw_tickle \
#                          --cmake-args -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#                      clang-tidy -p build/rmw_tickle rmw_tickle/src/*.c
#
#   the DDS comparison harnesses - vendor-API code written to follow each vendor's own official
#                  examples (examples/perf_hil/cyclonedds/common.h says so explicitly), needing that
#                  vendor's headers. CI never tidies them either: cpp-linter only checks files a
#                  push changed, and these change rarely.
#
# clang-format covers both, because that half IS gated the moment one of them is touched - which is
# how a formatting failure reached main on 2026-09-23.
LINT_TIDY_EXCLUDE_PATHS = rmw_tickle|examples/perf_hil/(cyclonedds|fastdds)

# tools/typesupport/tests/fixtures_ros2_adapter/'s hand-written rosidl_runtime_c stand-ins
# cross-include each other by ROS 2's own relative-path convention ("rosidl_runtime_c/string.h"),
# so they only resolve with their own directory on the include path. check-all.yml synthesises
# exactly this as a compile_commands.json entry for the same reason.
LINT_TIDY_ARGS = --extra-arg=-I$(REPO_ROOT)/tools/typesupport/tests/fixtures_ros2_adapter

# Which binaries this actually used, printed rather than assumed. The whole class of bug this
# target exists to catch is "I ran the check and it passed" - so a run that silently used a
# different clang-tidy than CI's, or found no clang-tidy at all and checked nothing, has to be
# visible in the output. CI pins version 19 (tsnlab/check -> cpp-linter-action); a developer box on
# a current distro has 21 or newer and the two disagree in both directions. To reproduce the gate
# exactly:
#
#     python3 -m venv /tmp/lintenv && /tmp/lintenv/bin/pip install clang-format==19.1.0 clang-tidy==19.1.0
#     make lint CLANG_FORMAT=/tmp/lintenv/bin/clang-format CLANG_TIDY=/tmp/lintenv/bin/clang-tidy
#
# --warnings-as-errors below, because CI's cpp-linter fails on any finding at all: without it a
# green `make lint` that printed warnings would still be a red CI, which is the exact gap this
# target exists to close. The tree is clean as of 2026-09-23, so anything new here is new.
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy

lint:
	@cf=$$(command -v $(CLANG_FORMAT) || true); ct=$$(command -v $(CLANG_TIDY) || true); \
	 if [ -z "$$cf" ] || [ -z "$$ct" ]; then \
	     echo "lint: cannot resolve $(CLANG_FORMAT) and/or $(CLANG_TIDY) - refusing to report a pass" >&2; \
	     exit 1; \
	 fi; \
	 echo "lint: $$cf ($$($(CLANG_FORMAT) --version))"; \
	 echo "lint: $$ct ($$($(CLANG_TIDY) --version | grep -i 'LLVM version' | tr -s ' '))"
	@set -e; \
	 tracked=$$(git -C $(REPO_ROOT) ls-files '*.c' '*.h' | grep -Ev '$(LINT_EXCLUDE_RE)' || true); \
	 if [ -z "$$tracked" ]; then echo "lint: no tracked sources matched - refusing to report a pass" >&2; exit 1; fi; \
	 existing=""; \
	 for f in $$tracked; do \
	     [ -f "$(REPO_ROOT)/$$f" ] && existing="$$existing $(REPO_ROOT)/$$f"; \
	 done; \
	 if [ -z "$$existing" ]; then echo "lint: every tracked source is missing from the tree" >&2; exit 1; fi; \
	 printf '%s\n' $$existing | xargs $(CLANG_FORMAT) --dry-run --Werror; \
	 printf '%s\n' $$existing | grep -Ev '$(REPO_ROOT)/($(LINT_TIDY_EXCLUDE_PATHS))' \
	     `# clang-tidy prints "Error while trying to load a compilation database" here and carries` \
	     `# on: there is no compile_commands.json for the core library, so it falls back to the` \
	     `# --extra-arg flags below, which is the intended arrangement. Pre-existing and not a` \
	     `# failure - the exit status is what says whether the check passed.` \
	     `# --quiet drops clang-tidy's own "Suppressed N warnings (N in non-user code)" summary. It` \
	     `# is genuinely misleading on a passing run: the figure is six digits, counts diagnostics` \
	     `# from system headers nobody can act on, and has already been read once as "the lint found` \
	     `# 135,000 problems". The per-file "N warnings generated" lines come from clang itself and` \
	     `# stay - filtering the tool's own stderr in a target whose job is to be trusted seemed a` \
	     `# worse trade than a few noisy lines. Verified --quiet still reports real findings.` \
	     | xargs $(CLANG_TIDY) --quiet --warnings-as-errors='*' \
	         $(patsubst -I%,--extra-arg=-I%,$(CPPFLAGS)) $(LINT_TIDY_ARGS)

include netns.mk

clean:
	# Removes every BUILD_TYPE's objects (obj/debug, obj/release, ...), not just the one
	# currently selected, so switching BUILD_TYPE and running `clean` doesn't leave the
	# other mode's stale cache behind.
	rm -rf obj/*/
	rm -f libtickle.a
	rm -f $(foreach bin,$(EXAMPLE_BINS),$(word 1,$(subst :, ,$(bin))))
