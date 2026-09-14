/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

// NO_SYS=0: lwIP runs its own FreeRTOS task (the "tcpip thread") and serializes all stack access
// through it, using contrib/ports/freertos/sys_arch.c for the FreeRTOS mutex/mailbox/thread
// primitives that requires. hal_freertos.c only ever talks to lwIP through its socket API
// (LWIP_SOCKET below), never touching netif/pbuf internals directly - the tcpip thread owns those.
#define NO_SYS 0
#define LWIP_NETCONN 1
#define LWIP_SOCKET 1
#define LWIP_COMPAT_SOCKETS 1
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
// hal_freertos.c's tt_bind() sets SO_BROADCAST/SO_REUSEADDR, same as hal_linux.c.
#define SO_REUSE 1
#define LWIP_BROADCAST_PING 0

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 0  // No real Ethernet under milestone 2's loopback netif - see net_init.c
#define LWIP_DHCP 0 // Static IP only - two fixed QEMU instances, no DHCP server exists
#define LWIP_DNS 0
#define LWIP_IGMP 0 // Broadcast, not multicast - TickLE never joins a multicast group

// milestone 2's netif loops packets back to itself via lwIP's own generic
// netif_loop_output()/netif_poll() queue (see net_init.c) - this is what turns that on.
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_LOOPBACK_MAX_PBUFS 8

#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0
#define LWIP_NETIF_HOSTNAME 0
#define LWIP_STATS 0
#define LWIP_TCP 0 // TickLE is UDP-only

// Sized for TickLE's own tt_MAX_BUFFER_LENGTH (1472B, one Ethernet-MTU UDP payload) plus lwIP's
// own header/pbuf overhead - generous rather than tightly tuned, since this target's only job
// right now is correctness testing, not memory-constrained production use.
#define MEM_SIZE (16 * 1024)
#define PBUF_POOL_SIZE 8
#define PBUF_POOL_BUFSIZE 1600
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_NETCONN 8

#define TCPIP_THREAD_STACKSIZE 1024
// Plain number, not tskIDLE_PRIORITY-relative: lwipopts.h is processed without FreeRTOS.h in
// scope. configMAX_PRIORITIES is 5 (FreeRTOSConfig.h); this sits above the default task
// priority (1) used by main.c's own task.
#define TCPIP_THREAD_PRIO 3
#define DEFAULT_THREAD_STACKSIZE 512
#define DEFAULT_RAW_RECVMBOX_SIZE 8
#define DEFAULT_UDP_RECVMBOX_SIZE 8
#define DEFAULT_TCP_RECVMBOX_SIZE 8
#define DEFAULT_ACCEPTMBOX_SIZE 8
#define TCPIP_MBOX_SIZE 8
