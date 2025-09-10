#include "rmw_tickle_c/rmw_tickle.h"
#include "rmw/rmw.h"
#include "rmw/error_handling.h"
#include "rcutils/error_handling.h"
#include "rcutils/strdup.h"

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
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
  rcutils_allocator_t * allocator = &init_options->allocator;
  RCUTILS_CHECK_ALLOCATOR(allocator, return RMW_RET_INVALID_ARGUMENT);
  if (strcmp(init_options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  return RMW_RET_OK;
}

rmw_ret_t
rmw_init_options_copy(
  const rmw_init_options_t * src,
  rmw_init_options_t * dst)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);
  
  if (strcmp(src->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Implementation identifiers does not match");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  
  if (NULL != dst->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized dst");
    return RMW_RET_INVALID_ARGUMENT;
  }
  
  // Copy the basic structure
  memcpy(dst, src, sizeof(rmw_init_options_t));
  
  // Copy the enclave string if it exists
  if (src->enclave != NULL) {
    dst->enclave = rcutils_strdup(src->enclave, src->allocator);
    if (NULL == dst->enclave) {
      return RMW_RET_BAD_ALLOC;
    }
  } else {
    dst->enclave = NULL;
  }
  
  return RMW_RET_OK;
}

rmw_ret_t
rmw_init(const rmw_init_options_t * options, rmw_context_t * context)
{
  puts("HELLO rmw_init!!!!!!!!!!!!!");
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  context->instance_id = 0;
  context->implementation_identifier = RMW_TICKLE_IDENTIFIER;
  context->options = *options;
  
  // Allocate and initialize context implementation
  rmw_tickle_context_impl_t * impl = (rmw_tickle_context_impl_t *)options->allocator.allocate(
    sizeof(rmw_tickle_context_impl_t), options->allocator.state);
  if (impl == NULL) {
    RMW_SET_ERROR_MSG("Failed to allocate context implementation");
    return RMW_RET_BAD_ALLOC;
  }
  
  // Initialize the context implementation
  memset(impl, 0, sizeof(rmw_tickle_context_impl_t));
  
  // Initialize graph guard condition
  impl->graph_guard_condition.implementation_identifier = RMW_TICKLE_IDENTIFIER;
  impl->graph_guard_condition.data = NULL;
  
  context->impl = impl;


  return RMW_RET_OK;
}

rmw_ret_t
rmw_shutdown(rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  return RMW_RET_OK;
}

rmw_ret_t
rmw_context_fini(rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

  if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
    RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  // Free the context implementation
  if (context->impl != NULL) {
    context->options.allocator.deallocate(context->impl, context->options.allocator.state);
    context->impl = NULL;
  }

  // Finalize the init options
  rmw_ret_t ret = rmw_init_options_fini(&context->options);
  if (ret != RMW_RET_OK) {
    return ret;
  }

  return RMW_RET_OK;
}
