#pragma once

#include <tickle/tickle.h>

#include "rmw/rmw.h"
#include "rcutils/allocator.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"

#ifdef __cplusplus
extern "C"
{
#endif

// TickLE RMW implementation identifier
#define RMW_TICKLE_IDENTIFIER "rmw_tickle"

// TickLE serialization format
#define RMW_TICKLE_SERIALIZATION_FORMAT "tickle"

// External identifier variables
extern const char * const rmw_tickle_identifier;
extern const char * const rmw_tickle_serialization_format;

// TickLE context implementation
typedef struct rmw_tickle_context_impl_t
{
  // Graph guard condition for node discovery
  rmw_guard_condition_t graph_guard_condition;
  // Placeholder for TickLE context data
  // This can be extended with TickLE-specific context information
  int dummy;  // Temporary field to avoid empty struct
} rmw_tickle_context_impl_t;

// RMW implementation functions
const char * rmw_get_implementation_identifier(void);
rmw_init_options_t rmw_get_zero_initialized_init_options(void);

// TickLE specific node data
typedef struct rmw_tickle_node_t
{
  rmw_node_t rmw_node;        // RMW node structure (must be first)
  struct tt_Node tickle_node;
  rcutils_allocator_t allocator;
  const rmw_context_t * context;  // Store context reference
} rmw_tickle_node_t;

// TickLE specific publisher data
typedef struct rmw_tickle_publisher_t
{
  rmw_publisher_t rmw_publisher;  // RMW publisher structure (must be first)
  struct tt_Publisher tickle_publisher;
  rmw_tickle_node_t * node;
  const rosidl_message_type_support_t * type_support;
} rmw_tickle_publisher_t;

// TickLE specific subscriber data
typedef struct rmw_tickle_subscriber_t
{
  struct tt_Subscriber tickle_subscriber;
  rmw_tickle_node_t * node;
  const rosidl_message_type_support_t * type_support;
} rmw_tickle_subscriber_t;

// TickLE specific client data
typedef struct rmw_tickle_client_t
{
  struct tt_Client tickle_client;
  rmw_tickle_node_t * node;
  const rosidl_service_type_support_t * type_support;
} rmw_tickle_client_t;

// TickLE specific service data
typedef struct rmw_tickle_service_t
{
  struct tt_Server tickle_server;
  rmw_tickle_node_t * node;
  const rosidl_service_type_support_t * type_support;
} rmw_tickle_service_t;

// TickLE specific guard condition data
typedef struct rmw_tickle_guard_condition_t
{
  bool has_triggered;
  rcutils_allocator_t allocator;
} rmw_tickle_guard_condition_t;

// TickLE specific wait set data
typedef struct rmw_tickle_wait_set_t
{
  rmw_tickle_guard_condition_t ** guard_conditions;
  size_t guard_condition_count;
  rcutils_allocator_t allocator;
} rmw_tickle_wait_set_t;

#ifdef __cplusplus
}
#endif
