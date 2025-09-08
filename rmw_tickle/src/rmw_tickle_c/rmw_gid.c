// Copyright 2024 TickLE RMW Implementation
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

#include "rmw_tickle_c/rmw_tickle.h"
#include "rmw/rmw.h"
#include "rmw/error_handling.h"
#include "rcutils/error_handling.h"

#include <stdio.h>
#include <string.h>

rmw_ret_t
rmw_get_gid_for_publisher(
  const rmw_publisher_t * publisher,
  rmw_gid_t * gid)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(publisher->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // For now, we don't have GID support in TickLE
  // Set a dummy GID
  memset(gid->data, 0, RMW_GID_STORAGE_SIZE);
  gid->implementation_identifier = RMW_TICKLE_IDENTIFIER;
  
  printf("rmw_get_gid_for_publisher: returning dummy GID (not implemented for TickLE)\n");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_compare_gids_equal(
  const rmw_gid_t * gid1,
  const rmw_gid_t * gid2,
  bool * result)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid1, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(gid2, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(result, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(gid1->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match for gid1");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  if (strcmp(gid2->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match for gid2");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // Compare GIDs
  *result = memcmp(gid1->data, gid2->data, RMW_GID_STORAGE_SIZE) == 0;
  
  printf("rmw_compare_gids_equal: comparing GIDs (not fully implemented for TickLE)\n");
  return RMW_RET_OK;
}
