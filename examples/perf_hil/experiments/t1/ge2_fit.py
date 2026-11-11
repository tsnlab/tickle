"""Fit the three-level loss model (slow quiet/busy, fast good/bad) to the PSU wlan_pcap statistics.

Model per directed link, wall-clock time t in ms:
  env state: quiet (mean quiet_s) / busy (mean busy_s), exponential sojourns.
  fast state: good (error good_p) / bad (error bad_p), exponential sojourns; mean good sojourn is good_ms in the quiet
  env and good_ms / busy_k in the busy env; mean bad sojourn is bad_ms.
A frame is sent every 10 ms; an attempt that fails is repeated after a backoff (0.4, 0.8, 1.6, 3.2, 3.2, 3.2 ms), at most 7
attempts. Outputs: attempts distribution, retry-flag share, per-second retry rate quantiles.
"""
import math
import random
import sys


def sim(params, seconds=3600, seed=1):
    good_ms, bad_ms, good_p, bad_p, quiet_s, busy_s, busy_k = params
    rnd = random.Random(seed)
    env_busy = False
    env_until = rnd.expovariate(1.0 / (quiet_s * 1000.0))
    bad = False
    until = rnd.expovariate(1.0 / good_ms)
    t = 0.0
    att = [0] * 9
    sec_n = {}
    sec_r = {}
    nframes = int(seconds * 100)
    backoff = [0.4, 0.8, 1.6, 3.2, 3.2, 3.2]
    for i in range(nframes):
        t0 = i * 10.0
        t = t0
        k = 0
        while True:
            k += 1
            while t >= env_until:
                env_busy = not env_busy
                env_until += rnd.expovariate(1.0 / ((busy_s if env_busy else quiet_s) * 1000.0))
            while t >= until:
                bad = not bad
                mean = bad_ms if bad else (good_ms / busy_k if env_busy else good_ms)
                until += rnd.expovariate(1.0 / mean)
            p = bad_p if bad else good_p
            if rnd.random() >= p or k >= 7:
                break
            t += backoff[min(k - 1, 5)]
        att[min(k, 8)] += 1
        s = int(t0 / 1000.0)
        sec_n[s] = sec_n.get(s, 0) + 1
        if k > 1:
            sec_r[s] = sec_r.get(s, 0) + 1
    tot = sum(att)
    retry_share = 1.0 - att[1] / tot
    mean_att = sum(k * v for k, v in enumerate(att)) / tot
    rates = sorted(sec_r.get(s, 0) / n for s, n in sec_n.items())
    q = lambda p: rates[int(p * (len(rates) - 1))]
    return retry_share, mean_att, att, (q(0.5), q(0.9), q(0.99))


TARGETS = {
    "psu_cs": dict(retry=0.080, mean=1.09, p50=0.02, p90=0.27, p99=0.55, a2=1546 / 55766, a3=764 / 55766, tail5=176 / 55766),
    "library": dict(retry=0.083, mean=1.09, p50=0.00, p90=0.14, p99=0.30, a2=20099 / 460067, a3=8139 / 460067, tail5=248 / 460067),
    "pioneer": dict(retry=0.195, mean=1.23, p50=0.11, p90=0.50, p99=0.68, a2=5560 / 74924, a3=2514 / 74924, tail5=321 / 74924),
    "ug": dict(retry=0.020, mean=1.02, p50=0.00, p90=0.07, p99=0.16, a2=2678 / 206465, a3=365 / 206465, tail5=3 / 206465),
}


def cost(res, tg):
    retry, mean, att, (p50, p90, p99) = res
    n = sum(att)
    a2, a3 = att[2] / n, att[3] / n
    return (abs(retry - tg["retry"]) / max(tg["retry"], 0.01)
            + abs(a2 - tg["a2"]) / max(tg["a2"], 0.005) * 0.5
            + abs(a3 - tg["a3"]) / max(tg["a3"], 0.003) * 0.5
            + abs(p90 - tg["p90"]) / max(tg["p90"], 0.05)
            + abs(p99 - tg["p99"]) / max(tg["p99"], 0.1) * 0.5
            + abs(p50 - tg["p50"]) / 0.1
            + 2.0 * abs(mean - tg["mean"]) / (tg["mean"] - 1.0 + 0.01)
            + 2.0 * abs(sum(att[5:]) / n - tg["tail5"]) / (tg["tail5"] + 0.002))


def main():
    name = sys.argv[1]
    tg = TARGETS[name]
    rnd = random.Random(7)
    best = None
    for it in range(int(sys.argv[2]) if len(sys.argv) > 2 else 400):
        params = (
            math.exp(rnd.uniform(math.log(30), math.log(3000))),   # good_ms
            math.exp(rnd.uniform(math.log(2), math.log(60))),      # bad_ms
            rnd.choice([0.0, 0.002, 0.005, 0.01]),                 # good_p
            rnd.uniform(0.3, 0.85),                                # bad_p
            math.exp(rnd.uniform(math.log(2), math.log(60))),      # quiet_s
            math.exp(rnd.uniform(math.log(1), math.log(15))),      # busy_s
            math.exp(rnd.uniform(math.log(2), math.log(60))),      # busy_k
        )
        res = sim(params, seconds=900)
        c = cost(res, tg)
        if best is None or c < best[0]:
            best = (c, params)
    c, params = best
    res = sim(params, seconds=3600, seed=3)
    print(name, "cost %.2f" % c, "params good_ms %.0f bad_ms %.1f good_p %.3f bad_p %.2f quiet_s %.1f busy_s %.1f busy_k %.1f" % params)
    print("  model retry %.3f mean_att %.3f att %s sec p50/p90/p99 %.2f/%.2f/%.2f" % (res[0], res[1], res[2][1:], *res[3]))
    print("  target retry %.3f mean %.2f a2 %.4f a3 %.4f sec p50/p90/p99 %.2f/%.2f/%.2f" % (tg["retry"], tg["mean"], tg["a2"], tg["a3"], tg["p50"], tg["p90"], tg["p99"]))


main()
