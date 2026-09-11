#include "@(name).h"

#include <stdint.h>
@[if needs_string_h]@
#include <string.h>
@[end if]@
@[if is_fixed_size]@
#include <stddef.h> // NULL, in *_decode_inplace
@[end if]@

@[if needs_config_h]@
#include <tickle/config.h> // tt_MAX_STRING_LENGTH
@[end if]@
@[if needs_hal_h]@
#include <tickle/hal.h>
@[end if]@
@[for nested_name in nested_includes]@
#include "@(nested_name).h"
@[end for]@

@(struct_c)
