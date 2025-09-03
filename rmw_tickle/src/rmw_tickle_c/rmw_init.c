#include "rmw_tickle_c/rmw_tickle.h"
#include "rmw/rmw.h"
#include "rcutils/error_handling.h"

#include <stdio.h>
#include <string.h>

rmw_ret_t
rmw_init_options_init(rmw_init_options_t * init_options, rcutils_allocator_t allocator)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator.allocate, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator.deallocate, RMW_RET_INVALID_ARGUMENT);

  init_options->instance_id = 0;
  init_options->domain_id = 0;
  // Initialize security options to zero
  memset(&init_options->security_options, 0, sizeof(rmw_security_options_t));
  init_options->enclave = NULL;
  init_options->allocator = allocator;
  init_options->impl = NULL;
  
  // Set the implementation identifier
  init_options->implementation_identifier = RMW_TICKLE_IDENTIFIER;

  return RMW_RET_OK;
}

rmw_ret_t
rmw_init_options_fini(rmw_init_options_t * init_options)
{
  puts("HELLO rmw_init_options_fini!!!!!!!!!!!!!");
  return RMW_RET_OK;
}

rmw_ret_t
rmw_init(const rmw_init_options_t * options, rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_UROS_TRACE_ERROR("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  context->instance_id = 0;
  context->implementation_identifier = RMW_TICKLE_IDENTIFIER;
  context->options = *options;

  puts("HELLO!!!!!!!!!!!!!");

  return RMW_RET_OK;
}

rmw_ret_t
rmw_shutdown(rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_UROS_TRACE_ERROR("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  return RMW_RET_OK;
}

rmw_ret_t
rmw_context_fini(rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_UROS_TRACE_ERROR("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // Finalize the init options
  rmw_ret_t ret = rmw_init_options_fini(&context->options);
  if (ret != RMW_RET_OK) {
    return ret;
  }

  return RMW_RET_OK;
}
