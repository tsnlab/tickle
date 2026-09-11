#include "@(name).h"

#include <stdint.h>
@[if needs_string_h]@
#include <string.h>
@[end if]@

@[if needs_config_h]@
#include <tickle/config.h> // tt_MAX_STRING_LENGTH
@[end if]@
#include <tickle/hal.h>
#include <tickle/tickle.h>

struct tt_Topic @(name)Topic = {
    .name = "@(name)Topic",
    .data_size = sizeof(struct @(data_name)),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)@(data_name)_encode_size,
    .data_encode = (tt_DATA_ENCODE)@(data_name)_encode,
    .data_decode = (tt_DATA_DECODE)@(data_name)_decode,
    .data_free = (tt_DATA_FREE)@(data_name)_free,
};

@(data_struct_c)
