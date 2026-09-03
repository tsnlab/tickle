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

# Platform detection (still overridable: `make PLATFORM=generic` forces cross-building)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    PLATFORM ?= linux
else
    PLATFORM ?= generic
endif

# HAL source file based on platform
HAL_SRC = $(SRC)/hal_$(PLATFORM).c

# Automatically find all .c files in src directory, excluding hal_*.c files
SRC_FILES = $(filter-out $(SRC)/hal_%.c, $(wildcard $(SRC)/*.c))
# Add platform-specific HAL file
SRC_FILES += $(HAL_SRC)
OBJS = $(patsubst %.c,$(OBJ)/%.o,$(SRC_FILES))

# Register each example binary as "<binary-name>:<its directory>". Every other .c file in
# that same directory (e.g. the generated codec SetBool.c/UInt64.c) is treated as a shared
# source compiled into that binary too. Adding a new example binary is then a one-line
# addition here instead of a hand-written target + object list.
EXAMPLE_BINS := client:examples/set_bool server:examples/set_bool \
                publisher:examples/uint64 subscriber:examples/uint64 \
                ping:examples/ping_pong pong:examples/ping_pong \
                perf_client:examples/perf perf_server:examples/perf

# Directories the object rule below needs to exist first, derived from EXAMPLE_BINS so a
# newly-registered example directory doesn't also need a manual entry here. Order-only
# prerequisites (see `|` below) so make doesn't try to relink everything just because a
# sibling .o's mkdir touched the directory's mtime, and so -j doesn't race multiple
# `mkdir -p` calls against a per-file rule.
OBJ_DIRS = $(OBJ)/src $(sort $(addprefix $(OBJ)/,$(foreach bin,$(EXAMPLE_BINS),$(word 2,$(subst :, ,$(bin))))))

.PHONY: all library examples set_bool uint64 ping_pong perf lint clean

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
# obj/<type>/src/tickle.o and examples/set_bool/SetBool.c becomes
# obj/<type>/examples/set_bool/SetBool.o.
$(OBJ)/%.o: %.c | $(OBJ_DIRS)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIRS):
	mkdir -p $@

libtickle.a: $(OBJS)
	$(AR) crv $@ $^

# Generates one target per EXAMPLE_BINS entry: <name>_MAIN is that binary's own source,
# <name>_SHARED is every other .c file living alongside it in the same directory (minus any
# OTHER registered binary's main file - two binaries can share a directory, e.g. client.c
# and server.c both sit in examples/set_bool/), and the link recipe compiles+links exactly
# those two sets against the library.
EXAMPLE_MAIN_FILES := $(foreach bin,$(EXAMPLE_BINS),$(word 2,$(subst :, ,$(bin)))/$(word 1,$(subst :, ,$(bin))).c)

ALL_EXAMPLE_OBJS :=
define EXAMPLE_RULE
$(1)_MAIN := $(2)/$(1).c
$(1)_SHARED := $$(filter-out $(EXAMPLE_MAIN_FILES),$$(wildcard $(2)/*.c))
$(1)_OBJS := $$(patsubst %.c,$(OBJ)/%.o,$$($(1)_MAIN) $$($(1)_SHARED))
ALL_EXAMPLE_OBJS += $$($(1)_OBJS)

$(1): $$($(1)_OBJS) libtickle.a
	$$(CC) $$(CFLAGS) $$(LDFLAGS) -o $$@ $$($(1)_OBJS) $$(LDLIBS)
endef
$(foreach bin,$(EXAMPLE_BINS),$(eval $(call EXAMPLE_RULE,$(word 1,$(subst :, ,$(bin))),$(word 2,$(subst :, ,$(bin))))))

ALL_OBJS = $(OBJS) $(ALL_EXAMPLE_OBJS)

# Pull in the auto-generated per-object dependency files (headers each .o actually used),
# so changing a header rebuilds everything that includes it. Silently ignored on a clean tree.
-include $(ALL_OBJS:.o=.d)

lint:
	find . -name '*.[ch]' -exec clang-format --dry-run --Werror {} +
	find . -name '*.[ch]' -exec clang-tidy --extra-arg=-I$(INCLUDE) --extra-arg=-I$(SRC) {} +

include netns.mk

clean:
	# Removes every BUILD_TYPE's objects (obj/debug, obj/release, ...), not just the one
	# currently selected, so switching BUILD_TYPE and running `clean` doesn't leave the
	# other mode's stale cache behind.
	rm -rf obj/*/
	rm -f libtickle.a
	rm -f $(foreach bin,$(EXAMPLE_BINS),$(word 1,$(subst :, ,$(bin))))
