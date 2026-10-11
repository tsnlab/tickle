# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# frag_rx: the FreeRTOS test tier's image (platform/freertos: board, net_init.c, FreeRTOSConfig.h, its
# lwipopts.h) with a receive counter in place of the TickLE role main. Used by ../freertos_frag_q.sh.
# Built out of tree: make OUT=<dir> [FREERTOS=<kernel>] [LWIP=<lwip>] [POOL=<PBUF_POOL_SIZE override>]
ROOT := $(abspath ../../../..)
PLAT := $(ROOT)/platform/freertos
FREERTOS ?= $(ROOT)/third_party/FreeRTOS-Kernel
LWIP ?= $(ROOT)/third_party/lwip
OUT ?= $(CURDIR)/out
POOL ?=
CROSS ?= riscv64-unknown-elf-
CC = $(CROSS)gcc
RISCV_PORT = $(FREERTOS)/portable/GCC/RISC-V
CHIP_EXT = $(RISCV_PORT)/chip_specific_extensions/RV32I_CLINT_no_extensions
LWIP_PORT = $(LWIP)/contrib/ports/freertos
ARCH = -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medlow
POOL_FLAG = $(if $(POOL),-DFRAG_POOL_OVERRIDE=$(POOL),)
BUILD = $(OUT)/build$(if $(POOL),-pool$(POOL),)
# Same flags as platform/freertos/Makefile; this directory comes first so its lwipopts.h wins.
CFLAGS = $(ARCH) -O1 -g -Wall -Wextra -ffreestanding -fno-common -DTT_PLATFORM_FREERTOS -DNET_NODE_ID=2 \
         -Dtt_HAL_FREERTOS_NETCONN=1 $(POOL_FLAG) --specs=picolibc.specs \
         -I$(CURDIR) -I$(PLAT) -I$(PLAT)/board -I$(PLAT)/lwip_arch -I$(FREERTOS)/include -I$(RISCV_PORT) \
         -I$(LWIP)/src/include -I$(LWIP_PORT)/include -I$(ROOT)/include
ASFLAGS = $(ARCH) -I$(CHIP_EXT) -I$(PLAT)
LDFLAGS = $(ARCH) -nostartfiles --specs=picolibc.specs -T $(PLAT)/board/link.ld -Wl,--gc-sections
SRCS = $(CURDIR)/frag_rx.c $(PLAT)/net_init.c $(PLAT)/app_hooks.c \
       $(PLAT)/board/uart.c $(PLAT)/board/stdio_uart.c $(PLAT)/board/runtime_stubs.c $(PLAT)/board/virtio_net.c \
       $(FREERTOS)/tasks.c $(FREERTOS)/list.c $(FREERTOS)/queue.c $(FREERTOS)/timers.c $(FREERTOS)/event_groups.c \
       $(FREERTOS)/portable/MemMang/heap_4.c $(RISCV_PORT)/port.c \
       $(addprefix $(LWIP)/src/core/,def.c inet_chksum.c init.c ip.c mem.c memp.c netif.c pbuf.c stats.c sys.c \
         timeouts.c udp.c ipv4/ip4.c ipv4/ip4_addr.c ipv4/ip4_frag.c ipv4/icmp.c) \
       $(addprefix $(LWIP)/src/api/,api_lib.c api_msg.c err.c netbuf.c netifapi.c sockets.c tcpip.c) \
       $(LWIP_PORT)/sys_arch.c
ASMS = $(PLAT)/board/start.S $(RISCV_PORT)/portASM.S
OBJS = $(foreach s,$(SRCS) $(ASMS),$(BUILD)$(s).o)
ELF = $(OUT)/frag_rx$(if $(POOL),-pool$(POOL),).elf

all: $(ELF)
$(BUILD)%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<
$(BUILD)%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<
$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
-include $(OBJS:.o=.d)
.PHONY: all
