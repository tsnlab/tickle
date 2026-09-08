#include <errno.h>
#include <ifaddrs.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "consts.h"
#include "log.h"

#define SEC_NS 1000000000LL
#define MS_NS 1000000LL

#define UNUSED(x) (void)(x)

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
};

uint64_t tt_get_ns() {
    struct timespec ts;
    // CLOCK_REALTIME lives in a glibc-private header; <time.h> (included above) is the correct public header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    clock_gettime(CLOCK_REALTIME, &ts);

    return ((uint64_t)ts.tv_sec * SEC_NS) + ts.tv_nsec;
}

int32_t tt_get_node_id() {
    // Get unique node ID in the network using IP address x.x.x.id
    uint32_t broadcast_ip = inet_addr(_tt_CONFIG.broadcast);

    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs) != 0) {
        TT_LOG_ERROR("Cannot get network interfaces: %s", strerror(errno));
        return -1;
    }

    uint8_t node_id = 0;

    struct ifaddrs* ifaddr = ifaddrs;
    while (ifaddr != NULL) {
        if (ifaddr->ifa_addr != NULL && ifaddr->ifa_netmask != NULL && ifaddr->ifa_addr->sa_family == AF_INET &&
            ifaddr->ifa_netmask->sa_family == AF_INET) {
            uint32_t addr = ((struct sockaddr_in*)ifaddr->ifa_addr)->sin_addr.s_addr;
            uint32_t netmask = ((struct sockaddr_in*)ifaddr->ifa_netmask)->sin_addr.s_addr;

            if (node_id == 0) {
                node_id = addr >> 24;
            } else if ((addr & netmask) == (broadcast_ip & netmask)) {
                node_id = (addr & ~netmask) >> BITS_IN_3BYTES;
            }
        }
        ifaddr = ifaddr->ifa_next;
    }

    freeifaddrs(ifaddrs);

    return node_id;
}

tt_ret_t tt_bind(struct tt_Node* node) {
    node->hal.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.sock < 0) {
        TT_LOG_ERROR("Cannot create UDP socket: %s", strerror(errno));
        return tt_RET_IO_ERROR;
    }

    int optval = 1;
    // SOL_SOCKET/SO_* live in glibc-private headers; <sys/socket.h> (included above) is the correct public header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket reuseaddr: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    optval = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket broadcast: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Best-effort: the kernel clamps this to net.core.[rw]mem_max for an unprivileged process,
    // so a failure or a smaller-than-requested result here isn't fatal, just less headroom
    // against bursty drops.
    int buffer_size = tt_SOCKET_BUFFER_SIZE;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_SNDBUF, (const void*)&buffer_size, sizeof(int)) < 0) {
        TT_LOG_WARNING("Cannot set socket send buffer size: %s", strerror(errno));
    }
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_RCVBUF, (const void*)&buffer_size, sizeof(int)) < 0) {
        TT_LOG_WARNING("Cannot set socket receive buffer size: %s", strerror(errno));
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    addr.sin_port = htons(_tt_CONFIG.port);

    if (bind(node->hal.sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind socket to %s:%d: %s", _tt_CONFIG.addr, _tt_CONFIG.port, strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Precompute the broadcast destination once instead of re-parsing _tt_CONFIG.broadcast with
    // inet_addr() on every single tt_send() call.
    node->hal.broadcast_addr.sin_family = AF_INET;
    node->hal.broadcast_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.broadcast);
    node->hal.broadcast_addr.sin_port = htons(_tt_CONFIG.port);

    return tt_RET_OK;
}

void tt_close(struct tt_Node* node) {
    if (close(node->hal.sock) == -1) {
        TT_LOG_ERROR("Cannot close socket: %s", strerror(errno));
    }
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    return (int32_t)sendto(node->hal.sock, buf, len, 0, (struct sockaddr*)&node->hal.broadcast_addr,
                           sizeof(struct sockaddr_in));
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    // Wait for readability with poll() instead of arming SO_RCVTIMEO via setsockopt() before
    // every recvfrom(): the timeout here changes on nearly every call (it tracks whatever
    // scheduled event is due next), and re-arming a socket option that often is pure overhead -
    // poll() just takes the timeout as a plain argument, no socket mutation needed.
    if (timeout >= 0) {
        int timeout_ms;
        if (timeout == 0) {
            // This function's contract (see hal.h) is "0 for no timeout", i.e. block until data
            // arrives - not "don't wait at all", which is what poll()'s own timeout=0 means.
            timeout_ms = -1;
        } else {
            timeout_ms = (int)(timeout / MS_NS);
            if (timeout_ms == 0) {
                // Sub-millisecond positive timeouts would round down to 0, which poll() treats
                // as "don't wait at all" - round up so a short-but-nonzero wait still waits.
                timeout_ms = 1;
            }
        }

        // struct pollfd/POLLIN/poll() live in a glibc-private header; <poll.h> (included above) is
        // the correct public header.
        // NOLINTNEXTLINE(misc-include-cleaner)
        struct pollfd pfd = {.fd = node->hal.sock, .events = POLLIN, .revents = 0};
        int poll_ret = poll(&pfd, 1, timeout_ms); // NOLINT(misc-include-cleaner)
        if (poll_ret == 0) {
            return -1; // Timeout
        }
        if (poll_ret < 0) {
            // NOLINTNEXTLINE(misc-include-cleaner) -- EINTR lives in the same private header as EAGAIN below
            if (errno == EINTR) {
                return -1; // Treat an interrupted wait like a timeout; the caller just polls again
            }
            return -2; // I/O error
        }
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    int32_t ret = (int32_t)recvfrom(node->hal.sock, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    if (ret < 0) {
        // EAGAIN/EWOULDBLOCK live in glibc-private headers; <errno.h> (included above) is the correct public header.
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // Timeout
        }
        return -2; // I/O error
    }

    return ret;
}
