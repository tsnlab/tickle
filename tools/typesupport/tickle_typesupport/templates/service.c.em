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

struct tt_Service @(name)Service = {
    .name = "@(name)Service",
    .request_size = sizeof(struct @(request_name)),
    .response_size = sizeof(struct @(response_name)),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE)@(request_name)_encode_size,
    .request_encode = (tt_REQUEST_ENCODE)@(request_name)_encode,
    .request_decode = (tt_REQUEST_DECODE)@(request_name)_decode,
    .request_free = (tt_REQUEST_FREE)@(request_name)_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)@(response_name)_encode_size,
    .response_encode = (tt_RESPONSE_ENCODE)@(response_name)_encode,
    .response_decode = (tt_RESPONSE_DECODE)@(response_name)_decode,
    .response_free = (tt_RESPONSE_FREE)@(response_name)_free,
    .call_retry_interval = 0, // 0 means auto
    .call_retry_count = 0,    // 0 means tt_CALL_RETRY_COUNT
};

@(request_struct_c)
@(response_struct_c)
