#pragma once

#include <tickle/tickle.h>

struct rtt__msg__RttData {
    uint64_t payload;
};

extern struct tt_Topic rtt__msg__RttTopic;