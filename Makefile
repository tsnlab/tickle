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
override LDLIBS += -ltickle

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

SETBOOL_SRCS = examples/set_bool/SetBool.c
UINT64_SRCS = examples/uint64/UInt64.c
SETBOOL_OBJS = $(patsubst %.c,$(OBJ)/%.o,$(SETBOOL_SRCS))
UINT64_OBJS = $(patsubst %.c,$(OBJ)/%.o,$(UINT64_SRCS))

ALL_OBJS = $(OBJS) $(SETBOOL_OBJS) $(UINT64_OBJS) \
           $(OBJ)/examples/set_bool/client.o $(OBJ)/examples/set_bool/server.o \
           $(OBJ)/examples/uint64/publisher.o $(OBJ)/examples/uint64/subscriber.o

# Directories the object rule below needs to exist first. Order-only prerequisites (see
# `|` below) so make doesn't try to relink everything just because a sibling .o's mkdir
# touched the directory's mtime, and so -j doesn't race multiple `mkdir -p` calls against
# a per-file rule.
OBJ_DIRS = $(OBJ)/src $(OBJ)/examples/set_bool $(OBJ)/examples/uint64

.PHONY: all library examples set_bool uint64 lint createns deletens runclient runserver runpublisher runsubscriber dump1 dump2 clean

all:
	$(MAKE) library
	$(MAKE) examples

library: libtickle.a

examples: set_bool uint64

set_bool: client server

uint64: publisher subscriber

# Generic rule: mirrors every source file's path under $(OBJ)/, so src/tickle.c becomes
# obj/<type>/src/tickle.o and examples/set_bool/SetBool.c becomes
# obj/<type>/examples/set_bool/SetBool.o.
$(OBJ)/%.o: %.c | $(OBJ_DIRS)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIRS):
	mkdir -p $@

libtickle.a: $(OBJS)
	$(AR) crv $@ $^

client: $(OBJ)/examples/set_bool/client.o $(SETBOOL_OBJS) libtickle.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)/examples/set_bool/client.o $(SETBOOL_OBJS) $(LDLIBS)

server: $(OBJ)/examples/set_bool/server.o $(SETBOOL_OBJS) libtickle.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)/examples/set_bool/server.o $(SETBOOL_OBJS) $(LDLIBS)

publisher: $(OBJ)/examples/uint64/publisher.o $(UINT64_OBJS) libtickle.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)/examples/uint64/publisher.o $(UINT64_OBJS) $(LDLIBS)

subscriber: $(OBJ)/examples/uint64/subscriber.o $(UINT64_OBJS) libtickle.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)/examples/uint64/subscriber.o $(UINT64_OBJS) $(LDLIBS)

# Pull in the auto-generated per-object dependency files (headers each .o actually used),
# so changing a header rebuilds everything that includes it. Silently ignored on a clean tree.
-include $(ALL_OBJS:.o=.d)

lint:
	find . -name '*.[ch]' -exec clang-format --dry-run --Werror {} +
	find . -name '*.[ch]' -exec clang-tidy --extra-arg=-I$(INCLUDE) --extra-arg=-I$(SRC) {} +

createns:
# Ref: https://medium.com/@tech_18484/how-to-create-network-namespace-in-linux-host-83ad56c4f46f
# create namespace
	sudo ip netns add ns1
	sudo ip netns add ns2
# create cable
	sudo ip link add veth1 type veth peer name veth2
# attach cable
	sudo ip link set veth1 netns ns1
	sudo ip link set veth2 netns ns2
# set ip
	sudo ip -n ns1 addr add 192.168.10.1/24 dev veth1
	sudo ip -n ns2 addr add 192.168.10.2/24 dev veth2
# bring up interface
	sudo ip -n ns1 link set veth1 up
	sudo ip -n ns2 link set veth2 up
# NS1 info
	@echo "# Namespace #1"
	sudo ip netns exec ns1 ip addr
	sudo ip netns exec ns1 ip route
	sudo ip netns exec ns1 ping -c 1 192.168.10.2
# NS2 info
	@echo "\n# Namespace #2"
	sudo ip netns exec ns2 ip addr
	sudo ip netns exec ns2 ip route
	sudo ip netns exec ns2 ping -c 1 192.168.10.1

deletens:
	sudo ip netns delete ns1
	sudo ip netns delete ns2

runclient: client
	sudo ip netns exec ns1 ./client

runserver: server
	sudo ip netns exec ns2 ./server

runpublisher: publisher
	sudo ip netns exec ns1 ./publisher

runsubscriber: subscriber
	sudo ip netns exec ns2 ./subscriber

dump1:
	sudo ip netns exec ns1 tcpdump -l -xxx -i veth1

dump2:
	sudo ip netns exec ns2 tcpdump -l -xxx -i veth2

clean:
	# Removes every BUILD_TYPE's objects (obj/debug, obj/release, ...), not just the one
	# currently selected, so switching BUILD_TYPE and running `clean` doesn't leave the
	# other mode's stale cache behind.
	rm -rf obj/*/
	rm -f libtickle.a
	rm -f client
	rm -f server
	rm -f publisher
	rm -f subscriber
