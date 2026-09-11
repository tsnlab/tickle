#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <tickle/config.h> // tt_MAX_BUFFER_LENGTH
#include <tickle/tickle.h>
@[for nested_name in nested_includes]@
#include "@(nested_name).h"
@[end for]@

@(data_struct_h)
extern struct tt_Topic @(name)Topic;
