#pragma once

// Shared HAL/time mocks for tests that include production .c files directly.
// Tests can override the exported state variables after test_mock_reset().

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tickle/config.h> // NOLINT(misc-include-cleaner)
#include <tickle/tickle.h> // NOLINT(misc-include-cleaner)

// Define mock HAL symbols in one test binary so tickle.c can be included directly.
#ifdef TEST_MOCK_DEFINE_STORAGE
struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
};

// Mock state defaults to a quiet node with no incoming packets.
uint64_t test_mock_now = 0;
int32_t test_mock_node_id = 1;
int32_t test_mock_bind_return = 0;
int32_t test_mock_receive_return = -1;
bool test_mock_send_return_override = false;
int32_t test_mock_send_return = 0;
int32_t test_mock_lock_init_count = 0;
int32_t test_mock_lock_deinit_count = 0;
int32_t test_mock_lock_count = 0;
int32_t test_mock_unlock_count = 0;
int32_t test_mock_lock_depth = 0;
int32_t test_mock_max_lock_depth = 0;
tt_lock_t* test_mock_last_lock = NULL;
#else
extern uint64_t test_mock_now;
extern int32_t test_mock_node_id;
extern int32_t test_mock_bind_return;
extern int32_t test_mock_receive_return;
extern bool test_mock_send_return_override;
extern int32_t test_mock_send_return;
extern int32_t test_mock_lock_init_count;
extern int32_t test_mock_lock_deinit_count;
extern int32_t test_mock_lock_count;
extern int32_t test_mock_unlock_count;
extern int32_t test_mock_lock_depth;
extern int32_t test_mock_max_lock_depth;
extern tt_lock_t* test_mock_last_lock;
#endif

// Tests may override these globals after reset to drive specific timing or HAL outcomes.
static inline void test_mock_reset(void) {
    test_mock_now = 0;
    test_mock_node_id = 1;
    test_mock_bind_return = 0;
    test_mock_receive_return = -1;
    test_mock_send_return_override = false;
    test_mock_send_return = 0;
    test_mock_lock_init_count = 0;
    test_mock_lock_deinit_count = 0;
    test_mock_lock_count = 0;
    test_mock_unlock_count = 0;
    test_mock_lock_depth = 0;
    test_mock_max_lock_depth = 0;
    test_mock_last_lock = NULL;
}

#ifdef TEST_MOCK_DEFINE_STORAGE
// These functions replace the platform HAL symbols when building a test binary.
uint64_t tt_get_ns() {
    return test_mock_now;
}

int32_t tt_get_node_id() {
    return test_mock_node_id;
}

int32_t tt_bind(struct tt_Node* node) {
    (void)node;
    return test_mock_bind_return;
}

void tt_close(struct tt_Node* node) {
    (void)node;
}

void tt_lock_init(tt_lock_t* lock) {
    test_mock_lock_init_count++;
    test_mock_last_lock = lock;
}

void tt_lock_deinit(tt_lock_t* lock) {
    test_mock_lock_deinit_count++;
    test_mock_last_lock = lock;
}

tt_lock_state_t tt_lock(tt_lock_t* lock) {
    test_mock_lock_count++;
    test_mock_lock_depth++;
    if (test_mock_lock_depth > test_mock_max_lock_depth) {
        test_mock_max_lock_depth = test_mock_lock_depth;
    }
    test_mock_last_lock = lock;
    return 0;
}

void tt_unlock(tt_lock_t* lock, tt_lock_state_t state) {
    (void)state;
    test_mock_unlock_count++;
    test_mock_lock_depth--;
    test_mock_last_lock = lock;
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    (void)node;
    (void)buf;

    if (test_mock_send_return_override) {
        return test_mock_send_return;
    }

    return (int32_t)len;
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    (void)node;
    (void)buf;
    (void)len;
    (void)ip;
    (void)port;
    return test_mock_receive_return;
}
#endif
