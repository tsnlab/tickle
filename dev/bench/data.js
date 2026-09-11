window.BENCHMARK_DATA = {
  "lastUpdate": 1789097408597,
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
          "id": "77635b8cbd9565ce31fb32cc856fe1ec862aec6a",
          "message": "dashboard: per-commit history grid instead of a single snapshot\n\nRows are now commits on main (newest first, last 30); columns are each\nplatform's tiers - Linux (build / unit / integration), FreeRTOS\n(build / integration), Raspberry Pi HIL (build / integration / throughput\n/ RTT / small-msg rate).\n\nstatus.json holds a \"history\" array; dashboard.py merge upserts a row by\ncommit SHA and fills only its own section, so test-all.yml and\nperformance.yml (separate jobs, finishing out of order) each contribute\ntheir columns to the same row. Rows sort by date desc; a commit whose\nother section hasn't landed yet shows \"·\" for those cells.\n\nrun_perf.sh / test-all.yml now stamp each fragment with commit + date.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T23:22:47+09:00",
          "tree_id": "21163381175d9da94077047ad263ae453f10962d",
          "url": "https://github.com/tsnlab/tickle/commit/77635b8cbd9565ce31fb32cc856fe1ec862aec6a"
        },
        "date": 1789050213732,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "3d6a3b67656a7a9f0eb5ba4c0873b7e317004660",
          "message": "HIL: rpi#1 moved from 10.1.1.207 to 10.1.1.214\n\nDHCP reassigned the client Pi. Overridable via RPI_CLIENT_HOST as before.\n(The stale .207 host key was also dropped from the runner's known_hosts;\nStrictHostKeyChecking=accept-new re-learns .214 / .213 on first connect.)\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T08:26:55+09:00",
          "tree_id": "60347e59892e78095865aa3aacdee8aff3cd3e5c",
          "url": "https://github.com/tsnlab/tickle/commit/3d6a3b67656a7a9f0eb5ba4c0873b7e317004660"
        },
        "date": 1789082878453,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.2,
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
          "id": "59972304b7f1c7d246f16df5faf65b5a72cb5bb6",
          "message": "typesupport M0: CDR-4 spec, CallRequestHeader padding, tool scaffold\n\nFoundations for tools/typesupport/ (generates TickLE codecs from ROS 2\n.msg/.srv - see tools/typesupport/PLAN.md for the full design):\n\n- DESIGN.md: new \"Interface serialization (TickLE CDR-4)\" section -\n  4-byte alignment (not 8: keeps batched DATA payloads aligned without\n  padding tt_SubmessageHeader), uint16 string/array length prefixes (a\n  single-datagram payload is always < 2^16), capacity resolution for\n  variable arrays, #pragma pack(4) generated structs.\n\n- Wire change (pre-1.0, approved): tt_CallRequestHeader grows 7 -> 8\n  bytes (added `reserved`) so its CDR payload lands 4-aligned like\n  DATA/CALLRESPONSE already do. tt_Node's tx_buffer/rx_buffer are\n  _Alignas(4); _Static_assert guards in tickle.c pin all of this down as\n  a compile-time invariant rather than a comment. make test-all (both\n  platforms) and HIL still pass.\n\n- tools/typesupport/ scaffold: pyproject.toml pinning empy==3.3.4 (to be\n  used from M1 on); tickle_typesupport/_rosidl_parser.py, a verbatim vendor\n  of rosidl_adapter/parser.py (Apache-2.0, ros2/rosidl@jazzy) - the\n  canonical ROS 2 .msg/.srv grammar, stdlib-only, chosen over a pip\n  dependency to avoid version skew and guarantee compatibility\n  structurally; a `--dump-ir` CLI stub; a parse-smoke test suite against\n  seven real ROS 2 interface files (fetched into tests/fixtures_ros2/)\n  covering nested types, fixed/bounded/unbounded arrays, constants and\n  defaults, plus TickLE's own four example interfaces. Verified with a\n  clean `pip install -e .` in a fresh venv.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T10:19:32+09:00",
          "tree_id": "cb7a137296c0ea270bd4589a44301572295cc9c2",
          "url": "https://github.com/tsnlab/tickle/commit/59972304b7f1c7d246f16df5faf65b5a72cb5bb6"
        },
        "date": 1789089628392,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.228,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.019,
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
          "id": "1e6c68b4b3a37cb65be5cb74035e713ce8e68305",
          "message": "typesupport M1: scalars, strings, .srv, constants, defaults - full codec generator\n\nImplements the actual empy templates + IR pipeline PLAN.md's M1 calls for, on top of M0's\nvendored parser and CDR-4 spec:\n\n- model.py: WireField/Constant/WireStruct/TopicIR/ServiceIR IR dataclasses, with the CDR-4\n  size/alignment tables for every scalar type.\n- adapt.py: turns a parsed MessageSpecification/ServiceSpecification into that IR (scalars +\n  string/wstring only for now - arrays and nested messages raise UnsupportedFieldError, deferred\n  to M2/M3 per the milestone table).\n- layout.py: walks a struct's fields once, working out per-field static byte offsets/padding\n  until the first variable-size (string) field, after which padding has to be computed at\n  runtime - this is what lets emit.py tell a compile-time-constant pad from a memset one.\n- emit.py: renders each field's actual encode/decode/encode_size C, matching DESIGN.md's new\n  \"Interface serialization (TickLE CDR-4)\" section - direct pointer casts for naturally-aligned\n  scalars (no memcpy, per CDR-4's whole point), byte-swap-in-a-union for cross-endian floats,\n  uint16-length-prefixed strings aliasing the input buffer on decode (never copied), capacity/\n  buffer-bounds checks throughout. Also generates rosidl_generator_c-style constants (enum for\n  integers, static const for float/string) and an optional *_init() when a message has defaults.\n- render.py / templates/*.em: turns the IR into (header, source) text via empy 3.3.4, expanding\n  struct.h.em/struct.c.em once per WireStruct in Python and splicing the result into the outer\n  topic/service template rather than nesting interpreters.\n- postprocess.py: prepends the license/provenance banner, then always runs the real clang-format\n  over the result (-assume-filename=<Name>.c/.h, not a placeholder - needed so clang-format's\n  main-header-first IncludeCategories rule actually fires) so generated code is never shipped\n  unformatted.\n- cli.py: generate_interface() ties it together; `-O/--outdir`, `--name`, `--style-dir` added\n  alongside M0's --dump-ir.\n\nVerified against UInt64.msg, SetBool.srv and Trigger.srv (a zero-field request, an empty-string\nedge case, and every scalar width):\n- tests/test_roundtrip.py (10 cases, via ctypes against a real compiled .so): basic roundtrip,\n  zero value, empty string, NULL/short-buffer rejection, zero-field message, and an explicit\n  byte-layout check for the padding gap CDR-4 predicts between `bool` and the following string.\n- tests/test_crossendian.py (3 cases): decode() actually byte-swaps when told the input isn't\n  native-endian, built by hand with struct.pack rather than through our own encode().\n- tests/test_lint.py: the project's real clang-tidy, inherited from the repo-root .clang-tidy via\n  a local override copied into a real tools/typesupport/.pytest_lint_tmp/ subtree (clang-tidy's\n  own config search walks up from the analyzed file's path, not cwd, so files outside the repo\n  tree can't inherit it and silently get clang-tidy's unrelated defaults instead). Chasing this\n  down to green surfaced and fixed several real generator bugs: a missing <tickle/config.h> for\n  tt_MAX_STRING_LENGTH, redundant (int32_t) casts and lowercase `u` suffixes on the runtime\n  alignment expression (now shared via _runtime_align_expr()), operator-precedence parens missing\n  around that same expression (silently narrowing an unsigned `&` result before the mask instead\n  of after), a redundant same-type cast on uint64 decode, and <string.h>/<tickle/config.h> being\n  included even when a struct never uses memcpy/memset/tt_MAX_STRING_LENGTH.\n- tests/test_golden.py: pins the exact post-clang-format output so future emit.py/template\n  changes show up as a diff here, not just a passing-by-accident behavioral test.\n\ntests/golden/ also carries its own .clang-tidy (inherits root, disables the same three checks\nM5's examples/.clang-tidy will make permanent) so these checked-in ROS 2-named snapshots don't\nfail the repo-wide `make lint` the way M0's bare files would have; test_golden.py excludes it\nfrom the generated-vs-golden file-set comparison.\n\nDeviation from PLAN.md's M1 line item: examples/.clang-tidy itself is NOT created yet - creating\nit now would prematurely relax lint checking on every existing hand-written example driver, since\nnothing under examples/ is generated until M5's actual cutover. tests/golden/.clang-tidy plus\ntest_lint.py's own ad-hoc override cover M1's own needs in the meantime.\n\nmake test / make sanitize / make lint all still pass unchanged (M1 touches nothing outside\ntools/typesupport/).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T11:01:09+09:00",
          "tree_id": "2d5be8ec4886fd69c86a0d3330fdc09df2a24f0b",
          "url": "https://github.com/tsnlab/tickle/commit/1e6c68b4b3a37cb65be5cb74035e713ce8e68305"
        },
        "date": 1789092171290,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "83495ae164ef9144d5975a0ecf412afe770c2ad7",
          "message": "typesupport M2: fixed + variable arrays, capacity resolution, datagram-size assert\n\nAdds array field support to the generator, per PLAN.md/DESIGN.md's \"Fixed arrays\" / \"Variable\narrays\" / \"Capacity\" rules:\n\n- model.py: WireField grows array_mode (\"fixed\" | \"variable\"), array_size, capacity and\n  capacity_source, plus element_ctype/element_size/element_align helpers. wire_align/wire_size\n  extend naturally - a fixed array behaves like a single wider field (known size/align, so\n  layout.py's existing static-vs-runtime-padding logic needs no changes at all), a variable array\n  behaves like a string (wire_size None, wire_align 2 for its uint16 count prefix).\n- adapt.py: rejects arrays of strings and nested-typed arrays (still out of scope / M3) and array\n  default values (M6); resolves a variable array's capacity via PLAN.md's priority order - a\n  trailing `# @capacity <N>` comment annotation, then a ROS 2 upper bound `T[<=N]`, then\n  auto-derived from what's left of a single datagram once every other field's own worst-case size\n  is accounted for (restricted to a single trailing unbounded array, so \"what's left\" is\n  well-defined - anything else asks for an explicit annotation instead of guessing).\n- emit.py: fixed arrays get a plain memcpy (1-byte elements) or a per-element byte-swap loop\n  (wider ones, unioned for floats) in both encode and decode; variable arrays get the same plus\n  their own uint16 count prefix and a capacity check (`count > capacity` -> -2) on both the way\n  out (an over-large data->*_count) and the way in (an over-large count read off a malformed or\n  hostile peer's wire) - never trusting either to write past the fixed-capacity C array\n  underneath.\n- layout.py: new max_wire_size() - the struct's worst-case wire size from what's actually fixed\n  at generate time (a resolved array capacity counts; a plain M1 string does not, since unlike an\n  array it has no fixed C buffer at all - see the function's own docstring on why DESIGN.md's\n  \"Capacity\" wording, \"a variable array's *or bounded string's* C buffer\", doesn't yet apply to\n  the char*-aliasing strings M1 shipped). Backs a new `_Static_assert(<max size> <=\n  tt_MAX_BUFFER_LENGTH)` struct.h.em now emits for every message, matching PLAN.md's \"always\n  assert the message fits in one datagram\" rule (previously only the sizeof/wire_size assert\n  existed, and only for fully fixed-size messages).\n- examples/Bulk.msg (new): a large variable-length payload, exercising the auto-derived-capacity\n  path end to end - not yet wire-compatible with examples/linux/perf/Bulk.c's hand-written\n  version (which reuses its own `size` field as the array's length instead of a separate wire\n  count, to avoid storing the length twice); reconciling the two is M4/M5's job, per the\n  milestone table, and the hand-written perf driver is untouched here.\n- tests/fixtures_own/Arrays.msg (new, test-only): exercises every other array shape in one\n  message - a fixed byte array, a fixed int32 array, a ROS 2 upper-bounded variable array, and an\n  annotated-capacity variable array of floats.\n- tests/test_capacity.py (new): the capacity/rejection behavior above, for both the bounded and\n  auto-derived cases, both directions (struct-side and wire-side).\n- tests/test_roundtrip.py / test_crossendian.py: roundtrip coverage for Bulk/Arrays, plus a\n  hand-built-wire cross-endian case proving a fixed array's per-element swap and a variable\n  array's count-prefix-and-element swap both work (not just scalars/strings, already covered).\n\nChasing test_lint.py to green after generating Arrays/Bulk surfaced two more real generator bugs\n(neither array-specific in cause): a pointer-offset multiplication done in uint32_t and then\nimplicitly widened to size_t (bugprone-implicit-widening-of-multiplication-result - fixed by\nwidening one operand explicitly before the multiply), and the new datagram-size assert failing\noutright on every string-bearing message before the max_wire_size scoping fix above (a plain\nstring's \"worst case\" naively taken as tt_MAX_STRING_LENGTH is 65535, dwarfing\ntt_MAX_BUFFER_LENGTH on its own regardless of anything else in the message).\n\ntests/golden/ updated (SetBool.h / Trigger.h / UInt64.h gain the new config.h include + assert\nline; Arrays.h/.c and Bulk.h/.c added). make test / make sanitize / make lint / FreeRTOS lint all\nstill pass unchanged (M2 touches nothing outside tools/typesupport/ and the new examples/Bulk.msg\nsource file, which nothing yet builds from).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T11:22:35+09:00",
          "tree_id": "bc8159f8019a79b58a4159e1291667fb8a2e5553",
          "url": "https://github.com/tsnlab/tickle/commit/83495ae164ef9144d5975a0ecf412afe770c2ad7"
        },
        "date": 1789093406098,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.208,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.046,
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
          "id": "631dc2417b1826676685ff76e16bbe1a65d104c6",
          "message": "typesupport M3: nested messages, -I include path, std_msgs/Header builtin\n\nAdds nested-message support, per PLAN.md/DESIGN.md's \"Nested messages\" rule (\"the nested type's\nfields are inlined recursively at the current offset - no header, no extra alignment beyond what\nthe first nested field needs\"):\n\n- resolve.py (new): finds and parses a nested field's own `.msg` - from a caller-supplied `-I`\n  search path (ROS 2's `pkg/msg/Name.msg` layout) first, falling back to builtins.py. A\n  Resolver caches by (pkg_name, msg_name) so a type nested from more than one field - std_msgs/\n  Header from many messages, or even two fields of the same message (geometry_msgs/Twist's own\n  linear/angular, both Vector3) - is parsed/adapted exactly once and shares one WireStruct, which\n  is what makes \"one generated file per nested dependency, no duplicate symbols\" possible.\n- builtins.py (new): built-in `.msg` text for builtin_interfaces/Time and std_msgs/Header, so\n  referencing either \"just works\" without vendoring those two upstream packages behind a caller's\n  own -I path - which still takes priority if given (see resolve.py's own docstring).\n- model.py: WireField gains kind \"nested\" (a resolved WireStruct). wire_align/wire_size delegate\n  entirely to the nested struct's own first field / overall size, needing no new layout rules of\n  their own - the same reason layout.py's plan_fields()/compute() needed zero changes for this\n  milestone.\n- adapt.py: adapt_field/adapt_struct/adapt_message/adapt_service all thread an optional Resolver\n  through; a nested field calls resolver.resolve_struct(pkg, name, adapt_struct) and gets back a\n  \"struct pkg__Name\" field (arrays of nested types and defaults on a nested field are out of\n  scope, same as arrays of strings already were). _resolve_auto_capacities is tightened: instead\n  of treating a preceding string as tt_MAX_STRING_LENGTH-worst-case, it now refuses auto-\n  derivation outright whenever any preceding field (string, or a nested struct that itself isn't\n  fixed-size) isn't fixed-size at all - tt_MAX_STRING_LENGTH (65535) alone already exceeds\n  tt_MAX_BUFFER_LENGTH, so treating it as a real budget would make auto-derivation fail even for\n  messages whose strings are, in practice, only ever a few bytes - this is why\n  tests/fixtures_own/Image.msg needs its own explicit `@capacity` annotation rather than the\n  upstream file's bare unbounded array (see that file's own comment).\n- emit.py: a nested field's encode/decode/encode_size just delegate to the nested type's own\n  generated functions (`<Nested>_encode(&data->field, payload + encoded, len - encoded)` etc.) -\n  its own error codes propagate unchanged, and it needs no header/alignment of its own beyond\n  what the generic per-field alignment logic (already handling strings/arrays identically)\n  already does before dispatching to it.\n- render.py / cli.py: a nested dependency gets its own `<pkg>__<Name>.h/.c` pair (via new\n  templates/nested.{h,c}.em - same struct.h.em/struct.c.em content as any interface's own data\n  struct, minus the tt_Topic/tt_Service wrapper a nested type has no wire existence to warrant),\n  written into the same `-O` output directory as the interface that needed it (deduplicated\n  automatically: multiple interfaces sharing one dependency each just overwrite it with identical\n  bytes). cli.py gains a repeatable `-I/--include-dir` flag.\n\nChasing the newly-added nested test fixtures through test_lint.py (now covering *.h too, not just\n*.c - a gap this surfaced) found two more conditionally-unnecessary includes, following the same\npattern as M1/M2's needs_string_h/needs_config_h: `<tickle/tickle.h>` is only ever needed by a\n*_Topic/_Service wrapper (never by a bare nested struct's own file), and `<tickle/hal.h>` is only\nneeded when a struct's own fields (not a nested field, which delegates rather than calling\n_tt_bswap_* itself) actually call something from it - true of every existing example, but not of\ne.g. Twist (composed entirely of two nested Vector3 fields, nothing scalar/string/array of its\nown). Both are now conditional (needs_hal_h, new) the same way the other two already were.\n\nNew interfaces: tests/fixtures_own/Stamped.msg (std_msgs/Header, resolved purely via builtins.py\n- no -I needed) and .../Image.msg (a real sensor_msgs/Image shape - nested Header + M2's array\nsupport together - see its own comment on why this is a fixtures_own/ copy, not tests/\nfixtures_ros2/Image.msg's unmodified one), plus tests/fixtures_ros2/geometry_msgs/Twist.msg\n(already present for parser-fidelity testing) now also exercised through actual code generation\nvia an explicit -I path, covering the \"same nested type from two fields\" caching case tests/\nfixtures_own's own two builtin-only fixtures don't. Roundtrip / cross-endian coverage added for\nall three (test_roundtrip.py, test_crossendian.py - the latter specifically proving is_native_\nendian reaches a nested field's own decode through the delegation).\n\ntests/golden/ updated (unaffected: UInt64/SetBool/Trigger/Bulk/Arrays - byte-identical; new:\nStamped/Image/Twist plus their three shared nested dependencies). make test / make sanitize /\nmake lint / FreeRTOS lint all still pass unchanged.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T11:43:08+09:00",
          "tree_id": "672614ace3f45244970163a280173550c115d067",
          "url": "https://github.com/tsnlab/tickle/commit/631dc2417b1826676685ff76e16bbe1a65d104c6"
        },
        "date": 1789094640669,
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
          "id": "3a78ffeb6e3ea9d25b7772341108e2dff6a63766",
          "message": "typesupport M4 (part 1): auto encode_inplace/decode_inplace for all-fixed-size structs\n\nAny struct where is_fixed_size holds (UInt64Data, SetBoolRequest, geometry_msgs__Vector3,\nTwistData - the last composed entirely of nested fields, nothing scalar of its own, showing the\noptimization needs no per-field analysis at all) now also gets:\n\n  int32_t <Name>_encode_inplace(struct <Name>* data, const uint8_t** payload_out);\n  struct <Name>* <Name>_decode_inplace(const uint8_t* payload, uint32_t len, bool is_native_endian);\n\nper DESIGN.md's \"Struct layout\" note: #pragma pack(4) already makes the struct's own memory\nbyte-identical to its CDR-4 wire form on every supported ABI, so there's nothing to serialize -\nencode_inplace just hands back the struct's own address, decode_inplace just casts the payload\npointer (after checking is_native_endian and length), matching examples/linux/perf/Bulk.c's own\nhand-written pattern for the same idea (Bulk itself doesn't qualify here - see below).\n\nWired into struct.h.em/struct.c.em (guarded by the same is_fixed_size flag the sizeof/wire_size\n_Static_assert already uses) and, only for a Topic whose data is fixed-size, into the generated\ntt_Topic initializer's .data_encode_inplace/.data_decode_inplace (tt_Service has no such fields\nat all - RPC always copies; only Publish/Subscribe's zero-copy path this mirrors, per opt2/opt3\nearlier this session, is affected).\n\nThis is the unambiguous half of M4's own table entry. The other half - \"fully regenerate Bulk\"\nwith the done-criterion \"Bulk golden == the hand-written version\" - needs a design decision this\ncommit doesn't make: examples/Bulk.msg's generated form (M2, a real uint16 length-prefixed\nvariable array) and examples/linux/perf/Bulk.c's hand-written one (which reuses its own `size`\nfield as the array's length instead of a separate wire count, precisely to make the *whole*\nmessage - not just a fixed prefix of it - inplace-aliasable) are two different wire formats by\ndesign, and reconciling them means either teaching CDR-4 a new \"alias an array's length to\nanother field\" convention, or replacing the hand-tuned zero-copy Bulk with the slower generated\none - the latter risks regressing the exact HIL throughput benchmark this session's own opt2/opt3\nwork improved. Flagging for a decision rather than guessing.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T11:50:50+09:00",
          "tree_id": "15679e4753c49f7d3488942a851fa7f4a7d313dc",
          "url": "https://github.com/tsnlab/tickle/commit/3a78ffeb6e3ea9d25b7772341108e2dff6a63766"
        },
        "date": 1789095104109,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.201,
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
          "id": "15da35360862c9aa84ead93de780ed9be31fbf8c",
          "message": "typesupport: fix CI clang-tidy failure - <stddef.h> for NULL in *_decode_inplace\n\nThe M4 commit's decode_inplace bodies return NULL on rejection, but nothing included the header\nthat actually declares it. My own local clang-tidy (21.1.8) didn't flag this, but CI's\ncpp-linter pins clang-tidy 19.1.1, which does (\"no header providing NULL is directly included\")\n- caught by Check all on push, not caught locally. Fixed properly rather than just for the one\nCI version: <stddef.h> is NULL's real, portable home, included in topic.c.em/service.c.em/\nnested.c.em exactly when a decode_inplace exists to use it (the same is_fixed_size flag already\ngating everything else about this feature).\n\ntests/golden/ updated (every file that gained *_decode_inplace in the previous commit gains the\ninclude here too). make test / make lint pass; test_lint.py already covered this locally under\nits own clang-tidy version - the gap was purely the version-to-version NULL-attribution\ndifference, not a hole in what gets checked.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T12:07:03+09:00",
          "tree_id": "cd3654fefba46f0e6dbded460f68d3c509c2f519",
          "url": "https://github.com/tsnlab/tickle/commit/15da35360862c9aa84ead93de780ed9be31fbf8c"
        },
        "date": 1789096068573,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.2,
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
          "id": "502a865c09cfd90c7c6620a4492b561536eeca7b",
          "message": "typesupport M4 (part 2): prefix-aliasable inplace for a trailing byte array\n\nAnswers M4's open question from the previous commit: examples/Bulk.msg (a fixed `seq` header\nplus one trailing unbounded `payload` array) now gets real *_encode_inplace/*_decode_inplace too,\nwithout inventing a new CDR-4 wire convention or giving up the extra wire-format generality M2\nalready committed to (a real uint16 length prefix, not reusing an unrelated field as the length).\n\nThe trick: emit_struct_fields now declares a variable array's uint16 count member *before* its\ndata buffer (matching wire order, not source order) - for a struct that isn't fully fixed-size\nbut does end in exactly one such array with 1-byte elements, preceded only by fixed-size fields\n(layout.prefix_array_field, new), #pragma pack(4) then makes the struct's own memory byte-\nidentical to the wire bytes up through however many elements are actually in use, even though the\nstruct as a whole has a much larger declared capacity. *_encode_inplace hands back the struct's\nown address with a data-dependent size (fixed prefix + 2 + data->*_count, not sizeof); *_decode_\ninplace reads the count straight out of the payload before aliasing it, bounds-checking exactly\nlike the copying *_decode() does. Same idea as examples/linux/perf/Bulk.c's own hand-written\nversion, generalized to work for any message with this one common shape, and to interoperate with\na real self-describing wire length instead of reusing a same-named struct field as one.\n\nAlso adds a `#define <STRUCT>__<FIELD>_CAPACITY <N>` for every variable array field (struct.h.em)\n- lets application code reference the generator's resolved capacity symbolically instead of\nhardcoding it, matching what the hand-written Bulk.h already exposed as its own\nBULK_MAX_PAYLOAD_SIZE for exactly this reason.\n\nField reordering changes memory layout for every existing variable-array struct (Bulk, Arrays,\nImage) without changing wire behavior at all - encode/decode already address fields by name, not\nstruct position. tests/golden/ updated; test_roundtrip.py's ctypes mirrors reordered to match,\nplus new tests actually driving Bulk's new inplace pair through ctypes (encode_inplace aliases\nstruct memory with a count-dependent size; decode_inplace round-trips through a real *_encode()\ncall and rejects over-capacity/reverse-endian input) - which also caught two dangling-pointer bugs\nin the *_decode_inplace tests added in the previous commit (passing buf.raw[:size], a fresh bytes\nobject with nothing keeping it alive, into a function whose entire contract is aliasing whatever\nbuffer it's given - same class of bug _roundtrip()'s own docstring already warns about for a\ndecoded string).\n\nSets up examples/linux/perf/Bulk.c/h's own cutover to the generated version as a separate, next\ncommit - this one only changes what the generator is capable of.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T12:18:17+09:00",
          "tree_id": "ed3785f87e1285ea1d63891ab2bb08a376bb6ffa",
          "url": "https://github.com/tsnlab/tickle/commit/502a865c09cfd90c7c6620a4492b561536eeca7b"
        },
        "date": 1789096749481,
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
          "id": "ec1bd41761823d5dc3d7d196b6176e2243fd510e",
          "message": "Cut examples/linux/perf/Bulk.c/h over to the generated version\n\nPer the user's decision on M4's open question (replace the hand-written zero-copy Bulk with the\ngenerated one, while keeping the zero-copy property - see the two \"typesupport M4\" commits just\nbefore this one for how the generator itself was extended to make that possible): examples/\nlinux/perf/Bulk.{c,h} are no longer hand-written - `tickle-typesupport examples/Bulk.msg` output,\ncommitted as-is (M5 will wire this into a `make regen` Makefile rule; for now it's generated once\nand checked in like every other codec).\n\nWire-visible difference from the old hand-written version: `payload` now carries its own real\nuint16 length prefix (2 bytes/message) instead of reusing `seq`'s sibling `size` field as the\narray's length - the deliberate generality M2 already committed CDR-4 to. Everything else -\nincluding the zero-copy *_encode_inplace/*_decode_inplace path opt2/opt3 built this benchmark\naround - is preserved: layout.prefix_array_field (previous commit) makes the generated BulkData\njust as inplace-aliasable as the hand-written one was, given the array's own uint16 count member\nis now declared before its buffer (matching wire order) rather than after.\n\nField renames follow from the .msg's own field names (`size` -> `payload_count`, `bytes` ->\n`payload`) - updated in both platforms' drivers:\n  - examples/linux/perf/perf_client.c / perf_server.c\n  - examples/freertos/perf/main_perf_client.c / main_perf_server.c (reuses the same Bulk.{c,h} -\n    see platform/freertos/Makefile's ROLE_SRCS for perf_client/perf_server)\nBULK_MAX_PAYLOAD_SIZE is now BULKDATA__PAYLOAD_CAPACITY, generated for every variable array field\nby struct.h.em - not perf-specific, and available (spelled after the struct/field name) for any\nfuture generated message with one.\n\nAlso (both driver .c files, discovered by clang-tidy while checking this commit's own diff -\npre-existing, unrelated to the cutover itself): removed <stdlib.h>/<string.h>, neither used\ndirectly by perf_client.c/perf_server.c despite being included.\n\nNew examples/linux/perf/.clang-tidy, scoped to this one directory: Bulk.c/h's own generated\nnumeric literals (offsets/sizes/capacities) would otherwise trip the repo-wide readability-\nmagic-numbers check that hand-written code should keep - a placeholder for what PLAN.md's M5\nmilestone makes permanent (examples/.clang-tidy, once code generation's output is flattened into\nits own examples/<proto>/ directories, separate from hand-written drivers like perf_client.c/\nperf_server.c, which keep the real check here in the meantime unaffected. Bulk's own naming\nalready satisfies the repo-root .clang-tidy's existing `(SetBool|UInt64|Ping|Bulk).*` whitelist.\n\nVerified: `make test` / `make sanitize` / `make lint` / FreeRTOS build (both perf_client and\nperf_server ROLEs) + lint all pass; `make test-freertos` (full QEMU round-trip, all four role\npairs) PASS, perf pair sustaining ~136 Mbps - no observable throughput regression from the extra\n2-byte wire overhead. HIL throughput will be confirmed by Performance Test on push.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T12:28:58+09:00",
          "tree_id": "a1b9bad4fa07031bef379bed76e104458f03676b",
          "url": "https://github.com/tsnlab/tickle/commit/ec1bd41761823d5dc3d7d196b6176e2243fd510e"
        },
        "date": 1789097403720,
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
          "id": "77635b8cbd9565ce31fb32cc856fe1ec862aec6a",
          "message": "dashboard: per-commit history grid instead of a single snapshot\n\nRows are now commits on main (newest first, last 30); columns are each\nplatform's tiers - Linux (build / unit / integration), FreeRTOS\n(build / integration), Raspberry Pi HIL (build / integration / throughput\n/ RTT / small-msg rate).\n\nstatus.json holds a \"history\" array; dashboard.py merge upserts a row by\ncommit SHA and fills only its own section, so test-all.yml and\nperformance.yml (separate jobs, finishing out of order) each contribute\ntheir columns to the same row. Rows sort by date desc; a commit whose\nother section hasn't landed yet shows \"·\" for those cells.\n\nrun_perf.sh / test-all.yml now stamp each fragment with commit + date.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-10T23:22:47+09:00",
          "tree_id": "21163381175d9da94077047ad263ae453f10962d",
          "url": "https://github.com/tsnlab/tickle/commit/77635b8cbd9565ce31fb32cc856fe1ec862aec6a"
        },
        "date": 1789050217237,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.416,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.023,
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
          "id": "3d6a3b67656a7a9f0eb5ba4c0873b7e317004660",
          "message": "HIL: rpi#1 moved from 10.1.1.207 to 10.1.1.214\n\nDHCP reassigned the client Pi. Overridable via RPI_CLIENT_HOST as before.\n(The stale .207 host key was also dropped from the runner's known_hosts;\nStrictHostKeyChecking=accept-new re-learns .214 / .213 on first connect.)\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T08:26:55+09:00",
          "tree_id": "60347e59892e78095865aa3aacdee8aff3cd3e5c",
          "url": "https://github.com/tsnlab/tickle/commit/3d6a3b67656a7a9f0eb5ba4c0873b7e317004660"
        },
        "date": 1789082881591,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.424,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 903.14,
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
          "id": "1e6c68b4b3a37cb65be5cb74035e713ce8e68305",
          "message": "typesupport M1: scalars, strings, .srv, constants, defaults - full codec generator\n\nImplements the actual empy templates + IR pipeline PLAN.md's M1 calls for, on top of M0's\nvendored parser and CDR-4 spec:\n\n- model.py: WireField/Constant/WireStruct/TopicIR/ServiceIR IR dataclasses, with the CDR-4\n  size/alignment tables for every scalar type.\n- adapt.py: turns a parsed MessageSpecification/ServiceSpecification into that IR (scalars +\n  string/wstring only for now - arrays and nested messages raise UnsupportedFieldError, deferred\n  to M2/M3 per the milestone table).\n- layout.py: walks a struct's fields once, working out per-field static byte offsets/padding\n  until the first variable-size (string) field, after which padding has to be computed at\n  runtime - this is what lets emit.py tell a compile-time-constant pad from a memset one.\n- emit.py: renders each field's actual encode/decode/encode_size C, matching DESIGN.md's new\n  \"Interface serialization (TickLE CDR-4)\" section - direct pointer casts for naturally-aligned\n  scalars (no memcpy, per CDR-4's whole point), byte-swap-in-a-union for cross-endian floats,\n  uint16-length-prefixed strings aliasing the input buffer on decode (never copied), capacity/\n  buffer-bounds checks throughout. Also generates rosidl_generator_c-style constants (enum for\n  integers, static const for float/string) and an optional *_init() when a message has defaults.\n- render.py / templates/*.em: turns the IR into (header, source) text via empy 3.3.4, expanding\n  struct.h.em/struct.c.em once per WireStruct in Python and splicing the result into the outer\n  topic/service template rather than nesting interpreters.\n- postprocess.py: prepends the license/provenance banner, then always runs the real clang-format\n  over the result (-assume-filename=<Name>.c/.h, not a placeholder - needed so clang-format's\n  main-header-first IncludeCategories rule actually fires) so generated code is never shipped\n  unformatted.\n- cli.py: generate_interface() ties it together; `-O/--outdir`, `--name`, `--style-dir` added\n  alongside M0's --dump-ir.\n\nVerified against UInt64.msg, SetBool.srv and Trigger.srv (a zero-field request, an empty-string\nedge case, and every scalar width):\n- tests/test_roundtrip.py (10 cases, via ctypes against a real compiled .so): basic roundtrip,\n  zero value, empty string, NULL/short-buffer rejection, zero-field message, and an explicit\n  byte-layout check for the padding gap CDR-4 predicts between `bool` and the following string.\n- tests/test_crossendian.py (3 cases): decode() actually byte-swaps when told the input isn't\n  native-endian, built by hand with struct.pack rather than through our own encode().\n- tests/test_lint.py: the project's real clang-tidy, inherited from the repo-root .clang-tidy via\n  a local override copied into a real tools/typesupport/.pytest_lint_tmp/ subtree (clang-tidy's\n  own config search walks up from the analyzed file's path, not cwd, so files outside the repo\n  tree can't inherit it and silently get clang-tidy's unrelated defaults instead). Chasing this\n  down to green surfaced and fixed several real generator bugs: a missing <tickle/config.h> for\n  tt_MAX_STRING_LENGTH, redundant (int32_t) casts and lowercase `u` suffixes on the runtime\n  alignment expression (now shared via _runtime_align_expr()), operator-precedence parens missing\n  around that same expression (silently narrowing an unsigned `&` result before the mask instead\n  of after), a redundant same-type cast on uint64 decode, and <string.h>/<tickle/config.h> being\n  included even when a struct never uses memcpy/memset/tt_MAX_STRING_LENGTH.\n- tests/test_golden.py: pins the exact post-clang-format output so future emit.py/template\n  changes show up as a diff here, not just a passing-by-accident behavioral test.\n\ntests/golden/ also carries its own .clang-tidy (inherits root, disables the same three checks\nM5's examples/.clang-tidy will make permanent) so these checked-in ROS 2-named snapshots don't\nfail the repo-wide `make lint` the way M0's bare files would have; test_golden.py excludes it\nfrom the generated-vs-golden file-set comparison.\n\nDeviation from PLAN.md's M1 line item: examples/.clang-tidy itself is NOT created yet - creating\nit now would prematurely relax lint checking on every existing hand-written example driver, since\nnothing under examples/ is generated until M5's actual cutover. tests/golden/.clang-tidy plus\ntest_lint.py's own ad-hoc override cover M1's own needs in the meantime.\n\nmake test / make sanitize / make lint all still pass unchanged (M1 touches nothing outside\ntools/typesupport/).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T11:01:09+09:00",
          "tree_id": "2d5be8ec4886fd69c86a0d3330fdc09df2a24f0b",
          "url": "https://github.com/tsnlab/tickle/commit/1e6c68b4b3a37cb65be5cb74035e713ce8e68305"
        },
        "date": 1789092174506,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.344,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.228,
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
          "id": "631dc2417b1826676685ff76e16bbe1a65d104c6",
          "message": "typesupport M3: nested messages, -I include path, std_msgs/Header builtin\n\nAdds nested-message support, per PLAN.md/DESIGN.md's \"Nested messages\" rule (\"the nested type's\nfields are inlined recursively at the current offset - no header, no extra alignment beyond what\nthe first nested field needs\"):\n\n- resolve.py (new): finds and parses a nested field's own `.msg` - from a caller-supplied `-I`\n  search path (ROS 2's `pkg/msg/Name.msg` layout) first, falling back to builtins.py. A\n  Resolver caches by (pkg_name, msg_name) so a type nested from more than one field - std_msgs/\n  Header from many messages, or even two fields of the same message (geometry_msgs/Twist's own\n  linear/angular, both Vector3) - is parsed/adapted exactly once and shares one WireStruct, which\n  is what makes \"one generated file per nested dependency, no duplicate symbols\" possible.\n- builtins.py (new): built-in `.msg` text for builtin_interfaces/Time and std_msgs/Header, so\n  referencing either \"just works\" without vendoring those two upstream packages behind a caller's\n  own -I path - which still takes priority if given (see resolve.py's own docstring).\n- model.py: WireField gains kind \"nested\" (a resolved WireStruct). wire_align/wire_size delegate\n  entirely to the nested struct's own first field / overall size, needing no new layout rules of\n  their own - the same reason layout.py's plan_fields()/compute() needed zero changes for this\n  milestone.\n- adapt.py: adapt_field/adapt_struct/adapt_message/adapt_service all thread an optional Resolver\n  through; a nested field calls resolver.resolve_struct(pkg, name, adapt_struct) and gets back a\n  \"struct pkg__Name\" field (arrays of nested types and defaults on a nested field are out of\n  scope, same as arrays of strings already were). _resolve_auto_capacities is tightened: instead\n  of treating a preceding string as tt_MAX_STRING_LENGTH-worst-case, it now refuses auto-\n  derivation outright whenever any preceding field (string, or a nested struct that itself isn't\n  fixed-size) isn't fixed-size at all - tt_MAX_STRING_LENGTH (65535) alone already exceeds\n  tt_MAX_BUFFER_LENGTH, so treating it as a real budget would make auto-derivation fail even for\n  messages whose strings are, in practice, only ever a few bytes - this is why\n  tests/fixtures_own/Image.msg needs its own explicit `@capacity` annotation rather than the\n  upstream file's bare unbounded array (see that file's own comment).\n- emit.py: a nested field's encode/decode/encode_size just delegate to the nested type's own\n  generated functions (`<Nested>_encode(&data->field, payload + encoded, len - encoded)` etc.) -\n  its own error codes propagate unchanged, and it needs no header/alignment of its own beyond\n  what the generic per-field alignment logic (already handling strings/arrays identically)\n  already does before dispatching to it.\n- render.py / cli.py: a nested dependency gets its own `<pkg>__<Name>.h/.c` pair (via new\n  templates/nested.{h,c}.em - same struct.h.em/struct.c.em content as any interface's own data\n  struct, minus the tt_Topic/tt_Service wrapper a nested type has no wire existence to warrant),\n  written into the same `-O` output directory as the interface that needed it (deduplicated\n  automatically: multiple interfaces sharing one dependency each just overwrite it with identical\n  bytes). cli.py gains a repeatable `-I/--include-dir` flag.\n\nChasing the newly-added nested test fixtures through test_lint.py (now covering *.h too, not just\n*.c - a gap this surfaced) found two more conditionally-unnecessary includes, following the same\npattern as M1/M2's needs_string_h/needs_config_h: `<tickle/tickle.h>` is only ever needed by a\n*_Topic/_Service wrapper (never by a bare nested struct's own file), and `<tickle/hal.h>` is only\nneeded when a struct's own fields (not a nested field, which delegates rather than calling\n_tt_bswap_* itself) actually call something from it - true of every existing example, but not of\ne.g. Twist (composed entirely of two nested Vector3 fields, nothing scalar/string/array of its\nown). Both are now conditional (needs_hal_h, new) the same way the other two already were.\n\nNew interfaces: tests/fixtures_own/Stamped.msg (std_msgs/Header, resolved purely via builtins.py\n- no -I needed) and .../Image.msg (a real sensor_msgs/Image shape - nested Header + M2's array\nsupport together - see its own comment on why this is a fixtures_own/ copy, not tests/\nfixtures_ros2/Image.msg's unmodified one), plus tests/fixtures_ros2/geometry_msgs/Twist.msg\n(already present for parser-fidelity testing) now also exercised through actual code generation\nvia an explicit -I path, covering the \"same nested type from two fields\" caching case tests/\nfixtures_own's own two builtin-only fixtures don't. Roundtrip / cross-endian coverage added for\nall three (test_roundtrip.py, test_crossendian.py - the latter specifically proving is_native_\nendian reaches a nested field's own decode through the delegation).\n\ntests/golden/ updated (unaffected: UInt64/SetBool/Trigger/Bulk/Arrays - byte-identical; new:\nStamped/Image/Twist plus their three shared nested dependencies). make test / make sanitize /\nmake lint / FreeRTOS lint all still pass unchanged.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T11:43:08+09:00",
          "tree_id": "672614ace3f45244970163a280173550c115d067",
          "url": "https://github.com/tsnlab/tickle/commit/631dc2417b1826676685ff76e16bbe1a65d104c6"
        },
        "date": 1789094643850,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.309,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 900.352,
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
          "id": "3a78ffeb6e3ea9d25b7772341108e2dff6a63766",
          "message": "typesupport M4 (part 1): auto encode_inplace/decode_inplace for all-fixed-size structs\n\nAny struct where is_fixed_size holds (UInt64Data, SetBoolRequest, geometry_msgs__Vector3,\nTwistData - the last composed entirely of nested fields, nothing scalar of its own, showing the\noptimization needs no per-field analysis at all) now also gets:\n\n  int32_t <Name>_encode_inplace(struct <Name>* data, const uint8_t** payload_out);\n  struct <Name>* <Name>_decode_inplace(const uint8_t* payload, uint32_t len, bool is_native_endian);\n\nper DESIGN.md's \"Struct layout\" note: #pragma pack(4) already makes the struct's own memory\nbyte-identical to its CDR-4 wire form on every supported ABI, so there's nothing to serialize -\nencode_inplace just hands back the struct's own address, decode_inplace just casts the payload\npointer (after checking is_native_endian and length), matching examples/linux/perf/Bulk.c's own\nhand-written pattern for the same idea (Bulk itself doesn't qualify here - see below).\n\nWired into struct.h.em/struct.c.em (guarded by the same is_fixed_size flag the sizeof/wire_size\n_Static_assert already uses) and, only for a Topic whose data is fixed-size, into the generated\ntt_Topic initializer's .data_encode_inplace/.data_decode_inplace (tt_Service has no such fields\nat all - RPC always copies; only Publish/Subscribe's zero-copy path this mirrors, per opt2/opt3\nearlier this session, is affected).\n\nThis is the unambiguous half of M4's own table entry. The other half - \"fully regenerate Bulk\"\nwith the done-criterion \"Bulk golden == the hand-written version\" - needs a design decision this\ncommit doesn't make: examples/Bulk.msg's generated form (M2, a real uint16 length-prefixed\nvariable array) and examples/linux/perf/Bulk.c's hand-written one (which reuses its own `size`\nfield as the array's length instead of a separate wire count, precisely to make the *whole*\nmessage - not just a fixed prefix of it - inplace-aliasable) are two different wire formats by\ndesign, and reconciling them means either teaching CDR-4 a new \"alias an array's length to\nanother field\" convention, or replacing the hand-tuned zero-copy Bulk with the slower generated\none - the latter risks regressing the exact HIL throughput benchmark this session's own opt2/opt3\nwork improved. Flagging for a decision rather than guessing.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T11:50:50+09:00",
          "tree_id": "15679e4753c49f7d3488942a851fa7f4a7d313dc",
          "url": "https://github.com/tsnlab/tickle/commit/3a78ffeb6e3ea9d25b7772341108e2dff6a63766"
        },
        "date": 1789095107362,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.401,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 900.577,
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
          "id": "15da35360862c9aa84ead93de780ed9be31fbf8c",
          "message": "typesupport: fix CI clang-tidy failure - <stddef.h> for NULL in *_decode_inplace\n\nThe M4 commit's decode_inplace bodies return NULL on rejection, but nothing included the header\nthat actually declares it. My own local clang-tidy (21.1.8) didn't flag this, but CI's\ncpp-linter pins clang-tidy 19.1.1, which does (\"no header providing NULL is directly included\")\n- caught by Check all on push, not caught locally. Fixed properly rather than just for the one\nCI version: <stddef.h> is NULL's real, portable home, included in topic.c.em/service.c.em/\nnested.c.em exactly when a decode_inplace exists to use it (the same is_fixed_size flag already\ngating everything else about this feature).\n\ntests/golden/ updated (every file that gained *_decode_inplace in the previous commit gains the\ninclude here too). make test / make lint pass; test_lint.py already covered this locally under\nits own clang-tidy version - the gap was purely the version-to-version NULL-attribution\ndifference, not a hole in what gets checked.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T12:07:03+09:00",
          "tree_id": "cd3654fefba46f0e6dbded460f68d3c509c2f519",
          "url": "https://github.com/tsnlab/tickle/commit/15da35360862c9aa84ead93de780ed9be31fbf8c"
        },
        "date": 1789096071895,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.317,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 895.636,
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
          "id": "502a865c09cfd90c7c6620a4492b561536eeca7b",
          "message": "typesupport M4 (part 2): prefix-aliasable inplace for a trailing byte array\n\nAnswers M4's open question from the previous commit: examples/Bulk.msg (a fixed `seq` header\nplus one trailing unbounded `payload` array) now gets real *_encode_inplace/*_decode_inplace too,\nwithout inventing a new CDR-4 wire convention or giving up the extra wire-format generality M2\nalready committed to (a real uint16 length prefix, not reusing an unrelated field as the length).\n\nThe trick: emit_struct_fields now declares a variable array's uint16 count member *before* its\ndata buffer (matching wire order, not source order) - for a struct that isn't fully fixed-size\nbut does end in exactly one such array with 1-byte elements, preceded only by fixed-size fields\n(layout.prefix_array_field, new), #pragma pack(4) then makes the struct's own memory byte-\nidentical to the wire bytes up through however many elements are actually in use, even though the\nstruct as a whole has a much larger declared capacity. *_encode_inplace hands back the struct's\nown address with a data-dependent size (fixed prefix + 2 + data->*_count, not sizeof); *_decode_\ninplace reads the count straight out of the payload before aliasing it, bounds-checking exactly\nlike the copying *_decode() does. Same idea as examples/linux/perf/Bulk.c's own hand-written\nversion, generalized to work for any message with this one common shape, and to interoperate with\na real self-describing wire length instead of reusing a same-named struct field as one.\n\nAlso adds a `#define <STRUCT>__<FIELD>_CAPACITY <N>` for every variable array field (struct.h.em)\n- lets application code reference the generator's resolved capacity symbolically instead of\nhardcoding it, matching what the hand-written Bulk.h already exposed as its own\nBULK_MAX_PAYLOAD_SIZE for exactly this reason.\n\nField reordering changes memory layout for every existing variable-array struct (Bulk, Arrays,\nImage) without changing wire behavior at all - encode/decode already address fields by name, not\nstruct position. tests/golden/ updated; test_roundtrip.py's ctypes mirrors reordered to match,\nplus new tests actually driving Bulk's new inplace pair through ctypes (encode_inplace aliases\nstruct memory with a count-dependent size; decode_inplace round-trips through a real *_encode()\ncall and rejects over-capacity/reverse-endian input) - which also caught two dangling-pointer bugs\nin the *_decode_inplace tests added in the previous commit (passing buf.raw[:size], a fresh bytes\nobject with nothing keeping it alive, into a function whose entire contract is aliasing whatever\nbuffer it's given - same class of bug _roundtrip()'s own docstring already warns about for a\ndecoded string).\n\nSets up examples/linux/perf/Bulk.c/h's own cutover to the generated version as a separate, next\ncommit - this one only changes what the generator is capable of.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T12:18:17+09:00",
          "tree_id": "ed3785f87e1285ea1d63891ab2bb08a376bb6ffa",
          "url": "https://github.com/tsnlab/tickle/commit/502a865c09cfd90c7c6620a4492b561536eeca7b"
        },
        "date": 1789096752636,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 936.393,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 900.445,
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
          "id": "ec1bd41761823d5dc3d7d196b6176e2243fd510e",
          "message": "Cut examples/linux/perf/Bulk.c/h over to the generated version\n\nPer the user's decision on M4's open question (replace the hand-written zero-copy Bulk with the\ngenerated one, while keeping the zero-copy property - see the two \"typesupport M4\" commits just\nbefore this one for how the generator itself was extended to make that possible): examples/\nlinux/perf/Bulk.{c,h} are no longer hand-written - `tickle-typesupport examples/Bulk.msg` output,\ncommitted as-is (M5 will wire this into a `make regen` Makefile rule; for now it's generated once\nand checked in like every other codec).\n\nWire-visible difference from the old hand-written version: `payload` now carries its own real\nuint16 length prefix (2 bytes/message) instead of reusing `seq`'s sibling `size` field as the\narray's length - the deliberate generality M2 already committed CDR-4 to. Everything else -\nincluding the zero-copy *_encode_inplace/*_decode_inplace path opt2/opt3 built this benchmark\naround - is preserved: layout.prefix_array_field (previous commit) makes the generated BulkData\njust as inplace-aliasable as the hand-written one was, given the array's own uint16 count member\nis now declared before its buffer (matching wire order) rather than after.\n\nField renames follow from the .msg's own field names (`size` -> `payload_count`, `bytes` ->\n`payload`) - updated in both platforms' drivers:\n  - examples/linux/perf/perf_client.c / perf_server.c\n  - examples/freertos/perf/main_perf_client.c / main_perf_server.c (reuses the same Bulk.{c,h} -\n    see platform/freertos/Makefile's ROLE_SRCS for perf_client/perf_server)\nBULK_MAX_PAYLOAD_SIZE is now BULKDATA__PAYLOAD_CAPACITY, generated for every variable array field\nby struct.h.em - not perf-specific, and available (spelled after the struct/field name) for any\nfuture generated message with one.\n\nAlso (both driver .c files, discovered by clang-tidy while checking this commit's own diff -\npre-existing, unrelated to the cutover itself): removed <stdlib.h>/<string.h>, neither used\ndirectly by perf_client.c/perf_server.c despite being included.\n\nNew examples/linux/perf/.clang-tidy, scoped to this one directory: Bulk.c/h's own generated\nnumeric literals (offsets/sizes/capacities) would otherwise trip the repo-wide readability-\nmagic-numbers check that hand-written code should keep - a placeholder for what PLAN.md's M5\nmilestone makes permanent (examples/.clang-tidy, once code generation's output is flattened into\nits own examples/<proto>/ directories, separate from hand-written drivers like perf_client.c/\nperf_server.c, which keep the real check here in the meantime unaffected. Bulk's own naming\nalready satisfies the repo-root .clang-tidy's existing `(SetBool|UInt64|Ping|Bulk).*` whitelist.\n\nVerified: `make test` / `make sanitize` / `make lint` / FreeRTOS build (both perf_client and\nperf_server ROLEs) + lint all pass; `make test-freertos` (full QEMU round-trip, all four role\npairs) PASS, perf pair sustaining ~136 Mbps - no observable throughput regression from the extra\n2-byte wire overhead. HIL throughput will be confirmed by Performance Test on push.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T12:28:58+09:00",
          "tree_id": "a1b9bad4fa07031bef379bed76e104458f03676b",
          "url": "https://github.com/tsnlab/tickle/commit/ec1bd41761823d5dc3d7d196b6176e2243fd510e"
        },
        "date": 1789097407525,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.615,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 892.703,
            "unit": "Mbps"
          }
        ]
      }
    ]
  }
}