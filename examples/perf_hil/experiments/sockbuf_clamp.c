/* sockbuf_clamp.c - LD_PRELOAD shim: every setsockopt(SO_SNDBUF / SO_RCVBUF) asks at most 212992 bytes, as
 * if net.core.wmem_max and rmem_max were the rig Pis' 212992. The PC's are 4194304 and global (not per network
 * namespace), and the PC's sudo cannot change them; without this a PC run grants rmw_tickle the 4 MB socket buffers it
 * asks for, which the rig never does (its subscriber logs "receive buffer is 425984 bytes (asked 4194304)"), and the
 * publisher never waits for send space (send_waits 0 on the PC against ~15 a sample on the rig).
 * Used by i4be_gap_pc.sh for every arm alike. The real call is the system call instruction itself (x86-64: the PC
 * only), neither dlsym(RTLD_NEXT) nor syscall(): under dlsym perf_test hung at start in a futex (2026-10-15), and
 * syscall() is itself interposed by tests/support/refuse_pbuf_ring.so, which the rmw_tickle subscribers also preload
 * (together they hung the same way).
 * Build: cc -shared -fPIC -O2 -o sockbuf_clamp.so sockbuf_clamp.c */
#include <errno.h>

#include <sys/socket.h>

#define RIG_SOCKBUF_MAX 212992 // net.core.rmem_max and wmem_max on the rig's Pis
#define NR_SETSOCKOPT 54L      // __NR_setsockopt on x86-64

static int real(int sock, int level, int name, const void* val, socklen_t len) {
    long ret;
    register long arg4 __asm__("r10") = (long)val;
    register long arg5 __asm__("r8") = (long)len;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(NR_SETSOCKOPT), "D"((long)sock), "S"((long)level), "d"((long)name), "r"(arg4), "r"(arg5)
                     : "rcx", "r11", "memory");
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc's own names are reserved (__fd, ...)
int setsockopt(int sock, int level, int name, const void* val, socklen_t len) {
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET, SO_SNDBUF, SO_RCVBUF: <sys/socket.h> via private headers
    if (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF) && val && len >= sizeof(int)) {
        int cap = RIG_SOCKBUF_MAX;
        int asked = *(const int*)val;
        if (asked > cap) {
            return real(sock, level, name, &cap, sizeof cap);
        }
    }
    return real(sock, level, name, val, len);
}
