/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include "net_init.h"

#include <lwip/netif.h>
#include <lwip/sys.h>
#include <lwip/tcpip.h>

// Matches TickLE's own netns.mk dev/test convention (192.168.10.0/24) rather than the real
// _tt_NODE_ADDRESS/_tt_NODE_BROADCAST defaults (0.0.0.0/255.255.255.255) - a static address is
// required since there's no DHCP server (see lwipopts.h), and this specific one is just this
// milestone's own single-node loopback sanity check, not yet the two-QEMU-instance segment
// milestone 4 sets up.
#define NET_IP_ADDR0 192
#define NET_IP_ADDR1 168
#define NET_IP_ADDR2 10
#define NET_IP_ADDR3 1

#define LOOP_NETIF_MTU 1500 // A real Ethernet-sized MTU, even though nothing here is Ethernet

static struct netif loop_netif;

// This netif has no real link underneath it - every packet handed to output() is queued
// straight back to input() by lwIP's own generic netif_loop_output() (enabled via
// LWIP_NETIF_LOOPBACK in lwipopts.h), which is exactly the mechanism LWIP_HAVE_LOOPIF's built-in
// 127.0.0.1 interface uses internally. The difference here is this netif carries this milestone's
// own static broadcast-capable address instead of the dedicated loopback range, so hal_freertos.c
// exercises the real tt_bind()/tt_send()/tt_receive() path end-to-end.
static err_t loop_netif_output_ip4(struct netif* netif, struct pbuf* p, const ip4_addr_t* ipaddr) {
    (void)ipaddr;
    return netif_loop_output(netif, p);
}

static err_t loop_netif_init(struct netif* netif) {
    netif->name[0] = 'l';
    netif->name[1] = '0';
    netif->mtu = LOOP_NETIF_MTU;
    netif->output = loop_netif_output_ip4;
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP | NETIF_FLAG_BROADCAST;
    return ERR_OK;
}

static void tcpip_init_done(void* arg) {
    sys_sem_t* ready = arg;
    sys_sem_signal(ready);
}

void net_init(void) {
    sys_sem_t ready;
    sys_sem_new(&ready, 0);
    tcpip_init(tcpip_init_done, &ready);
    sys_sem_wait(&ready);
    sys_sem_free(&ready);

    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    IP4_ADDR(&ip, NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);

    netif_add(&loop_netif, &ip, &netmask, &gateway, NULL, loop_netif_init, tcpip_input);
    netif_set_default(&loop_netif);
    netif_set_up(&loop_netif);
    netif_set_link_up(&loop_netif);
}
