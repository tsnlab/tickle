#pragma once

#define tt_SECOND 1000000000ULL
#define tt_MILLISECOND 1000000ULL
#define tt_MICROSECOND 1000ULL

#define tt_NODE_CYCLE tt_MILLISECOND                   // nanosecond
#define tt_NODE_UPDATE_INTERVAL (10 * tt_SECOND)       // nanoseond  TODO: Temporary value for debugging
#define tt_NODE_TX_INTERVAL tt_MILLISECOND             // nanoseond
#define tt_RELIABLE_DEADLINE 0                         // nanosecond, 0 is auto
#define tt_RELIABLE_RETRY 3                            // count
#define tt_CALL_RETRY_INTERVAL (5 * tt_MILLISECOND)    // Default value
#define tt_CALL_RETRY_COUNT 3                          // count
#define tt_SERVER_CACHE_TIMEOUT (100 * tt_MILLISECOND) // (Client server latency) * (CALL_RETRY_COUNT + 1)
#define tt_RECEIVE_TIMEOUT (100 * tt_MICROSECOND)      // Network socket default receive timeout

#define tt_MAX_ENDPOINT_COUNT 256  // Maximum number of endpoints (data or services)
#define tt_MAX_NAME_LENGTH 255     // Maximum length of endpoint name
#define tt_MAX_STRING_LENGTH 65535 // Maximum length of string
// RX/TX buffering size to flush: the largest UDP payload a standard 1500-byte Ethernet MTU
// can carry without IP fragmentation. 1500 (MTU) - 20 (IPv4 header) - 8 (UDP header) = 1472.
// Previously 1480, which is 8 bytes *larger* than that limit - a packet in the 1473-1480
// byte range would pass this check yet still fragment at the IP layer on a standard network.
#define tt_MAX_BUFFER_LENGTH 1472

// Node ID values are the last byte of the IPv4 address on the local network.
// Valid node IDs are 1..254, because 0 is reserved for invalid/unassigned and
// 255 is reserved for the broadcast address.
#define tt_NODE_ID_INVALID 0x00
#define tt_NODE_ID_BROADCAST 0xff
#define tt_MAX_SCHEDULER_LENGTH 128  // Scheduling queue
#define tt_MAX_SERVER_CACHE_COUNT 64 // >= # of client

#define _tt_NODE_ADDRESS "0.0.0.0"
#define _tt_NODE_PORT 8282
#define _tt_NODE_BROADCAST "255.255.255.255"

struct _tt_Config {
    char* addr;
    int port;
    char* broadcast;
};

extern struct _tt_Config _tt_CONFIG;
