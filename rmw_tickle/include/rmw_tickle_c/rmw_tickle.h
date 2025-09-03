#ifndef RMW_TICKLE__RMW_TICKLE_H_
#define RMW_TICKLE__RMW_TICKLE_H_

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

// RMW implementation functions
const char * rmw_get_implementation_identifier(void);
rmw_init_options_t rmw_get_zero_initialized_init_options(void);

// TickLE specific node data
typedef struct rmw_tickle_node_t
{
  struct tt_Node tickle_node;
  rcutils_allocator_t allocator;
} rmw_tickle_node_t;

// TickLE specific publisher data
typedef struct rmw_tickle_publisher_t
{
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

#endif  // RMW_TICKLE__RMW_TICKLE_H_
