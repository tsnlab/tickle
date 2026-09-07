#pragma once

#include <netinet/in.h>

// Linux-specific hardware abstraction layer structure
struct tt_hal {
    int sock;
    struct sockaddr_in broadcast_addr; // Precomputed once in tt_bind(), reused by every tt_send()
};
