#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <tickle/config.h> // tt_MAX_BUFFER_LENGTH
#include <tickle/tickle.h>

@(request_struct_h)
@(response_struct_h)
extern struct tt_Service @(name)Service;
