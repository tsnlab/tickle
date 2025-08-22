#include <bits/time.h>
#include <ifaddrs.h>
#include <stdint.h>
#include <stdio.h>
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

#define SEC_NS 1000000000ULL

#define UNUSED(x) (void)(x)

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
};

// Endian checking functions
bool tt_is_native_endian(struct tt_Header* header) {
    return *((uint16_t*)header->magic) == (((uint16_t)'T' << 8) | 'K');
}

bool tt_is_reverse_endian(struct tt_Header* header) {
    return *((uint16_t*)header->magic) == (((uint16_t)'K' << 8) | 'T');
}

// Hash function
uint32_t tt_hash_id(const char* type, const char* name) {
    uint8_t type_len = _tt_strnlen(type, tt_MAX_NAME_LENGTH);
    uint8_t name_len = _tt_strnlen(name, tt_MAX_NAME_LENGTH);

    uint32_t hash = 0;
    ((uint8_t*)&hash)[1] = type_len;
    ((uint8_t*)&hash)[3] = name_len;

    int count = (int)(type_len / sizeof(uint32_t));
    for (int i = 0; i < count; i++) {
        hash += ((const uint32_t*)type)[i];
    }

    size_t rest = type_len % sizeof(uint32_t);
    if (rest > 0) {
        uint32_t tail = 0;
        size_t offset = count * sizeof(uint32_t);
        _tt_memcpy(&tail, type + offset, rest);
        hash += tail;
    }

    count = (int)(name_len / sizeof(uint32_t));
    for (int i = 0; i < count; i++) {
        hash += ((const uint32_t*)name)[i];
    }

    rest = name_len % sizeof(uint32_t);
    if (rest > 0) {
        uint32_t tail = 0;
        size_t offset = count * sizeof(uint32_t);
        _tt_memcpy(&tail, name + offset, rest);
        hash += tail;
    }

    return hash;
}

// Basic encoding/decoding utility functions
void* tt_encode_buffer(void* buffer, uint32_t* tail, uint32_t len) {
    uint8_t* buf = (uint8_t*)buffer + *tail;
    *tail += len;
    return buf;
}

void* tt_decode_buffer(void* buffer, uint32_t* head, uint32_t tail, uint32_t length) {
    if (*head + length > tail) {
        return NULL;
    }

    void* p = (uint8_t*)buffer + *head;
    *head += length;
    return p;
}

// String encoding/decoding functions
bool tt_encode_string(void* buffer, uint32_t* tail, uint32_t buffer_size, const char* str) {
    size_t str_len = _tt_strnlen(str, tt_MAX_STRING_LENGTH) + 1; // including '\0'

    if (str_len < 0) {
        return false;
    }

    if (*tail + sizeof(uint16_t) + str_len >= buffer_size) {
        return false;
    }

    *(uint16_t*)((uint8_t*)buffer + *tail) = str_len;
    *tail += sizeof(uint16_t);

    _tt_memcpy((uint8_t*)buffer + *tail, str, str_len);
    *tail += str_len;

    return true;
}

bool tt_decode_string(void* buffer, uint32_t* head, uint32_t tail, uint16_t* str_len, char** str) {
    if (*head + sizeof(uint16_t) > tail) {
        return false;
    }

    *str_len = *(uint16_t*)((uint8_t*)buffer + *head);
    *head += sizeof(uint16_t);

    if (*head + *str_len > tail) {
        return false;
    }

    *str = (char*)((uint8_t*)buffer + *head);
    *head += *str_len;

    return true;
}

uint64_t tt_get_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return ((uint64_t)ts.tv_sec * SEC_NS) + ts.tv_nsec;
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
        if (ifaddr->ifa_addr != NULL && ifaddr->ifa_netmask != NULL &&
            ifaddr->ifa_addr->sa_family == AF_INET && ifaddr->ifa_netmask->sa_family == AF_INET) {
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

int32_t tt_bind(struct tt_Node* node) {
    node->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->sock < 0) {
        perror("Cannot create UDP socket");
        return tt_CANNOT_CREATE_SOCK;
    }

    int optval = 1;
    if (setsockopt(node->sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) < 0) {
        perror("Cannot set socket reuseaddr");
        return tt_CANNOT_SET_REUSEADDR;
    }

    optval = 1;
    if (setsockopt(node->sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        perror("Cannot set socket broadcast");
        return tt_CANNOT_SET_BROADCAST;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT_IN_MICROSECONDS;

    if (setsockopt(node->sock, SOL_SOCKET, SO_RCVTIMEO, (const void*)&timeout, sizeof(struct timeval)) < 0) {
        perror("Cannot set socket receive timeout");
        return tt_CANNOT_SET_TIMEOUT;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    addr.sin_port = htons(_tt_CONFIG.port);

    if (bind(node->sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot binding socket: %s:%d", _tt_CONFIG.addr, _tt_CONFIG.port);
        perror("Cannot binding socket\n");
        return tt_CANNOT_BIND_SOCKET;
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

    return (int32_t)sendto(node->sock, buf, len, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct sockaddr_in addr;
    int addr_len = sizeof(struct sockaddr_in);

    int32_t ret = (int32_t)recvfrom(node->sock, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    return ret;
}
