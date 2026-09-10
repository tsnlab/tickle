window.BENCHMARK_DATA = {
  "lastUpdate": 1789049722277,
  "repoUrl": "https://github.com/tsnlab/tickle",
  "entries": {
    "Latency (ping/pong)": [
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "5567772612d070110d51a4d4df78d070c9f30ca0",
          "message": "Track perf-test result history with github-action-benchmark\n\n* run_perf.sh: parse rtt avg/mdev, packet loss, and send/recv Mbps out\n  of the example binaries' own stdout into latency-benchmark.json and\n  throughput-benchmark.json (github-action-benchmark's custom format)\n* performance.yml: feed those into benchmark-action/github-action-benchmark,\n  which stores each run's numbers on gh-pages and fails the job if a\n  metric regresses past 200% of its previous value\n* Verified the JSON extraction locally against a real run on rpi#1/rpi#2\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T08:37:51Z",
          "url": "https://github.com/tsnlab/tickle/commit/5567772612d070110d51a4d4df78d070c9f30ca0"
        },
        "date": 1788770435598,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 2.249,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.942,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "5567772612d070110d51a4d4df78d070c9f30ca0",
          "message": "Track perf-test result history with github-action-benchmark\n\n* run_perf.sh: parse rtt avg/mdev, packet loss, and send/recv Mbps out\n  of the example binaries' own stdout into latency-benchmark.json and\n  throughput-benchmark.json (github-action-benchmark's custom format)\n* performance.yml: feed those into benchmark-action/github-action-benchmark,\n  which stores each run's numbers on gh-pages and fails the job if a\n  metric regresses past 200% of its previous value\n* Verified the JSON extraction locally against a real run on rpi#1/rpi#2\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T08:37:51Z",
          "url": "https://github.com/tsnlab/tickle/commit/5567772612d070110d51a4d4df78d070c9f30ca0"
        },
        "date": 1788770495957,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 2.075,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 1.423,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "96a034650eef3e16ae7b5459db66a4183e44a63a",
          "message": "Add -l (log level) and -n (endpoint/topic name) to all examples\n\n* Expose tt_LogLevel and tt_log_set_level() through a new public\n  include/tickle/log.h instead of examples reaching into the\n  library-internal src/log.h\n* Every example gets -l debug|info|warning|error|none and -n to\n  override its hardcoded topic/service name at runtime\n* perf_client.c already had a parse_args()/cli_options split to stay\n  under the cognitive-complexity lint threshold; apply the same split\n  to the other 7 examples now that two more flags push them over it\n  too\n* Verified: full project builds and lints clean; -l debug surfaces\n  the library's DEBUG-level packet logging that's normally suppressed\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T17:54:55+09:00",
          "tree_id": "dfdc86f7549c254fc0f30643b981d879a4b1b9e5",
          "url": "https://github.com/tsnlab/tickle/commit/96a034650eef3e16ae7b5459db66a4183e44a63a"
        },
        "date": 1788771376115,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 2.943,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 1.057,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "973c53666bdefb77800023c5ffe1edea412a641e",
          "message": "Cut syscall overhead and latency jitter in the receive/send path\n\nFour changes, each measured on rpi#1/rpi#2 before committing:\n\n* A: Replace the SO_RCVTIMEO + recvfrom() pattern in tt_receive() with\n  poll(). The old code re-armed SO_RCVTIMEO via setsockopt() on almost\n  every call because the requested timeout tracks whatever scheduled\n  event is due next and so changes on nearly every iteration; poll()\n  takes the timeout as a plain argument instead, so no socket mutation\n  is needed at all. Measured on a ping run (-c 30 -i 0.1): setsockopt\n  calls went from 1255 to 2, and rtt avg/mdev dropped from ~2.1/1.0ms\n  to ~1.45/0.47ms.\n* B: Precompute the broadcast sockaddr_in once in tt_bind() instead of\n  re-parsing _tt_CONFIG.broadcast with inet_addr() on every tt_send().\n* C: Give tt_Client a fixed-size cache_buf and point client->cache\n  into it instead of malloc'ing/freeing a buffer on every RPC call\n  (only one call can be outstanding at a time already, so a fixed\n  buffer sized like tx_buffer's worst case covers every real request).\n* D: Request a larger SO_SNDBUF/SO_RCVBUF as best-effort insurance\n  against bursty drops; the kernel clamps it to net.core.[rw]mem_max\n  for an unprivileged process, so this is a no-op on hosts already at\n  that ceiling (both test Pis, in fact - default was already at the\n  208KB max) but helps on hosts with more headroom.\n\nThroughput was already at ~935 Mbps on the Pis' Gigabit test link\n(near line rate for this payload size) both before and after, with\n0 dropped packets throughout - the real payoff here is latency and\nCPU/syscall overhead, which will matter more on the actual 10Base-T1S\ntarget than on this Gigabit stand-in.\n\nRegression-tested the SIGINT/hang fixes from the previous commit\nagainst this rewrite: 5/5 clean exits when the peer dies mid-run,\n3/3 clean SIGINT exits with no responder.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T18:11:55+09:00",
          "tree_id": "d1f927afd6bc00aee98bb4f2a1dae20c56e46f67",
          "url": "https://github.com/tsnlab/tickle/commit/973c53666bdefb77800023c5ffe1edea412a641e"
        },
        "date": 1788772664472,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 1.318,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.305,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "824072fb57f3356c542c07d9b0b6a0c4eb1a2069",
          "message": "Flush RPC request/response immediately instead of waiting on the 1ms tick\n\ntt_Client_call(), call_retry(), and the server's response send all\npassed is_flush=false to end_encode(), so a message with nothing else\nqueued sat in tx_buffer until either it filled up or node_flush()'s\nperiodic tt_NODE_TX_INTERVAL (1ms) tick caught it. That's the right\ndefault for pub/sub (no reason to force a flush per publish when\nbatching is free), but wrong for RPC: the caller is synchronously\nblocked on the reply, so both legs of the round trip were eating up\nto ~1ms of pure batching delay for no benefit.\n\nMeasured on rpi#1/rpi#2 (ping -c 30 -i 0.1): rtt avg 1.451ms -> 0.217ms,\nmdev 0.466ms -> 0.018ms. Throughput (perf_client/perf_server, unaffected\nsince publish still batches) stayed at ~930 Mbps, 0 dropped. Re-ran the\nSIGINT/hang regression checks: 3/3 clean exits both ways.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T18:21:52+09:00",
          "tree_id": "d23d50ec984849fdff26f689f2f07bc4ef0cf3fc",
          "url": "https://github.com/tsnlab/tickle/commit/824072fb57f3356c542c07d9b0b6a0c4eb1a2069"
        },
        "date": 1788773378906,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.044,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "a90c53b1e0f3d362bb6b8ca7d63769b77586dff9",
          "message": "Fix run_pair.sh hanging forever when pong's QEMU gets SIGTTIN-stopped\n\nqemu-system-riscv32 -nographic reads stdin (serial console + monitor\nmultiplexing). pong's instance was backgrounded without redirecting its\nstdin away from the invoking terminal, so if it tried to read stdin while\nbackgrounded, the shell's job control could stop it with SIGTTIN. A stopped\nprocess doesn't respond to the later kill (SIGTERM) - it stays stopped, not\nterminated - so the script's wait on it blocked indefinitely. Redirect both\ninstances' stdin from /dev/null so neither depends on foreground/background\njob-control semantics; applied the same defensive fix to netns_run_pair.sh's\nbackgrounded pong.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-08T16:28:18+09:00",
          "tree_id": "27f0d331d5de4092325b019b1f7d937881ec69d9",
          "url": "https://github.com/tsnlab/tickle/commit/a90c53b1e0f3d362bb6b8ca7d63769b77586dff9"
        },
        "date": 1788852640759,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.215,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "83e073ee8f91fa856ccd49b30892669800e2e0a4",
          "message": "Clean up HAL layer for 1.0: consistent contract, drop dead generic platform\n\n- Move tt_get_ns()'s declaration from tickle.h into hal.h, alongside the rest\n  of the per-platform HAL contract (tt_get_node_id/tt_bind/tt_close/tt_send/\n  tt_receive) it belongs with - it was the one HAL function declared outside\n  hal.h for no reason.\n- Standardize zero-arg HAL functions on `(void)` (declarations in hal.h and\n  definitions in hal_linux.c) instead of empty parens, matching what\n  hal_freertos.c and tests/test_mock.h's mock already did.\n- Remove the dead `UNUSED` macro duplicated into both hal_linux.c and\n  hal_freertos.c but never actually used in either.\n- Delete hal_generic.h and the TT_PLATFORM_GENERIC branch: it had no matching\n  src/hal_generic.c, so selecting it never actually built - a known gap now\n  closed by making unsupported platforms fail loudly (a #error naming the two\n  real options) instead of silently offering a HAL that doesn't exist. The\n  top-level Makefile's PLATFORM fallback errors the same way for non-Linux\n  hosts instead of guessing 'generic'.\n- Document why hal_freertos.c's tt_bind() has no SO_SNDBUF/SO_RCVBUF tuning\n  (unlike hal_linux.c): lwIP has no SO_SNDBUF at all, and SO_RCVBUF support\n  is compiled out by default - not an oversight.\n\nVerified: native build + unit tests + top-level lint, FreeRTOS cross-build +\nits own lint, and a full QEMU two-instance round trip (make test-qemu) all\nstill pass.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-08T16:36:30+09:00",
          "tree_id": "7704fc46cf3c93c1cb8072cdda77fc8edd9f2ba3",
          "url": "https://github.com/tsnlab/tickle/commit/83e073ee8f91fa856ccd49b30892669800e2e0a4"
        },
        "date": 1788853527642,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.214,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "b50ac2a39af82b047ecaf0f746489e95e79f2f09",
          "message": "Fix run_perf.sh's binary paths after the platform/linux/ move\n\nThe example binaries build under platform/linux/ now, not the repo root -\nrun_paired_test()'s `cd ~/tickle && ./ping` has been failing with \"No such\nfile or directory\" since that reorganization landed. update_and_build()'s\n`make all` is fine as-is (the root Makefile forwards to platform/linux/).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T13:35:28+09:00",
          "tree_id": "5c8461b3813807bf2485197ba8d2b21a74e58dfa",
          "url": "https://github.com/tsnlab/tickle/commit/b50ac2a39af82b047ecaf0f746489e95e79f2f09"
        },
        "date": 1789014963901,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "46e9f52a8e8b3821ac147893bd5e72ad5f0f262a",
          "message": "Fix run_perf.sh's binary paths after the platform/linux/ move\n\nThe example binaries build under platform/linux/ now, not the repo root -\nrun_paired_test()'s `cd ~/tickle && ./ping` has been failing with \"No such\nfile or directory\" since that reorganization landed. update_and_build()'s\n`make all` is fine as-is (the root Makefile forwards to platform/linux/).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T04:35:28Z",
          "url": "https://github.com/tsnlab/tickle/commit/46e9f52a8e8b3821ac147893bd5e72ad5f0f262a"
        },
        "date": 1789015133647,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.213,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 2,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "dd0e9e9051e86cc69d544b45e6392a7b92e91b9c",
          "message": "Don't let perf_client's -i 0 loop block in poll() between sends\n\nperf_client's \"as fast as poll() allows\" loop was publish() then\ntt_Node_poll(&node, -1), which resolves to a 100us tt_RECEIVE_TIMEOUT\nreceive wait. That wait is invisible while broadcasting - the node loops\nits own packets back, so poll() returns immediately - but once the\nPublisher unicasts to its lone discovered Subscriber it gets nothing back,\nturning the 100us into a hard per-iteration floor: ~40x slower on the HIL\nPis (883 -> 21 Mbps), lossless either way, purely a send-rate effect.\n\nIn the -i 0 path it now asks for a minimal non-blocking poll (1ns, since\ntt_Node_poll treats 0 as \"do nothing\"): run due scheduler work plus one\nreceive drain, then straight back to publishing. Rate-limited runs\n(-i > 0) keep -1 - they're idle between sends anyway.\n\nDESIGN.md's discovery/unicast section gets the matching note: a high-rate\nPublisher must not block in poll() between sends, and Publisher-side\nunicast is for cutting broadcast traffic on low-subscriber topics, not a\nthroughput optimization.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T13:46:46+09:00",
          "tree_id": "beb4f7f40cefbfd447a3baeae7da59f6ad7087e8",
          "url": "https://github.com/tsnlab/tickle/commit/dd0e9e9051e86cc69d544b45e6392a7b92e91b9c"
        },
        "date": 1789015641842,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "43efa03b099bcfac106052c28691669f5d42df7c",
          "message": "opt6: find_endpoint() via lazy-rebuilt open-addressed hash index\n\nO(1) lookup keyed by endpoint id instead of an O(endpoint_count) scan.\nendpoint_index[] is rebuilt from endpoints[] on the next lookup after any\nadd/remove. No effect on a few-endpoint node; a scaling change.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T06:34:10Z",
          "url": "https://github.com/tsnlab/tickle/commit/43efa03b099bcfac106052c28691669f5d42df7c"
        },
        "date": 1789022202594,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.209,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.048,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "2c0f03a89a8fa3720cb44dbb36c448222fc2c729",
          "message": "run_perf.sh: add a 100-byte small-message perf run\n\nFull-MTU throughput is already at GbE line rate on the HIL Pis, so it can't\nshow whether internal changes cut per-message CPU cost. Small messages\n(batched several per packet) are per-message-CPU-bound instead - message\nrate there is the signal.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T15:56:06+09:00",
          "tree_id": "caf74c75bea97950bb6c0313865706ce908b4a70",
          "url": "https://github.com/tsnlab/tickle/commit/2c0f03a89a8fa3720cb44dbb36c448222fc2c729"
        },
        "date": 1789023410296,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.009,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "1329b73d353f3ebf1543a4ade4a0a58db468f91e",
          "message": "Merge branch 'main' into perf-experiments",
          "timestamp": "2026-09-10T06:56:13Z",
          "url": "https://github.com/tsnlab/tickle/commit/1329b73d353f3ebf1543a4ade4a0a58db468f91e"
        },
        "date": 1789023485428,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.01,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "7751a62fb66deb3ef0789577162a4e5be4db1636",
          "message": "opt6: find_endpoint() via lazy-rebuilt open-addressed hash index\n\nO(1) lookup keyed by endpoint id instead of an O(endpoint_count) scan.\nendpoint_index[] is rebuilt from endpoints[] on the next lookup after any\nadd/remove. No effect on a few-endpoint node; a scaling change.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:16:43+09:00",
          "tree_id": "f84a813dd58415b3f7bc84c45049748d4ff3171a",
          "url": "https://github.com/tsnlab/tickle/commit/7751a62fb66deb3ef0789577162a4e5be4db1636"
        },
        "date": 1789028257325,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "74f70176a06a8442eb6819d958a1421da725eb0b",
          "message": "test-linux: run in a veth namespace pair, not shared loopback\n\ntest.sh put both nodes in one network namespace on loopback, bound to the\nsame wildcard address and told apart only by -I. A kernel delivers a\nunicast packet aimed at one such socket to whichever bound last, so that\nsetup could not validate any unicast path - a server's CallResponse, a\nPublisher/Client that discovered a peer, the reactive discovery reply -\nand produced misleading ping_pong/perf loss once those landed.\n\nIt now sets up its own veth-joined pair (tickle-ns1/tickle-ns2, real\ndistinct 192.168.10.1/.2), torn down on exit, and drops -I so node IDs\ncome from tt_get_node_id() auto-detection for real. Needs passwordless\nsudo for `ip`; `make test` stays privilege-free. netns.mk's manual runX\ntargets are unchanged and independent (distinct names, no dependency).\n\nFolds in the throwaway test-netns.sh. Docs (README, CONTRIBUTING, Makefile,\nDESIGN, netns.mk) updated to match.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:35:51+09:00",
          "tree_id": "b00bfca9b46c7d696307bed9ccb5768be8abf239",
          "url": "https://github.com/tsnlab/tickle/commit/74f70176a06a8442eb6819d958a1421da725eb0b"
        },
        "date": 1789029396370,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "b8f8c7618dfe4e8c8a5b219e2d7aca899ae70322",
          "message": "test.sh: shellcheck-clean (SC1072/1073, SC2164, SC2024)\n\nThe `# shellcheck disable=SCxxx - freeform text` style is a parse error in\ncurrent shellcheck (SC1072/1073) - split the rationale onto its own comment\nline. Also `cd || exit 1` (SC2164), and suppress SC2024 (the log redirects\nare the non-root caller's shell's, on purpose). No behavior change.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:45:37+09:00",
          "tree_id": "13b21fd44632af04ac1478945487877a1c8eab88",
          "url": "https://github.com/tsnlab/tickle/commit/b8f8c7618dfe4e8c8a5b219e2d7aca899ae70322"
        },
        "date": 1789029981929,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.015,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "19423dfe85ac9e7b70211cbdaef27e2ce107713b",
          "message": "Make the remaining shell scripts shellcheck-clean too\n\nplatform/freertos/test.sh: `cd || exit 1` (SC2164), quote $server_role /\n$client_node in the make lines (SC2086). run_perf.sh: disable SC2029 on\nthe ssh_run helper (the argument is meant to expand on the remote host).\nNo behavior change - lets `Check all` (ludeeus/action-shellcheck) pass.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:47:32+09:00",
          "tree_id": "e2e16ff7c027c0e95215c47b29712ebc6aac5a8a",
          "url": "https://github.com/tsnlab/tickle/commit/19423dfe85ac9e7b70211cbdaef27e2ce107713b"
        },
        "date": 1789030096373,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "eccaaa69a2dcfeccd1531009a2050a356c47626b",
          "message": "CI: qemu-system-riscv -> qemu-system-misc on ubuntu-latest\n\nGitHub's ubuntu-latest is Ubuntu 24.04 (noble) now, where the RISC-V\nsystem emulators moved from the qemu-system-riscv package into\nqemu-system-misc. Test all has been failing at the toolchain-install step\nfor this since the runner image bumped; nothing downstream (make test-all,\nthe FreeRTOS QEMU round trip) had run.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T18:17:01+09:00",
          "tree_id": "1ee2d0100196c4721430e1e57e4bbe71aeb59c23",
          "url": "https://github.com/tsnlab/tickle/commit/eccaaa69a2dcfeccd1531009a2050a356c47626b"
        },
        "date": 1789031865428,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.014,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "9f300060a1fd96f0086f0d8895d7771f575511c3",
          "message": "Fix FreeRTOS RX-drain hang: gate tt_try_receive() on a zero-timeout select()\n\nopt1 (21a640a) added drain_rx(), which calls tt_try_receive() repeatedly\nuntil it reports nothing left. On Linux that's a recvfrom(MSG_DONTWAIT)\nper call and works. On lwIP, MSG_DONTWAIT is not honored per-call without\nO_NONBLOCK on the socket, so the recvfrom() after the last datagram blocks\nforever - tt_Node_poll() never returns and the scheduler stops running.\nStandalone repro: the FreeRTOS publisher sent \"data=0\" once and went\nsilent instead of every 500ms.\n\nMirror tt_receive()'s existing lwIP pattern in this file (select() as the\nreadiness primitive, since lwIP has no poll()) but with a {0,0} timeout:\nonly recvfrom() when the socket is readable, otherwise return -1\nimmediately. make test-freertos now passes all four scenarios.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T19:05:39+09:00",
          "tree_id": "8bb3e59937a6e05e57b4db6ed1876a4a584f10bd",
          "url": "https://github.com/tsnlab/tickle/commit/9f300060a1fd96f0086f0d8895d7771f575511c3"
        },
        "date": 1789034808113,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "b37baa8bcaeeb0056d9b551cb9b8dec11c2f17d2",
          "message": "CI: give check-all.yml a lint context for src/hal_freertos.c\n\ncheck-all.yml's cpp-linter runs clang-tidy on every changed .c file, but\n`bear -- make all` only builds the Linux platform - so src/hal_freertos.c\nhad no compile-DB entry and clang-tidy parsed it with no lwIP/picolibc\nincludes, failing with `'lwip/netif.h' file not found` plus a cascade of\n\"no header providing X\" warnings. This stayed hidden until now only\nbecause no green-CI commit had touched that file since check-all.yml was\nrepaired; the QEMU fix is the first, so it tripped it.\n\n- check-all.yml: synthesise a compile-DB entry for hal_freertos.c with\n  the same cross-compile flags platform/freertos/Makefile's own `lint`\n  target uses (--target, -nostdinc, explicit picolibc/gcc -isystem, the\n  FreeRTOS + lwIP submodule include paths). No FreeRTOS build - clang-tidy\n  only needs the flags and headers.\n- .clang-tidy: ignore lwip/* for misc-include-cleaner. lwIP's public API\n  is a handful of umbrella headers that deliberately re-export from\n  private sub-headers; \"include the exact provider\" doesn't apply.\n- hal_freertos.c: NOLINT the errno-macro uses (picolibc routes them via\n  <sys/errno.h>) and the tt_send_iov iovec const-cast, matching the\n  inline-NOLINT style hal_linux.c already uses for the same patterns.\n\nplatform/freertos/Makefile's lint still covers this file too (it disables\nthese checks wholesale for the cross-compiled HAL); check-all.yml now\njust stops choking on it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T19:26:36+09:00",
          "tree_id": "02464e5dca99c4dcf3653d1d5c2cfdfe9dcc134c",
          "url": "https://github.com/tsnlab/tickle/commit/b37baa8bcaeeb0056d9b551cb9b8dec11c2f17d2"
        },
        "date": 1789036043917,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.014,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "70fd890609acc2a965398492496bdb07a0beb6d4",
          "message": "CI: exclude third_party from check-all.yml's shellcheck\n\nsubmodules: true (added in b37baa8 for hal_freertos.c's lint context)\nalso drops third_party/lwip's own shell scripts on disk, which shellcheck\nthen flagged (SC2148/SC2045/...). cpp-linter auto-skips submodules;\nshellcheck needs shellcheck_ignore_paths.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T19:28:26+09:00",
          "tree_id": "c0943223a7b83f79c777f79a78f382d598730118",
          "url": "https://github.com/tsnlab/tickle/commit/70fd890609acc2a965398492496bdb07a0beb6d4"
        },
        "date": 1789036151307,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "41898282+github-actions[bot]@users.noreply.github.com",
            "name": "github-actions[bot]",
            "username": "github-actions[bot]"
          },
          "committer": {
            "email": "41898282+github-actions[bot]@users.noreply.github.com",
            "name": "github-actions[bot]",
            "username": "github-actions[bot]"
          },
          "distinct": true,
          "id": "d55a094648e28aeebd2dbf4255b756f20d91e443",
          "message": "CI: publish a per-platform status table on the benchmark page\n\ngithub.io only had the perf history charts. Add a single table above them\n(https://tsnlab.github.io/tickle/dev/bench/) showing, per platform, whether\nit compiled, whether each test tier passed, and - for the HIL row - the\nlatest throughput / latency / small-message rate. Refreshed on every push\nto main.\n\n- .github/scripts/dashboard.py: merges a producer's section into\n  dev/bench/status.json and (re-)injects the rendered table into\n  dev/bench/index.html between HTML markers. github-action-benchmark\n  regenerates that file every perf run, so the markers are re-inserted\n  (after </header>) whenever they've gone.\n- .github/scripts/publish_dashboard.sh: shared entry point - clones\n  gh-pages, folds in one section, pushes with a fetch+reapply retry loop\n  (test-all.yml, performance.yml and github-action-benchmark can all push\n  there at once on a main push). Also drops a root index.html redirect so\n  the site root stops 404ing. Runs on ubuntu-latest (python3 + git, no jq).\n- test-all.yml: each compile + test tier is now its own step (so the table\n  can pinpoint which broke); a Gate step still fails the job on any tier\n  failure; on a push to main a Publish step writes the \"buildtest\" section\n  (Linux x86-64 + FreeRTOS/QEMU rows).\n- performance.yml / run_perf.sh: run_perf.sh writes perf-frag.json (RPi\n  build/test outcome + numbers, seeded red so an early abort still shows);\n  a new publish job on ubuntu-latest folds it in as the \"perf\" section.\n\nPRs still run every tier (Gate enforces green) but don't touch the page.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T23:14:29+09:00",
          "tree_id": "96724d27c41532234bec4f8fee9a24dc4e4f5c1b",
          "url": "https://github.com/tsnlab/tickle/commit/d55a094648e28aeebd2dbf4255b756f20d91e443"
        },
        "date": 1789049718512,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
          },
          {
            "name": "packet loss",
            "value": 0,
            "unit": "%"
          }
        ]
      }
    ],
    "Throughput (perf_client/perf_server)": [
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "5567772612d070110d51a4d4df78d070c9f30ca0",
          "message": "Track perf-test result history with github-action-benchmark\n\n* run_perf.sh: parse rtt avg/mdev, packet loss, and send/recv Mbps out\n  of the example binaries' own stdout into latency-benchmark.json and\n  throughput-benchmark.json (github-action-benchmark's custom format)\n* performance.yml: feed those into benchmark-action/github-action-benchmark,\n  which stores each run's numbers on gh-pages and fails the job if a\n  metric regresses past 200% of its previous value\n* Verified the JSON extraction locally against a real run on rpi#1/rpi#2\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T08:37:51Z",
          "url": "https://github.com/tsnlab/tickle/commit/5567772612d070110d51a4d4df78d070c9f30ca0"
        },
        "date": 1788770439175,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.143,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 823.226,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "5567772612d070110d51a4d4df78d070c9f30ca0",
          "message": "Track perf-test result history with github-action-benchmark\n\n* run_perf.sh: parse rtt avg/mdev, packet loss, and send/recv Mbps out\n  of the example binaries' own stdout into latency-benchmark.json and\n  throughput-benchmark.json (github-action-benchmark's custom format)\n* performance.yml: feed those into benchmark-action/github-action-benchmark,\n  which stores each run's numbers on gh-pages and fails the job if a\n  metric regresses past 200% of its previous value\n* Verified the JSON extraction locally against a real run on rpi#1/rpi#2\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T08:37:51Z",
          "url": "https://github.com/tsnlab/tickle/commit/5567772612d070110d51a4d4df78d070c9f30ca0"
        },
        "date": 1788770499022,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.08,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 821.003,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "96a034650eef3e16ae7b5459db66a4183e44a63a",
          "message": "Add -l (log level) and -n (endpoint/topic name) to all examples\n\n* Expose tt_LogLevel and tt_log_set_level() through a new public\n  include/tickle/log.h instead of examples reaching into the\n  library-internal src/log.h\n* Every example gets -l debug|info|warning|error|none and -n to\n  override its hardcoded topic/service name at runtime\n* perf_client.c already had a parse_args()/cli_options split to stay\n  under the cognitive-complexity lint threshold; apply the same split\n  to the other 7 examples now that two more flags push them over it\n  too\n* Verified: full project builds and lints clean; -l debug surfaces\n  the library's DEBUG-level packet logging that's normally suppressed\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T17:54:55+09:00",
          "tree_id": "dfdc86f7549c254fc0f30643b981d879a4b1b9e5",
          "url": "https://github.com/tsnlab/tickle/commit/96a034650eef3e16ae7b5459db66a4183e44a63a"
        },
        "date": 1788771378731,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 932.607,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 817.622,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "973c53666bdefb77800023c5ffe1edea412a641e",
          "message": "Cut syscall overhead and latency jitter in the receive/send path\n\nFour changes, each measured on rpi#1/rpi#2 before committing:\n\n* A: Replace the SO_RCVTIMEO + recvfrom() pattern in tt_receive() with\n  poll(). The old code re-armed SO_RCVTIMEO via setsockopt() on almost\n  every call because the requested timeout tracks whatever scheduled\n  event is due next and so changes on nearly every iteration; poll()\n  takes the timeout as a plain argument instead, so no socket mutation\n  is needed at all. Measured on a ping run (-c 30 -i 0.1): setsockopt\n  calls went from 1255 to 2, and rtt avg/mdev dropped from ~2.1/1.0ms\n  to ~1.45/0.47ms.\n* B: Precompute the broadcast sockaddr_in once in tt_bind() instead of\n  re-parsing _tt_CONFIG.broadcast with inet_addr() on every tt_send().\n* C: Give tt_Client a fixed-size cache_buf and point client->cache\n  into it instead of malloc'ing/freeing a buffer on every RPC call\n  (only one call can be outstanding at a time already, so a fixed\n  buffer sized like tx_buffer's worst case covers every real request).\n* D: Request a larger SO_SNDBUF/SO_RCVBUF as best-effort insurance\n  against bursty drops; the kernel clamps it to net.core.[rw]mem_max\n  for an unprivileged process, so this is a no-op on hosts already at\n  that ceiling (both test Pis, in fact - default was already at the\n  208KB max) but helps on hosts with more headroom.\n\nThroughput was already at ~935 Mbps on the Pis' Gigabit test link\n(near line rate for this payload size) both before and after, with\n0 dropped packets throughout - the real payoff here is latency and\nCPU/syscall overhead, which will matter more on the actual 10Base-T1S\ntarget than on this Gigabit stand-in.\n\nRegression-tested the SIGINT/hang fixes from the previous commit\nagainst this rewrite: 5/5 clean exits when the peer dies mid-run,\n3/3 clean SIGINT exits with no responder.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T18:11:55+09:00",
          "tree_id": "d1f927afd6bc00aee98bb4f2a1dae20c56e46f67",
          "url": "https://github.com/tsnlab/tickle/commit/973c53666bdefb77800023c5ffe1edea412a641e"
        },
        "date": 1788772667221,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 923.578,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 803.321,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "824072fb57f3356c542c07d9b0b6a0c4eb1a2069",
          "message": "Flush RPC request/response immediately instead of waiting on the 1ms tick\n\ntt_Client_call(), call_retry(), and the server's response send all\npassed is_flush=false to end_encode(), so a message with nothing else\nqueued sat in tx_buffer until either it filled up or node_flush()'s\nperiodic tt_NODE_TX_INTERVAL (1ms) tick caught it. That's the right\ndefault for pub/sub (no reason to force a flush per publish when\nbatching is free), but wrong for RPC: the caller is synchronously\nblocked on the reply, so both legs of the round trip were eating up\nto ~1ms of pure batching delay for no benefit.\n\nMeasured on rpi#1/rpi#2 (ping -c 30 -i 0.1): rtt avg 1.451ms -> 0.217ms,\nmdev 0.466ms -> 0.018ms. Throughput (perf_client/perf_server, unaffected\nsince publish still batches) stayed at ~930 Mbps, 0 dropped. Re-ran the\nSIGINT/hang regression checks: 3/3 clean exits both ways.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-07T18:21:52+09:00",
          "tree_id": "d23d50ec984849fdff26f689f2f07bc4ef0cf3fc",
          "url": "https://github.com/tsnlab/tickle/commit/824072fb57f3356c542c07d9b0b6a0c4eb1a2069"
        },
        "date": 1788773381566,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 909.698,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 798.053,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "a90c53b1e0f3d362bb6b8ca7d63769b77586dff9",
          "message": "Fix run_pair.sh hanging forever when pong's QEMU gets SIGTTIN-stopped\n\nqemu-system-riscv32 -nographic reads stdin (serial console + monitor\nmultiplexing). pong's instance was backgrounded without redirecting its\nstdin away from the invoking terminal, so if it tried to read stdin while\nbackgrounded, the shell's job control could stop it with SIGTTIN. A stopped\nprocess doesn't respond to the later kill (SIGTERM) - it stays stopped, not\nterminated - so the script's wait on it blocked indefinitely. Redirect both\ninstances' stdin from /dev/null so neither depends on foreground/background\njob-control semantics; applied the same defensive fix to netns_run_pair.sh's\nbackgrounded pong.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-08T16:28:18+09:00",
          "tree_id": "27f0d331d5de4092325b019b1f7d937881ec69d9",
          "url": "https://github.com/tsnlab/tickle/commit/a90c53b1e0f3d362bb6b8ca7d63769b77586dff9"
        },
        "date": 1788852643438,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 902.202,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 789.201,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "83e073ee8f91fa856ccd49b30892669800e2e0a4",
          "message": "Clean up HAL layer for 1.0: consistent contract, drop dead generic platform\n\n- Move tt_get_ns()'s declaration from tickle.h into hal.h, alongside the rest\n  of the per-platform HAL contract (tt_get_node_id/tt_bind/tt_close/tt_send/\n  tt_receive) it belongs with - it was the one HAL function declared outside\n  hal.h for no reason.\n- Standardize zero-arg HAL functions on `(void)` (declarations in hal.h and\n  definitions in hal_linux.c) instead of empty parens, matching what\n  hal_freertos.c and tests/test_mock.h's mock already did.\n- Remove the dead `UNUSED` macro duplicated into both hal_linux.c and\n  hal_freertos.c but never actually used in either.\n- Delete hal_generic.h and the TT_PLATFORM_GENERIC branch: it had no matching\n  src/hal_generic.c, so selecting it never actually built - a known gap now\n  closed by making unsupported platforms fail loudly (a #error naming the two\n  real options) instead of silently offering a HAL that doesn't exist. The\n  top-level Makefile's PLATFORM fallback errors the same way for non-Linux\n  hosts instead of guessing 'generic'.\n- Document why hal_freertos.c's tt_bind() has no SO_SNDBUF/SO_RCVBUF tuning\n  (unlike hal_linux.c): lwIP has no SO_SNDBUF at all, and SO_RCVBUF support\n  is compiled out by default - not an oversight.\n\nVerified: native build + unit tests + top-level lint, FreeRTOS cross-build +\nits own lint, and a full QEMU two-instance round trip (make test-qemu) all\nstill pass.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-08T16:36:30+09:00",
          "tree_id": "7704fc46cf3c93c1cb8072cdda77fc8edd9f2ba3",
          "url": "https://github.com/tsnlab/tickle/commit/83e073ee8f91fa856ccd49b30892669800e2e0a4"
        },
        "date": 1788853530282,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 886.872,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 776.107,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "b50ac2a39af82b047ecaf0f746489e95e79f2f09",
          "message": "Fix run_perf.sh's binary paths after the platform/linux/ move\n\nThe example binaries build under platform/linux/ now, not the repo root -\nrun_paired_test()'s `cd ~/tickle && ./ping` has been failing with \"No such\nfile or directory\" since that reorganization landed. update_and_build()'s\n`make all` is fine as-is (the root Makefile forwards to platform/linux/).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T13:35:28+09:00",
          "tree_id": "5c8461b3813807bf2485197ba8d2b21a74e58dfa",
          "url": "https://github.com/tsnlab/tickle/commit/b50ac2a39af82b047ecaf0f746489e95e79f2f09"
        },
        "date": 1789014966578,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 21.528,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 20.695,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "dd0e9e9051e86cc69d544b45e6392a7b92e91b9c",
          "message": "Don't let perf_client's -i 0 loop block in poll() between sends\n\nperf_client's \"as fast as poll() allows\" loop was publish() then\ntt_Node_poll(&node, -1), which resolves to a 100us tt_RECEIVE_TIMEOUT\nreceive wait. That wait is invisible while broadcasting - the node loops\nits own packets back, so poll() returns immediately - but once the\nPublisher unicasts to its lone discovered Subscriber it gets nothing back,\nturning the 100us into a hard per-iteration floor: ~40x slower on the HIL\nPis (883 -> 21 Mbps), lossless either way, purely a send-rate effect.\n\nIn the -i 0 path it now asks for a minimal non-blocking poll (1ns, since\ntt_Node_poll treats 0 as \"do nothing\"): run due scheduler work plus one\nreceive drain, then straight back to publishing. Rate-limited runs\n(-i > 0) keep -1 - they're idle between sends anyway.\n\nDESIGN.md's discovery/unicast section gets the matching note: a high-rate\nPublisher must not block in poll() between sends, and Publisher-side\nunicast is for cutting broadcast traffic on low-subscriber topics, not a\nthroughput optimization.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T13:46:46+09:00",
          "tree_id": "beb4f7f40cefbfd447a3baeae7da59f6ad7087e8",
          "url": "https://github.com/tsnlab/tickle/commit/dd0e9e9051e86cc69d544b45e6392a7b92e91b9c"
        },
        "date": 1789015644598,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 934.557,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 899.521,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "2c0f03a89a8fa3720cb44dbb36c448222fc2c729",
          "message": "run_perf.sh: add a 100-byte small-message perf run\n\nFull-MTU throughput is already at GbE line rate on the HIL Pis, so it can't\nshow whether internal changes cut per-message CPU cost. Small messages\n(batched several per packet) are per-message-CPU-bound instead - message\nrate there is the signal.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T15:56:06+09:00",
          "tree_id": "caf74c75bea97950bb6c0313865706ce908b4a70",
          "url": "https://github.com/tsnlab/tickle/commit/2c0f03a89a8fa3720cb44dbb36c448222fc2c729"
        },
        "date": 1789023413068,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 935.369,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 896.672,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "committer": {
            "name": "Semih Kim",
            "username": "semihlab",
            "email": "semih.kim@gmail.com"
          },
          "id": "1329b73d353f3ebf1543a4ade4a0a58db468f91e",
          "message": "Merge branch 'main' into perf-experiments",
          "timestamp": "2026-09-10T06:56:13Z",
          "url": "https://github.com/tsnlab/tickle/commit/1329b73d353f3ebf1543a4ade4a0a58db468f91e"
        },
        "date": 1789023488617,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.314,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 887.887,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "7751a62fb66deb3ef0789577162a4e5be4db1636",
          "message": "opt6: find_endpoint() via lazy-rebuilt open-addressed hash index\n\nO(1) lookup keyed by endpoint id instead of an O(endpoint_count) scan.\nendpoint_index[] is rebuilt from endpoints[] on the next lookup after any\nadd/remove. No effect on a few-endpoint node; a scaling change.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:16:43+09:00",
          "tree_id": "f84a813dd58415b3f7bc84c45049748d4ff3171a",
          "url": "https://github.com/tsnlab/tickle/commit/7751a62fb66deb3ef0789577162a4e5be4db1636"
        },
        "date": 1789028260160,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.318,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 899.45,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "74f70176a06a8442eb6819d958a1421da725eb0b",
          "message": "test-linux: run in a veth namespace pair, not shared loopback\n\ntest.sh put both nodes in one network namespace on loopback, bound to the\nsame wildcard address and told apart only by -I. A kernel delivers a\nunicast packet aimed at one such socket to whichever bound last, so that\nsetup could not validate any unicast path - a server's CallResponse, a\nPublisher/Client that discovered a peer, the reactive discovery reply -\nand produced misleading ping_pong/perf loss once those landed.\n\nIt now sets up its own veth-joined pair (tickle-ns1/tickle-ns2, real\ndistinct 192.168.10.1/.2), torn down on exit, and drops -I so node IDs\ncome from tt_get_node_id() auto-detection for real. Needs passwordless\nsudo for `ip`; `make test` stays privilege-free. netns.mk's manual runX\ntargets are unchanged and independent (distinct names, no dependency).\n\nFolds in the throwaway test-netns.sh. Docs (README, CONTRIBUTING, Makefile,\nDESIGN, netns.mk) updated to match.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:35:51+09:00",
          "tree_id": "b00bfca9b46c7d696307bed9ccb5768be8abf239",
          "url": "https://github.com/tsnlab/tickle/commit/74f70176a06a8442eb6819d958a1421da725eb0b"
        },
        "date": 1789029399153,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.315,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 899.409,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "b8f8c7618dfe4e8c8a5b219e2d7aca899ae70322",
          "message": "test.sh: shellcheck-clean (SC1072/1073, SC2164, SC2024)\n\nThe `# shellcheck disable=SCxxx - freeform text` style is a parse error in\ncurrent shellcheck (SC1072/1073) - split the rationale onto its own comment\nline. Also `cd || exit 1` (SC2164), and suppress SC2024 (the log redirects\nare the non-root caller's shell's, on purpose). No behavior change.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:45:37+09:00",
          "tree_id": "13b21fd44632af04ac1478945487877a1c8eab88",
          "url": "https://github.com/tsnlab/tickle/commit/b8f8c7618dfe4e8c8a5b219e2d7aca899ae70322"
        },
        "date": 1789029984648,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.321,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 899.72,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "19423dfe85ac9e7b70211cbdaef27e2ce107713b",
          "message": "Make the remaining shell scripts shellcheck-clean too\n\nplatform/freertos/test.sh: `cd || exit 1` (SC2164), quote $server_role /\n$client_node in the make lines (SC2086). run_perf.sh: disable SC2029 on\nthe ssh_run helper (the argument is meant to expand on the remote host).\nNo behavior change - lets `Check all` (ludeeus/action-shellcheck) pass.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T17:47:32+09:00",
          "tree_id": "e2e16ff7c027c0e95215c47b29712ebc6aac5a8a",
          "url": "https://github.com/tsnlab/tickle/commit/19423dfe85ac9e7b70211cbdaef27e2ce107713b"
        },
        "date": 1789030099270,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.386,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 899.806,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "eccaaa69a2dcfeccd1531009a2050a356c47626b",
          "message": "CI: qemu-system-riscv -> qemu-system-misc on ubuntu-latest\n\nGitHub's ubuntu-latest is Ubuntu 24.04 (noble) now, where the RISC-V\nsystem emulators moved from the qemu-system-riscv package into\nqemu-system-misc. Test all has been failing at the toolchain-install step\nfor this since the runner image bumped; nothing downstream (make test-all,\nthe FreeRTOS QEMU round trip) had run.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T18:17:01+09:00",
          "tree_id": "1ee2d0100196c4721430e1e57e4bbe71aeb59c23",
          "url": "https://github.com/tsnlab/tickle/commit/eccaaa69a2dcfeccd1531009a2050a356c47626b"
        },
        "date": 1789031868199,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.353,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 898.45,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "9f300060a1fd96f0086f0d8895d7771f575511c3",
          "message": "Fix FreeRTOS RX-drain hang: gate tt_try_receive() on a zero-timeout select()\n\nopt1 (21a640a) added drain_rx(), which calls tt_try_receive() repeatedly\nuntil it reports nothing left. On Linux that's a recvfrom(MSG_DONTWAIT)\nper call and works. On lwIP, MSG_DONTWAIT is not honored per-call without\nO_NONBLOCK on the socket, so the recvfrom() after the last datagram blocks\nforever - tt_Node_poll() never returns and the scheduler stops running.\nStandalone repro: the FreeRTOS publisher sent \"data=0\" once and went\nsilent instead of every 500ms.\n\nMirror tt_receive()'s existing lwIP pattern in this file (select() as the\nreadiness primitive, since lwIP has no poll()) but with a {0,0} timeout:\nonly recvfrom() when the socket is readable, otherwise return -1\nimmediately. make test-freertos now passes all four scenarios.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T19:05:39+09:00",
          "tree_id": "8bb3e59937a6e05e57b4db6ed1876a4a584f10bd",
          "url": "https://github.com/tsnlab/tickle/commit/9f300060a1fd96f0086f0d8895d7771f575511c3"
        },
        "date": 1789034810819,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.371,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 894.986,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "b37baa8bcaeeb0056d9b551cb9b8dec11c2f17d2",
          "message": "CI: give check-all.yml a lint context for src/hal_freertos.c\n\ncheck-all.yml's cpp-linter runs clang-tidy on every changed .c file, but\n`bear -- make all` only builds the Linux platform - so src/hal_freertos.c\nhad no compile-DB entry and clang-tidy parsed it with no lwIP/picolibc\nincludes, failing with `'lwip/netif.h' file not found` plus a cascade of\n\"no header providing X\" warnings. This stayed hidden until now only\nbecause no green-CI commit had touched that file since check-all.yml was\nrepaired; the QEMU fix is the first, so it tripped it.\n\n- check-all.yml: synthesise a compile-DB entry for hal_freertos.c with\n  the same cross-compile flags platform/freertos/Makefile's own `lint`\n  target uses (--target, -nostdinc, explicit picolibc/gcc -isystem, the\n  FreeRTOS + lwIP submodule include paths). No FreeRTOS build - clang-tidy\n  only needs the flags and headers.\n- .clang-tidy: ignore lwip/* for misc-include-cleaner. lwIP's public API\n  is a handful of umbrella headers that deliberately re-export from\n  private sub-headers; \"include the exact provider\" doesn't apply.\n- hal_freertos.c: NOLINT the errno-macro uses (picolibc routes them via\n  <sys/errno.h>) and the tt_send_iov iovec const-cast, matching the\n  inline-NOLINT style hal_linux.c already uses for the same patterns.\n\nplatform/freertos/Makefile's lint still covers this file too (it disables\nthese checks wholesale for the cross-compiled HAL); check-all.yml now\njust stops choking on it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T19:26:36+09:00",
          "tree_id": "02464e5dca99c4dcf3653d1d5c2cfdfe9dcc134c",
          "url": "https://github.com/tsnlab/tickle/commit/b37baa8bcaeeb0056d9b551cb9b8dec11c2f17d2"
        },
        "date": 1789036046683,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.315,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.358,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "committer": {
            "email": "semih.kim@gmail.com",
            "name": "Semih Kim",
            "username": "semihlab"
          },
          "distinct": true,
          "id": "70fd890609acc2a965398492496bdb07a0beb6d4",
          "message": "CI: exclude third_party from check-all.yml's shellcheck\n\nsubmodules: true (added in b37baa8 for hal_freertos.c's lint context)\nalso drops third_party/lwip's own shell scripts on disk, which shellcheck\nthen flagged (SC2148/SC2045/...). cpp-linter auto-skips submodules;\nshellcheck needs shellcheck_ignore_paths.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T19:28:26+09:00",
          "tree_id": "c0943223a7b83f79c777f79a78f382d598730118",
          "url": "https://github.com/tsnlab/tickle/commit/70fd890609acc2a965398492496bdb07a0beb6d4"
        },
        "date": 1789036154483,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.308,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 900.005,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "41898282+github-actions[bot]@users.noreply.github.com",
            "name": "github-actions[bot]",
            "username": "github-actions[bot]"
          },
          "committer": {
            "email": "41898282+github-actions[bot]@users.noreply.github.com",
            "name": "github-actions[bot]",
            "username": "github-actions[bot]"
          },
          "distinct": true,
          "id": "d55a094648e28aeebd2dbf4255b756f20d91e443",
          "message": "CI: publish a per-platform status table on the benchmark page\n\ngithub.io only had the perf history charts. Add a single table above them\n(https://tsnlab.github.io/tickle/dev/bench/) showing, per platform, whether\nit compiled, whether each test tier passed, and - for the HIL row - the\nlatest throughput / latency / small-message rate. Refreshed on every push\nto main.\n\n- .github/scripts/dashboard.py: merges a producer's section into\n  dev/bench/status.json and (re-)injects the rendered table into\n  dev/bench/index.html between HTML markers. github-action-benchmark\n  regenerates that file every perf run, so the markers are re-inserted\n  (after </header>) whenever they've gone.\n- .github/scripts/publish_dashboard.sh: shared entry point - clones\n  gh-pages, folds in one section, pushes with a fetch+reapply retry loop\n  (test-all.yml, performance.yml and github-action-benchmark can all push\n  there at once on a main push). Also drops a root index.html redirect so\n  the site root stops 404ing. Runs on ubuntu-latest (python3 + git, no jq).\n- test-all.yml: each compile + test tier is now its own step (so the table\n  can pinpoint which broke); a Gate step still fails the job on any tier\n  failure; on a push to main a Publish step writes the \"buildtest\" section\n  (Linux x86-64 + FreeRTOS/QEMU rows).\n- performance.yml / run_perf.sh: run_perf.sh writes perf-frag.json (RPi\n  build/test outcome + numbers, seeded red so an early abort still shows);\n  a new publish job on ubuntu-latest folds it in as the \"perf\" section.\n\nPRs still run every tier (Gate enforces green) but don't touch the page.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T23:14:29+09:00",
          "tree_id": "96724d27c41532234bec4f8fee9a24dc4e4f5c1b",
          "url": "https://github.com/tsnlab/tickle/commit/d55a094648e28aeebd2dbf4255b756f20d91e443"
        },
        "date": 1789049721305,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.412,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 895.752,
            "unit": "Mbps"
          }
        ]
      }
    ]
  }
}