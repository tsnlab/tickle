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

#include <FreeRTOS.h>
#include <stdio.h>
#include <string.h>
#include <task.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/sys.h>
#include <lwip/tcpip.h>

#include "virtio_net.h"

// Matches TickLE's own netns.mk dev/test convention (192.168.10.0/24) rather than the real
// _tt_NODE_ADDRESS/_tt_NODE_BROADCAST defaults (0.0.0.0/255.255.255.255) - a static address is
// required since there's no DHCP server (see lwipopts.h).
#define NET_IP_ADDR0 192
#define NET_IP_ADDR1 168
#define NET_IP_ADDR2 10
#define NET_IP_ADDR3 1

#define NET_NETIF_MTU 1500

#define ETH_ADDR_LEN 6
#define ETH_HDR_LEN 14
#define ETHTYPE_IPV4 0x0800

#define NET_POLL_TASK_STACK_WORDS 1024
#define NET_POLL_TASK_PRIORITY 2
#define NET_POLL_IDLE_DELAY_MS 1

static struct netif virtio_netif;

// Arbitrary, locally-administered (see the U/L bit, 0x02) address: TickLE identifies nodes by
// static IP, never by MAC, and this driver never does ARP (see virtio_netif_output_ip4) - so
// this only has to be *a* valid source address for the Ethernet framing virtio-net requires
// underneath, not a real or unique one.
static const uint8_t local_mac[ETH_ADDR_LEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

// TickLE only ever sends UDP broadcast (see README's "Security & concurrency model"), so every
// frame's destination is the Ethernet broadcast address - there's no unicast destination to
// resolve, and so no need for lwIP's ARP module (LWIP_ARP is 0 - see lwipopts.h) at all.
static err_t virtio_netif_output_ip4(struct netif* netif, struct pbuf* p, const ip4_addr_t* ipaddr) {
    (void)netif;
    (void)ipaddr;

    static uint8_t tx_frame[VIRTIO_NET_MAX_FRAME_SIZE];

    if (p->tot_len > sizeof(tx_frame) - ETH_HDR_LEN) {
        return ERR_MEM;
    }

    memset(tx_frame, 0xff, ETH_ADDR_LEN); // Destination: broadcast
    memcpy(tx_frame + ETH_ADDR_LEN, local_mac, ETH_ADDR_LEN);
    tx_frame[2 * ETH_ADDR_LEN] = (ETHTYPE_IPV4 >> 8) & 0xff;
    tx_frame[(2 * ETH_ADDR_LEN) + 1] = ETHTYPE_IPV4 & 0xff;
    pbuf_copy_partial(p, tx_frame + ETH_HDR_LEN, p->tot_len, 0);

    virtio_net_send(tx_frame, (uint16_t)(ETH_HDR_LEN + p->tot_len));
    return ERR_OK;
}

static err_t virtio_netif_init(struct netif* netif) {
    netif->name[0] = 'v';
    netif->name[1] = '0';
    netif->mtu = NET_NETIF_MTU;
    netif->output = virtio_netif_output_ip4;
    // Deliberately no NETIF_FLAG_ETHARP: this netif's Ethernet framing is added/stripped here
    // and in net_poll_task below, not by lwIP's own ethernet_input()/etharp_output() - netif
    // ->input (tcpip_input) is handed a bare IP packet either way, same as milestone 2's
    // loopback netif this one replaces.
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP | NETIF_FLAG_BROADCAST;
    return ERR_OK;
}

// virtio_net_recv() is polling/non-blocking (this driver has no interrupt to wait on - see
// virtio_net.c), so this task exists purely to keep polling it and feed whatever arrives into
// lwIP's tcpip thread via tcpip_input(). The 1ms idle delay matches tickle.c's own
// tt_NODE_TX_INTERVAL polling cadence rather than being load-bearing on its own.
static void net_poll_task(void* param) {
    struct netif* netif = param;
    static uint8_t rx_frame[VIRTIO_NET_MAX_FRAME_SIZE];

    for (;;) {
        int32_t frame_len = virtio_net_recv(rx_frame, sizeof(rx_frame));
        if (frame_len < ETH_HDR_LEN) {
            vTaskDelay(pdMS_TO_TICKS(NET_POLL_IDLE_DELAY_MS));
            continue;
        }

        uint16_t payload_len = (uint16_t)(frame_len - ETH_HDR_LEN);
        struct pbuf* p = pbuf_alloc(PBUF_RAW, payload_len, PBUF_POOL);
        if (p == NULL) {
            continue;
        }

        pbuf_take(p, rx_frame + ETH_HDR_LEN, payload_len);
        if (tcpip_input(p, netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
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

    if (!virtio_net_init()) {
        printf("net_init: no virtio-net device found - network will not work\n");
    }

    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    IP4_ADDR(&ip, NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);

    netif_add(&virtio_netif, &ip, &netmask, &gateway, NULL, virtio_netif_init, tcpip_input);
    netif_set_default(&virtio_netif);
    netif_set_up(&virtio_netif);
    netif_set_link_up(&virtio_netif);

    xTaskCreate(net_poll_task, "net_poll", NET_POLL_TASK_STACK_WORDS, &virtio_netif, NET_POLL_TASK_PRIORITY, NULL);
}
