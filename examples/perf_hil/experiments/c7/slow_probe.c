// C7 probe: test_large_sample.c's slow-reader sim (resent_through_a_queue()) with a per-step trace, per-sample
// latency and a parameter sweep. Built by slow_probe.sh against a chosen tickle.c (TICKLE_C).
// Args: samples service_us capacity loss_per_mille seed trace hz paced
#define main test_large_sample_main
#include "../../../../tests/test_large_sample.c"
#undef main

static uint64_t last_r;
static uint64_t last_d;
static uint64_t last_a;
static int last_del;

static void probe_trace(int trace, uint64_t t0) {
    if (trace && (pub.retransmitted != last_r || sim_socket_drops != last_d || delivered_count != last_del)) {
        printf("T %.3f ms acks+%llu resent+%llu drops+%llu queue %u delivered %d\n", (double)(test_mock_now - t0) / 1e6,
               (unsigned long long)(sim_sent[RECEIVER_ID] - last_a), (unsigned long long)(pub.retransmitted - last_r),
               (unsigned long long)(sim_socket_drops - last_d), sim_late_tail - sim_late_head, delivered_count - 1);
        last_r = pub.retransmitted;
        last_d = sim_socket_drops;
        last_a = sim_sent[RECEIVER_ID];
        last_del = delivered_count;
    }
}

static void run(int samples, uint64_t service_ns, uint32_t capacity, uint32_t loss, uint32_t seed, int trace,
                uint32_t hz, int paced) {
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    EXPECT_EQ_INT(tt_RET_OK, sim_publish(1, tt_SECOND));
    sim_settle(1, tt_SECOND);
    sim_rng = seed;
    sim_reader_service_ns = service_ns;
    sim_reader_capacity = capacity;
    sim_loss_per_mille = loss;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1;
    uint32_t before = pub.retransmitted;
    uint64_t dup0 = receiver.large.duplicate;
    uint64_t acks0 = sim_sent[RECEIVER_ID];
    uint64_t held0 = sender.large.resend_held;
    uint64_t t0 = test_mock_now;
    uint64_t pubt[64];
    last_r = pub.retransmitted;
    last_d = 0;
    last_a = sim_sent[RECEIVER_ID];
    for (int i = 0; i < samples; i++) {
        pubt[i] = test_mock_now;
        EXPECT_EQ_INT(tt_RET_OK, sim_publish((uint8_t)(i + 2), tt_SECOND));
        probe_trace(trace, t0);
        uint64_t next = t0 + ((uint64_t)(i + 1) * tt_SECOND / hz);
        while (test_mock_now < next && (!paced || sim_next_due() <= next) && sim_step()) {
            probe_trace(trace, t0);
        }
        if (test_mock_now < next) {
            test_mock_now = next;
        }
    }
    uint64_t since = test_mock_now;
    while (delivered_count < samples + 1 && test_mock_now - since < 4 * tt_SECOND && sim_step()) {
        probe_trace(trace, t0);
    }
    uint64_t lat_max = 0;
    uint64_t lat_sum = 0;
    int n = delivered_count - 1 < samples ? delivered_count - 1 : samples;
    for (int i = 0; i < n; i++) {
        uint64_t l = delivered_at[i + 1] - pubt[i];
        if (trace) {
            printf("L %d %.1f\n", i, (double)l / 1e6);
        }
        lat_sum += l;
        lat_max = l > lat_max ? l : lat_max;
    }
    printf("R paced %d svc %llu us cap %u loss %u seed %u hz %u: units %d resent %u drops %llu dup %llu acks %llu held "
           "%llu "
           "delivered %d/%d lat mean %.1f max %.1f ms end %.1f ms\n",
           paced, (unsigned long long)(service_ns / 1000), capacity, loss, seed, hz, samples * 723,
           pub.retransmitted - before, (unsigned long long)sim_socket_drops,
           (unsigned long long)(receiver.large.duplicate - dup0), (unsigned long long)(sim_sent[RECEIVER_ID] - acks0),
           (unsigned long long)(sender.large.resend_held - held0), n, samples, n ? (double)lat_sum / n / 1e6 : 0.0,
           (double)lat_max / 1e6, (double)(test_mock_now - t0) / 1e6);
    printf("X orig_drops %llu path_lost %llu", (unsigned long long)sim_socket_drops_original,
           (unsigned long long)sim_dropped[SENDER_ID]);
#if tt_ACKNACK_ROOM
    printf(" room_cut %llu room_sent %llu adverts %llu zero %llu sum %llu", (unsigned long long)sender.large.room_cut,
           (unsigned long long)sender.large.room_sent, (unsigned long long)receiver.large.room_adverts,
           (unsigned long long)receiver.large.room_zero, (unsigned long long)receiver.large.room_sum);
#endif
    printf("\n");
}

int main(int argc, char** argv) {
    int samples = argc > 1 ? atoi(argv[1]) : 10;
    uint64_t svc = argc > 2 ? strtoull(argv[2], NULL, 10) * 1000ULL : 20000ULL;
    uint32_t cap = argc > 3 ? (uint32_t)atoi(argv[3]) : 400;
    uint32_t loss = argc > 4 ? (uint32_t)atoi(argv[4]) : 0;
    uint32_t seed = argc > 5 ? (uint32_t)atoi(argv[5]) : 7;
    int trace = argc > 6 ? atoi(argv[6]) : 0;
    uint32_t hz = argc > 7 ? (uint32_t)atoi(argv[7]) : 30;
    int paced = argc > 8 ? atoi(argv[8]) : 1; // 0: the test's loop, whose clock jumps past `next` to the next event
    tt_log_set_level(TT_LOG_ERROR);
    run(samples > 60 ? 60 : samples, svc, cap, loss, seed, trace, hz, paced);
    return 0;
}
