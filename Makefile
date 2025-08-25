INCLUDE=include
CC=gcc
AR=ar
CFLAGS=-I$(INCLUDE) -O0 -g

SRC=src
OBJ=obj

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
else ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
else ifeq ($(UNAME_S),FreeBSD)
    PLATFORM := freebsd
else ifeq ($(OS),Windows_NT)
    PLATFORM := windows
else
    PLATFORM := unknown
endif

# HAL source file based on platform
HAL_SRC = $(SRC)/hal_$(PLATFORM).c

# Automatically find all .c files in src directory, excluding hal.c files
SRC_FILES = $(filter-out $(SRC)/hal_%.c, $(wildcard $(SRC)/*.c))
# Add platform-specific HAL file
SRC_FILES += $(HAL_SRC)
OBJS = $(patsubst $(SRC)/%.c,$(OBJ)/%.o,$(SRC_FILES))

.PHONY: all lint createns deletens runclient runserver runpublisher runsubscriber dump1 dump2 clean

all: libtickle.a client server

$(OBJ)/%.o: $(SRC)/%.c
	$(CC) -c -o $@ $< $(CFLAGS)

libtickle.a: $(OBJS)
	$(AR) crv $@ $^

client:  examples/set_bool/SetBool.c examples/set_bool/client.c libtickle.a
	$(CC) -o $@ examples/set_bool/SetBool.c examples/set_bool/client.c -L. -ltickle $(CFLAGS)

server: examples/set_bool/SetBool.c examples/set_bool/server.c libtickle.a
	$(CC) -o $@ examples/set_bool/SetBool.c examples/set_bool/server.c -L. -ltickle $(CFLAGS)

publisher:  examples/uint64/UInt64.c examples/uint64/publisher.c libtickle.a
	$(CC) -o $@ examples/uint64/UInt64.c examples/uint64/publisher.c -L. -ltickle $(CFLAGS)

subscriber: examples/uint64/UInt64.c examples/uint64/subscriber.c libtickle.a
	$(CC) -o $@ examples/uint64/UInt64.c examples/uint64/subscriber.c -L. -ltickle $(CFLAGS)

lint:
	find . -name '*.[ch]' -exec clang-format --dry-run --Werror {} \;

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
