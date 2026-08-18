#pragma once

#include <stdint.h>

// Linux-specific hardware abstraction layer structure
struct tt_hal {
    int sock;
    uint64_t receive_timeout;
};
