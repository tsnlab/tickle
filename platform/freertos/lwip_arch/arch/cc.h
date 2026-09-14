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

// lwIP's own lwip/arch.h already covers this target correctly via its own #ifndef-guarded
// defaults: types/format macros from picolibc's stdint.h/inttypes.h, LITTLE_ENDIAN (correct for
// RISC-V), and PACK_STRUCT_STRUCT as __attribute__((packed)) (GCC path). This file exists only
// because lwip/arch.h unconditionally #includes "arch/cc.h".

// lwip/errno.h defines nothing on its own unless told where the E* codes come from; picolibc's
// own <errno.h> (via <sys/errno.h>) already has the full BSD/POSIX socket error set lwIP's
// sockets/netconn code expects (ENOBUFS, EHOSTUNREACH, EISCONN, ...), so just point it there
// instead of having lwIP define its own separate, possibly conflicting errno mechanism
// (LWIP_PROVIDE_ERRNO).
#define LWIP_ERRNO_STDINCLUDE
