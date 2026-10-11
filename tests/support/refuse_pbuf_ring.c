// LD_PRELOAD shim: IORING_REGISTER_PBUF_RING as the rig's Raspberry Pi 5s answered it. On 2026-10-11 their kernel
// (6.8.0-1064-raspi) refused the receive drain's buffer ring with EINVAL while this PC's (7.0) took it, so the drain
// never ran on the rig. This holds every registration to Linux 6.8's own rules (io_uring/kbuf.c,
// io_register_pbuf_ring(): resv[] zero, no flag but IOU_PBUF_RING_MMAP, ring_addr page-aligned and given only without
// it, entries a power of two below 65536) and, with TT_REFUSE_PBUF_RING set, refuses more:
//   user  a ring in the process's memory (flags 0) - EINVAL, as the Pis did; the kernel's own ring passes through
//   all   every buffer ring - EINVAL, leaving classic provided buffers
// Anything else passes through to the real kernel. Used by test_samehost.sh, with rx_drain_check.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming) - glibc feature-test macro, for RTLD_NEXT
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/io_uring.h>
#include <sys/syscall.h> // NOLINT(misc-include-cleaner) - __NR_*: see src/hal_linux.c

#define LINUX_6_8_MAX_ENTRIES 65536U // "cannot disambiguate full vs empty due to head/tail size"

// What Linux 6.8 refuses with EINVAL before it looks at the ring's pages.
static bool breaks_6_8_rules(const struct io_uring_buf_reg* reg) {
    long page = sysconf(_SC_PAGESIZE);
    bool mmap_flag = (reg->flags & IOU_PBUF_RING_MMAP) != 0;
    return reg->resv[0] != 0 || reg->resv[1] != 0 || reg->resv[2] != 0 || (reg->flags & ~IOU_PBUF_RING_MMAP) != 0 ||
           (!mmap_flag && page > 0 && (reg->ring_addr % (uint64_t)page) != 0) || (mmap_flag && reg->ring_addr != 0) ||
           reg->ring_entries == 0 || (reg->ring_entries & (reg->ring_entries - 1U)) != 0 ||
           reg->ring_entries >= LINUX_6_8_MAX_ENTRIES;
}

static bool refused_here(const struct io_uring_buf_reg* reg) {
    const char* mode = getenv("TT_REFUSE_PBUF_RING");
    if (breaks_6_8_rules(reg)) {
        return true;
    }
    if (mode == NULL) {
        return false;
    }
    return strcmp(mode, "all") == 0 || (strcmp(mode, "user") == 0 && (reg->flags & IOU_PBUF_RING_MMAP) == 0);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc's own name for it is reserved
long syscall(long number, ...) {
    va_list ap;
    va_start(ap, number);
    long a[6];
    for (int i = 0; i < 6; i++) {
        a[i] = va_arg(ap, long);
    }
    va_end(ap);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - the syscall's own argument, a pointer passed as a long
    const struct io_uring_buf_reg* reg = (const struct io_uring_buf_reg*)a[2];
    if (number == __NR_io_uring_register && a[1] == IORING_REGISTER_PBUF_RING && reg != NULL && refused_here(reg)) {
        errno = EINVAL;
        return -1;
    }
    long (*real)(long, ...) = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    return real(number, a[0], a[1], a[2], a[3], a[4], a[5]);
}
