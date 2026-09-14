#include "@(name).h"

#include <stdint.h>
@[if needs_string_h]@
#include <string.h>
@[end if]@
@[if data_has_inplace]@
#include <stddef.h> // NULL, in *_decode_inplace
@[end if]@

@[if needs_config_h]@
#include <tickle/config.h> // tt_MAX_STRING_LENGTH
@[end if]@
@[if needs_hal_h]@
#include <tickle/hal.h>
@[end if]@
#include <tickle/tickle.h>
@[for nested_name in nested_includes]@
#include "@(nested_name).h"
@[end for]@

struct tt_Topic @(name)Topic = {
    .name = "@(name)Topic",
    .data_size = sizeof(struct @(data_name)),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)@(data_name)_encode_size,
    .data_encode = (tt_DATA_ENCODE)@(data_name)_encode,
@[if data_has_inplace]@
    .data_encode_inplace = (tt_DATA_ENCODE_INPLACE)@(data_name)_encode_inplace,
@[end if]@
    .data_decode = (tt_DATA_DECODE)@(data_name)_decode,
@[if data_has_inplace]@
    .data_decode_inplace = (tt_DATA_DECODE_INPLACE)@(data_name)_decode_inplace,
@[end if]@
    .data_free = (tt_DATA_FREE)@(data_name)_free,
};

@(data_struct_c)
