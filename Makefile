INCLUDE = include
SRC = src
OBJ = obj

# Toolchain: plain `?=` so these can be overridden from the environment or command line,
# e.g. `make CC=clang` or `CC=clang make`.
CC ?= gcc
AR ?= ar

# CFLAGS is the user's tuning knob (optimization/warnings) and is fully replaceable, e.g.
# `make CFLAGS=-O2` for a release-style build.
CFLAGS ?= -O0 -g -Wall -Wextra
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

.PHONY: all library examples set_bool uint64 lint createns deletens runclient runserver runpublisher runsubscriber dump1 dump2 clean

all:
	$(MAKE) library
	$(MAKE) examples

library: libtickle.a

examples: set_bool uint64

set_bool: client server

uint64: publisher subscriber

# Generic rule: mirrors every source file's path under $(OBJ)/, so src/tickle.c becomes
# obj/src/tickle.o and examples/set_bool/SetBool.c becomes obj/examples/set_bool/SetBool.o.
$(OBJ)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

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
	rm -rf $(OBJ)/*
	rm -f libtickle.a
	rm -f client
	rm -f server
	rm -f publisher
	rm -f subscriber
