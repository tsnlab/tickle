#include <ifaddrs.h>
#include <stdio.h>
#include <tickle.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
};

uint64_t tt_get_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int32_t tt_get_node_id() {
    // Get unique node ID in the network using IP address x.x.x.id
    uint32_t broadcast_ip = inet_addr(_tt_CONFIG.broadcast);

    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs) != 0) {
        perror("Cannot get network interfaces");
        return -1;
    }

    uint8_t node_id = 0;

    struct ifaddrs* ifaddr = ifaddrs;
    while (ifaddr != NULL) {
        if (ifaddr->ifa_addr->sa_family == AF_INET && ifaddr->ifa_netmask->sa_family == AF_INET) {
            uint32_t addr = ((struct sockaddr_in*)ifaddr->ifa_addr)->sin_addr.s_addr;
            uint32_t netmask = ((struct sockaddr_in*)ifaddr->ifa_netmask)->sin_addr.s_addr;

            if (node_id == 0) {
                node_id = addr >> 24;
            } else if ((addr & netmask) == (broadcast_ip & netmask)) {
                node_id = (addr & ~netmask) >> 24;
            }
        }
        ifaddr = ifaddr->ifa_next;
    }

    freeifaddrs(ifaddrs);

    return node_id;
}

int32_t tt_bind(struct tt_Node* node) {
    node->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->sock < 0) {
        perror("Cannot create UDP socket");
        return -3;
    }

    int optval = 1;
    if (setsockopt(node->sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) < 0) {
        perror("Cannot set socket reuseaddr");
        return -4;
    }

    optval = 1;
    if (setsockopt(node->sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        perror("Cannot set socket broadcast");
        return -5;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 10;

    if (setsockopt(node->sock, SOL_SOCKET, SO_RCVTIMEO, (const void*)&timeout, sizeof(struct timeval)) < 0) {
        perror("Cannot set socket receive timeout");
        return -6;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    addr.sin_port = htons(_tt_CONFIG.port);

    if (bind(node->sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
        printf("Cannot binding socket: %s:%d\n", _tt_CONFIG.addr, _tt_CONFIG.port);
        perror("Cannot binding socket\n");
        return -7;
    }

    return 0;
}

void tt_close(struct tt_Node* node) {
    close(node->sock);
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.broadcast);
    addr.sin_port = htons(_tt_CONFIG.port);

    return sendto(node->sock, buf, len, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct sockaddr_in addr;
    int addr_len = sizeof(struct sockaddr_in);

    int ret = recvfrom(node->sock, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    return ret;
}
