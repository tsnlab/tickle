#pragma once

#include <tickle/tickle.h>

struct service__srv__ServiceRequest {
    bool status;
    float fp;
    uint32_t array[10];
    char msg[32];
};

struct service__srv__ServiceResponse {
    bool status;
    float fp;
    uint32_t array[10];
    char msg[32];
};

extern struct tt_Service service__srv__ServiceService;
