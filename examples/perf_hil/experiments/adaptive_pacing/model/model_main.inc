static void load_sched(const char* str) {
    g_sched_n = 0;
    const char* p = str;
    while (*p != 0) {
        unsigned t = 0;
        unsigned r = 0;
        int n = 0;
        if (sscanf(p, "%u:%u%n", &t, &r, &n) != 2) {
            break;
        }
        g_sched[g_sched_n].t_ns = (uint64_t)t * 1000000000ULL;
        g_sched[g_sched_n].bps = (uint64_t)r * 1000000ULL;
        g_sched_n++;
        p += n;
        while (*p == ' ') {
            p++;
        }
    }
}

struct pat {
    const char* name;
    const char* sched;
    uint64_t dur_s;
    uint64_t one_way_ms;
    uint32_t loss_ppm;
    uint64_t tick_ms;
};

static const struct pat PATS[] = {
    {"P5a", "0:30 10:10 20:30 30:10 40:30 50:10 60:30 70:10 80:30 90:10 100:30 110:10 120:30 130:10 140:30 150:10 160:30 170:10", 180, 5, 10000, 0},
    {"P5b", "0:30 30:10 60:30 90:10 120:30 150:10", 180, 5, 10000, 0},
    {"P7", "0:20 5:19 10:6 15:24 20:26 25:7 30:14 35:15 40:25 45:12 50:25 55:25 60:14 65:8 70:18 75:16 80:12 85:22 90:30 95:9 100:22 105:25 110:23 115:17 120:17 125:27 130:16 135:16 140:11 145:20 150:7 155:23 160:28 165:12 170:27 175:29", 180, 5, 10000, 0},
    {"P6", "0:30 60:4 65:30", 150, 5, 10000, 0},
    {"P3", "0:30 5:29 10:28 15:27 20:27 25:26 30:25 35:24 40:23 45:22 50:21 55:20 60:20 65:19 70:18 75:17 80:16 85:15 90:14 95:14 100:13 105:12 110:11 115:10 120:9 125:8 130:7 135:7 140:6 145:5 150:4", 180, 5, 10000, 0},
    {"P4", "0:4 5:5 10:6 15:7 20:7 25:8 30:9 35:10 40:11 45:12 50:13 55:14 60:14 65:15 70:16 75:17 80:18 85:19 90:20 95:20 100:21 105:22 110:23 115:24 120:25 125:26 130:27 135:27 140:28 145:29 150:30", 180, 5, 10000, 0},
    {"STEP", "0:4 40:30", 120, 5, 10000, 0},
    {"M1", "0:30", 100, 5, 10000, 0},
    {"M2", "0:30", 100, 20, 30000, 0},
    {"S1", "0:4", 100, 5, 10000, 0},
    {"S2", "0:4", 100, 20, 30000, 0},
};

static double pct_where(const struct result* r, int lo, int hi) {
    uint64_t c = 0;
    for (int i = lo; i < hi && i < 512; i++) {
        c += r->ratio_hist[i];
    }
    return r->steps_counted != 0 ? 100.0 * (double)c / (double)r->steps_counted : 0.0;
}

static double median_ratio(const struct result* r) {
    uint64_t half = r->steps_counted / 2U;
    uint64_t acc = 0;
    for (int i = 0; i < 512; i++) {
        acc += r->ratio_hist[i];
        if (acc >= half) {
            return (double)i / 100.0;
        }
    }
    return 0.0;
}

static uint64_t pctl(const struct result* r, unsigned pc) {
    uint64_t tmp[64];
    uint32_t n = r->samples_done < 64U ? r->samples_done : 64U;
    for (uint32_t i = 0; i < n; i++) {
        tmp[i] = r->completion_ns[i];
    }
    for (uint32_t i = 1; i < n; i++) {
        uint64_t v = tmp[i];
        uint32_t j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    if (n == 0) {
        return 0;
    }
    uint32_t k = (n * pc) / 100U;
    return tmp[k < n ? k : n - 1U];
}

int main(int argc, char** argv) {
    const char* only = argc > 1 && strcmp(argv[1], "all") != 0 ? argv[1] : NULL;
    const uint64_t SEC = 1000000000ULL;
    if (argc > 2) {
        g_oracle_noise_pct = atoi(argv[2]);
    }
    g_defer_repairs = getenv("DEFER") != NULL;
    if (getenv("SEED") != NULL) {
        g_seed = (uint32_t)atoi(getenv("SEED"));
    }
    bool fixed = argc > 3 && strcmp(argv[3], "fixed") == 0;
    unsigned tick = argc > 4 ? (unsigned)atoi(argv[4]) : 0U;
    printf("%-5s %-6s | %5s %5s %5s %6s %5s %4s | %3s %6s %6s %6s | %6s | recov\n", "pat", "size", "med", "in30", "over1", "ov1.3", "und.7", "cuts", "n", "p50", "p99", "max", "retx%");
    for (size_t pi = 0; pi < sizeof(PATS) / sizeof(PATS[0]); pi++) {
        if (only != NULL && strcmp(only, PATS[pi].name) != 0) {
            continue;
        }
        for (int big = 0; big < 2; big++) {
            load_sched(PATS[pi].sched);
            struct link_cfg cfg = {{30000000ULL, 30000000ULL, 30000000ULL}, 1000 * SEC, 2000 * SEC, 100U * UNIT_WIRE, PATS[pi].one_way_ms * MS, (uint64_t)tick * MS, PATS[pi].loss_ppm, 3 * MS, true, true};
            g_model_sample_units = big != 0 ? 723U : 89U;
            static struct result res;
            run(&cfg, fixed ? MODE_FIXED : MODE_ADAPTIVE, 28200000ULL, 1000000000ULL, 100000ULL, 4 * SEC, PATS[pi].dur_s * SEC, 1U, &res);
            double rec = -1.0;
            if (strcmp(PATS[pi].name, "STEP") == 0) {
                for (uint32_t k = 400; k < 1200; k++) {
                    if (res.rate_series[k] >= 20810U) { rec = (double)(k - 400) / 10.0; break; }
                }
            }
            // entries into "under 0.7 x capacity" after 2 s: the over-cut count
            int cuts = 0;
            bool was_under = false;
            for (uint32_t k = 20; k < 2048U && (uint64_t)k * 100ULL * MS < PATS[pi].dur_s * SEC; k++) {
                uint64_t capb = link_rate(&cfg, (uint64_t)k * 100ULL * MS) * 991ULL / 1000ULL / 1000U;
                bool under = (uint64_t)res.rate_series[k] * 10U < capb * 7U;
                cuts += (under && !was_under) ? 1 : 0;
                was_under = under;
            }
            printf("%-5s %-6s | %5.2f %4.0f%% %4.0f%% %5.0f%% %4.0f%% %4d | %3u %6.0f %6.0f %6.0f | %5.1f%% | %s\n", PATS[pi].name, big != 0 ? "1MiB" : "128KiB", median_ratio(&res), pct_where(&res, 70, 131), pct_where(&res, 101, 512), pct_where(&res, 131, 512), pct_where(&res, 0, 70), cuts, res.samples_done, ms(pctl(&res, 50)), ms(pctl(&res, 99)), ms(pctl(&res, 100)), res.sent_units != 0 ? 100.0 * (double)res.repair_units / (double)res.sent_units : 0.0, rec < 0 ? "-" : (snprintf((char[1]){0}, 1, "") , "ok"));
            if (rec >= 0) { printf("      recovery %.1f s\n", rec); }
        }
    }
    return 0;
}
