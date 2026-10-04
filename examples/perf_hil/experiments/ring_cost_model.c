// 링의 샘플당 비용을 네 arm 으로 분해한다: 복사인가, 캐시라인 충돌인가, 둘 다인가, 어느 쪽도 아닌가.
//
// 왜 이 모양인가 (Dev 의 지적, 2026-10-04): 단일 스레드 memcpy 벤치는 하한조차 못 준다. 실제 독자는
// **다른 코어가 방금 쓴** 슬롯에서 읽으므로 단순 캐시 미스가 아니라 coherence 미스이고, 한 코어에서
// 쓰고 읽는 벤치는 그것을 전혀 담지 못한다. 그래서 쓰기/읽기를 서로 다른 코어에 핀한다 - 벤치가 쓰는
// 코어(클라이언트 2, 서버 1)와 같은 배치로.
//
// 그리고 같은 벤치가 두 질문을 한 번에 가른다. 네 arm 은 컴파일 타임 플래그로만 다르다:
//   A1  기본            인덱스 한 라인 + 페이로드 복사      <- 기준
//   A2  -DPAD_INDICES   인덱스를 각자 라인으로 + 복사       <- 캐시라인 효과만 분리
//   A3  -DNO_COPY       인덱스 한 라인 + 복사 없음          <- 복사 비용만 분리
//   A4  둘 다                                               <- 바닥
//
// 실제 헤더에서 확인된 전제 (offsetof, 2026-10-04): slots 20, slot_bytes 24, write_index 28,
// read_index 32, reader_waiting 36 - 전부 line 0. 작성자는 매 샘플 write_index 에 CAS 하고,
// 독자는 매 전달 slots/slot_bytes 를 읽는다. 읽기 전용 기하 정보가 뜨거운 쓰기 대상과 같은 라인에 있다.
// A2 는 그 배치를 풀어서 그 비용만 떼어낸다.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEGMENT_BYTES (768 * 1024)
#define SLOT_BYTES 1472
#define SLOT_STRIDE (16 + SLOT_BYTES)
#define SLOTS 512 /* 768K / 1488 = 528 -> 2의 거듭제곱으로 내려 512, 실제 geometry 와 같은 규칙 */
#define PAYLOAD 1424
#define FILL_BYTE 0xa5
#define DEFAULT_RECORDS 2000000u
#define CACHE_LINE 64
#define NS_PER_S 1000000000.0
#define NS_PER_US 1000.0

#ifdef PAD_INDICES
#define PAD(n) char pad##n[64];
#else
#define PAD(n)
#endif

struct hdr {
    uint32_t magic;
    uint32_t incarnation;
    uint32_t slots;      /* 읽기 전용, 독자가 매 전달 읽는다 */
    uint32_t slot_bytes; /* 읽기 전용, 독자가 매 전달 읽는다 */
    PAD(0)
    uint32_t write_index; /* 작성자가 매 샘플 CAS */
    PAD(1)
    uint32_t read_index; /* 독자가 매 전달 release store */
    uint32_t reader_waiting;
};

struct slot {
    uint32_t length;
    uint32_t sender_ip;
    uint16_t sender_port;
    uint16_t seq_span;
    uint32_t sequence;
};

static uint8_t* g_ring;
static struct hdr* g_hdr;
static uint64_t g_n;
static volatile int g_done;

static uint8_t* slot_at(uint32_t idx) {
    uint32_t slots = g_hdr->slots;
    uint32_t slot_sz = g_hdr->slot_bytes; /* 독자/작성자 모두 매번 읽는다 - 실제와 같게 */
    return g_ring + sizeof(struct hdr) + ((size_t)(idx & (slots - 1)) * (size_t)(16 + slot_sz));
}

static void pin(int cpu) {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
}

static void* writer(void* arg) {
    (void)arg;
    pin(2);
    static uint8_t src[PAYLOAD];
    memset(src, FILL_BYTE, sizeof(src));
    for (uint64_t i = 0; i < g_n;) {
        uint32_t claimed = __atomic_load_n(&g_hdr->write_index, __ATOMIC_RELAXED);
        uint32_t read_idx = __atomic_load_n(&g_hdr->read_index, __ATOMIC_ACQUIRE);
        if (claimed - read_idx >= g_hdr->slots) {
            continue;
        }
        struct slot* slot_p = (struct slot*)slot_at(claimed);
        if (__atomic_load_n(&slot_p->sequence, __ATOMIC_ACQUIRE) != claimed) {
            continue;
        }
        if (!__atomic_compare_exchange_n(&g_hdr->write_index, &claimed, claimed + 1U, 1, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED)) {
            continue;
        }
#ifndef NO_COPY
        memcpy((uint8_t*)slot_p + 16, src, PAYLOAD);
#endif
        slot_p->length = PAYLOAD;
        __atomic_store_n(&slot_p->sequence, claimed + 1U, __ATOMIC_RELEASE);
        i++;
    }
    return NULL;
}

static void* reader(void* arg) {
    (void)arg;
    pin(1);
    static uint8_t dst[PAYLOAD];
    for (uint64_t i = 0; i < g_n;) {
        uint32_t read_idx = __atomic_load_n(&g_hdr->read_index, __ATOMIC_RELAXED);
        struct slot* slot_p = (struct slot*)slot_at(read_idx);
        if (__atomic_load_n(&slot_p->sequence, __ATOMIC_ACQUIRE) != read_idx + 1U) {
            continue;
        }
#ifndef NO_COPY
        memcpy(dst, (uint8_t*)slot_p + 16, PAYLOAD);
#endif
        __atomic_store_n(&slot_p->sequence, read_idx + g_hdr->slots, __ATOMIC_RELEASE);
        __atomic_store_n(&g_hdr->read_index, read_idx + 1U, __ATOMIC_RELEASE);
        i++;
    }
    g_done = 1;
    return NULL;
}

int main(int argc, char** argv) {
    g_n = (argc > 1) ? strtoull(argv[1], NULL, 10) : DEFAULT_RECORDS;
    g_ring = aligned_alloc(CACHE_LINE, (size_t)SEGMENT_BYTES);
    if (!g_ring) {
        return 1;
    }
    memset(g_ring, 0, (size_t)SEGMENT_BYTES);
    g_hdr = (struct hdr*)g_ring;
    g_hdr->slots = SLOTS;
    g_hdr->slot_bytes = SLOT_BYTES;
    for (uint32_t i = 0; i < SLOTS; i++) {
        ((struct slot*)slot_at(i))->sequence = i;
    }

    struct timespec beg;
    struct timespec end;
    // NOLINTNEXTLINE(misc-include-cleaner) - pthread_t comes from <pthread.h> above
    pthread_t writer_th;
    pthread_t reader_th;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC comes from <time.h> above
    clock_gettime(CLOCK_MONOTONIC, &beg);
    pthread_create(&reader_th, NULL, reader, NULL);
    pthread_create(&writer_th, NULL, writer, NULL);
    pthread_join(writer_th, NULL);
    pthread_join(reader_th, NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ns = ((double)(end.tv_sec - beg.tv_sec) * NS_PER_S) + (double)(end.tv_nsec - beg.tv_nsec);
    const char* arm =
#if defined(PAD_INDICES) && defined(NO_COPY)
        "A4_pad_nocopy";
#elif defined(PAD_INDICES)
        "A2_pad_copy";
#elif defined(NO_COPY)
        "A3_nopad_nocopy";
#else
        "A1_nopad_copy";
#endif
    printf("RESULT: arm=%s records=%llu us_per_record=%.4f hdr_bytes=%zu payload=%d slots=%d\n", arm,
           (unsigned long long)g_n, elapsed_ns / NS_PER_US / (double)g_n, sizeof(struct hdr), PAYLOAD, SLOTS);
    return 0;
}
