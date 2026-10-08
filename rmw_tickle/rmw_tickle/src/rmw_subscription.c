/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's Milestone 3: rmw_create_subscription()/rmw_destroy_subscription()/
// rmw_take()/rmw_take_with_info(). Milestone 7 added real QoS handling on top: rmw_tickle_
// validate_qos_profile() (rmw_qos.c) rejects anything the QoS roadmap (PLAN.md) hasn't implemented
// yet, and the surviving qos_profile->depth sizes the queue for real (see rmw_create_subscription()
// below), replacing the fixed placeholder capacity this milestone originally shipped with.

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>  // fprintf() - the loan line at rmw_destroy_subscription()
#include <stdlib.h> // getenv()/strtoull() - resolve_reorder_slots()
#include <string.h>

#include <tickle/config.h> // tt_CONTEXT_UPDATE_INTERVAL
#include <tickle/hal.h>    // tt_ret_t/tt_RET_OK/tt_get_ns
#include <tickle/tickle.h>
#include <tickle/trace.h>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"
#include "rcutils/types/rcutils_ret.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/event_callback_type.h" // rmw_event_callback_t (g2)
#include "rmw/message_sequence.h"    // rmw_message_sequence_t, rmw_message_info_sequence_t - rmw_take_sequence() (g5)
#include "rmw/qos_policy_kind.h"     // rmw_qos_policy_kind_t - check_subscription_qos_incompatible()
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"
#include "rmw/time.h" // rmw_time_point_value_t
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// Runs on the poll thread, inside tt_Context_poll() (see rmw_node.c) - the node lock is already
// held by the caller. `data` (topic->data_size bytes, decoded by TickLE) may have string/array
// fields aliasing node->rx_buffer (DESIGN.md's "Strings" rule: TickLE's own decode() never
// copies), valid only until this function returns - topic->data_free() runs right after. So the
// tickle->ros conversion has to happen *here*, synchronously, not deferred to whenever rmw_take()
// is next called: callbacks->from_tickle() is what actually performs the deep copy (rosidl_
// runtime_c__String__assign() et al. - see ros2_adapter.py's own emit_from_tickle() doc comment),
// producing a ROS message independently owned from that point on, safe to queue for later.
// Wakes anyone blocked in rmw_wait() - shared by subscriber_callback() (a newly queued message),
// check_subscription_deadline() (a fresh deadline miss), and check_subscription_liveliness() (an
// alive_count change) - same wait_mutex/wait_cond broadcast pattern every one of them needs, for
// the identical reason (rmw_tickle_context_impl_t's own doc comment, rmw_tickle.h).
static void wake_wait_cond(rmw_tickle_context_impl_t* context_impl) {
    pthread_mutex_lock(&context_impl->wait_mutex);
    pthread_cond_broadcast(&context_impl->wait_cond);
    pthread_mutex_unlock(&context_impl->wait_mutex);
    rmw_tickle_poke_polling_executor(context_impl);
}

// Milestone 45 - shell_pool's own doc comment (rmw_tickle.h). Caller must already hold queue_mutex.
// NULL (pool empty) is a normal, expected outcome the caller falls back to zero_allocate() for -
// not an error.
static void* shell_pool_pop(rmw_tickle_subscriber_t* sub_impl) {
    if (sub_impl->shell_pool_count == 0) {
        return NULL;
    }
    return sub_impl->shell_pool[--sub_impl->shell_pool_count];
}

// Returns `shell` to the pool, zeroing it first - shell_pool's own doc comment (rmw_tickle.h)
// explains why that's load-bearing, not optional. Caller must already hold queue_mutex. Falls back
// to a real deallocate() only if the pool is somehow already full - shouldn't happen (it's sized
// queue_capacity, the most shells that can ever be genuinely in flight at once - see that field's
// own doc comment), but a defensive bound costs nothing next to silently overflowing shell_pool[].
//
// A C++ message (rosidl_typesupport_tickle_cpp) is not zeroed: it is an object, and memset() would
// wreck it. It does not need to be - its from_tickle() assigns every field, so a shell that still
// holds an old sample (dropped, not taken) is simply overwritten.
static void shell_pool_push(rmw_tickle_subscriber_t* sub_impl, void* shell) {
    if (NULL == sub_impl->callbacks->ros_move) {
        memset(shell, 0, sub_impl->callbacks->ros_struct_size);
    }
    if (sub_impl->shell_pool_count < sub_impl->queue_capacity) {
        sub_impl->shell_pool[sub_impl->shell_pool_count++] = shell;
    } else {
        rmw_tickle_ros_message_destroy(sub_impl->callbacks, shell, &sub_impl->allocator);
    }
}

// What core hands subscriber_callback() as the sample: the payload itself, rmw_tickle's per-message header
// (RMW_TICKLE_PSN_BYTES, rmw_tickle.h) and the type's CDR behind it. Core's in-place decode hook takes no
// context, and the callback that reads this runs straight after it on the same thread, so one per thread
// is enough.
struct payload_view {
    const uint8_t* payload;
    uint32_t length;
    bool is_native;
};

static _Thread_local struct payload_view current_payload;

static struct tt_Data* view_payload(const uint8_t* payload, uint32_t length, bool is_native) {
    current_payload.payload = payload;
    current_payload.length = length;
    current_payload.is_native = is_native;
    return (struct tt_Data*)&current_payload;
}

// Core requires a copying decode as well; view_payload() never declines, so this is never reached.
static int32_t refuse_copying_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                     bool is_native_endian) {
    (void)data;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return -1;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

// Reads rmw_tickle's per-message header off the payload core delivered, and decodes the CDR behind
// it straight into `ros_message` - a shell from the pool (LARGE_MESSAGE_PLAN.md stage 1). Returns
// false for a message that is not rmw_tickle's shape, or whose CDR does not decode.
//
// The direct codec writes every field of the shell, so one that still holds the previous sample is
// overwritten rather than merged: a string is assigned, a sequence finalised and reinitialised at
// its new count, a fixed array copied over. That is what makes a pooled shell safe to reuse, and
// what the pass-1 harness's check 1b asserts per type by decoding into a shell that deliberately
// holds the sample before.
//
// The fallback, for a callbacks struct with no direct codec, is the old two-step: decode into the
// subscription's own TickLE struct, convert out of it, and free what the decode allocated.
static bool decode_with_psn(rmw_tickle_subscriber_t* sub_impl, const struct payload_view* view,
                            uint64_t* publication_sequence_number, void* ros_message) {
    uint32_t header = rmw_tickle_psn_read(view->payload, view->length, view->is_native, publication_sequence_number);
    if (0 == header) {
        return false;
    }
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = sub_impl->callbacks;
    if (NULL != callbacks->direct_decode) {
        return callbacks->direct_decode(ros_message, view->payload + header, view->length - header, view->is_native) >=
               0;
    }
    struct tt_Data* tickle = (struct tt_Data*)sub_impl->decode_scratch;
    if (callbacks->tickle_decode(tickle, view->payload + header, view->length - header, view->is_native) < 0) {
        return false;
    }
    bool converted = callbacks->from_tickle(sub_impl->decode_scratch, ros_message);
    callbacks->tickle_free(tickle);
    return converted;
}

// (g3, RMW_GAPS_PLAN.md) The entry for the writer core is delivering from now, claimed if it is new: a free entry,
// or else the least recently seen writer's - whose loss is then under-counted if it returns, never over-counted.
static rmw_tickle_writer_psn_t* writer_entry(rmw_tickle_subscriber_t* sub_impl, uint32_t source, uint32_t entity_id) {
    uint32_t victim = 0;
    for (uint32_t i = 0; i < tt_MAX_PEER_COUNT; i++) {
        rmw_tickle_writer_psn_t* entry = &sub_impl->lost_writers[i];
        if (entry->last_psn != 0 && entry->source == source && entry->entity_id == entity_id) {
            sub_impl->lost_last_writer = i;
            return entry;
        }
        // A free entry beats a used one; among used ones, the least recently seen.
        const rmw_tickle_writer_psn_t* best = &sub_impl->lost_writers[victim];
        bool better = (entry->last_psn == 0 && best->last_psn != 0) ||
                      (entry->last_psn != 0 && best->last_psn != 0 && entry->last_seen < best->last_seen);
        victim = better ? i : victim;
    }
    rmw_tickle_writer_psn_t* entry = &sub_impl->lost_writers[victim];
    entry->source = source;
    entry->entity_id = entity_id;
    entry->last_psn = 0; // no baseline yet
    sub_impl->lost_last_writer = victim;
    return entry;
}

// (g3) How many messages this delivery shows lost: the psns its writer skipped since the last one seen from it. The
// first from a writer only sets its baseline; a psn at most RMW_TICKLE_LATE_ARRIVAL_WINDOW behind the next expected
// is a late arrival and changes nothing; one further back is the writer starting again.
static uint64_t messages_lost_before(rmw_tickle_subscriber_t* sub_impl, uint64_t psn) {
    uint32_t source = sub_impl->tickle_subscriber.last_source;
    uint32_t entity_id = sub_impl->tickle_subscriber.last_entity_id;
    rmw_tickle_writer_psn_t* entry = &sub_impl->lost_writers[sub_impl->lost_last_writer];
    if (entry->last_psn == 0 || entry->source != source || entry->entity_id != entity_id) {
        entry = writer_entry(sub_impl, source, entity_id);
    }
    entry->last_seen = ++sub_impl->lost_clock;
    uint64_t last = entry->last_psn;
    if (0 == last || (psn <= last && last - psn >= RMW_TICKLE_LATE_ARRIVAL_WINDOW)) {
        entry->last_psn = psn; // a new writer, or one that started again
        return 0;
    }
    if (psn <= last) {
        return 0; // late, or a duplicate
    }
    entry->last_psn = psn;
    // Samples core's segment drain passed over because newer ones were already queued behind them
    // (tt_Subscriber.keep_last_depth) left this gap too. They were received and superseded, which KEEP_LAST
    // allows, and are not lost.
    uint64_t gap = psn - last - 1;
    uint64_t superseded = sub_impl->tickle_subscriber.delivering_superseded;
    return gap > superseded ? gap - superseded : 0;
}

static void count_messages_lost(rmw_tickle_subscriber_t* sub_impl, uint64_t psn) {
    uint64_t lost = messages_lost_before(sub_impl, psn);
    if (0 == lost) {
        return;
    }
    int count = lost > (uint64_t)INT32_MAX ? INT32_MAX : (int)lost;
    atomic_fetch_add(&sub_impl->message_lost.total_count, count);
    atomic_fetch_add(&sub_impl->message_lost.unread_count, count);
    wake_wait_cond(sub_impl->node->context_impl);
    rmw_tickle_callback_slot_notify(&sub_impl->message_lost.callback, (size_t)count); // (g2)
}

// Loaned messages (docs/RMW.md): decides whether this subscription lends, and attaches the context's socket-path
// receive buffers the first time one does - under the node lock, so two creates cannot both attach. A pool that
// cannot be allocated or attached leaves the subscription lending decoded shells only (every loan a copy), which is
// still a loan; nothing fails.
static bool setup_loans(rmw_tickle_subscriber_t* sub_impl) {
    sub_impl->loans = rmw_tickle_type_can_loan(sub_impl->callbacks);
    const char* ring = getenv("RMW_TICKLE_LOAN_RING_SLOTS");
    sub_impl->loan_ring_slots = NULL != ring && 0 == strcmp(ring, "1");
    if (!sub_impl->loans) {
        return false;
    }
    rmw_tickle_context_impl_t* context_impl = sub_impl->node->context_impl;
    tt_Context_lock(&context_impl->tickle_context);
    if (NULL == context_impl->loan_rx_pool) {
        size_t words = (size_t)RMW_TICKLE_LOAN_RX_POOL_BUFFERS * (tt_RX_POOL_BUFFER_BYTES / sizeof(uint64_t));
        uint64_t* pool =
            (uint64_t*)context_impl->allocator.zero_allocate(words, sizeof(uint64_t), context_impl->allocator.state);
        if (NULL != pool && tt_RET_OK == tt_Context_set_rx_pool(&context_impl->tickle_context, pool,
                                                                (uint8_t)RMW_TICKLE_LOAN_RX_POOL_BUFFERS)) {
            context_impl->loan_rx_pool = pool;
        } else {
            context_impl->allocator.deallocate(pool, context_impl->allocator.state);
        }
    }
    tt_Context_unlock(&context_impl->tickle_context);
    return true;
}

// The depth core's segment drain may rely on (tt_Subscriber.keep_last_depth). KEEP_LAST: the queue keeps the newest
// queue_limit samples and overwrites the rest, so a sample with that many newer ones queued behind it in the ring would
// only be decoded to be overwritten, and the drain passes over it. KEEP_ALL: 0, which never skips anything.
static uint16_t keep_last_depth_for(const rmw_tickle_subscriber_t* sub_impl) {
    if (sub_impl->keep_all) {
        return 0;
    }
    return (uint16_t)(sub_impl->queue_limit > UINT16_MAX ? UINT16_MAX : sub_impl->queue_limit);
}

// How many samples one subscription's queue holds. KEEP_LAST takes qos_profile->depth, which is
// what DEPTH means. KEEP_ALL ignores depth - DDS does not define one for it - and divides a byte
// budget instead, which is the bound DDS actually puts on KEEP_ALL (RESOURCE_LIMITS). See
// rmw_tickle_reader_keep_all_budget_bytes() and RMW_GAPS_PLAN.md g13.
static size_t resolve_queue_capacity(const rmw_qos_profile_t* qos_profile,
                                     const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks) {
    if (RMW_QOS_POLICY_HISTORY_KEEP_ALL != qos_profile->history) {
        return qos_profile->depth != RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT ? qos_profile->depth
                                                                         : RMW_TICKLE_SUBSCRIPTION_QUEUE_DEFAULT_DEPTH;
    }
    // One queued sample costs its entry plus the shell it is decoded into, so that pair is what the
    // budget is divided by - counting only the entry would understate it by the size of the
    // message, which for a large type is essentially all of it.
    unsigned long long per_sample =
        (unsigned long long)sizeof(rmw_tickle_queued_message_t) + (unsigned long long)callbacks->ros_struct_size;
    unsigned long long capacity = rmw_tickle_reader_keep_all_budget_bytes() / per_sample;
    if (capacity < 1) {
        capacity = 1; // a type larger than the whole budget still gets somewhere to put one
    }
    if (capacity > RMW_TICKLE_READER_KEEP_ALL_MAX_DEPTH) {
        capacity = RMW_TICKLE_READER_KEEP_ALL_MAX_DEPTH;
    }
    return (size_t)capacity;
}

// g13 (RMW_GAPS_PLAN.md) - core asks this before it records anything about an arriving sample
// (tt_SUBSCRIBER_ACCEPT_CALLBACK, tickle.h), and only a KEEP_ALL subscription sets it. Returning
// false means "nowhere to put it": core then never receives the sample at all, so a RELIABLE writer
// still holds it and the ordinary gap exchange brings it back once the application has taken
// something. That is what lets a bounded queue refuse to overwrite an unread sample without losing
// one. A BEST_EFFORT writer has nothing to bring it back with, so there the decline is a drop -
// counted and warned about by core either way (tt_Subscriber.accept_declines).
//
// THE STALL THIS BUYS, stated where it is caused: an application that stops taking will, once this
// queue is full, stop its RELIABLE publishers. That is what "never destroy an unread sample" means
// and what rosbag2 asks for, but it presents as a publisher that has stopped making progress.
//
// Lock order: this runs on the poll thread with the node lock already held, and takes queue_mutex
// underneath it, exactly as subscriber_callback() below does. rmw_take() takes queue_mutex alone
// and only ever lowers queue_count, so a decision made here cannot be invalidated before
// subscriber_callback() acts on it - room found stays room.
static bool subscriber_accept(struct tt_Subscriber* subscriber, uint32_t seq_no, void* param) {
    (void)subscriber;
    (void)seq_no;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)param;
    pthread_mutex_lock(&sub_impl->queue_mutex);
    bool room = sub_impl->queue_count < sub_impl->queue_limit;
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    return room;
}

// Loaned messages (docs/RMW.md, "Loaned messages"): whether `payload` lies in one of the context's receive buffers -
// its inline rx_buffer or the pool rmw_tickle attached - rather than in a ring slot or in storage core copied it into.
static bool in_receive_buffer(const rmw_tickle_context_impl_t* context_impl, const uint8_t* payload) {
    const uint8_t* inline_buffer = context_impl->tickle_context.rx_buffer;
    if (payload >= inline_buffer && payload < inline_buffer + sizeof(context_impl->tickle_context.rx_buffer)) {
        return true;
    }
    const uint8_t* pool = (const uint8_t*)context_impl->loan_rx_pool;
    return NULL != pool && payload >= pool &&
           payload < pool + ((size_t)RMW_TICKLE_LOAN_RX_POOL_BUFFERS * tt_RX_POOL_BUFFER_BYTES);
}

// Loaned messages: keeps the sample being delivered where it arrived, when its bytes there can be read as the message
// itself - native byte order, exactly the type's in-place size, aligned for it - and fills *in_place, *sample and
// *psn. False, having kept nothing: the caller decodes it into a shell as for any subscription. Runs on the
// delivering thread, inside the callback, which is the only place core lets a sample be retained.
//
// A sample in a ring slot is kept only with loan_ring_slots: the slot stops its ring one lap later for every writer
// into this context (DESIGN.md 10), and a loan lasts as long as the application holds it. A refusal from core -
// a sample put together from fragments, released from a reorder buffer, delivered locally, or no handle or spare
// buffer left - is the same false.
static bool retain_in_place(rmw_tickle_subscriber_t* sub_impl, struct tt_Subscriber* tt_sub,
                            const struct payload_view* view, uint64_t* psn, const void** in_place,
                            struct tt_Sample* sample) {
    if (!sub_impl->loans || !view->is_native) {
        return false;
    }
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = sub_impl->callbacks;
    uint32_t header = rmw_tickle_psn_read(view->payload, view->length, view->is_native, psn);
    if (0 == header || view->length - header != callbacks->inplace_bytes) {
        return false;
    }
    const uint8_t* message = view->payload + header;
    if (((uintptr_t)message & (callbacks->ros_struct_align - 1U)) != 0) {
        return false;
    }
    if (!sub_impl->loan_ring_slots && !in_receive_buffer(sub_impl->node->context_impl, view->payload)) {
        return false;
    }
    pthread_mutex_lock(&sub_impl->queue_mutex);
    bool room = sub_impl->retained_queued < RMW_TICKLE_LOAN_RETAIN_PER_SUBSCRIPTION;
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    if (!room || tt_RET_OK != tt_Sample_retain(tt_sub, sample)) {
        return false;
    }
    *in_place = message;
    return true;
}

// Gives back a retained sample. Never with queue_mutex held: the release takes the node lock, and the poll thread
// takes the two the other way round.
static void release_sample(rmw_tickle_subscriber_t* sub_impl, struct tt_Sample* sample) {
    if (0 != sample->handle) {
        (void)tt_Sample_release(&sub_impl->node->context_impl->tickle_context, sample);
    }
}

static void subscriber_callback(struct tt_Subscriber* tt_sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)seq_no; // core's: counts datagrams once messages fragment - the psn comes from rmw_tickle's own header
    TT_TRACE(tt_TRACE_DELIVER);
    rmw_tickle_subscriber_t* sub_impl =
        (rmw_tickle_subscriber_t*)((char*)tt_sub - offsetof(rmw_tickle_subscriber_t, tickle_subscriber));
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = sub_impl->callbacks;
    uint64_t publication_sequence_number = 0;
    const void* in_place = NULL;
    struct tt_Sample sample = {.payload = NULL, .length = 0, .handle = 0, .is_native_endian = true};
    void* ros_message = NULL;

    // Loaned messages: a sample kept where it arrived needs no shell and no decode.
    if (!retain_in_place(sub_impl, tt_sub, (const struct payload_view*)data, &publication_sequence_number, &in_place,
                         &sample)) {
        // Milestone 45 - reuse an already-zeroed shell from the pool instead of a fresh zero_allocate()
        // when one's available (see shell_pool's own doc comment, rmw_tickle.h) - falls back to a real
        // allocation exactly as before whenever the pool's empty (e.g. before any rmw_take() has ever
        // returned one, or a sustained burst deeper than queue_capacity). Taken before the decode now,
        // because the decode writes into it (LARGE_MESSAGE_PLAN.md stage 1).
        pthread_mutex_lock(&sub_impl->queue_mutex);
        ros_message = shell_pool_pop(sub_impl);
        pthread_mutex_unlock(&sub_impl->queue_mutex);
        if (NULL == ros_message) {
            ros_message = rmw_tickle_ros_message_create(callbacks, &sub_impl->allocator);
            if (NULL == ros_message) {
                return; // Nothing more useful to do from inside a poll-thread callback - drop silently.
            }
        }

        if (!decode_with_psn(sub_impl, (const struct payload_view*)data, &publication_sequence_number, ros_message)) {
            // Not an rmw_tickle message, or its CDR does not decode: nothing to hand up. The shell goes
            // back, zeroed on the way (shell_pool_push) - a half-written one must not be handed out.
            pthread_mutex_lock(&sub_impl->queue_mutex);
            shell_pool_push(sub_impl, ros_message);
            pthread_mutex_unlock(&sub_impl->queue_mutex);
            return;
        }
    }
    count_messages_lost(sub_impl, publication_sequence_number); // (g3) MESSAGE_LOST
    TT_TRACE(tt_TRACE_DECODED);
    TT_TRACE(tt_TRACE_CONVERTED);

    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_subscriber_t.last_activity_time's own doc
    // comment. This function already runs under the node lock (its own module doc comment above),
    // the same lock check_subscription_deadline() reads this under - harmless to set even when
    // deadline_period_ns is 0 (unused in that case).
    sub_impl->last_activity_time = tt_get_ns();

    struct tt_Sample evicted = {.payload = NULL, .length = 0, .handle = 0, .is_native_endian = true};
    pthread_mutex_lock(&sub_impl->queue_mutex);
    if (sub_impl->queue_count == sub_impl->queue_capacity) {
        if (sub_impl->keep_all) {
            // g13 - unreachable in the ordinary course, because subscriber_accept() declined this
            // sample before core recorded it, rmw_take() only ever makes room, and the queue holds
            // RMW_TICKLE_KEEP_ALL_HEADROOM beyond the admission limit for samples released from core's
            // reorder buffer (queue_limit). Kept because
            // "never destroy an unread sample" is the whole promise of KEEP_ALL, and the one thing
            // that must not happen if it is ever reached is the eviction below. Drop the arriving
            // sample instead, and give its shell back rather than leaking it.
            //
            // Counted and said once, not merely commented: reaching this means the hook was not
            // consulted, and then this is silent loss of a sample the writer has been told was
            // received. A branch that says it was reached is the difference between finding that
            // out and not.
            sub_impl->keep_all_unconsulted_drops++;
            bool say_once = 1 == sub_impl->keep_all_unconsulted_drops;
            if (NULL != ros_message) {
                shell_pool_push(sub_impl, ros_message);
            }
            pthread_mutex_unlock(&sub_impl->queue_mutex);
            release_sample(sub_impl, &sample);
            if (say_once) {
                RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                                       "subscription %s: KEEP_ALL queue full at the enqueue, which means the accept "
                                       "hook was not consulted - dropping this sample. This should be unreachable; "
                                       "a sample the writer considers delivered has been lost",
                                       sub_impl->rmw_subscription.topic_name);
            }
            return;
        }
        // KEEP_LAST behavior - drop the oldest queued message to make room for this one, back into
        // the pool rather than freeing it outright (Milestone 45).
        rmw_tickle_queued_message_t* oldest = &sub_impl->queue[sub_impl->queue_head];
        if (NULL != oldest->in_place) {
            evicted = oldest->sample; // released below, once queue_mutex is
            sub_impl->retained_queued--;
        } else {
            shell_pool_push(sub_impl, oldest->ros_message);
        }
        sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
        sub_impl->queue_count--;
    }
    size_t tail_index = (sub_impl->queue_head + sub_impl->queue_count) % sub_impl->queue_capacity;
    sub_impl->queue[tail_index].ros_message = ros_message;
    sub_impl->queue[tail_index].in_place = in_place;
    sub_impl->queue[tail_index].sample = sample;
    if (NULL != in_place) {
        sub_impl->retained_queued++;
    }
    sub_impl->queue[tail_index].source_timestamp =
        time; // publisher's own wire timestamp - see tickle.c's process_data()
    sub_impl->queue[tail_index].received_timestamp = tt_get_ns();
    sub_impl->queue[tail_index].publication_sequence_number = publication_sequence_number;
    sub_impl->queue[tail_index].reception_sequence_number = sub_impl->reception_sequence_number++;
    // Valid only inside this callback, so it is read here and not at take time.
    tt_Subscriber_delivering_writer(tt_sub, &sub_impl->queue[tail_index].sender_node_id,
                                    &sub_impl->queue[tail_index].sender_entity_id);
    sub_impl->queue_count++;
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    release_sample(sub_impl, &evicted);

    // Wake anyone blocked in rmw_wait() on this queue becoming non-empty - wake_wait_cond()'s own
    // doc comment explains why the broadcast must happen under wait_mutex even though queue_count
    // itself is guarded by the separate queue_mutex above.
    wake_wait_cond(sub_impl->node->context_impl);
    // (g2) An EventsExecutor's callback, after the queue's mutex is released, so it may take the message.
    rmw_tickle_callback_slot_notify(&sub_impl->on_new_message, 1);
    TT_TRACE(tt_TRACE_SIGNALED);
}

// QoS roadmap #2 (DEADLINE) - see rmw_publisher.c's own check_publisher_deadline() doc comment,
// same reasoning/threading, for a Subscription's REQUESTED_DEADLINE_MISSED instead.
static void check_subscription_deadline(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)param;
    if (time - sub_impl->last_activity_time >= sub_impl->deadline_period_ns) {
        atomic_fetch_add(&sub_impl->deadline_missed.total_count, 1);
        atomic_fetch_add(&sub_impl->deadline_missed.unread_count, 1);
        wake_wait_cond(sub_impl->node->context_impl);                            // see its own doc comment
        rmw_tickle_callback_slot_notify(&sub_impl->deadline_missed.callback, 1); // (g2)
    }
    // Deadline monitoring simply stops here on a reschedule failure - see rmw_publisher.c's own
    // check_publisher_deadline() doc comment on this same pattern.
    (void)tt_Context_schedule(&sub_impl->node->context_impl->tickle_context, time + sub_impl->deadline_period_ns,
                              check_subscription_deadline, sub_impl);
}

// QoS roadmap #3 (LIVELINESS) - RMW_EVENT_LIVELINESS_CHANGED: "how many Publishers on my topic are
// alive right now" (rmw_tickle_count_matching_locked(), rmw_graph.c), turned into alive/not-alive
// counts. Called with the node lock held, from the core's discovery callback whenever a Publisher appears,
// lapses, revives or departs (rmw_node.c's discovery_callback(), LIVELINESS_PLAN.md amendment 3) - the
// core's verdict is timed at the lease expiry, so the event is too. Until 2026-09-26 this re-scanned every
// lease instead, which put a second lease of delay on top of the core's.
void rmw_tickle_update_subscription_liveliness_locked(rmw_tickle_subscriber_t* sub_impl) {
    size_t current = rmw_tickle_count_matching_locked(sub_impl->node->context_impl,
                                                      sub_impl->rmw_subscription.topic_name, tt_KIND_TOPIC_PUBLISHER);
    // QoS roadmap #3 follow-up - the live not_alive_count snapshot (tombstoned Publishers on this
    // topic, struct tt_DiscoveredEntity.alive's own doc comment, tickle.h), independent of the
    // alive_count-delta-based not_alive.total_count/unread_count tracking just below (which stays
    // as-is - that's still the right way to derive "how many *went* not-alive", this is just the
    // separate "how many are *currently* not-alive" question rmw_liveliness_changed_status_t's own
    // not_alive_count field asks).
    size_t current_not_alive = rmw_tickle_count_not_alive_matching_locked(
        sub_impl->node->context_impl, sub_impl->rmw_subscription.topic_name, tt_KIND_TOPIC_PUBLISHER);
    rmw_tickle_liveliness_changed_status_t* status = &sub_impl->liveliness_changed;
    if ((int)current > status->last_alive_count) {
        int delta = (int)current - status->last_alive_count;
        atomic_fetch_add(&status->alive.total_count, delta);
        atomic_fetch_add(&status->alive.unread_count, delta);
        wake_wait_cond(sub_impl->node->context_impl);
        rmw_tickle_callback_slot_notify(&status->alive.callback, (size_t)delta); // (g2)
    } else if ((int)current < status->last_alive_count) {
        int delta = status->last_alive_count - (int)current;
        atomic_fetch_add(&status->not_alive.total_count, delta);
        atomic_fetch_add(&status->not_alive.unread_count, delta);
        wake_wait_cond(sub_impl->node->context_impl);
        rmw_tickle_callback_slot_notify(&status->alive.callback, (size_t)delta); // (g2) one slot for the event
    }
    atomic_store(&status->alive_count, (int)current);
    atomic_store(&status->not_alive_count, (int)current_not_alive);
    status->last_alive_count = (int)current;
}

// Milestone 31/28(a) observability follow-on - how often check_subscription_qos_incompatible()
// below re-scans the discovery table. See rmw_publisher.c's own identically-named/valued macro
// for the full reasoning (shared cadence, not a shared symbol - each file defines its own, the
// same convention RMW_TICKLE_WATCHDOG_CHECK_INTERVAL_NS already uses).
#define RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS tt_CONTEXT_UPDATE_INTERVAL

// Bytes one reorder slot needs, for a subscription on this type.
//
// Same two sources, same precedence, and for the same reason as the publisher's own
// resolve_keep_all_record_bytes() (rmw_publisher.c, which spells the reasoning out in full): the
// generated per-type maximum when the generator could compute one, tt_MAX_BUFFER_LENGTH when it
// could not. There is nothing a human knows about a bounded type that beats a computed bound on
// it.
//
// Under-sizing is safe rather than merely tolerable: a payload too large for the stride is
// treated exactly like a full buffer, so the sample is re-requested instead of held. That costs
// retransmissions and is counted (reorder_overflow), never correctness.
//
// Capped at RMW_TICKLE_REORDER_SLOT_PAYLOAD (default 2 KiB) whatever the type (the storage design
// the user approved on 2026-09-24): with tt_MAX_BUFFER_LENGTH at 65507 an unbounded type would
// otherwise reserve a 64 KiB slot, times a window of up to 4096. A sample larger than the stride
// takes the overflow path above - re-requested rather than held - which costs a retransmission
// only when a large sample arrives out of order.
#define RMW_TICKLE_REORDER_SLOT_PAYLOAD_DEFAULT 2048ULL
// Total bytes of reorder buffer one RELIABLE subscription may reserve (RMW_TICKLE_REORDER_BYTES):
// slots are the window or budget / stride, whichever is fewer - Plan's review of the design, since
// a stride cap alone still leaves window x 2 KiB per subscription. Fewer slots than the window just
// means an earlier overflow, which is the same safe path.
//
// The default is exactly what an unbounded type reserved before any budget existed: a full window
// of slots sized for the standard 1472-byte datagram (~1.5 MiB). So at today's
// tt_MAX_BUFFER_LENGTH nothing changes - a first cut used 1 MiB, which quietly cut an unbounded
// type's window from 1024 to ~700 slots (Plan's review) - and at 65507 the same ~1.5 MiB caps it.
#define RMW_TICKLE_REORDER_BYTES_DEFAULT \
    ((unsigned long long)RMW_TICKLE_REORDER_SLOTS * (sizeof(struct tt_ReorderSlot) + tt_ETHERNET_UDP_PAYLOAD))

// An environment knob of this file: `fallback` when unset, empty, unparseable, zero or above
// `max` - a malformed tuning value must not stop a node starting (the rule every knob here uses).
static unsigned long long resolve_env_bytes(const char* name, unsigned long long fallback, unsigned long long max) {
    const char* env = getenv(name);
    if (NULL == env || '\0' == env[0]) {
        return fallback;
    }
    char* end = NULL;
    unsigned long long value = strtoull(env, &end, 10);
    if (end == env || (end != NULL && '\0' != *end) || value == 0 || value > max) {
        return fallback;
    }
    return value;
}

static uint16_t resolve_reorder_slot_bytes(const rmw_tickle_subscriber_t* sub_impl) {
    unsigned long long payload = (unsigned long long)tt_MAX_BUFFER_LENGTH;
    if (NULL != sub_impl->callbacks &&
        ROSIDL_TYPESUPPORT_TICKLE_C_ENCODED_SIZE_UNBOUNDED != sub_impl->callbacks->tickle_max_encoded_size) {
        payload = (unsigned long long)sub_impl->callbacks->tickle_max_encoded_size;
    }
    payload += RMW_TICKLE_PSN_BYTES; // every message carries rmw_tickle's header ahead of its CDR
    // Whatever the type, a slot never holds more than one control datagram's payload: a larger message
    // is held fragment by fragment, one per slot (DATAFRAG_PLAN.md section 13), and a whole DATA is
    // at most a control datagram. So a large type is held rather than re-requested, in slots of 1.5 KiB.
    unsigned long long datagram_payload = (unsigned long long)tt_CONTROL_MAX_LENGTH;
    if (payload > datagram_payload) {
        payload = datagram_payload;
    }
    // tt_Subscriber.reorder_slot_bytes is 16 bits: the cap keeps a slot, header included, within it.
    unsigned long long cap =
        resolve_env_bytes("RMW_TICKLE_REORDER_SLOT_PAYLOAD", RMW_TICKLE_REORDER_SLOT_PAYLOAD_DEFAULT,
                          UINT16_MAX - sizeof(struct tt_ReorderSlot));
    if (payload > cap) {
        payload = cap;
    }
    return (uint16_t)(sizeof(struct tt_ReorderSlot) + payload);
}

// How many slots, defaulting to the window bound above. RMW_TICKLE_REORDER_SLOTS trades memory
// back for retransmissions; 0 or unparseable falls back rather than failing subscription
// creation, the same reasoning the publisher's own knobs use - a malformed tuning value should not
// stop a node starting, and this one can only cost throughput.
static uint16_t resolve_reorder_slots(uint16_t slot_bytes) {
    unsigned long long slots =
        resolve_env_bytes("RMW_TICKLE_REORDER_SLOTS", RMW_TICKLE_REORDER_SLOTS, RMW_TICKLE_REORDER_SLOTS);
    unsigned long long budget =
        resolve_env_bytes("RMW_TICKLE_REORDER_BYTES", RMW_TICKLE_REORDER_BYTES_DEFAULT, UINT32_MAX);
    unsigned long long affordable = budget / slot_bytes;
    if (affordable < slots) {
        slots = affordable > 0 ? affordable : 1;
    }
    return (uint16_t)slots;
}

// Milestone 31/28(a) observability follow-on - RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE's own
// periodic check, the Subscription-side counterpart to rmw_publisher.c's own check_publisher_qos_
// incompatible() - see its own doc comment for the full reasoning (no wire-level trigger exists,
// so this periodically re-derives a live count from the existing discovery table instead, the
// same pattern check_subscription_liveliness() above already established).
static void check_subscription_qos_incompatible(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)param;
    rmw_qos_policy_kind_t last_kind = RMW_QOS_POLICY_INVALID;
    size_t current = rmw_tickle_count_incompatible_publishers_locked(
        sub_impl->node->context_impl, sub_impl->rmw_subscription.topic_name, sub_impl->tickle_subscriber.reliable,
        sub_impl->tickle_subscriber.durable, sub_impl->tickle_subscriber.liveliness_manual,
        sub_impl->tickle_subscriber.deadline_duration_ns, sub_impl->tickle_subscriber.liveliness_lease_duration_ns,
        &last_kind);
    rmw_tickle_qos_incompatible_status_t* status = &sub_impl->requested_qos_incompatible;
    if ((int)current > status->last_incompatible_count) {
        int delta = (int)current - status->last_incompatible_count;
        atomic_fetch_add(&status->base.total_count, delta);
        atomic_fetch_add(&status->base.unread_count, delta);
        status->last_policy_kind = last_kind;
        wake_wait_cond(sub_impl->node->context_impl);
        rmw_tickle_callback_slot_notify(&status->base.callback, (size_t)delta); // (g2)
    }
    // See check_publisher_qos_incompatible()'s own identical comment - deliberately never
    // decrements total_count/unread_count on a drop, only tracks the high-water mark to detect a
    // later re-appearance as a genuinely new incompatible match.
    status->last_incompatible_count = (int)current;

    // Monitoring simply stops here on a reschedule failure - same reasoning as check_subscription_
    // deadline()'s own identical pattern above.
    (void)tt_Context_schedule(&sub_impl->node->context_impl->tickle_context,
                              time + RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS, check_subscription_qos_incompatible,
                              sub_impl);
}

rmw_subscription_t* rmw_create_subscription(const rmw_node_t* node, const rosidl_message_type_support_t* type_support,
                                            const char* topic_name, const rmw_qos_profile_t* qos_profile,
                                            const rmw_subscription_options_t* subscription_options) {
    if (NULL == node) {
        RMW_SET_ERROR_MSG("node is null");
        return NULL;
    }
    if (NULL == topic_name) {
        RMW_SET_ERROR_MSG("topic_name is null");
        return NULL;
    }
    if (NULL == qos_profile) {
        RMW_SET_ERROR_MSG("qos_profile is null");
        return NULL;
    }
    if (NULL == subscription_options) {
        RMW_SET_ERROR_MSG("subscription_options is null");
        return NULL;
    }
    if (!rmw_tickle_identifier_matches(node->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return NULL;
    }
    if (rmw_tickle_validate_qos_profile(qos_profile, RMW_TICKLE_ENTITY_SUBSCRIPTION) != RMW_RET_OK) {
        return NULL; // error message already set
    }

    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks =
        rmw_tickle_get_message_callbacks(type_support, "subscription", topic_name);
    if (NULL == callbacks || !rmw_tickle_check_callbacks_usable(callbacks)) {
        return NULL; // already set, and it names this endpoint
    }

    rmw_tickle_node_t* node_impl = (rmw_tickle_node_t*)node->data;
    rcutils_allocator_t* allocator = &node_impl->allocator;

    // DDS QoS policy coverage inventory gap 1 (rmw_tickle/PLAN.md, 2026-09-21) - see rmw_publisher.
    // c's own rmw_create_publisher() for the identical Publisher-side counterpart and the full
    // reasoning (rmw_tickle_resolve_best_available()'s own doc comment, rmw_tickle.h, has the
    // algorithm). Reassigning the qos_profile parameter itself keeps every downstream reference
    // below already correct with no further edits needed.
    tt_Context_lock(&node_impl->context_impl->tickle_context);
    rmw_qos_profile_t resolved_qos = rmw_tickle_resolve_best_available(qos_profile, &node_impl->context_impl->discovery,
                                                                       topic_name, RMW_TICKLE_ENTITY_SUBSCRIPTION);
    tt_Context_unlock(&node_impl->context_impl->tickle_context);
    qos_profile = &resolved_qos;

    rmw_tickle_subscriber_t* sub_impl =
        (rmw_tickle_subscriber_t*)allocator->zero_allocate(1, sizeof(rmw_tickle_subscriber_t), allocator->state);
    if (NULL == sub_impl) {
        RMW_SET_ERROR_MSG("failed to allocate rmw_tickle_subscriber_t");
        return NULL;
    }
    sub_impl->node = node_impl;
    // Milestone 34 - see rmw_tickle_publisher_t.owning_node_name's own doc comment.
    sub_impl->owning_node_name = rcutils_strdup(node_impl->rmw_node.name, *allocator);
    sub_impl->owning_node_namespace = rcutils_strdup(node_impl->rmw_node.namespace_, *allocator);
    sub_impl->type_support = type_support;
    sub_impl->callbacks = callbacks;
    sub_impl->allocator = *allocator;
    sub_impl->qos = *qos_profile;

    sub_impl->topic.name = callbacks->ros_type_name; // see rmw_tickle_publisher_t.topic's own doc comment
    sub_impl->topic.data_size = (uint32_t)callbacks->tickle_struct_size;
    sub_impl->topic.data_encode_size = callbacks->tickle_encode_size;
    sub_impl->topic.data_encode = callbacks->tickle_encode;
    // The payload is handed over as it is (view_payload()), and subscriber_callback() decodes it past
    // rmw_tickle's per-message header into decode_scratch, then frees what the decode allocated.
    sub_impl->topic.data_decode_inplace = view_payload;
    sub_impl->topic.data_decode = refuse_copying_decode;
    sub_impl->topic.data_free = free_nothing;

    // Milestone 7's QoS roadmap item #1 (HISTORY/DEPTH): qos_profile->depth sizes the queue for
    // real, rather than a fixed compile-time bound - RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT (0, unset)
    // falls back to the previous placeholder default.
    //
    // g13 (RMW_GAPS_PLAN.md): KEEP_ALL ignores `depth` - DDS does not define one for it - and sizes
    // the queue from a byte budget instead, which is the bound DDS actually puts on KEEP_ALL
    // (RESOURCE_LIMITS). What makes it KEEP_ALL rather than a deep KEEP_LAST is not the number but
    // what a full queue does: it declines the arriving sample instead of destroying an unread one.
    sub_impl->keep_all = RMW_QOS_POLICY_HISTORY_KEEP_ALL == qos_profile->history;
    sub_impl->queue_limit = resolve_queue_capacity(qos_profile, callbacks);
    sub_impl->queue_capacity = sub_impl->queue_limit + (sub_impl->keep_all ? RMW_TICKLE_KEEP_ALL_HEADROOM : 0);
    sub_impl->queue = (rmw_tickle_queued_message_t*)allocator->zero_allocate(
        sub_impl->queue_capacity, sizeof(rmw_tickle_queued_message_t), allocator->state);
    if (NULL == sub_impl->queue) {
        RMW_SET_ERROR_MSG("failed to allocate subscriber queue");
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    // Phase 2 - the RELIABLE tracking window (see rmw_tickle_subscriber_t.tracking_bitmaps):
    // RMW_TICKLE_TRACKING_WORDS words per tracked writer, tt_MAX_PEER_COUNT of them.
    sub_impl->tracking_bitmaps = (uint64_t*)allocator->zero_allocate(
        (size_t)tt_MAX_PEER_COUNT * RMW_TICKLE_TRACKING_WORDS, sizeof(uint64_t), allocator->state);
    if (NULL == sub_impl->tracking_bitmaps) {
        RMW_SET_ERROR_MSG("failed to allocate subscriber tracking bitmaps");
        allocator->deallocate(sub_impl->queue, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    // The RELIABLE reorder buffer (see rmw_tickle_subscription_t.reorder_storage) - for a RELIABLE
    // subscription only. It used to be allocated for every one, on the grounds that a buffer a
    // BEST_EFFORT Subscriber never reads costs memory and nothing else; once each buffer is up to a
    // megabyte (the byte budget above), that memory is the point. The decision reads the same
    // resolved QoS `tickle_subscriber.reliable` is set from below, so the two cannot disagree.
    bool reorder_wanted = RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability;
    uint16_t reorder_slot_bytes = reorder_wanted ? resolve_reorder_slot_bytes(sub_impl) : 0;
    uint16_t reorder_slots = reorder_wanted ? resolve_reorder_slots(reorder_slot_bytes) : 0;
    size_t reorder_words = ((size_t)reorder_slots * reorder_slot_bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    sub_impl->reorder_storage =
        reorder_wanted ? (uint64_t*)allocator->zero_allocate(reorder_words, sizeof(uint64_t), allocator->state) : NULL;
    if (reorder_wanted && NULL == sub_impl->reorder_storage) {
        RMW_SET_ERROR_MSG("failed to allocate subscriber reorder buffer");
        allocator->deallocate(sub_impl->tracking_bitmaps, allocator->state);
        allocator->deallocate(sub_impl->queue, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    // Milestone 45 - shell_pool's own doc comment (rmw_tickle.h). Sized queue_capacity, same as
    // queue[] itself - the most shells that can ever be genuinely in flight at once.
    sub_impl->shell_pool = (void**)allocator->zero_allocate(sub_impl->queue_capacity, sizeof(void*), allocator->state);
    // Only the fallback needs it: with a direct codec the payload decodes straight into a pooled
    // shell, so this subscription keeps no message-sized buffer of its own.
    sub_impl->decode_scratch = NULL != callbacks->direct_decode
                                   ? NULL
                                   : allocator->zero_allocate(1, callbacks->tickle_struct_size, allocator->state);
    if (NULL == sub_impl->shell_pool || (NULL == sub_impl->decode_scratch && NULL == callbacks->direct_decode)) {
        RMW_SET_ERROR_MSG("failed to allocate subscriber shell_pool");
        allocator->deallocate((void*)sub_impl->shell_pool, allocator->state);
        allocator->deallocate(sub_impl->decode_scratch, allocator->state);
        allocator->deallocate(sub_impl->queue, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    // (g2) Its callback slots, before anything can be delivered to it.
    rmw_tickle_callback_slot_init(&sub_impl->on_new_message);
    rmw_tickle_callback_slot_init(&sub_impl->deadline_missed.callback);
    rmw_tickle_callback_slot_init(&sub_impl->liveliness_changed.alive.callback);
    rmw_tickle_callback_slot_init(&sub_impl->requested_qos_incompatible.base.callback);
    rmw_tickle_callback_slot_init(&sub_impl->matched.base.callback);           // (g3)
    rmw_tickle_callback_slot_init(&sub_impl->incompatible_type.base.callback); // (g3)
    rmw_tickle_callback_slot_init(&sub_impl->message_lost.callback);           // (g3)
    if (pthread_mutex_init(&sub_impl->queue_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize subscriber queue mutex");
        allocator->deallocate((void*)sub_impl->shell_pool, allocator->state);
        allocator->deallocate(sub_impl->decode_scratch, allocator->state);
        allocator->deallocate(sub_impl->reorder_storage, allocator->state);
        allocator->deallocate(sub_impl->tracking_bitmaps, allocator->state); // Phase 2
        allocator->deallocate(sub_impl->queue, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    sub_impl->rmw_subscription.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    sub_impl->rmw_subscription.data = sub_impl;
    sub_impl->rmw_subscription.topic_name = rcutils_strdup(topic_name, *allocator);
    sub_impl->rmw_subscription.options = *subscription_options;
    sub_impl->rmw_subscription.can_loan_messages = setup_loans(sub_impl);
    sub_impl->rmw_subscription.is_cft_enabled = false;
    if (NULL == sub_impl->rmw_subscription.topic_name) {
        RMW_SET_ERROR_MSG("failed to allocate topic_name");
        pthread_mutex_destroy(&sub_impl->queue_mutex);
        allocator->deallocate((void*)sub_impl->shell_pool, allocator->state);
        allocator->deallocate(sub_impl->decode_scratch, allocator->state);
        allocator->deallocate(sub_impl->reorder_storage, allocator->state);
        allocator->deallocate(sub_impl->tracking_bitmaps, allocator->state); // Phase 2
        allocator->deallocate(sub_impl->queue, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    // Held from the create until every announced field is final (reliable, durable, deadline, liveliness), released
    // after the match count below or on the failure path. Released in between, the poll thread could announce this
    // subscription with the create's reset QoS under its new generation, and the generation dedup would keep that
    // wrong record on the remote side for good - see rmw_publisher.c's rmw_create_publisher(), where it was found.
    tt_Context_lock(&node_impl->context_impl->tickle_context);
    tt_ret_t ret = tt_Node_create_subscriber(node_impl->core_node, &sub_impl->tickle_subscriber, &sub_impl->topic,
                                             sub_impl->rmw_subscription.topic_name, subscriber_callback);
    if (ret != tt_RET_OK) {
        tt_Context_unlock(&node_impl->context_impl->tickle_context);
        RMW_SET_ERROR_MSG("tt_Context_create_subscriber() failed");
        pthread_mutex_destroy(&sub_impl->queue_mutex);
        allocator->deallocate((char*)sub_impl->rmw_subscription.topic_name, allocator->state);
        allocator->deallocate((void*)sub_impl->shell_pool, allocator->state);
        allocator->deallocate(sub_impl->decode_scratch, allocator->state);
        allocator->deallocate(sub_impl->reorder_storage, allocator->state);
        allocator->deallocate(sub_impl->tracking_bitmaps, allocator->state); // Phase 2
        allocator->deallocate(sub_impl->queue, allocator->state);
        allocator->deallocate(sub_impl, allocator->state);
        return NULL;
    }

    // Caller-owned storage is attached AFTER tt_Context_create_subscriber(), never before.
    //
    // That function initialises the fields it owns, which includes setting reorder_storage back to
    // NULL - so a buffer attached beforehand is silently discarded, and the Subscriber runs the
    // no-buffer fallback while every pointer here still looks correct. Found by measurement rather
    // than by reading: under 8% injected loss a RELIABLE subscription reported
    // reorder_overflow=47195 with reorder_held_peak=0 - forty-seven thousand samples re-requested
    // by a buffer that never held one - and receive throughput collapsed to a quarter.
    //
    // tracking_bitmaps moved with it. It happened to work where it was, because
    // tt_Context_create_subscriber() did not zero that pair - a property of core's init list rather
    // than a contract, and the warning here said so. Since 2026-09-24 it does zero it (NULL/0 is
    // the documented default, and leaving it unset handed non-zeroed callers a garbage pointer), so
    // attaching it before create would now be discarded exactly like reorder_storage was.
    sub_impl->tickle_subscriber.tracking_bitmaps = sub_impl->tracking_bitmaps;
    sub_impl->tickle_subscriber.tracking_words = RMW_TICKLE_TRACKING_WORDS;
    sub_impl->tickle_subscriber.reorder_storage = sub_impl->reorder_storage;
    sub_impl->tickle_subscriber.reorder_slots = reorder_slots;
    sub_impl->tickle_subscriber.reorder_slot_bytes = reorder_slot_bytes;
    // Stated once per subscription rather than left to be inferred from a counter that reads 0 for
    // two different reasons. "reorder_held_peak=0" means either "nothing needed holding" or "this
    // subscription has no buffer", and telling those apart from outside cost an experiment.
    RCUTILS_LOG_DEBUG_NAMED("rmw_tickle", "subscription %s: reorder buffer %p, %u slots of %u bytes",
                            sub_impl->rmw_subscription.topic_name, (void*)sub_impl->reorder_storage,
                            (unsigned)reorder_slots, (unsigned)reorder_slot_bytes);

    // QoS roadmap #5 (RELIABILITY) - see tt_Subscriber.reliable's own doc comment (tickle.h).
    // Plain field access, no allocation needed (unlike the Publisher side's reliable_cache) -
    // process_data()/update_reliable_ack() (tickle.c) track ack state directly on tickle_
    // subscriber itself.
    // g13 - only a KEEP_ALL subscription sets the hook, so a KEEP_LAST one is byte for byte the
    // subscriber it was before g13 existed: core skips the call on a NULL pointer, and nothing else
    // on the path changed.
    sub_impl->tickle_subscriber.accept_callback = sub_impl->keep_all ? subscriber_accept : NULL;
    sub_impl->tickle_subscriber.accept_callback_param = sub_impl;
    sub_impl->tickle_subscriber.keep_last_depth = keep_last_depth_for(sub_impl);
    sub_impl->tickle_subscriber.reliable = RMW_QOS_POLICY_RELIABILITY_RELIABLE == qos_profile->reliability;

    // QoS roadmap #1 (RxO matching, Milestone 31) - see tt_Subscriber.durable's own doc comment
    // (tickle.h). Requesting TRANSIENT_LOCAL here is what actually activates process_data()'s own
    // subscriber_incompatible_with_writer() gate against a VOLATILE Publisher for a real ROS 2
    // Subscription - without this, every rmw_tickle Subscription would keep tickle_subscriber.
    // durable at its zero_allocate() default (false) regardless of its own requested QoS, and the
    // new gate could never fire for rmw_tickle's own actual users.
    sub_impl->tickle_subscriber.durable = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL == qos_profile->durability;

    // QoS roadmap #2 (DEADLINE) - see rmw_tickle_subscriber_t.deadline_period_ns's own doc
    // comment. rmw_qos.c already accepted any finite qos.deadline; RMW_QOS_DEADLINE_DEFAULT
    // ({0,0}) leaves deadline_period_ns at its zero_allocate() default (0 = not requested).
    rmw_duration_t deadline_ns = rmw_time_total_nsec(qos_profile->deadline);
    if (deadline_ns > 0) {
        sub_impl->deadline_period_ns = (uint64_t)deadline_ns;
        sub_impl->last_activity_time = tt_get_ns();
        tt_Context_lock(&node_impl->context_impl->tickle_context);
        // A failure here just leaves deadline monitoring inactive for this Subscription - see
        // rmw_publisher.c's own check_publisher_deadline() doc comment on this same pattern.
        (void)tt_Context_schedule(&node_impl->context_impl->tickle_context, tt_get_ns() + sub_impl->deadline_period_ns,
                                  check_subscription_deadline, sub_impl);
        tt_Context_unlock(&node_impl->context_impl->tickle_context);
    }

    // QoS roadmap #6 (LIFESPAN) - see rmw_tickle_subscriber_t.lifespan_ns's own doc comment.
    // rmw_qos.c already accepted any finite qos.lifespan; RMW_QOS_LIFESPAN_DEFAULT ({0,0}) leaves
    // lifespan_ns at its zero_allocate() default (0 = not requested, no cost) - no scheduling
    // needed, unlike DEADLINE just above, since this is a plain age check rmw_take_with_info()
    // makes on demand rather than a periodic timer.
    rmw_duration_t lifespan_ns = rmw_time_total_nsec(qos_profile->lifespan);
    if (lifespan_ns > 0) {
        sub_impl->lifespan_ns = (uint64_t)lifespan_ns;
    }

    // QoS roadmap #2 (DEADLINE) / #3 (LIVELINESS) RxO, Milestone 49 - what this Subscription
    // announces on the wire (tickle_subscriber.deadline_duration_ns/liveliness_lease_duration_ns/
    // .liveliness_manual, tickle.h) as *requested*, computed directly from qos_profile here
    // rather than reusing sub_impl->deadline_period_ns/liveliness_lease_ns above: the latter stays
    // 0 until/unless the application actually requests the matching RMW_EVENT_* (rmw_subscription_
    // event_init()'s own lazy-start doc comment for LIVELINESS_CHANGED especially), but RxO
    // compatibility must reflect what was requested via QoS regardless of which events the
    // application happens to ask for. deadline_period_ns specifically is *not* lazy (set
    // unconditionally above), so reusing it here is safe and avoids a redundant conversion.
    sub_impl->tickle_subscriber.deadline_duration_ns = sub_impl->deadline_period_ns;
    sub_impl->tickle_subscriber.liveliness_manual =
        RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC == qos_profile->liveliness;
    sub_impl->tickle_subscriber.liveliness_lease_duration_ns =
        rmw_tickle_wire_lease_ns(qos_profile->liveliness_lease_duration);

    // (g3) Its QoS is complete only now: it, and the local Publishers on its topic, count their matches.
    tt_Context_lock(&node_impl->context_impl->tickle_context);
    rmw_tickle_update_matches_locked(node_impl->context_impl, sub_impl->rmw_subscription.topic_name, 0);
    tt_Context_unlock(&node_impl->context_impl->tickle_context);
    tt_Context_unlock(&node_impl->context_impl->tickle_context); // the create's: every announced field is final now
    // (g9) A durable subscription joining late gets the durable backlog of this process's own publishers on its
    // topic, as it would a remote one's - only now, with its durability set.
    tt_Subscriber_deliver_local_backlog(&sub_impl->tickle_subscriber);
    return &sub_impl->rmw_subscription;
}

rmw_ret_t rmw_destroy_subscription(rmw_node_t* node, rmw_subscription_t* subscription) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(node->implementation_identifier) ||
        !rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;

    tt_Context_lock(&sub_impl->node->context_impl->tickle_context);
    // QoS roadmap #2/#3 (DEADLINE/LIVELINESS) - cancel any still-armed check before the
    // subscriber it closes over is freed below; both are no-ops if never started (tt_Context_
    // unschedule() just finds nothing matching).
    if (sub_impl->deadline_period_ns != 0) {
        tt_Context_unschedule(&sub_impl->node->context_impl->tickle_context, check_subscription_deadline, sub_impl);
    }
    // The QoS-incompatible check reschedules itself every period once its event is initialised; left armed,
    // it ran on this freed subscription - a SIGSEGV in test_events one run in twenty (test_event_teardown.c).
    if (sub_impl->requested_qos_incompatible_monitoring_started) {
        tt_Context_unschedule(&sub_impl->node->context_impl->tickle_context, check_subscription_qos_incompatible,
                              sub_impl);
    }
    tt_Subscriber_destroy(&sub_impl->tickle_subscriber);
    // (g3) The Publishers it was matched with, and those it had another type from, recount without it.
    rmw_tickle_update_matches_locked(sub_impl->node->context_impl, sub_impl->rmw_subscription.topic_name,
                                     tt_KIND_TOPIC_PUBLISHER);
    tt_Context_unlock(&sub_impl->node->context_impl->tickle_context);

    // Drain anything still queued - rmw_take() never got to these. Freed directly, not pushed
    // through shell_pool_push() - the pool itself is about to be freed too, right below.
    //
    // Loaned messages: a queued sample kept where it arrived, and every loan the application has not returned, is given
    // back to core here - the subscription is gone, so nothing could return it later. A loan still held is the
    // application's error (rmw.h: return every loan before destroying the subscription); it is said, and its pointer is
    // invalid from now on. No poll thread delivery can race this: tt_Subscriber_destroy() above ran under the node
    // lock. The releases wait until queue_mutex is released (release_sample()).
    struct tt_Sample queued_samples[RMW_TICKLE_LOAN_RETAIN_PER_SUBSCRIPTION];
    size_t queued_sample_count = 0;
    pthread_mutex_lock(&sub_impl->queue_mutex);
    while (sub_impl->queue_count > 0) {
        rmw_tickle_queued_message_t* head = &sub_impl->queue[sub_impl->queue_head];
        if (NULL != head->in_place) {
            queued_samples[queued_sample_count++] = head->sample;
        } else {
            rmw_tickle_ros_message_destroy(sub_impl->callbacks, head->ros_message, &sub_impl->allocator);
        }
        sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
        sub_impl->queue_count--;
    }
    // One line per subscription that lent anything, so a measurement can say how many loans were read where they
    // arrived (rmw_loan_bench.sh reads it).
    if (sub_impl->loans_in_place + sub_impl->loans_copied > 0) {
        (void)fprintf(stderr, "rmw_tickle: subscription %s loans_in_place=%llu loans_copied=%llu\n",
                      sub_impl->rmw_subscription.topic_name, (unsigned long long)sub_impl->loans_in_place,
                      (unsigned long long)sub_impl->loans_copied);
    }
    if (sub_impl->loans_out_count > 0) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle", "subscription %s destroyed with %zu loaned message(s) not returned",
                               sub_impl->rmw_subscription.topic_name, sub_impl->loans_out_count);
    }
    size_t loans_held = sub_impl->loans_out_count;
    sub_impl->loans_out_count = 0;
    // Milestone 45 - every shell currently sitting in shell_pool (as opposed to still queued,
    // drained just above, or out with an application that already called rmw_take()) also needs
    // freeing here - nothing else ever will.
    while (sub_impl->shell_pool_count > 0) {
        rmw_tickle_ros_message_destroy(sub_impl->callbacks, sub_impl->shell_pool[--sub_impl->shell_pool_count],
                                       &sub_impl->allocator);
    }
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    for (size_t i = 0; i < queued_sample_count; i++) {
        release_sample(sub_impl, &queued_samples[i]);
    }
    for (size_t i = 0; i < loans_held; i++) {
        rmw_tickle_subscription_loan_t* loan = &sub_impl->loans_out[i];
        if (NULL != loan->shell) {
            rmw_tickle_ros_message_destroy(sub_impl->callbacks, loan->shell, &sub_impl->allocator);
        } else {
            release_sample(sub_impl, &loan->sample);
        }
    }
    pthread_mutex_destroy(&sub_impl->queue_mutex);
    rmw_tickle_callback_slot_fini(&sub_impl->on_new_message);
    rmw_tickle_callback_slot_fini(&sub_impl->deadline_missed.callback);
    rmw_tickle_callback_slot_fini(&sub_impl->liveliness_changed.alive.callback);
    rmw_tickle_callback_slot_fini(&sub_impl->requested_qos_incompatible.base.callback);
    rmw_tickle_callback_slot_fini(&sub_impl->matched.base.callback);
    rmw_tickle_callback_slot_fini(&sub_impl->incompatible_type.base.callback);
    rmw_tickle_callback_slot_fini(&sub_impl->message_lost.callback);

    rcutils_allocator_t allocator = sub_impl->allocator;
    allocator.deallocate((char*)sub_impl->rmw_subscription.topic_name, allocator.state);
    allocator.deallocate(sub_impl->queue, allocator.state);
    allocator.deallocate((void*)sub_impl->shell_pool, allocator.state); // Milestone 45
    allocator.deallocate(sub_impl->loans_out, allocator.state);
    allocator.deallocate(sub_impl->decode_scratch, allocator.state);
    allocator.deallocate(sub_impl->tracking_bitmaps, allocator.state); // Phase 2 - the tracking window
    allocator.deallocate(sub_impl->owning_node_name, allocator.state);
    allocator.deallocate(sub_impl->owning_node_namespace, allocator.state);
    allocator.deallocate(sub_impl, allocator.state);
    return RMW_RET_OK;
}

// What a take reports about the message it just handed over. Shared with (g1) the serialized take,
// which owes its caller the same information.
static void fill_message_info(const rmw_tickle_subscriber_t* sub_impl, const rmw_tickle_queued_message_t* entry,
                              rmw_message_info_t* message_info) {
    (void)sub_impl;
    message_info->source_timestamp = (rmw_time_point_value_t)entry->source_timestamp;
    message_info->received_timestamp = (rmw_time_point_value_t)entry->received_timestamp;
    message_info->publication_sequence_number = entry->publication_sequence_number;
    message_info->reception_sequence_number = entry->reception_sequence_number;
    // Until 2026-10-02 this reported sixteen zero bytes on every sample, so nothing could match a received
    // sample to the writer that sent it - the field's only purpose - while rmw_get_gid_for_publisher() was
    // returning a real per-instance id all along. Same layout as that function builds, from the same pair, so
    // a gid taken from the graph and a gid taken from a sample compare equal for one writer.
    memset(&message_info->publisher_gid, 0, sizeof(message_info->publisher_gid));
    message_info->publisher_gid.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    message_info->publisher_gid.data[0] = entry->sender_node_id;
    memcpy(&message_info->publisher_gid.data[1], &entry->sender_entity_id, sizeof(entry->sender_entity_id));
    message_info->from_intra_process = false;
}

// The head of the queue, or false when there is none. Shared by rmw_take_with_info() and (g1) the
// serialized take - the QoS bookkeeping in it is the same for both, and a second copy would drift.
//
// An entry holding a retained sample (loaned messages) leaves the queue here like any other; the caller releases it
// when done with its bytes. One that LIFESPAN drops is released here, after queue_mutex - there are at most
// RMW_TICKLE_LOAN_RETAIN_PER_SUBSCRIPTION of them in the queue.
//
// The work itself, with queue_mutex held: the samples LIFESPAN dropped go to `expired` (room for
// RMW_TICKLE_LOAN_RETAIN_PER_SUBSCRIPTION), for the caller to release once it has let the mutex go.
static bool dequeue_locked(rmw_tickle_subscriber_t* sub_impl, rmw_tickle_queued_message_t* entry,
                           struct tt_Sample* expired, size_t* expired_count) {
    // QoS roadmap #6 (LIFESPAN) - see rmw_tickle_subscriber_t.lifespan_ns's own doc comment. Drops
    // (not returns) any already-expired entries from the front before taking the real head - "as
    // if it had never been sent", same wording tickle.c's own reliable_cache-side skip uses. A
    // no-op loop when lifespan_ns == 0 (not requested).
    while (sub_impl->queue_count > 0 && sub_impl->lifespan_ns != 0 &&
           tt_get_ns() - sub_impl->queue[sub_impl->queue_head].source_timestamp >= sub_impl->lifespan_ns) {
        rmw_tickle_queued_message_t* head = &sub_impl->queue[sub_impl->queue_head];
        if (NULL != head->in_place) {
            expired[(*expired_count)++] = head->sample;
            sub_impl->retained_queued--;
        } else {
            shell_pool_push(sub_impl, head->ros_message); // Milestone 45
        }
        sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
        sub_impl->queue_count--;
    }
    bool found = sub_impl->queue_count > 0;
    if (found) {
        *entry = sub_impl->queue[sub_impl->queue_head];
        sub_impl->queue_head = (sub_impl->queue_head + 1) % sub_impl->queue_capacity;
        sub_impl->queue_count--;
        if (NULL != entry->in_place) {
            sub_impl->retained_queued--;
        }
    }
    return found;
}

static void release_samples(rmw_tickle_subscriber_t* sub_impl, struct tt_Sample* samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        release_sample(sub_impl, &samples[i]);
    }
}

static bool dequeue_one(rmw_tickle_subscriber_t* sub_impl, rmw_tickle_queued_message_t* entry) {
    struct tt_Sample expired[RMW_TICKLE_LOAN_RETAIN_PER_SUBSCRIPTION];
    size_t expired_count = 0;
    pthread_mutex_lock(&sub_impl->queue_mutex);
    bool found = dequeue_locked(sub_impl, entry, expired, &expired_count);
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    release_samples(sub_impl, expired, expired_count);
    return found;
}

rmw_ret_t rmw_take_with_info(const rmw_subscription_t* subscription, void* ros_message, bool* taken,
                             rmw_message_info_t* message_info, rmw_subscription_allocation_t* allocation) {
    TT_TRACE(tt_TRACE_TAKE_ENTER);
    (void)allocation; // pre-allocated-message optimization, not implemented
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;

    rmw_tickle_queued_message_t entry;
    if (!dequeue_one(sub_impl, &entry)) {
        *taken = false;
        return RMW_RET_OK;
    }

    // Shallow copy: entry.ros_message's own string/array fields (rosidl_runtime_c__String et al.)
    // were heap-allocated by subscriber_callback()'s from_tickle() call and are transferred here
    // as-is (the pointers, not their backing buffers) - ros_message now owns them, and only the
    // outer entry.ros_message shell itself (not its fields) gets freed below.
    //
    // Known limitation: this assumes `ros_message` is a fresh/blank buffer (matching rclcpp's own
    // typical std::allocate_shared<MessageT>() per rmw_take() call for non-loaned subscriptions,
    // the only kind this rmw currently creates - can_loan_messages is always false, see rmw_
    // create_subscription()) - overwriting an *already populated* ros_message's own owned string/
    // array fields this way would leak their old backing buffers rather than fini() them first.
    // Fixing that generally needs a per-message __fini() function pointer rosidl_typesupport_
    // tickle_c doesn't generate yet - tracked as follow-on work, not solved here.
    //
    // A C++ message is moved instead (rmw_tickle_ros_message_move()), which replaces what the
    // caller's object held rather than leaking it.
    //
    // Loaned messages: an entry kept where it arrived is copied out - the one copy an ordinary take of it costs - with
    // the same decoder (for an in-place type, field-wise copies), and released.
    if (NULL != entry.in_place) {
        int32_t decoded = sub_impl->callbacks->direct_decode(ros_message, (const uint8_t*)entry.in_place,
                                                             (uint32_t)sub_impl->callbacks->inplace_bytes, true);
        release_sample(sub_impl, &entry.sample);
        if (decoded < 0) {
            RMW_SET_ERROR_MSG("direct_decode() failed on a sample that was received whole");
            *taken = false;
            return RMW_RET_ERROR;
        }
    } else {
        rmw_tickle_ros_message_move(sub_impl->callbacks, ros_message, entry.ros_message);
        // Milestone 45 - shell_pool's own doc comment (rmw_tickle.h): the shallow copy above already
        // transferred every owned pointer field out of entry.ros_message, so shell_pool_push()'s own
        // memset() is exactly what makes reusing this same buffer safe, not just freeing it faster.
        pthread_mutex_lock(&sub_impl->queue_mutex);
        shell_pool_push(sub_impl, entry.ros_message);
        pthread_mutex_unlock(&sub_impl->queue_mutex);
    }
    *taken = true;
    TT_TRACE(tt_TRACE_TAKEN);

    if (NULL != message_info) {
        fill_message_info(sub_impl, &entry, message_info);
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_take(const rmw_subscription_t* subscription, void* ros_message, bool* taken,
                   rmw_subscription_allocation_t* allocation) {
    return rmw_take_with_info(subscription, ros_message, taken, NULL, allocation);
}

// (g5, RMW_GAPS_PLAN.md) Up to `count` messages in queue order, each taken as rmw_take_with_info() takes one. rmw.h's
// contract: a NULL argument, count 0 or a sequence too small is refused with both sequences unchanged, and so are
// they when nothing is taken. Until g5 the symbol was not defined, so rcl_take_sequence() failed and
// rmw_implementation logged a failed lookup at every start.
rmw_ret_t rmw_take_sequence(const rmw_subscription_t* subscription, size_t count,
                            rmw_message_sequence_t* message_sequence,
                            rmw_message_info_sequence_t* message_info_sequence, size_t* taken,
                            rmw_subscription_allocation_t* allocation) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(message_sequence, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(message_info_sequence, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    if (0 == count || message_sequence->capacity < count || message_info_sequence->capacity < count) {
        RMW_SET_ERROR_MSG("rmw_take_sequence: count must be non-zero and within both sequences' capacity");
        return RMW_RET_INVALID_ARGUMENT;
    }
    size_t got = 0;
    for (; got < count; got++) {
        bool one = false;
        rmw_ret_t ret = rmw_take_with_info(subscription, message_sequence->data[got], &one,
                                           &message_info_sequence->data[got], allocation);
        if (RMW_RET_OK != ret) {
            return ret;
        }
        if (!one) {
            break;
        }
    }
    *taken = got;
    if (got > 0) {
        message_sequence->size = got;
        message_info_sequence->size = got;
    }
    return RMW_RET_OK;
}

// See rmw_publisher.c's own rmw_publisher_get_actual_qos() doc comment - same reasoning.
rmw_ret_t rmw_subscription_get_actual_qos(const rmw_subscription_t* subscription, rmw_qos_profile_t* qos) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    *qos = sub_impl->qos;
    return RMW_RET_OK;
}

// See rmw_publisher.c's own rmw_publisher_event_init() doc comment - same reasoning, for
// subscription-side QoS events. QoS roadmap #2 (DEADLINE) and #3 (LIVELINESS) are done -
// RMW_EVENT_REQUESTED_DEADLINE_MISSED/RMW_EVENT_LIVELINESS_CHANGED are real, queryable events now
// (rmw_take_event(), rmw_event.c); anything else still returns RMW_RET_UNSUPPORTED.
rmw_ret_t rmw_subscription_event_init(rmw_event_t* rmw_event, const rmw_subscription_t* subscription,
                                      rmw_event_type_t event_type) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(rmw_event, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    switch (event_type) {
    case RMW_EVENT_REQUESTED_DEADLINE_MISSED:
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = sub_impl;
        rmw_event->event_type = event_type;
        return RMW_RET_OK;
    case RMW_EVENT_LIVELINESS_CHANGED:
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = sub_impl;
        rmw_event->event_type = event_type;
        // Lazy, idempotent start (see rmw_tickle_subscriber_t.liveliness_monitoring's own doc comment):
        // the first call takes the current counts; from then on the core's discovery callback keeps
        // them. A later call just rewires the same rmw_event_t.
        if (!sub_impl->liveliness_monitoring) {
            tt_Context_lock(&sub_impl->node->context_impl->tickle_context);
            sub_impl->liveliness_monitoring = true;
            rmw_tickle_update_subscription_liveliness_locked(sub_impl);
            tt_Context_unlock(&sub_impl->node->context_impl->tickle_context);
        }
        return RMW_RET_OK;
    case RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE:
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = sub_impl;
        rmw_event->event_type = event_type;
        // Lazy, idempotent start - see rmw_tickle_subscriber_t.requested_qos_incompatible_
        // monitoring_started's own doc comment (rmw_tickle.h), same pattern as LIVELINESS_CHANGED
        // just above.
        if (!sub_impl->requested_qos_incompatible_monitoring_started) {
            sub_impl->requested_qos_incompatible_monitoring_started = true;
            tt_Context_lock(&sub_impl->node->context_impl->tickle_context);
            // A failure here just leaves this monitoring inactive for this Subscription - same
            // reasoning as the deadline/liveliness scheduling above.
            (void)tt_Context_schedule(&sub_impl->node->context_impl->tickle_context,
                                      tt_get_ns() + RMW_TICKLE_QOS_INCOMPATIBLE_CHECK_PERIOD_NS,
                                      check_subscription_qos_incompatible, sub_impl);
            tt_Context_unlock(&sub_impl->node->context_impl->tickle_context);
        }
        return RMW_RET_OK;
    case RMW_EVENT_SUBSCRIPTION_MATCHED:           // (g3) counted from creation, whether asked for or not
    case RMW_EVENT_SUBSCRIPTION_INCOMPATIBLE_TYPE: // (g3)
    case RMW_EVENT_MESSAGE_LOST:                   // (g3)
        rmw_event->implementation_identifier = RMW_TICKLE_IDENTIFIER;
        rmw_event->data = sub_impl;
        rmw_event->event_type = event_type;
        return RMW_RET_OK;
    default:
        RMW_SET_ERROR_MSG("rmw_tickle does not support this subscription QoS event yet");
        return RMW_RET_UNSUPPORTED;
    }
}

// Loaned messages (docs/RMW.md, "Loaned messages"). A loaning subscription (can_loan_messages: the type's wire bytes
// are its message, callbacks->inplace_bytes) hands out the queued message itself instead of copying it into the
// caller's: a sample core kept where it arrived (retain_in_place()) is lent as those bytes, read in place; any other is
// lent as the shell it was decoded into. Either way the take copies nothing and allocates nothing - what the ordinary
// take's copy into the caller's message, and rclcpp's allocation of that message, would have cost. Returned with
// rmw_return_loaned_message_from_subscription(), which gives the sample back to core or the shell back to the pool.
//
// A subscription that does not lend answers RMW_RET_UNSUPPORTED, as rmw.h asks; rclcpp then never calls these
// (it reads can_loan_messages first). test_rmw_implementation's TestSubscriptionUseLoan fixture expects that answer for
// a type it cannot loan, and skips its loan cases on it.
static rmw_ret_t take_loaned(const rmw_subscription_t* subscription, void** loaned_message, bool* taken,
                             rmw_message_info_t* message_info) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(loaned_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    if (!sub_impl->loans) {
        RMW_SET_ERROR_MSG("rmw_tickle cannot loan this type: its wire bytes are not its message in memory");
        return RMW_RET_UNSUPPORTED;
    }
    if (NULL != *loaned_message) {
        RMW_SET_ERROR_MSG("*loaned_message must be NULL");
        return RMW_RET_INVALID_ARGUMENT;
    }

    // Room for the loan, the dequeue and the record of the loan under one hold of queue_mutex: a loaned take costs the
    // ordinary take's one lock, not three.
    struct tt_Sample expired[RMW_TICKLE_LOAN_RETAIN_PER_SUBSCRIPTION];
    size_t expired_count = 0;
    rmw_tickle_queued_message_t entry;
    pthread_mutex_lock(&sub_impl->queue_mutex);
    if (sub_impl->loans_out_count == sub_impl->loans_out_capacity) {
        size_t capacity =
            sub_impl->loans_out_capacity == 0 ? sub_impl->queue_capacity : sub_impl->loans_out_capacity * 2;
        rmw_tickle_subscription_loan_t* grown = (rmw_tickle_subscription_loan_t*)sub_impl->allocator.reallocate(
            sub_impl->loans_out, capacity * sizeof(*grown), sub_impl->allocator.state);
        if (NULL == grown) {
            pthread_mutex_unlock(&sub_impl->queue_mutex);
            RMW_SET_ERROR_MSG("failed to allocate the loan table");
            return RMW_RET_BAD_ALLOC;
        }
        sub_impl->loans_out = grown;
        sub_impl->loans_out_capacity = capacity;
    }
    bool found = dequeue_locked(sub_impl, &entry, expired, &expired_count);
    rmw_tickle_subscription_loan_t loan = {.message = NULL, .shell = NULL, .sample = {.handle = 0}};
    if (found) {
        loan.message = NULL != entry.in_place ? entry.in_place : entry.ros_message;
        loan.shell = entry.ros_message;
        loan.sample = entry.sample;
        sub_impl->loans_out[sub_impl->loans_out_count++] = loan;
        if (NULL != entry.in_place) {
            sub_impl->loans_in_place++;
        } else {
            sub_impl->loans_copied++;
        }
    }
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    release_samples(sub_impl, expired, expired_count);
    if (!found) {
        *taken = false;
        return RMW_RET_OK;
    }

    *loaned_message = (void*)loan.message;
    *taken = true;
    TT_TRACE(tt_TRACE_TAKEN);
    if (NULL != message_info) {
        fill_message_info(sub_impl, &entry, message_info);
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_take_loaned_message(const rmw_subscription_t* subscription, void** loaned_message, bool* taken,
                                  rmw_subscription_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    return take_loaned(subscription, loaned_message, taken, NULL);
}

rmw_ret_t rmw_take_loaned_message_with_info(const rmw_subscription_t* subscription, void** loaned_message, bool* taken,
                                            rmw_message_info_t* message_info,
                                            rmw_subscription_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(message_info, RMW_RET_INVALID_ARGUMENT);
    return take_loaned(subscription, loaned_message, taken, message_info);
}

rmw_ret_t rmw_return_loaned_message_from_subscription(const rmw_subscription_t* subscription, void* loaned_message) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(loaned_message, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    if (!sub_impl->loans) {
        RMW_SET_ERROR_MSG("rmw_tickle cannot loan this type: its wire bytes are not its message in memory");
        return RMW_RET_UNSUPPORTED;
    }
    pthread_mutex_lock(&sub_impl->queue_mutex);
    size_t i = 0;
    while (i < sub_impl->loans_out_count && sub_impl->loans_out[i].message != loaned_message) {
        i++;
    }
    if (i == sub_impl->loans_out_count) {
        pthread_mutex_unlock(&sub_impl->queue_mutex);
        RMW_SET_ERROR_MSG("not a message this subscription has loaned out, or one already returned");
        return RMW_RET_INVALID_ARGUMENT;
    }
    rmw_tickle_subscription_loan_t loan = sub_impl->loans_out[i];
    sub_impl->loans_out[i] = sub_impl->loans_out[--sub_impl->loans_out_count];
    if (NULL != loan.shell) {
        shell_pool_push(sub_impl, loan.shell);
    }
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    release_sample(sub_impl, &loan.sample);
    return RMW_RET_OK;
}

static size_t messages_waiting(const void* entity) {
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)entity;
    pthread_mutex_lock(&sub_impl->queue_mutex);
    size_t waiting = sub_impl->queue_count;
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    return waiting;
}

// (g2, RMW_GAPS_PLAN.md) rclcpp's and rclpy's EventsExecutor: called with 1 per message queued; set while messages are
// waiting, called once with how many. See rmw_tickle_callback_slot_t (rmw_tickle.h) for the lock and NULL contracts.
rmw_ret_t rmw_subscription_set_on_new_message_callback(rmw_subscription_t* subscription, rmw_event_callback_t callback,
                                                       const void* user_data) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    rmw_tickle_callback_slot_set(&sub_impl->on_new_message, callback, user_data, messages_waiting, sub_impl);
    return RMW_RET_OK;
}

// (g1) A serialized take encodes the queued message again rather than keeping a second queue of
// raw bytes. The same subscription can be taken either way, so queueing both would cost every
// subscription memory for a path most never use - and re-encoding settles the byte order for free,
// since the queued message is native whatever arrived. The cost is one encode per serialized take,
// on the path `ros2 bag record` uses, which is not the hot one.
//
// The shell goes back to the pool afterwards. A C message's fields are released first (ros_fini,
// which since g1 the generator sets for C as well): unlike an ordinary take, nothing transferred
// them out, so shell_pool_push()'s memset alone would drop every string and sequence in it. A C++
// message is pushed as it stands - the pool's next decode assigns over every field, and destroying
// the object would leave the pool holding storage no longer holding an object.
static rmw_ret_t take_serialized(const rmw_subscription_t* subscription, rmw_serialized_message_t* serialized_message,
                                 bool* taken, rmw_message_info_t* message_info) {
    TT_TRACE(tt_TRACE_TAKE_ENTER);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(serialized_message, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
    if (!rmw_tickle_identifier_matches(subscription->implementation_identifier)) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)subscription->data;
    const rosidl_typesupport_tickle_c_message_callbacks_t* callbacks = sub_impl->callbacks;
    if (NULL == callbacks->direct_encode) {
        RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
            "%s was built without a direct codec, which a serialized take needs - rebuild the interface package",
            callbacks->ros_type_name);
        return RMW_RET_UNSUPPORTED;
    }

    rmw_tickle_queued_message_t entry;
    if (!dequeue_one(sub_impl, &entry)) {
        *taken = false;
        return RMW_RET_OK;
    }

    rmw_ret_t result = RMW_RET_OK;
    if (NULL != entry.in_place) {
        // Loaned messages: the bytes kept are the CDR itself, in native order.
        size_t bytes = callbacks->inplace_bytes;
        if (serialized_message->buffer_capacity < bytes &&
            rmw_serialized_message_resize(serialized_message, bytes) != RCUTILS_RET_OK) {
            RMW_SET_ERROR_MSG("failed to resize serialized_message");
            result = RMW_RET_BAD_ALLOC;
        } else {
            memcpy(serialized_message->buffer, entry.in_place, bytes);
            serialized_message->buffer_length = bytes;
        }
        release_sample(sub_impl, &entry.sample);
        if (RMW_RET_OK != result) {
            *taken = false;
            return result;
        }
        *taken = true;
        if (NULL != message_info) {
            fill_message_info(sub_impl, &entry, message_info);
        }
        return RMW_RET_OK;
    }
    int32_t size = callbacks->direct_encode_size(entry.ros_message);
    if (size < 0) {
        RMW_SET_ERROR_MSG("direct_encode_size() failed on a message that was decoded from the wire");
        result = RMW_RET_ERROR;
    } else if (serialized_message->buffer_capacity < (size_t)size &&
               rmw_serialized_message_resize(serialized_message, (size_t)size) != RCUTILS_RET_OK) {
        RMW_SET_ERROR_MSG("failed to resize serialized_message");
        result = RMW_RET_BAD_ALLOC;
    } else {
        int32_t written = callbacks->direct_encode(entry.ros_message, serialized_message->buffer,
                                                   (uint32_t)serialized_message->buffer_capacity);
        if (written < 0) {
            RMW_SET_ERROR_MSG("direct_encode() failed on a message that was decoded from the wire");
            result = RMW_RET_ERROR;
        } else {
            serialized_message->buffer_length = (size_t)written;
        }
    }

    if (NULL == callbacks->ros_move && NULL != callbacks->ros_fini) {
        callbacks->ros_fini(entry.ros_message); // a C message: nothing took its fields
    }
    pthread_mutex_lock(&sub_impl->queue_mutex);
    shell_pool_push(sub_impl, entry.ros_message);
    pthread_mutex_unlock(&sub_impl->queue_mutex);

    if (RMW_RET_OK != result) {
        *taken = false;
        return result;
    }
    *taken = true;
    TT_TRACE(tt_TRACE_TAKEN);
    if (NULL != message_info) {
        fill_message_info(sub_impl, &entry, message_info);
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_take_serialized_message(const rmw_subscription_t* subscription,
                                      rmw_serialized_message_t* serialized_message,
                                      bool* taken, // NOLINT(readability-non-const-parameter)
                                      rmw_subscription_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    return take_serialized(subscription, serialized_message, taken, NULL);
}

rmw_ret_t rmw_take_serialized_message_with_info(const rmw_subscription_t* subscription,
                                                rmw_serialized_message_t* serialized_message,
                                                bool* taken, // NOLINT(readability-non-const-parameter)
                                                rmw_message_info_t* message_info,
                                                rmw_subscription_allocation_t* allocation) {
    (void)allocation; // pre-allocated-message optimization, not implemented
    return take_serialized(subscription, serialized_message, taken, message_info);
}
