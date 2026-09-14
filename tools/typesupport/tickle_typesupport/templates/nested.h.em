@# A nested dependency's own header (std_msgs__Header.h, say): the struct + codec function
@# declarations a parent interface's #include reaches into - no tt_Topic/tt_Service wrapper of
@# its own, since a nested message has no independent existence on the wire (DESIGN.md: "no
@# header, no extra alignment beyond what the first nested field needs").
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <tickle/config.h> // tt_MAX_BUFFER_LENGTH
@[for nested_name in nested_includes]@
#include "@(nested_name).h"
@[end for]@

@(struct_h)
