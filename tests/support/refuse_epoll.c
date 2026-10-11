// LD_PRELOAD shim: epoll_create1() fails with ENOSYS, as on a kernel or libc without it. The Linux HAL then keeps no
// wait set and every wait - a deferred one included (rx_defer_check.c) - goes through ppoll(), the path every machine
// took before 2026-10-06 and any older one still takes.
#include <errno.h>

int epoll_create1(int flags) {
    (void)flags;
    errno = ENOSYS;
    return -1;
}
