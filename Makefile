INCLUDE = include
SRC = src

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
OBJS = $(patsubst %.c,$(OBJ)/%.o,$(SRC_FILES))

# Register each example binary as "<binary-name>:<its directory>". Every other .c file in that
# same directory (its protocol's generated codec, e.g. SetBool.c/UInt64.c - shared verbatim by
# examples/freertos/'s drivers for the same protocol too, via their own ROLE_INCLUDES/ROLE_SRCS in
# platform/freertos/Makefile) is treated as a shared source compiled into that binary too. Adding
# a new example binary is then a one-line addition here instead of a hand-written target + object
# list.
EXAMPLE_BINS := client:examples/linux/set_bool server:examples/linux/set_bool \
                publisher:examples/linux/uint64 subscriber:examples/linux/uint64 \
                ping:examples/linux/ping_pong pong:examples/linux/ping_pong \
                perf_client:examples/linux/perf perf_server:examples/linux/perf

# Every example binary links this too (see examples/linux/common/cli_opts.h) - unlike EXAMPLE_BINS'
# per-directory _SHARED files below, it lives outside every example's own directory, so it's
# added to each one's object list explicitly instead of being picked up by that directory's
# wildcard.
COMMON_SRCS = examples/linux/common/cli_opts.c
COMMON_OBJS = $(patsubst %.c,$(OBJ)/%.o,$(COMMON_SRCS))

# Directories the object rule below needs to exist first, derived from EXAMPLE_BINS so a
# newly-registered example directory doesn't also need a manual entry here. Order-only
# prerequisites (see `|` below) so make doesn't try to relink everything just because a
# sibling .o's mkdir touched the directory's mtime, and so -j doesn't race multiple
# `mkdir -p` calls against a per-file rule.
OBJ_DIRS = $(OBJ)/src $(OBJ)/examples/linux/common \
           $(sort $(addprefix $(OBJ)/,$(foreach bin,$(EXAMPLE_BINS),$(word 2,$(subst :, ,$(bin))))))

# Unit tests: each tests/test_*.c #includes ../src/tickle.c directly (whitebox, to reach its
# static functions) and provides its own mock HAL (tests/test_mock.h), so it's linked against
# encoding.c/log.c only - never hal_linux.c or libtickle.a, and never runs real network I/O.
TEST_DIR = tests
TEST_SRCS = $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(OBJ)/$(TEST_DIR)/%,$(TEST_SRCS))

.PHONY: all library examples set_bool uint64 ping_pong perf test test-freertos test-all lint clean

all:
	$(MAKE) library
	$(MAKE) examples

library: libtickle.a

examples: set_bool uint64 ping_pong perf

set_bool: client server

uint64: publisher subscriber

ping_pong: ping pong

perf: perf_client perf_server

# Generic rule: mirrors every source file's path under $(OBJ)/, so src/tickle.c becomes
# obj/<type>/src/tickle.o and examples/linux/set_bool/SetBool.c becomes
# obj/<type>/examples/linux/set_bool/SetBool.o.
$(OBJ)/%.o: %.c | $(OBJ_DIRS)
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
$(1)_SHARED := $$(filter-out $(EXAMPLE_MAIN_FILES),$$(wildcard $(2)/*.c))
$(1)_OBJS := $$(patsubst %.c,$(OBJ)/%.o,$$($(1)_MAIN) $$($(1)_SHARED)) $(COMMON_OBJS)
ALL_EXAMPLE_OBJS += $$($(1)_OBJS)

$(1): $$($(1)_OBJS) libtickle.a
	$$(CC) $$(CFLAGS) $$(LDFLAGS) -o $$@ $$($(1)_OBJS) $$(LDLIBS)
endef
$(foreach bin,$(EXAMPLE_BINS),$(eval $(call EXAMPLE_RULE,$(word 1,$(subst :, ,$(bin))),$(word 2,$(subst :, ,$(bin))))))

ALL_OBJS = $(OBJS) $(ALL_EXAMPLE_OBJS)

# Pull in the auto-generated per-object dependency files (headers each .o actually used),
# so changing a header rebuilds everything that includes it. Silently ignored on a clean tree.
-include $(ALL_OBJS:.o=.d)

test: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
	    echo "-- $$bin --"; \
	    ./$$bin || exit 1; \
	done

# A real round trip under QEMU for the FreeRTOS platform - see platform/freertos/run_pair.sh.
# Needs the RISC-V toolchain (see platform/freertos/Makefile's own lint target for the exact
# packages). Named for the platform under test (matches examples/freertos/), not the emulator -
# see test-linux (netns.mk) for the same naming choice on the other platform.
test-freertos:
	platform/freertos/run_pair.sh

# Every test tier that's fully self-contained (no real hardware needed) in one target: unit
# tests (mock HAL), a real Linux-HAL round trip over network namespaces (test-linux, from
# netns.mk), and a real FreeRTOS-HAL round trip over emulated virtio-net (test-freertos). The two
# Raspberry Pi HIL performance test (.github/workflows/performance.yml) is deliberately not part
# of this - it needs the two real, exclusively-held Pis, so there's no "run it anywhere" version
# of it to add here.
test-all: test test-linux test-freertos

$(OBJ)/$(TEST_DIR)/%: $(TEST_DIR)/%.c $(SRC)/tickle.c $(SRC)/encoding.c $(SRC)/log.c | $(OBJ)/$(TEST_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(SRC)/encoding.c $(SRC)/log.c -lm

$(OBJ)/$(TEST_DIR):
	mkdir -p $@

# Excludes, and why each needs its own lint pass instead (see platform/freertos/Makefile's own
# `lint` target) rather than just being swept in here with everything else:
#   - third_party/*: vendored submodules (FreeRTOS-Kernel, lwIP) aren't our code to format/lint.
#   - platform/*: cross-compiled for RISC-V, needs -DTT_PLATFORM_FREERTOS and lwIP/FreeRTOS
#     include paths this host build knows nothing about, and FreeRTOSConfig.h's macro names
#     (configUSE_PREEMPTION, ...) are FreeRTOS's own API contract, not ours to rename to fit our
#     naming-convention checks.
#   - examples/freertos/*: same reason as platform/* - these are cross-compiled FreeRTOS role
#     drivers that happen to live under examples/ (mirroring examples/linux/) rather than
#     platform/freertos/ itself.
#   - hal_freertos.*: same reason again - cross-compiled code that happens to live in include/src
#     rather than platform/ (mirroring hal_linux.* placement).
LINT_EXCLUDES = -not -path './third_party/*' -not -path './platform/*' -not -path './examples/freertos/*' \
                -not -name 'hal_freertos.*'
lint:
	find . $(LINT_EXCLUDES) -name '*.[ch]' -exec clang-format --dry-run --Werror {} +
	find . $(LINT_EXCLUDES) -name '*.[ch]' -exec clang-tidy --extra-arg=-I$(INCLUDE) --extra-arg=-I$(SRC) {} +

include netns.mk

clean:
	# Removes every BUILD_TYPE's objects (obj/debug, obj/release, ...), not just the one
	# currently selected, so switching BUILD_TYPE and running `clean` doesn't leave the
	# other mode's stale cache behind.
	rm -rf obj/*/
	rm -f libtickle.a
	rm -f $(foreach bin,$(EXAMPLE_BINS),$(word 1,$(subst :, ,$(bin))))
