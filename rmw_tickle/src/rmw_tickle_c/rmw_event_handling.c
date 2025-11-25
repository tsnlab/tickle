// Copyright 2024 TickLE Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <rcutils/logging_macros.h>
#include <rmw/allocators.h>
#include <rmw/rmw.h>

#include "rmw_tickle_c/rmw_tickle.h"

rmw_ret_t rmw_take_event(const rmw_event_t* event_handle, void* event_info, bool* taken) {
    (void)event_handle;
    (void)event_info;
    (void)taken;

    RCUTILS_LOG_DEBUG("rmw_take_event: function not implemented for TickLE");
    return RMW_RET_UNSUPPORTED;
}
