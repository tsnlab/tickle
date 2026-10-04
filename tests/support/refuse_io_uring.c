// LD_PRELOAD shim: io_uring_setup() fails with EPERM, as under Docker's default seccomp profile or
// kernel.io_uring_disabled=2. Used to test that a context refused io_uring still delivers, and says so
// (rx_hint=refused), without needing a container or root.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming) - glibc feature-test macro, for RTLD_NEXT
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>

#include <sys/syscall.h>

long syscall(long number, ...) {
    va_list ap;
    va_start(ap, number);
    long a[6];
    for (int i = 0; i < 6; i++) {
        a[i] = va_arg(ap, long);
    }
    va_end(ap);
    if (number == __NR_io_uring_setup) {
        errno = EPERM;
        return -1;
    }
    long (*real)(long, ...) = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    return real(number, a[0], a[1], a[2], a[3], a[4], a[5]);
}
