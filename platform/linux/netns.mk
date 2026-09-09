# Local dev/test helpers: run the example binaries across two Linux network namespaces
# connected by a veth pair, to exercise UDP broadcast without needing real network hardware - a
# manual, closer-to-real-network (each side gets its own real interface/address, auto-detecting
# its node ID the normal way) alternative to platform/linux/test.sh's own root-free loopback setup
# (see that file's own comment). Needs root (creating a namespace/veth needs CAP_NET_ADMIN);
# test-linux itself does not, so it doesn't use these targets.
# Split out of platform/linux/Makefile so build logic and this environment-specific tooling don't
# get tangled together - included from there (`include netns.mk`, same directory) rather than
# invoked as its own standalone Makefile, since there's no separate toolchain/target here that
# would need its own invocation. The runX targets' `./client` etc. resolve correctly because the
# binaries they depend on (client, ...) are built right here in platform/linux/ too now.
#
# test-linux itself (running platform/linux/test.sh) is defined at the repo root, not here - see
# that Makefile's own comment.

.PHONY: createns deletens runclient runserver runpublisher runsubscriber runping runpong runperf_client runperf_server dump1 dump2

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

runping: ping
	sudo ip netns exec ns1 ./ping

runpong: pong
	sudo ip netns exec ns2 ./pong

runperf_client: perf_client
	sudo ip netns exec ns1 ./perf_client

runperf_server: perf_server
	sudo ip netns exec ns2 ./perf_server

dump1:
	sudo ip netns exec ns1 tcpdump -l -xxx -i veth1

dump2:
	sudo ip netns exec ns2 tcpdump -l -xxx -i veth2
