window.BENCHMARK_DATA = {
  "lastUpdate": 1789378475121,
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
          "id": "de16f28a15b597acdd04653fa72ccc4afbdf30b0",
          "message": "Fix check-all.yml: synthesize compile-DB entries for every FreeRTOS role driver\n\nThe Bulk cutover commit's Check all failed on files it never should have needed to touch:\nexamples/freertos/perf/main_perf_client.c/main_perf_server.c, which happened to be the first\nexamples/freertos/**/*.c files ever part of a commit's diff since check-all.yml's compile_commands.json\nsynthesis was written. That synthesis only ever covered src/hal_freertos.c - every other\nFreeRTOS-only source (every examples/freertos/**/main_*.c role driver) has no compile-database\nentry and no cross-compile context, so clang-tidy can't even parse it ('FreeRTOS.h' file not\nfound) - a latent gap, not something this cutover caused, just the first commit to trip over it.\n\nGeneralized the synthesis into a small table (file -> its role-specific -I, matching platform/\nfreertos/Makefile's own ROLE_INCLUDES) so it covers hal_freertos.c and all eight role-driver\nmains uniformly, with the same cross-compile flags platform/freertos/Makefile's own `lint` target\nalready uses.\n\nThat still isn't enough on its own: platform/freertos/Makefile's `lint` target additionally\npasses an explicit --checks override on the command line (disabling readability-identifier-\nnaming/misc-include-cleaner/performance-no-int-to-ptr for the same \"layers three separately-\nvendored header trees\" reason platform/freertos/.clang-tidy documents) - a command-line flag,\nnot something a compile-database entry can carry, and not something check-all.yml's cpp-linter-\naction step (tidy-checks: '', i.e. \"just use .clang-tidy files\") ever applied. New examples/\nfreertos/.clang-tidy makes that same exemption available through file-based config inheritance\ntoo, mirroring platform/freertos/.clang-tidy's own reasoning verbatim for the same underlying\ncause.\n\nVerified locally: synthesizing the same compile_commands.json entries this workflow now would\nand running clang-tidy directly against both previously-failing files now reports zero findings\n(confirmed no findings via `--quiet` output containing no \"warning:\"/\"error:\" lines); `make lint`\n/ `make -C platform/freertos lint` both still pass unaffected (this only changes CI's own\nsynthesis and adds a new, correctly-scoped .clang-tidy - platform/freertos/Makefile's own lint\ntarget already works exactly as it did before).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T12:37:42+09:00",
          "tree_id": "c10b9680a1a33537695aefeb24dc60a3fa459b81",
          "url": "https://github.com/tsnlab/tickle/commit/de16f28a15b597acdd04653fa72ccc4afbdf30b0"
        },
        "date": 1789097907816,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.207,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.043,
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
          "id": "ae994a5151044415a05f46f5b7a3accd27dc1ccc",
          "message": "typesupport M5+M6: flatten remaining codecs, delete hand-written ones, array defaults, docs\n\nM5 - the last three hand-written codecs are gone; every real TickLE interface is now generated:\n\n- examples/{uint64,set_bool,ping_pong,perf}/ (new): each holds its own .msg/.srv source plus the\n  generated .c/.h next to it (tools/typesupport/PLAN.md's \"flatten\" layout - Bulk was already\n  here from the earlier cutover commit; UInt64/SetBool/PingPong join it now). Hand-written\n  examples/linux/{uint64,set_bool,ping_pong}/{UInt64,SetBool,PingPong}.{c,h} are deleted -\n  examples/ping_pong/PingPong.srv is new (the other three already had .msg/.srv sources; ping/\n  pong's request/response pair never did, since it predates this tool).\n- Drivers (examples/linux/*/, examples/freertos/*/main_*.c) are otherwise untouched: they\n  #include their protocol's header by bare name same as always, resolved now via a `-I` onto the\n  new directory instead of the file just sitting next to them - platform/linux/Makefile's\n  EXAMPLE_BINS gained a third (codec directory) field per binary and adds all four to CPPFLAGS;\n  platform/freertos/Makefile's ROLE_INCLUDES/ROLE_SRCS point at the new locations.\n- Each examples/<proto>/ gets its own .clang-tidy (disabling readability-identifier-naming/\n  magic-numbers/non-const-parameter - generated code, ROS 2's own naming, a fixed codec-ABI\n  signature) - not a single top-level examples/.clang-tidy as PLAN.md originally said, since\n  that would also reach the hand-written drivers under examples/{linux,freertos}/<proto>/ one\n  level up, which keep every check. The root .clang-tidy's now-unneeded `(SetBool|UInt64|Ping|\n  Bulk).*` naming whitelist is deleted.\n- New `make regen` (repo root Makefile): re-runs tickle-typesupport over all four interfaces in\n  place. check-all.yml now installs the package and runs it, failing the build on any diff (a\n  hand-edited generated file, or a .msg/.srv edited without regenerating, both get caught this\n  way rather than silently drifting).\n- Fixed a real packaging bug this surfaced: pyproject.toml had no package-data entry for\n  templates/*.em, so a normal `pip install` (unlike this repo's own dev-setup `pip install -e .`,\n  which just points at the source tree) would silently ship a package with no templates at all -\n  caught by testing check-all.yml's new step against a real, non-editable install in a fresh\n  venv before pushing, not just the editable one every other test here has used all along.\n\nM6 - array default values, the last generator feature PLAN.md called for:\n\n- adapt.py accepts a `.msg`'s array default (`uint8[4] x [1,2,3,4]`) instead of rejecting it -\n  validated once every field's capacity is fully resolved (a fixed array's default must supply\n  exactly its declared length; a variable array's must fit its capacity, auto-derived or not).\n- emit.py's *_init() sets each default element (and, for a variable array, its own _count) -\n  tests/fixtures_own/ArrayDefaults.msg (new, test-only) exercises both shapes, with a roundtrip\n  test proving a variable array's untouched tail stays at _init()'s own memset(0) rather than\n  something like the last default value leaking into it.\n- README.md (status, usage) and CONTRIBUTING.md (new \"Generated codecs\" section: edit the\n  .msg/.srv, `make regen`, never hand-edit generated output - and the naming-exemption line\n  fixed since the old SetBool/UInt64/Ping/Bulk whitelist it referenced is gone) updated.\n  PLAN.md's milestone table marked done. `.action`/multi-dimensional arrays were already\n  documented as out of scope (PLAN.md's own \"Out of scope\" line, unchanged).\n\nAlso: test_own_examples_still_parse (M0) referenced a stray top-level examples/Image.msg -\nidentical content to (and apparently an unintentional duplicate of) tests/fixtures_ros2/\nsensor_msgs/msg/Image.msg, not a real TickLE interface, added at some earlier point this\nsession before tests/fixtures_ros2/ existed. Deleted; the test now checks the four real\nflattened interfaces at their new locations instead.\n\nVerified: full typesupport pytest suite (50 tests: roundtrip/cross-endian/capacity/lint/golden,\ncovering every existing interface at its new location plus PingPong and ArrayDefaults);\n`make regen` reproduces byte-identical output from a completely fresh, real (non-editable) `pip\ninstall` in an empty venv - not just the dev `-e` one; `make test` / `make sanitize` / `make\nlint` on the main repo; full FreeRTOS build (all 8 roles) + lint; `make test-freertos` (real QEMU\nround trip, all four protocol pairs) PASS.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T14:26:48+09:00",
          "tree_id": "b64307a2c06fbf7418cde43ab4f7dd99bf547ef8",
          "url": "https://github.com/tsnlab/tickle/commit/ae994a5151044415a05f46f5b7a3accd27dc1ccc"
        },
        "date": 1789104463027,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.209,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.036,
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
          "id": "ffe33c41084d8631907e5ad12caaeb8f1a595577",
          "message": "CHANGELOG: record the CallRequestHeader wire change and tools/typesupport\n\nBoth landed as part of the typesupport milestone work (M0-M6) but were never recorded here -\nCONTRIBUTING.md asks for anything user-visible to go under ## [Unreleased].\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T14:28:53+09:00",
          "tree_id": "4d85de1aaf1449c2961f68dc210e6eca3bda3d2d",
          "url": "https://github.com/tsnlab/tickle/commit/ffe33c41084d8631907e5ad12caaeb8f1a595577"
        },
        "date": 1789104578670,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.201,
            "unit": "ms"
          },
          {
            "name": "rtt mdev",
            "value": 0.008,
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
          "id": "2277851ba8ba7171b51c777e480af6327a6ee075",
          "message": "Lint cleanup (repo-wide, `make lint` now zero warnings) + HIL jitter alert split out\n\n1. Pre-existing lint findings, none of them caused by the typesupport work, just never cleaned\n   up:\n   - examples/linux/{uint64/{publisher,subscriber},set_bool/{client,server},ping_pong/{ping,pong}}.c:\n     removed unused <stdlib.h>/<string.h> (neither ever called anything from either - same\n     \"misc-include-cleaner\" finding perf_client.c/perf_server.c already had fixed for the same\n     reason during the Bulk cutover).\n   - examples/linux/common/cli_opts.h: `1u` -> `1U` (readability-uppercase-literal-suffix, 5\n     spots). cli_opts.c: added a direct <stdint.h> for uint32_t (previously only reached\n     transitively through cli_opts.h).\n   - src/hal_linux.c: tt_send_iov()'s `(void*)(uintptr_t)hdr`/`body` casts were a redundant\n     integer round-trip just to discard `const` on an already-a-pointer value (performance-no-\n     int-to-ptr) - a straight `(void*)hdr` does the same thing without it, same fix already\n     applied to the generated Bulk.c during the earlier cutover. Also added NOLINT(misc-include-\n     cleaner) on <sys/uio.h> and its own `struct iovec` use - clang-tidy's IWYU mapping doesn't\n     know this glibc symbol's real (portable, POSIX-specified) home and flags both sides of a\n     correct, portable include; same class of false positive this file already suppresses for\n     CLOCK_REALTIME/SOL_SOCKET a few lines up.\n\n2. performance.yml: rtt mdev (ping's own mean-deviation/jitter stat) is inherently noisy on real\n   hardware - it alone tripped the Performance Test's 200% alert-threshold three separate times\n   in one session, each time on a commit nowhere near the ping/pong latency path (a check-all.yml\n   fix, a doc-only CHANGELOG commit, ...). Split into its own benchmark group (run_perf.sh now\n   writes a separate latency-jitter-benchmark.json) with fail-on-alert: false - still tracked and\n   graphed (summary-always, auto-push, same as every other group), just never fails the build on\n   its own. rtt avg and packet loss - the two latency-benchmark.json still carries - stay at\n   200%/fail-on-alert: true, since neither showed this flakiness and both do reflect a real\n   regression when they move.\n\nVerified: `make lint` / `make -C platform/freertos lint` both zero warnings (previously ~16\npre-existing findings across 9 files); `make test` / `make sanitize` still pass; run_perf.sh's\nnew JSON-writing logic checked directly (valid JSON, correct split) since HIL itself isn't\nreachable from here to run the real script end to end.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T17:52:38+09:00",
          "tree_id": "76846519aefc3f0c44d64f359e346119e925a5ff",
          "url": "https://github.com/tsnlab/tickle/commit/2277851ba8ba7171b51c777e480af6327a6ee075"
        },
        "date": 1789116807871,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "a517f871fcb345ec464ecabb62c465ebd33ebc0b",
          "message": "README.md: fix docs left stale by the typesupport flatten (M5), plus a \"no make install\" claim\n\n- examples/ layout paragraph: still described the pre-M5 shape (codec + driver together under\n  examples/linux/<protocol>/, examples/freertos/ cross-compiling straight out of that same\n  directory). Rewritten for the actual current layout: examples/<protocol>/ holds the .msg/.srv\n  + its generated codec; examples/linux/<protocol>/ and examples/freertos/<protocol>/ hold only\n  their own driver, both building against the same examples/<protocol>/ codec.\n- \"Integrating the library\": said \"There is no make install yet\" - it's existed since the 1.0\n  readiness pass (see CHANGELOG.md's own Added entry), this just never got mentioned here.\n- \"Message size: filling an Ethernet frame\": the numbers were for the hand-written Bulk (seq +\n  size, 8 bytes of its own header, BULK_MAX_PAYLOAD_SIZE = 1440) - stale since the cutover to the\n  generated codec, whose payload array carries a real uint16 wire length prefix instead (seq + a\n  2-byte count = 6 bytes of its own header, BULKDATA__PAYLOAD_CAPACITY = 1442, auto-derived by\n  tools/typesupport rather than hand-calculated). Recomputed and cross-checked against\n  perf_client's own -h output (`-s  payload bytes per message (default/max 1442: ...)`) and\n  BULKDATA__PAYLOAD_CAPACITY's real value before writing either down.\n\ntools/typesupport/PLAN.md: added the four adapt.py rejects that were never listed in \"Out of\nscope\" (arrays of strings, arrays of nested message types, defaults on a nested message field,\nand a bounded string's own capacity) - all four already raise a clear UnsupportedFieldError\nrather than generating something wrong, just weren't written down as deliberately out of scope\nversus simply forgotten.\n\nNo code changes; `make test` / `make lint` still pass (unaffected either way).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T09:05:47+09:00",
          "tree_id": "08db25c002b2b59d9722c1a39c038bac4cf21c25",
          "url": "https://github.com/tsnlab/tickle/commit/a517f871fcb345ec464ecabb62c465ebd33ebc0b"
        },
        "date": 1789344403201,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "a6e5a8c0f33fa0ddac227e02da529e324c0524a4",
          "message": "typesupport: implement bounded string (string<=N / @capacity) capacity\n\nDESIGN.md's \"Capacity\" rule already documented this (\"a variable\narray's *or bounded string's* C buffer\"), but adapt.py silently\ngenerated the exact same alias-only char* for a bounded string as for\na plain one - no fixed buffer, no capacity check. Give a bounded\nstring (a ROS 2 upper bound `string<=N`, or a plain `string` with an\nexplicit `# @capacity <N>` annotation) a real char[N+1] buffer and a\ncapacity check on both encode and decode, mirroring how a variable\narray's capacity already works. A plain, unbounded string is\nunaffected - auto-derivation (the rule's priority-3 case) is\ndeliberately not extended to strings, so no existing generated\ninterface changes shape.\n\nNew tests/fixtures_own/BoundedString.msg (+ golden output) covers both\ncapacity sources plus a default value; test_capacity.py adds the\nover-capacity encode/decode rejection cases.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T10:05:25+09:00",
          "tree_id": "c2775288d7991f98e7e643523b35a29cd56f7af1",
          "url": "https://github.com/tsnlab/tickle/commit/a6e5a8c0f33fa0ddac227e02da529e324c0524a4"
        },
        "date": 1789348051422,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
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
          "id": "d3b430e59c70aa5ad168578097f66d9495afbdc2",
          "message": "CHANGELOG: record discovery-unicast + bounded strings, cut v1.0.0\n\nBoth features were already merged (peer-discovery unicast: 1dcb059,\n0fded4b; bounded string capacity: this session) but never made it\ninto the changelog - CONTRIBUTING.md's own rule is to record anything\nuser-visible there. Fold them into the Added section, then promote\nUnreleased to the first tagged release.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T10:29:06+09:00",
          "tree_id": "c2a6e840c30eb779541de75023d8e345ff4821a9",
          "url": "https://github.com/tsnlab/tickle/commit/d3b430e59c70aa5ad168578097f66d9495afbdc2"
        },
        "date": 1789349396635,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
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
            "email": "halim9512@gmail.com",
            "name": "hseong",
            "username": "harimseong"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "f1b5b77d9f0dc36818a051e2d1c3fc558ba722b8",
          "message": "Integrate rmw_tickle initialization part (#20)\n\n* Integrate RMW partially with TickLE\n\n* global header\n* RMW initialization source\n\n* WIP: fix lint error\n\n* WIP: resolve ROS 2 dependency and generate compile_commands.json for github actions\n\n* rmw_tickle: fix build/config gaps found in review\n\nRebased onto main via the merge commit above; on top of that, fix\nwhat a review of this PR turned up:\n\n- Add package.xml/CMakeLists.txt: colcon had nothing to build -\n  check-all.yml's new \"Generate compile_commands.json for rmw_tickle\"\n  step ran `colcon build --packages-select rmw_tickle` against a\n  directory with no build manifest at all.\n- rmw_init() hardcoded `_tt_CONFIG.broadcast` to a specific /24. Read\n  it from TICKLE_BROADCAST_ADDR instead, defaulting to the HAL's own\n  compiled-in address when unset, so this doesn't silently break every\n  network that isn't 192.168.10.0/24.\n- Add the missing definitions for rmw_get_implementation_identifier(),\n  rmw_get_zero_initialized_init_options(), and the two identifier\n  externs the header declares but rmw_init.c never defined.\n- Add the standard TickLE license header to both new files\n  (CONTRIBUTING.md: every .c/.h needs one) and #include <stdbool.h>\n  explicitly for the bool field rmw_tickle.h uses instead of relying\n  on a transitive include.\n\ncheck-all.yml's own colcon step also moved from wrapping the whole\njob in a ROS Docker image (broke the existing `sudo apt install bear`\nstep with an unanswerable interactive prompt) to a single `docker run`\nscoped to just that step, merging its compile_commands.json into the\nexisting one instead of overwriting it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: use a ROS 2-preinstalled image tag for rmw_tickle\n\nCI's first real run (after the merge/fix commits) showed\n`/opt/ros/*/setup.bash: No such file or directory` -\nrostooling/setup-ros-docker:ubuntu-noble-latest turns out to be that\nproject's bare OS-base tag (APT repos configured, no actual ROS\npackages - see its own README), not a ROS-preinstalled one. Switch to\n...-ros-jazzy-ros-base-latest (ROS 2 Jazzy, the distro that targets\nUbuntu Noble) and install python3-colcon-common-extensions, which\nros-base doesn't pull in on its own.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: exclude colcon's build/ output from shellcheck\n\nThe rmw_tickle compile-db step's colcon build leaves ament-generated\nboilerplate under build/ (local_setup.{bash,sh,zsh},\ncolcon_command_prefix_build.sh, ...) - not ours to fix, and already\ngitignored. Extend shellcheck_ignore_paths the same way third_party\nalready is.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: also exclude colcon's install/ from shellcheck\n\nSame class of issue as the previous build/ fix, just under colcon's\nother generated-output directory (setup.{bash,sh,zsh},\nlocal_setup.{bash,sh,zsh}, package.{bash,sh}, ...).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: mount the rmw_tickle build container at $PWD, not /ws\n\nThe compile database that clang-tidy actually needed (rcutils/\nallocator.h et al. not found for rmw_init.c, reproducible on every\npull_request-triggered run) turned out to be a path mismatch: CMake\nbakes the build directory's absolute path into compile_commands.json,\nand colcon built at /ws inside the container while clang-tidy reads\nthe merged database back out on the *host*, where the runner's own\ncheckout lives at a completely different absolute path. Mounting the\ncontainer at the same absolute path the runner uses ($PWD) makes the\nrecorded paths resolve on both sides.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: install ROS 2 on the runner instead of via Docker\n\nThe path-mount fix wasn't enough - clang-tidy (run on the *host* by\ncpp-linter-action, after the rmw_tickle build step finishes) could\nresolve our own headers fine, but never rcutils/rmw/rosidl_runtime_c's:\nthose only ever existed inside the throwaway `--rm` container's\n/opt/ros, which is gone by the time linting happens on the bare\nrunner. Use ros-tooling/setup-ros@v0.7 (required-ros-distributions:\njazzy) instead - it installs ROS 2 straight onto the runner via apt,\nso colcon and clang-tidy see the exact same filesystem throughout, no\ncontainer/host split at all. It also brings colcon with it, so the\nmanual apt-get install step goes away too.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: source setup.bash before the rmw_tickle colcon build\n\nros-tooling/setup-ros@v0.7 installs ROS 2 onto the runner but doesn't\nput ament_cmake et al. on CMAKE_PREFIX_PATH for every subsequent step\nby itself - colcon build failed with \"Could not find a package\nconfiguration file provided by ament_cmake\". Source setup.bash first,\nsame as any fresh ROS 2 terminal would.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: pip install catkin_pkg for the rmw_tickle colcon build\n\nCMake's ament_package_xml.cmake shells out to `python3` for\ncatkin_pkg's package.xml parser - but actions/setup-python (run\nearlier, for the typesupport regen step) already put its own Python\n3.12 first on PATH, so that's the python3 CMake invokes, and it has\nno catkin_pkg (that's only installed for the system python3 that\nros-tooling/setup-ros's apt packages target). pip install it into\nwhichever python3 is currently active instead of reordering steps.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* rmw_tickle: fix include-cleaner and redundant-declaration findings\n\nNow that the compile database actually resolves (previous 4 commits),\nclang-tidy could finally see real findings instead of just failing to\nparse the file at all:\n\n- rmw_tickle.h re-declared rmw_get_implementation_identifier() and\n  rmw_get_zero_initialized_init_options() - both already declared in\n  rmw/rmw.h, which every .c file needing them already includes\n  directly (rmw_init.c does). Drop the redundant copies rather than\n  keep two declarations of the same function in sync by hand.\n- misc-include-cleaner: both files relied on rmw/rmw.h's own\n  transitive includes for rmw_node_t/rmw_publisher_t/rmw_context_t/\n  rmw_ret_t/rmw_init_options_t/rmw_security_options_t/etc. instead of\n  including each type's own defining header (rmw/types.h, rmw/init.h,\n  rmw/ret_types.h, rmw/init_options.h, rmw/security_options.h,\n  rcutils/allocator.h). rmw_tickle.h no longer needs rmw/rmw.h itself\n  at all once its own declarations are gone - it only ever used types,\n  never called an rmw_* function.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n---------\n\nCo-authored-by: github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>\nCo-authored-by: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T12:55:05+09:00",
          "tree_id": "b548eb0b8aad52fdcf3a44bab8b4b87979d7fb3b",
          "url": "https://github.com/tsnlab/tickle/commit/f1b5b77d9f0dc36818a051e2d1c3fc558ba722b8"
        },
        "date": 1789358150247,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
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
          "id": "65214ca07bf8a6583a91c74d9a9ad9a25658de02",
          "message": "rmw_tickle: write the implementation plan (PLAN.md)\n\nCaptures the design philosophy (TickLE stays single-threaded and\nmalloc-free; rmw_tickle owns locking and allocation; the one core\nextension is a narrow blocking-poll wake primitive, not a workaround),\nthe supported subset (tools/typesupport's own out-of-scope list plus\nTickLE's single-datagram size ceiling), the QoS roadmap (local/rmw-only\nwork first, wire-protocol work last), and an 11-milestone build order\nfrom TickLE core extensions through packaging and a scoped test suite.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T13:30:31+09:00",
          "tree_id": "f700c58e456ac729694986c6ba3f166cf9e766de",
          "url": "https://github.com/tsnlab/tickle/commit/65214ca07bf8a6583a91c74d9a9ad9a25658de02"
        },
        "date": 1789360276659,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
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
          "id": "560c55cb607196bd1475169f3f1a6f2c1051ca07",
          "message": "tt_Node_interrupt(): wake a blocking tt_Node_poll() from another thread\n\nrmw_tickle/PLAN.md's Milestone 0(a). Adds a private loopback UDP\nsocket (wake_sock/wake_addr) that tt_receive() polls alongside the\nreal one on both platforms, plus tt_wake_signal() (HAL) and its\npublic wrapper tt_Node_interrupt() (tickle.h) to write to it -\npoll()/select() wakes immediately, tt_receive() reports -3, and\ntt_Node_poll() returns the new tt_RET_INTERRUPTED. This is the one\nexception to a tt_Node's single-threaded rule (DESIGN.md's\n\"Concurrency\"): it adds no locking and lets no second thread touch\nnode-owned state, it only shortens how long a blocked tt_Node_poll()\ncall waits before yielding control back. rmw_tickle needs this so a\ndedicated thread can drive tt_Node_poll() in a loop while other calls\n(rmw_publish()) don't have to wait out its current timeout to get its\nattention.\n\nVerified with a standalone two-thread program against the real Linux\nHAL (not part of the permanent suite - tests/test_*.c is whitebox/\nmock-only by design, see platform/linux/Makefile's own comment): a\ntt_Node_poll() blocked on a 10s timeout returns tt_RET_INTERRUPTED\nwithin ~200ms of another thread calling tt_Node_interrupt(). Also\nconfirmed the signal is \"at least once, at or after the call\" rather\nthan \"only if currently blocked\" - one sent before anything is\nblocked still cuts short the very next tt_Node_poll() call, which the\ndoc comments and DESIGN.md now say explicitly.\n\ntests/test_node_interrupt.c covers handle_receive_result()'s dispatch\n(an interrupt ends the poll even when a scheduler entry was also about\nto fire, unlike a plain timeout) and tt_Node_interrupt()'s own\nargument validation via the mock HAL. Full platform/freertos/test.sh\n(uint64/set_bool/ping_pong/perf) and make test/sanitize/lint all green.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T13:53:42+09:00",
          "tree_id": "1a5661a4e60eeaa8e089e561f41204109d98764d",
          "url": "https://github.com/tsnlab/tickle/commit/560c55cb607196bd1475169f3f1a6f2c1051ca07"
        },
        "date": 1789361667377,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "1e76fb2ec4fc0ae40bc76cc13e966ed8b895caa9",
          "message": "Liveliness timeout: presume a silent peer gone after N missed UPDATEs\n\nrmw_tickle/PLAN.md's Milestone 0(b). Discovery previously only forgot\na remote node's peer-table entries when a *fresh* UPDATE said it no\nlonger hosts them, or when it sent tt_Node_destroy()'s own explicit\nfarewell UPDATE - a node that just stopped announcing at all (crash,\nnetwork partition, anything that skips the farewell) lingered in\nevery peer table forever, since process_update()'s own dedup early\nreturn (unchanged content) never touched any per-source timestamp.\n\nAdd tt_Node.update_last_seen[] (wall-clock time of the most recent\nannounce from that source, moved on *every* valid announce including\nthe dedup case - unlike update_last_modified[], which only moves on\nreal content change) and a new scheduled check_liveliness(), run every\ntt_NODE_UPDATE_INTERVAL alongside node_update()/node_flush(): a known\nnode with no announce heard for tt_LIVELINESS_MISS_THRESHOLD (config.h,\ndefault 3 - separate knob from the interval itself) consecutive\nintervals gets the same forget_peers_from_source() cleanup and update_\nseen[]/update_last_modified[] reset a farewell UPDATE would have\ntriggered, so a later announce from the same node id is treated as\nfirst contact again.\n\ntests/test_liveliness.c covers expiry past the threshold, no false\nexpiry before it, and the critical regression case: a repeated\n*unchanged* announce must still push update_last_seen[] forward, or a\nperfectly healthy node with static endpoints would eventually get\nfalsely expired despite never missing an announce.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T14:08:36+09:00",
          "tree_id": "3e2610cc027958f20924adaf299bd79a863a9d68",
          "url": "https://github.com/tsnlab/tickle/commit/1e76fb2ec4fc0ae40bc76cc13e966ed8b895caa9"
        },
        "date": 1789362561585,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "240fc4067b47adb7c2b0542cb61672aa451faa2f",
          "message": "Opt-in discovery API for graph introspection\n\nrmw_tickle/PLAN.md's Milestone 0(c) - the last piece of Milestone 0.\nEvery UPDATE decode_update_entities() sees was already discarded once\nit finished matching against local endpoints for peer tracking;\nnothing let a caller ask \"what remote entities exist at all\" for\n`ros2 topic list`-style introspection.\n\ntt_Node_set_discovery(node, discovery, callback, param) attaches a\ncaller-owned struct tt_Discovery (fixed capacity\ntt_MAX_DISCOVERED_ENTITIES, config.h, default 16, user-overridable) -\ndeliberately not embedded in struct tt_Node itself. Investigated\nmicro-ROS's own rmw_microxrcedds_c first: it's a thin XRCE-DDS client\nthat delegates essentially all real discovery/graph state to a\nseparate Agent process running full DDS elsewhere, never carrying that\nweight on the constrained device itself. If TickLE ever gets an\nanalogous split for FreeRTOS, the discovery cache belongs on whatever\nplays the Agent role (a full rmw_tickle node, presumably on Linux),\nnot on tt_Node - so tt_Node only holds a discovery pointer + callback\n+ param (3 pointers, +24 bytes measured), and a node nothing has\nattached to pays that alone regardless of platform.\n\ndecode_update_entities() upserts every remote entity it decodes\n(regardless of kind or whether a local endpoint matches) via the new\nupsert_discovered_entity(), firing the callback on appearance/refresh.\nforget_discovered_entities_from_source() mirrors forget_peers_from_\nsource() at both its existing call sites (a fresh, content-changed\nUPDATE; check_liveliness()'s timeout) to fire departed=true and clear\nthe entry. The callback itself is deliberately minimal (node_id,\nendpoint_id, kind, departed) - name/type are looked up separately via\ntt_Discovery_find() rather than paid for on every callback whether\nwanted or not.\n\ntests/test_discovery.c covers: no-op with nothing attached, record +\ncallback on a fresh announce, departure via both an explicit farewell\nUPDATE and the liveliness timeout, detaching stops future recording\nwithout clearing what's already there, and NULL-safety on the helpers.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T14:28:29+09:00",
          "tree_id": "2760bbe014c795dc024ce010c516160bcc007d82",
          "url": "https://github.com/tsnlab/tickle/commit/240fc4067b47adb7c2b0542cb61672aa451faa2f"
        },
        "date": 1789363754885,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "f0b39624ec2a103716929bf47aad0d538a9b2287",
          "message": "rmw_tickle Milestone 1(a): ros2_adapter.py converter generator\n\nAdds tickle_typesupport.ros2_adapter, generating a thin converter between a\nreal ROS 2 interface package's own rosidl_generator_c struct and TickLE's\nown, already-generated, already-tested struct/codec for that same message -\nfield-by-field copy plus bounds checks, calling the existing\n<Msg>_encode/_decode/_encode_size/_free codec completely unchanged rather\nthan regenerating CDR-4 logic a second time against ROS 2's struct shape.\nChosen specifically to minimize risk: this tool's own dev/test environment\nhas no ROS 2 install at all (no /opt/ros, rosidl_adapter not importable),\nso reusing TickLE's existing, CI-verified codec means only the converter\nitself - a much smaller surface - needs new verification.\n\nVerified fully offline via tests/fixtures_ros2_adapter/ (hand-written\nstand-ins for rosidl_generator_c/rosidl_runtime_c's public API shape) and\ntests/test_ros2_adapter.py, which compiles and round-trips the generated\nconverter against the real TickLE codec: scalars, fixed arrays,\nbounded/unbounded variable arrays (including over-capacity rejection), and\nbounded/unbounded strings (including over-capacity rejection). Zero\ncompiler warnings under -Wall -Wextra and zero clang-tidy findings under\nthe project's own .clang-tidy.\n\nBugs found and fixed during development:\n- TickLE header include was derived from the WireStruct's own c_name\n  (e.g. \"ArraysData.h\") instead of the interface-level generated filename\n  (e.g. \"Arrays.h\") that cli.generate_interface() actually produces.\n- ROS 2 header paths need snake_case filenames even though the struct name\n  keeps PascalCase (UInt64 -> u_int64.h) - added _camel_to_snake().\n- A struct with multiple variable-array element types emitted a duplicate\n  #include for primitives_sequence_functions.h, one per element type.\n- test_ros2_adapter.py's own independent render.render_topic() call hit\n  empy's global Interpreter._wasProxyInstalled state conflicting with\n  pytest's stdout capture across test modules (\"interpreter stdout proxy\n  lost\") when run as part of the full suite; fixed by reusing conftest.py's\n  shared, session-scoped generated_dir fixture instead of generating the\n  TickLE codec a second time.\n- clang-tidy (run with the project's real .clang-tidy config, not defaults)\n  flagged missing direct includes for bool/true/false, uint16_t, and both\n  paired struct headers, previously pulled in only transitively.\n\nrmw_tickle/PLAN.md's Milestone 1(b) (the CMake package + macro invoking\nthis generator as part of a real ROS 2 build) and 1(c) (automatic\nextension-point registration, stretch goal) remain pending - both need a\nreal ROS 2 CI environment to iterate against.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T15:46:08+09:00",
          "tree_id": "5d21c108a1c0e4f4230c6afc991487cb216ff242",
          "url": "https://github.com/tsnlab/tickle/commit/f0b39624ec2a103716929bf47aad0d538a9b2287"
        },
        "date": 1789368420813,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "3b8f4343a7f6cbd060fcab4240bb17589d80db4d",
          "message": "Fix check-all: clang-tidy can't lint fixtures_ros2_adapter/'s new headers\n\nf0b3962's tests/fixtures_ros2_adapter/ headers cross-include each other via\nROS 2's own relative-path convention (#include \"rosidl_runtime_c/string.h\"),\nbut being test fixtures rather than anything make all/bear actually\ncompiles, had no entry in compile_commands.json - cpp-linter's clang-tidy\nthen couldn't resolve those includes at all ('file not found'), failing\ncheck-all outright instead of just flagging a style issue.\n\n- check-all.yml: synthesise a compile_commands.json entry per fixture\n  header (same approach already used there for FreeRTOS-only sources),\n  giving clang-tidy the -I it needs to resolve the relative includes.\n- tests/fixtures_ros2_adapter/.clang-tidy: disable readability-identifier-\n  naming for this directory only, same rationale and precedent as tests/\n  golden/.clang-tidy - these are hand-written stand-ins for real ROS 2\n  headers, so they intentionally keep ROS 2's own PascalCase/dunder naming\n  rather than this project's lower_case convention.\n\nVerified locally: synthesising the same compile_commands.json entries and\nrunning clang-tidy -p against each of the 5 fixture headers now resolves\nevery include and reports zero warnings.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T15:54:43+09:00",
          "tree_id": "bce210e8078e603e04d5374082ef961d5756edc4",
          "url": "https://github.com/tsnlab/tickle/commit/3b8f4343a7f6cbd060fcab4240bb17589d80db4d"
        },
        "date": 1789368935820,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "c5c7acf0df0ebd99f31ef27d8d5d28d7236e0cdc",
          "message": "rmw_tickle Milestone 1(b)+(c): rosidl_typesupport_tickle_c, built together\n\nStarted 1(b) (the CMake package/macro wrapping ros2_adapter.py's converter)\nas planned - an explicit rosidl_typesupport_tickle_c_generate_interfaces()\nmacro call, with (c)'s automatic extension-point registration deferred as a\nstretch goal. Reading ros2/rosidl and ros2/rosidl_typesupport's own real\nCMake source (jazzy branch) turned up that this ordering doesn't work: the\nrosidl_message_type_support_t* handle rcl hands an rmw implementation is\nalways the one rosidl_typesupport_c builds for that specific message,\nlisting only whichever typesupport identifiers were registered as an\nament_index \"rosidl_typesupport_c\" resource (get_used_typesupports(),\nrosidl_typesupport_c/cmake/get_used_typesupports.cmake) *before* that\ninterface package's own rosidl_generate_interfaces() ran. Without that\nregistration, no amount of correct generated code is reachable from a real\nrmw_create_publisher() call - so (c) isn't an optional convenience on top\nof (b), it's a hard prerequisite, and both are built together here instead.\n\nNew rosidl_typesupport_tickle_c package:\n- CMakeLists.txt: ament_index_register_resource(\"rosidl_typesupport_c\")\n  (what get_used_typesupports() actually queries) + a small identifier.c\n  runtime library (rosidl_typesupport_tickle_c__identifier, same pattern as\n  rosidl_typesupport_introspection_c/src/identifier.c) + include/\n  message_type_support.h (this package's own private\n  rosidl_typesupport_tickle_c_message_callbacks_t - rosidl's typesupport\n  contract never inspects a handle's .data shape, so this only needs to\n  agree with rmw_tickle itself, reusing TickLE's own tt_DATA_ENCODE/\n  tt_DATA_DECODE/tt_DATA_ENCODE_SIZE/tt_DATA_FREE typedefs and the same\n  cast-a-per-type-function-to-a-generic-signature idiom examples/*/*.c's\n  own <Name>Topic definitions already use).\n- rosidl_typesupport_tickle_c-extras.cmake.in +\n  cmake/rosidl_typesupport_tickle_c_generate_interfaces.cmake:\n  ament_register_extension(\"rosidl_generate_idl_interfaces\", ...) - the\n  *current* extension point (rosidl_generate_interfaces.cmake's own\n  ament_execute_extensions() call; the identically-named-but-obsolete one\n  was replaced in Dashing) - registers a per-.msg add_custom_command\n  running the new `python3 -m tickle_typesupport.ros2_cli`, compiling\n  TickLE's own src/encoding.c/log.c straight into each interface package's\n  generated typesupport library (no installed ament/colcon TickLE package\n  exists to link against instead - same approach tools/typesupport/tests/\n  test_ros2_adapter.py's own offline round-trip test already uses).\n  rosidl_generate_interfaces_ABS_IDL_FILES turned out to hold rosidl_\n  adapter's *converted .idl* paths, not the original .msg tickle_typesupport\n  can actually parse - reconstructed from the known msg/<Name>.msg layout\n  instead of assumed to be usable directly.\n\ntickle_typesupport.ros2_adapter.render_type_support() (new): the\nrosidl_message_type_support_t wrapper + ROSIDL_TYPESUPPORT_INTERFACE__\nMESSAGE_SYMBOL_NAME-named accessor tying render_adapter()'s converter and\nTickLE's codec together. .typesupport_identifier is set lazily on first\naccess rather than in the static initializer - a plain extern const char*\nisn't a C constant expression, confirmed by hitting the same compiler error\nreal rosidl_typesupport_introspection_c-generated code works around the\nsame way (its own msg__type_support.c.em template, fetched from ros2/rosidl\n@ jazzy, doing exactly this). tickle_typesupport.ros2_cli (new): the CLI\nentry point the CMake extension invokes, tying cli.py's existing TickLE\ncodec generation together with ros2_adapter's two new pieces in one pass.\n\nNew rosidl_typesupport_tickle_c_tests package: a minimal real interface\n(msg/Simple.msg) whose test/test_dispatch.c proves reachability through the\n*exact* standard get_message_typesupport_handle() dispatch chain a real\nrmw_create_publisher() call would use, not just that generated code\ncompiles - the specific thing this whole detour was about. check-all.yml\nbuilds both new packages alongside rmw_tickle and runs this test as its own\nstep, before the lint pass.\n\nVerified everything not requiring a real ROS 2 install offline: ros2_cli.py\ngenerates all 5 files per message; the type-support wrapper's C syntax was\nchecked with clang -fsyntax-only against real rosidl_runtime_c/\nrosidl_typesupport_interface header text fetched from ros2/rosidl @ jazzy\n(not guessed) plus TickLE's own real tickle.h - zero warnings. The CMake\nextension-point mechanism itself (ament_register_extension/get_used_\ntypesupports/the .idl-vs-.msg path issue) could only be derived by reading\nros2/rosidl's and ros2/rosidl_typesupport's actual source, since this\nproject's own dev environment has no ROS 2 install at all - expect this to\nneed real CI iteration (ros-tooling/setup-ros) despite the research.\n\nKnown gaps for follow-on work, not solved here: .srv support (skipped with\na CMake warning), a nested message field's own converter/wrapper files, and\npackaging tickle_typesupport itself as an installable ament_cmake_python\npackage (a real end-user build currently needs `pip install <tickle repo>/\ntools/typesupport` done by hand ahead of time, same as check-all.yml's own\nCI already does).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:36:38+09:00",
          "tree_id": "fde5c52e758ce97f461d9ce774e7474ed8295a64",
          "url": "https://github.com/tsnlab/tickle/commit/c5c7acf0df0ebd99f31ef27d8d5d28d7236e0cdc"
        },
        "date": 1789371450290,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "100ba2323e1bb15efcd5b7d5d4d7144885b05f24",
          "message": "Fix check-all: rosidl_generator_py needs NumPy, not something we use\n\nrosidl_typesupport_tickle_c_tests declares <depend>rosidl_default_generators</depend>\n(matching how any real ROS 2 interface package declares its own generator\ndependency) - that transitively pulls in rosidl_generator_py too, unrelated\nto rosidl_typesupport_tickle_c itself, whose own CMake configure step\n(rosidl_generator_py_generate_interfaces.cmake) needs Python3's NumPy\nheaders and failed outright since this CI environment never installed it:\n\n  CMake Error ... Could NOT find Python3 (missing: Python3_NumPy_INCLUDE_DIRS NumPy)\n\npip install rather than `apt install python3-numpy`: actions/setup-python's\nown Python 3.12 is first on PATH (same reason catkin_pkg right below it is\npip-installed instead of apt-installed), so that's the python3 CMake's\nfind_package(Python3) resolves to - not the system one apt's package would\nland in.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:42:43+09:00",
          "tree_id": "55189154d1dfb2b6bbab7a57190861c20e33d36a",
          "url": "https://github.com/tsnlab/tickle/commit/100ba2323e1bb15efcd5b7d5d4d7144885b05f24"
        },
        "date": 1789371807978,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.205,
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
          "id": "f605a3324113a38f4d853019a5d12afe66537fc8",
          "message": "Fix check-all: return() inside our extension aborted every later one\n\nrosidl_typesupport_tickle_c_tests's colcon build failed with \"CMake Error:\nCannot determine link language\" for OTHER packages' own generated\ntypesupport targets (rosidl_typesupport_c, rosidl_typesupport_fastrtps_c/\n_cpp, rosidl_typesupport_introspection_cpp) - none of which our code\ntouches directly. Root cause: ament_execute_extensions() and\nrosidl_generate_interfaces() are both CMake macros, and include() inside a\nmacro runs in the *caller's* scope rather than a scope of its own (unlike a\nfunction's). Our extension's `if(NOT _generated_sources) return() endif()`\nearly-exit therefore didn't just exit our own file - it unwound the whole\nenclosing rosidl_generate_interfaces() call, silently skipping every\nextension registered after ours in the same run (whichever those happened\nto be for that package) without any error of its own. Their own\nadd_library() calls simply never ran, which is what actually surfaced as\n\"link language\" errors on their targets much later.\n\nFixed by wrapping the rest of the file's logic in `if(_generated_sources)`\ninstead of returning early - same effect, without the scope-unwinding\nhazard. (`continue()` inside the earlier foreach loop is unaffected - that\none is loop-scoped, not file/caller-scoped, by design.)\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:48:13+09:00",
          "tree_id": "8a7180e5bad908f2804683eef0ba9ee977786079",
          "url": "https://github.com/tsnlab/tickle/commit/f605a3324113a38f4d853019a5d12afe66537fc8"
        },
        "date": 1789372138849,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.202,
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
          "id": "812128ddc1a383868f57384ed4a192efa955f9db",
          "message": "Fix check-all: rosidl_typesupport_tickle_c_tests needs CXX too, not just C\n\nThe remaining \"CMake Error: Cannot determine link language\" failures (for\nrosidl_typesupport_c, rosidl_typesupport_fastrtps_c/_cpp, rosidl_typesupport_\nintrospection_cpp - none of them ours) persisted even after removing the\nscope-unwinding return() in our own extension. Root cause this time:\nproject(rosidl_typesupport_tickle_c_tests C) only enabled the C compiler/\nlinker, copying rmw_tickle/rmw_tickle/CMakeLists.txt's own C-only\nproject() - but that package never calls rosidl_generate_interfaces() at\nall, so it never needed CXX. Several of the *other* typesupport generators\nrosidl_generate_interfaces() invokes for our msg/Simple.msg generate .cpp\nsources (rosidl_typesupport_c's own dispatch file is .cpp despite its \"C\"\nname, and fastrtps_cpp/introspection_cpp are C++-only outright) - without\nCXX enabled, CMake can't determine a link language for any of their\ngenerated library targets, unrelated to our own rosidl_typesupport_tickle_c\nextension (C-only, and confirmed NOT in the failing-target list either\ntime). project(... C CXX) fixes it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:54:26+09:00",
          "tree_id": "ab8bd4ba201934430613136b63881050fa8b89ba",
          "url": "https://github.com/tsnlab/tickle/commit/812128ddc1a383868f57384ed4a192efa955f9db"
        },
        "date": 1789372511045,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "d70b26ce9a86f73f49059ffbabb949e81b6195d8",
          "message": "Fix check-all: trim rosidl_typesupport_tickle_c_tests deps, add lark\n\nrosidl_default_generators (what a real interface package normally depends\non) transitively pulls in rosidl_generator_py/_rs and ament_cmake_python's\nown egg-build step for this test package - none of which\nrosidl_typesupport_tickle_c itself needs, and which failed outright\n(ModuleNotFoundError: setuptools/lark) since this CI environment's\nactions/setup-python interpreter has neither. Swapped to depending only on\nwhat's actually needed: rosidl_generator_c (the message struct test/\ntest_dispatch.c includes) and rosidl_typesupport_c (ROSIDL_GET_MSG_TYPE_\nSUPPORT()'s own dispatch entry point).\n\nThat alone wasn't enough, though: rosidl_generator_c unconditionally\ndepends on rosidl_generator_type_description for every interface it\ngenerates regardless of which typesupports are involved, which needs both\nNumPy and lark - lark wasn't part of the earlier NumPy fix. pip install\nlark alongside it, same reasoning as numpy/catkin_pkg already there\n(actions/setup-python's own Python 3.12 is first on PATH, so that's the\npython3 CMake's find_package(Python3) resolves to - a system apt package\nwould land somewhere find_package(Python3) never looks).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:03:43+09:00",
          "tree_id": "16e4856bbdec713178e01ba8732d74d2a710aba9",
          "url": "https://github.com/tsnlab/tickle/commit/d70b26ce9a86f73f49059ffbabb949e81b6195d8"
        },
        "date": 1789373068450,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.206,
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
          "id": "7219c6d455ad7a31bde10170e81b8abf1f2c17c4",
          "message": "Fix check-all: rosidl_generate_interfaces() needs rosidl_cmake found too\n\nd70b26c dropped rosidl_default_generators in favor of a minimal dependency\nset (rosidl_generator_c + rosidl_typesupport_c), but rosidl_\ngenerate_interfaces() itself is provided by rosidl_cmake, which\nrosidl_default_generators had only been pulling in transitively - without\nit: \"Unknown CMake command rosidl_generate_interfaces\". find_package(\nrosidl_cmake REQUIRED) explicitly instead, and declare it as a\nbuildtool_depend in package.xml.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:07:59+09:00",
          "tree_id": "81a6a0e2411a9a6b24d93c1dc20a03bbc0e7a65b",
          "url": "https://github.com/tsnlab/tickle/commit/7219c6d455ad7a31bde10170e81b8abf1f2c17c4"
        },
        "date": 1789373323888,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "40a63fb1dabec592b2d4337f273f6bc696f0b012",
          "message": "Fix check-all: doubled include path, TickLE headers missing post-install\n\ntest_dispatch.c's build failed with \"tickle/tickle.h: No such file or\ndirectory\" while reading /home/runner/.../install/rosidl_typesupport_\ntickle_c/include/rosidl_typesupport_tickle_c/rosidl_typesupport_tickle_c/\nmessage_type_support.h - a doubled rosidl_typesupport_tickle_c/ segment.\n\nTwo separate bugs, both mine:\n- install(DIRECTORY include/ DESTINATION include/rosidl_typesupport_tickle_c)\n  duplicated the subdirectory: include/ already contains its own\n  rosidl_typesupport_tickle_c/ subfolder, so this landed headers at\n  .../include/rosidl_typesupport_tickle_c/rosidl_typesupport_tickle_c/*.h.\n  Fixed to DESTINATION include, matching rmw_tickle/rmw_tickle/CMakeLists.\n  txt's own working convention (-I root is plain \"include\", callers write\n  #include \"rosidl_typesupport_tickle_c/identifier.h\" with the\n  subdirectory) - and the matching target_include_directories()/\n  ament_export_include_directories()/install(TARGETS ... INCLUDES\n  DESTINATION ...) entries, all previously \"include/rosidl_typesupport_\n  tickle_c\" (the -I root itself, wrong) instead of plain \"include\".\n- Once that was fixed, TICKLE_ROOT/include was still missing for any\n  consumer reached through find_package() (like rosidl_typesupport_tickle_c_\n  tests/test/test_dispatch.c) rather than this package's own build: it was\n  only added under $<BUILD_INTERFACE:...>, and an installed/imported target\n  only ever sees its INSTALL_INTERFACE. TickLE has no installed package of\n  its own to find_package() instead (same reasoning as TICKLE_ROOT's own\n  baked-in-absolute-path design in the extras.cmake.in), so this now sits\n  outside any interface generator expression, unconditional either way.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:13:51+09:00",
          "tree_id": "d9d634a528a4a915787e487678c22b64794a66c2",
          "url": "https://github.com/tsnlab/tickle/commit/40a63fb1dabec592b2d4337f273f6bc696f0b012"
        },
        "date": 1789373675196,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "b9fb6606301d762f49ab724e8d0ed24992942aa0",
          "message": "Fix check-all: split colcon build so AMENT_PREFIX_PATH updates in between\n\nThe remaining failure was a real one, not a build/link problem: test_\ndispatch's own assert(ours != NULL) fired - get_message_typesupport_handle()\ncouldn't find rosidl_typesupport_tickle_c in rosidl_typesupport_c's own\ndispatch table for msg/Simple, meaning rmw_tickle/PLAN.md's Milestone 1(c)\nregistration wasn't actually visible at generate time for this specific\nmessage despite rosidl_typesupport_tickle_c having already built and\ninstalled successfully earlier in the very same colcon build.\n\nRoot cause, confirmed by reading ament_cmake's own CMake source (ament/\nament_cmake @ rolling): get_used_typesupports() (rosidl_typesupport_c/cmake/\nget_used_typesupports.cmake) calls ament_index_get_resources(), which reads\ncandidate prefixes from the AMENT_PREFIX_PATH *environment variable*\n(ament_index_get_prefix_path.cmake) - a completely different mechanism from\nCMAKE_PREFIX_PATH, which is what find_package() itself resolves against and\nwhich colcon *does* extend across packages within a single build run. AMENT_\nPREFIX_PATH only grows when install/setup.bash is sourced - which is why\nfind_package(rosidl_typesupport_tickle_c REQUIRED) succeeded (proving\nnothing about ament index visibility) while the resource registration\nitself stayed invisible to a package built in the same colcon invocation.\n\nSplit into two colcon build calls with `source install/setup.bash` between\nthem: rmw_tickle + rosidl_typesupport_tickle_c first, then rosidl_\ntypesupport_tickle_c_tests (the one that actually calls rosidl_generate_\ninterfaces() and needs rosidl_typesupport_tickle_c's registration to be\nlive) second.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:21:29+09:00",
          "tree_id": "749310f7c027a2d26db4d4e68293d5285cd7732f",
          "url": "https://github.com/tsnlab/tickle/commit/b9fb6606301d762f49ab724e8d0ed24992942aa0"
        },
        "date": 1789374134429,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "e82e4a4b3d157ffb6dcec9fec7ad23ea2b68aa8b",
          "message": "check-all: temporary diagnostics for the ament-index registration gap\n\nSplitting the colcon build call and sourcing install/setup.bash between\nthem (b9fb660) didn't fix test_dispatch's assert(ours != NULL) failure -\nsame failure, same line, even though that should have updated AMENT_PREFIX_\nPATH before rosidl_typesupport_tickle_c_tests's own build. Rather than\nguess again, print what's actually on disk and in the environment at that\npoint (marker file presence, AMENT_PREFIX_PATH value) so the next CI run's\nlog settles it directly. Will remove once the real cause is confirmed.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:26:34+09:00",
          "tree_id": "8f50c867b2abc9fa36ad99bfefbf09ef7be5fe13",
          "url": "https://github.com/tsnlab/tickle/commit/e82e4a4b3d157ffb6dcec9fec7ad23ea2b68aa8b"
        },
        "date": 1789374439578,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "4c358cacdec560103bc7dbddb973835cd84e20b3",
          "message": "Fix check-all: build our own typesupport libraries shared, not static\n\nThe diagnostic step (e82e4a4) confirmed the ament-index registration itself\nwas already correct: the marker file existed at install/rosidl_typesupport_\ntickle_c/share/ament_index/resource_index/rosidl_typesupport_c/rosidl_\ntypesupport_tickle_c, and AMENT_PREFIX_PATH included that install prefix.\nSo the split colcon build call wasn't the actual fix for the earlier\nfailure - but the same diagnostic output also showed install/rosidl_\ntypesupport_tickle_c/lib/librosidl_typesupport_tickle_c.a: a *static*\narchive, CMake's own default absent an explicit BUILD_SHARED_LIBS=ON.\n\nrosidl_typesupport_c's own runtime dispatch (rosidl_typesupport_c__get_\nmessage_typesupport_handle_function) loads a message's specific typesupport\nimplementation via dlopen() by convention name at *runtime*, once multiple\ntypesupports are registered (true here regardless of what we ourselves\nfind_package() - /opt/ros/jazzy's own apt-installed packages, e.g.\nintrospection_c/fastrtps_c, are still globally ament-index-discoverable) -\nimpossible for a plain .a. /opt/ros/jazzy's own packages are already shared\n(standard for apt-packaged ROS 2); only our own two packages, built fresh\nin this job, needed -DBUILD_SHARED_LIBS=ON told explicitly. Removed the\ntemporary diagnostic lines now that they've served their purpose.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:32:23+09:00",
          "tree_id": "d301e642814a50cdad73bd368b1d9c6940c981ba",
          "url": "https://github.com/tsnlab/tickle/commit/4c358cacdec560103bc7dbddb973835cd84e20b3"
        },
        "date": 1789374788361,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "8f87b196cfc4a1d44978e4350bb91b73550c431c",
          "message": "rmw_tickle Milestone 1(b)+(c): mark done, verified green in real CI\n\nCHANGELOG.md: record rosidl_typesupport_tickle_c as a real, working\ncapability now that check-all.yml's real ROS 2 CI (jazzy) has proven the\nwhole dispatch chain reachable end to end, not just offline-plausible.\n\nPLAN.md: Milestone 1 marked done. Recorded the handful of real CI round\ntrips it took beyond what reading rosidl's/CMake's own source predicted -\neach its own small ROS 2/CMake/colcon gotcha (rosidl_generator_type_\ndescription needing NumPy/lark regardless of typesupport choice; return()\ninside an ament_execute_extensions()-included file unwinding the caller's\nwhole macro scope; rosidl_generate_interfaces_ABS_IDL_FILES holding\nconverted .idl paths, not the original .msg; needing project(... C CXX);\ndoubled install include destinations; and rosidl_typesupport_c's own\ndlopen()-based runtime dispatch needing BUILD_SHARED_LIBS=ON, the least\nguessable one) - useful context for whoever tackles Milestone 2 next and\nhits similarly undocumented territory.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:41:45+09:00",
          "tree_id": "13c613278fa847983764b795e7f8ffd1fee75571",
          "url": "https://github.com/tsnlab/tickle/commit/8f87b196cfc4a1d44978e4350bb91b73550c431c"
        },
        "date": 1789375349623,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.204,
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
          "id": "c7a90b59eadb735e462aef67032b257d669329a9",
          "message": "rmw_tickle Milestone 2: rmw_create_node()/rmw_destroy_node()\n\nNew src/rmw_node.c: rmw_create_node()/rmw_destroy_node()/rmw_node_get_\ngraph_guard_condition(), implementing rmw_tickle/PLAN.md's threading model\n(TickLE row: \"single-threaded per tt_Node\"; rmw_tickle row: \"owns all lock/\nthread management - a background thread per node drives tt_Node_poll(); a\nper-node mutex serializes every other entry point against it\").\n\nrmw_tickle_node_t (rmw_tickle.h) gains poll_thread/mutex/poll_thread_\nrunning. poll_thread loops tt_Node_poll(&tickle_node, tt_RECEIVE_TIMEOUT),\nholding `mutex` only around each individual call - not across iterations,\nand not while blocked in the syscall underneath a single call for longer\nthan that short (100us, config.h) default timeout. Every other entry point\nthat will touch tickle_node (rmw_publish() et al., Milestone 3+) is meant to\ntt_Node_interrupt(&tickle_node) *then* lock `mutex` before doing so -\ntt_Node_interrupt() is the one tt_Node_* call Milestone 0(a) built\nspecifically to be safe from a different thread than whichever one is\nblocked in tt_Node_poll(). rmw_destroy_node() uses that exact pattern to\nstop poll_thread before tt_Node_destroy(): clear poll_thread_running,\ninterrupt, join, destroy.\n\n_tt_CONFIG (config.h) is a process-wide global, not per-node, so only one\ntt_Node can exist per process for now - a process-wide atomic flag rejects\na second rmw_create_node() call outright (RMW_RET-equivalent NULL + error\nmessage) rather than silently colliding with the first. \"Multiple ROS 2\nnodes per process\" stays the explicitly deferred PLAN.md item it already\nwas; this just makes the current limit fail loudly instead of silently.\n\nrmw_tickle/rmw_tickle/CMakeLists.txt: compiles TickLE's own src/tickle.c/\nencoding.c/log.c/hal_linux.c straight into librmw_tickle and links\nThreads::Threads - the first milestone that actually calls into TickLE's\nreal node lifecycle (tt_Node_create/_poll/_interrupt/_destroy); rmw_init.c\nonly ever touched the _tt_CONFIG global struct before this, needing none of\nTickLE's compiled code. No installed ament/colcon TickLE package exists to\nlink against instead - same reasoning already established for rosidl_\ntypesupport_tickle_c's own CMakeLists.txt.\n\nAlso filled in rmw_get_serialization_format() (rmw_init.c) - missing from\nthe #20-era scaffold despite RMW_TICKLE_SERIALIZATION_FORMAT/rmw_tickle_\nserialization_format already existing right next to it.\n\nNot yet verified against a real ROS 2 build - no local ROS 2 install to\ncheck rmw/rcutils header usage against directly, unlike Milestone 1's\nCMake-internals research (this one's API surface is small and copies\nrmw_init.c's own already-CI-verified macro/field usage directly, but the\nactual compile is still pending the next CI run).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:20:22+09:00",
          "tree_id": "ece24e51f6bc0a3f5c646ccafa3adc3e056b8115",
          "url": "https://github.com/tsnlab/tickle/commit/c7a90b59eadb735e462aef67032b257d669329a9"
        },
        "date": 1789377668645,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.205,
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
          "id": "dcf07a2854887ed57e951ace93e034a570c2d505",
          "message": "Fix check-all: mixed plain/keyword target_link_libraries on rmw_tickle\n\nament_target_dependencies() (called just above for rcutils/rmw/\nrosidl_runtime_c) internally uses the plain (non-keyword) target_link_\nlibraries() signature for this same target. My own target_link_libraries(\nrmw_tickle PUBLIC Threads::Threads) used the keyword form, which CMake\nrefuses to mix with plain-signature calls on the same target (\"All uses of\ntarget_link_libraries with a target must be either all-keyword or\nall-plain\"). Switched to the plain form to match.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:24:49+09:00",
          "tree_id": "180c3cf17511f3024ce0528a77b362b76b28f08c",
          "url": "https://github.com/tsnlab/tickle/commit/dcf07a2854887ed57e951ace93e034a570c2d505"
        },
        "date": 1789377933971,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.205,
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
          "id": "606898eae9477ed91dcc36ea890787a27a18ad41",
          "message": "rmw_tickle Milestone 2: mark done, verified green in real CI\n\nCHANGELOG.md: record rmw_create_node()/rmw_destroy_node()/rmw_node_get_\ngraph_guard_condition() as real, working entry points now that check-all.yml\nhas proven them green.\n\nPLAN.md: Milestone 2 marked done, noting the one CI-only issue found beyond\nlocal review (mixing CMake's plain and keyword target_link_libraries()\nsignatures on the same target, from ament_target_dependencies() and a\ndirect Threads::Threads link both touching rmw_tickle).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:33:45+09:00",
          "tree_id": "de5fdf7bc044791ac16c35e4070319f6c5e39573",
          "url": "https://github.com/tsnlab/tickle/commit/606898eae9477ed91dcc36ea890787a27a18ad41"
        },
        "date": 1789378470245,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt avg",
            "value": 0.203,
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
          "id": "ae994a5151044415a05f46f5b7a3accd27dc1ccc",
          "message": "typesupport M5+M6: flatten remaining codecs, delete hand-written ones, array defaults, docs\n\nM5 - the last three hand-written codecs are gone; every real TickLE interface is now generated:\n\n- examples/{uint64,set_bool,ping_pong,perf}/ (new): each holds its own .msg/.srv source plus the\n  generated .c/.h next to it (tools/typesupport/PLAN.md's \"flatten\" layout - Bulk was already\n  here from the earlier cutover commit; UInt64/SetBool/PingPong join it now). Hand-written\n  examples/linux/{uint64,set_bool,ping_pong}/{UInt64,SetBool,PingPong}.{c,h} are deleted -\n  examples/ping_pong/PingPong.srv is new (the other three already had .msg/.srv sources; ping/\n  pong's request/response pair never did, since it predates this tool).\n- Drivers (examples/linux/*/, examples/freertos/*/main_*.c) are otherwise untouched: they\n  #include their protocol's header by bare name same as always, resolved now via a `-I` onto the\n  new directory instead of the file just sitting next to them - platform/linux/Makefile's\n  EXAMPLE_BINS gained a third (codec directory) field per binary and adds all four to CPPFLAGS;\n  platform/freertos/Makefile's ROLE_INCLUDES/ROLE_SRCS point at the new locations.\n- Each examples/<proto>/ gets its own .clang-tidy (disabling readability-identifier-naming/\n  magic-numbers/non-const-parameter - generated code, ROS 2's own naming, a fixed codec-ABI\n  signature) - not a single top-level examples/.clang-tidy as PLAN.md originally said, since\n  that would also reach the hand-written drivers under examples/{linux,freertos}/<proto>/ one\n  level up, which keep every check. The root .clang-tidy's now-unneeded `(SetBool|UInt64|Ping|\n  Bulk).*` naming whitelist is deleted.\n- New `make regen` (repo root Makefile): re-runs tickle-typesupport over all four interfaces in\n  place. check-all.yml now installs the package and runs it, failing the build on any diff (a\n  hand-edited generated file, or a .msg/.srv edited without regenerating, both get caught this\n  way rather than silently drifting).\n- Fixed a real packaging bug this surfaced: pyproject.toml had no package-data entry for\n  templates/*.em, so a normal `pip install` (unlike this repo's own dev-setup `pip install -e .`,\n  which just points at the source tree) would silently ship a package with no templates at all -\n  caught by testing check-all.yml's new step against a real, non-editable install in a fresh\n  venv before pushing, not just the editable one every other test here has used all along.\n\nM6 - array default values, the last generator feature PLAN.md called for:\n\n- adapt.py accepts a `.msg`'s array default (`uint8[4] x [1,2,3,4]`) instead of rejecting it -\n  validated once every field's capacity is fully resolved (a fixed array's default must supply\n  exactly its declared length; a variable array's must fit its capacity, auto-derived or not).\n- emit.py's *_init() sets each default element (and, for a variable array, its own _count) -\n  tests/fixtures_own/ArrayDefaults.msg (new, test-only) exercises both shapes, with a roundtrip\n  test proving a variable array's untouched tail stays at _init()'s own memset(0) rather than\n  something like the last default value leaking into it.\n- README.md (status, usage) and CONTRIBUTING.md (new \"Generated codecs\" section: edit the\n  .msg/.srv, `make regen`, never hand-edit generated output - and the naming-exemption line\n  fixed since the old SetBool/UInt64/Ping/Bulk whitelist it referenced is gone) updated.\n  PLAN.md's milestone table marked done. `.action`/multi-dimensional arrays were already\n  documented as out of scope (PLAN.md's own \"Out of scope\" line, unchanged).\n\nAlso: test_own_examples_still_parse (M0) referenced a stray top-level examples/Image.msg -\nidentical content to (and apparently an unintentional duplicate of) tests/fixtures_ros2/\nsensor_msgs/msg/Image.msg, not a real TickLE interface, added at some earlier point this\nsession before tests/fixtures_ros2/ existed. Deleted; the test now checks the four real\nflattened interfaces at their new locations instead.\n\nVerified: full typesupport pytest suite (50 tests: roundtrip/cross-endian/capacity/lint/golden,\ncovering every existing interface at its new location plus PingPong and ArrayDefaults);\n`make regen` reproduces byte-identical output from a completely fresh, real (non-editable) `pip\ninstall` in an empty venv - not just the dev `-e` one; `make test` / `make sanitize` / `make\nlint` on the main repo; full FreeRTOS build (all 8 roles) + lint; `make test-freertos` (real QEMU\nround trip, all four protocol pairs) PASS.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T14:26:48+09:00",
          "tree_id": "b64307a2c06fbf7418cde43ab4f7dd99bf547ef8",
          "url": "https://github.com/tsnlab/tickle/commit/ae994a5151044415a05f46f5b7a3accd27dc1ccc"
        },
        "date": 1789104466381,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.575,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 895.162,
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
          "id": "ffe33c41084d8631907e5ad12caaeb8f1a595577",
          "message": "CHANGELOG: record the CallRequestHeader wire change and tools/typesupport\n\nBoth landed as part of the typesupport milestone work (M0-M6) but were never recorded here -\nCONTRIBUTING.md asks for anything user-visible to go under ## [Unreleased].\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T14:28:53+09:00",
          "tree_id": "4d85de1aaf1449c2961f68dc210e6eca3bda3d2d",
          "url": "https://github.com/tsnlab/tickle/commit/ffe33c41084d8631907e5ad12caaeb8f1a595577"
        },
        "date": 1789104581942,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.606,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.389,
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
          "id": "2277851ba8ba7171b51c777e480af6327a6ee075",
          "message": "Lint cleanup (repo-wide, `make lint` now zero warnings) + HIL jitter alert split out\n\n1. Pre-existing lint findings, none of them caused by the typesupport work, just never cleaned\n   up:\n   - examples/linux/{uint64/{publisher,subscriber},set_bool/{client,server},ping_pong/{ping,pong}}.c:\n     removed unused <stdlib.h>/<string.h> (neither ever called anything from either - same\n     \"misc-include-cleaner\" finding perf_client.c/perf_server.c already had fixed for the same\n     reason during the Bulk cutover).\n   - examples/linux/common/cli_opts.h: `1u` -> `1U` (readability-uppercase-literal-suffix, 5\n     spots). cli_opts.c: added a direct <stdint.h> for uint32_t (previously only reached\n     transitively through cli_opts.h).\n   - src/hal_linux.c: tt_send_iov()'s `(void*)(uintptr_t)hdr`/`body` casts were a redundant\n     integer round-trip just to discard `const` on an already-a-pointer value (performance-no-\n     int-to-ptr) - a straight `(void*)hdr` does the same thing without it, same fix already\n     applied to the generated Bulk.c during the earlier cutover. Also added NOLINT(misc-include-\n     cleaner) on <sys/uio.h> and its own `struct iovec` use - clang-tidy's IWYU mapping doesn't\n     know this glibc symbol's real (portable, POSIX-specified) home and flags both sides of a\n     correct, portable include; same class of false positive this file already suppresses for\n     CLOCK_REALTIME/SOL_SOCKET a few lines up.\n\n2. performance.yml: rtt mdev (ping's own mean-deviation/jitter stat) is inherently noisy on real\n   hardware - it alone tripped the Performance Test's 200% alert-threshold three separate times\n   in one session, each time on a commit nowhere near the ping/pong latency path (a check-all.yml\n   fix, a doc-only CHANGELOG commit, ...). Split into its own benchmark group (run_perf.sh now\n   writes a separate latency-jitter-benchmark.json) with fail-on-alert: false - still tracked and\n   graphed (summary-always, auto-push, same as every other group), just never fails the build on\n   its own. rtt avg and packet loss - the two latency-benchmark.json still carries - stay at\n   200%/fail-on-alert: true, since neither showed this flakiness and both do reflect a real\n   regression when they move.\n\nVerified: `make lint` / `make -C platform/freertos lint` both zero warnings (previously ~16\npre-existing findings across 9 files); `make test` / `make sanitize` still pass; run_perf.sh's\nnew JSON-writing logic checked directly (valid JSON, correct split) since HIL itself isn't\nreachable from here to run the real script end to end.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T17:52:38+09:00",
          "tree_id": "76846519aefc3f0c44d64f359e346119e925a5ff",
          "url": "https://github.com/tsnlab/tickle/commit/2277851ba8ba7171b51c777e480af6327a6ee075"
        },
        "date": 1789116814219,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.636,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 897.198,
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
          "id": "a517f871fcb345ec464ecabb62c465ebd33ebc0b",
          "message": "README.md: fix docs left stale by the typesupport flatten (M5), plus a \"no make install\" claim\n\n- examples/ layout paragraph: still described the pre-M5 shape (codec + driver together under\n  examples/linux/<protocol>/, examples/freertos/ cross-compiling straight out of that same\n  directory). Rewritten for the actual current layout: examples/<protocol>/ holds the .msg/.srv\n  + its generated codec; examples/linux/<protocol>/ and examples/freertos/<protocol>/ hold only\n  their own driver, both building against the same examples/<protocol>/ codec.\n- \"Integrating the library\": said \"There is no make install yet\" - it's existed since the 1.0\n  readiness pass (see CHANGELOG.md's own Added entry), this just never got mentioned here.\n- \"Message size: filling an Ethernet frame\": the numbers were for the hand-written Bulk (seq +\n  size, 8 bytes of its own header, BULK_MAX_PAYLOAD_SIZE = 1440) - stale since the cutover to the\n  generated codec, whose payload array carries a real uint16 wire length prefix instead (seq + a\n  2-byte count = 6 bytes of its own header, BULKDATA__PAYLOAD_CAPACITY = 1442, auto-derived by\n  tools/typesupport rather than hand-calculated). Recomputed and cross-checked against\n  perf_client's own -h output (`-s  payload bytes per message (default/max 1442: ...)`) and\n  BULKDATA__PAYLOAD_CAPACITY's real value before writing either down.\n\ntools/typesupport/PLAN.md: added the four adapt.py rejects that were never listed in \"Out of\nscope\" (arrays of strings, arrays of nested message types, defaults on a nested message field,\nand a bounded string's own capacity) - all four already raise a clear UnsupportedFieldError\nrather than generating something wrong, just weren't written down as deliberately out of scope\nversus simply forgotten.\n\nNo code changes; `make test` / `make lint` still pass (unaffected either way).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T09:05:47+09:00",
          "tree_id": "08db25c002b2b59d9722c1a39c038bac4cf21c25",
          "url": "https://github.com/tsnlab/tickle/commit/a517f871fcb345ec464ecabb62c465ebd33ebc0b"
        },
        "date": 1789344409188,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.643,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 892.191,
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
          "id": "a6e5a8c0f33fa0ddac227e02da529e324c0524a4",
          "message": "typesupport: implement bounded string (string<=N / @capacity) capacity\n\nDESIGN.md's \"Capacity\" rule already documented this (\"a variable\narray's *or bounded string's* C buffer\"), but adapt.py silently\ngenerated the exact same alias-only char* for a bounded string as for\na plain one - no fixed buffer, no capacity check. Give a bounded\nstring (a ROS 2 upper bound `string<=N`, or a plain `string` with an\nexplicit `# @capacity <N>` annotation) a real char[N+1] buffer and a\ncapacity check on both encode and decode, mirroring how a variable\narray's capacity already works. A plain, unbounded string is\nunaffected - auto-derivation (the rule's priority-3 case) is\ndeliberately not extended to strings, so no existing generated\ninterface changes shape.\n\nNew tests/fixtures_own/BoundedString.msg (+ golden output) covers both\ncapacity sources plus a default value; test_capacity.py adds the\nover-capacity encode/decode rejection cases.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T10:05:25+09:00",
          "tree_id": "c2775288d7991f98e7e643523b35a29cd56f7af1",
          "url": "https://github.com/tsnlab/tickle/commit/a6e5a8c0f33fa0ddac227e02da529e324c0524a4"
        },
        "date": 1789348057445,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.591,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.095,
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
          "id": "d3b430e59c70aa5ad168578097f66d9495afbdc2",
          "message": "CHANGELOG: record discovery-unicast + bounded strings, cut v1.0.0\n\nBoth features were already merged (peer-discovery unicast: 1dcb059,\n0fded4b; bounded string capacity: this session) but never made it\ninto the changelog - CONTRIBUTING.md's own rule is to record anything\nuser-visible there. Fold them into the Added section, then promote\nUnreleased to the first tagged release.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T10:29:06+09:00",
          "tree_id": "c2a6e840c30eb779541de75023d8e345ff4821a9",
          "url": "https://github.com/tsnlab/tickle/commit/d3b430e59c70aa5ad168578097f66d9495afbdc2"
        },
        "date": 1789349402641,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.624,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 902.319,
            "unit": "Mbps"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "halim9512@gmail.com",
            "name": "hseong",
            "username": "harimseong"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "f1b5b77d9f0dc36818a051e2d1c3fc558ba722b8",
          "message": "Integrate rmw_tickle initialization part (#20)\n\n* Integrate RMW partially with TickLE\n\n* global header\n* RMW initialization source\n\n* WIP: fix lint error\n\n* WIP: resolve ROS 2 dependency and generate compile_commands.json for github actions\n\n* rmw_tickle: fix build/config gaps found in review\n\nRebased onto main via the merge commit above; on top of that, fix\nwhat a review of this PR turned up:\n\n- Add package.xml/CMakeLists.txt: colcon had nothing to build -\n  check-all.yml's new \"Generate compile_commands.json for rmw_tickle\"\n  step ran `colcon build --packages-select rmw_tickle` against a\n  directory with no build manifest at all.\n- rmw_init() hardcoded `_tt_CONFIG.broadcast` to a specific /24. Read\n  it from TICKLE_BROADCAST_ADDR instead, defaulting to the HAL's own\n  compiled-in address when unset, so this doesn't silently break every\n  network that isn't 192.168.10.0/24.\n- Add the missing definitions for rmw_get_implementation_identifier(),\n  rmw_get_zero_initialized_init_options(), and the two identifier\n  externs the header declares but rmw_init.c never defined.\n- Add the standard TickLE license header to both new files\n  (CONTRIBUTING.md: every .c/.h needs one) and #include <stdbool.h>\n  explicitly for the bool field rmw_tickle.h uses instead of relying\n  on a transitive include.\n\ncheck-all.yml's own colcon step also moved from wrapping the whole\njob in a ROS Docker image (broke the existing `sudo apt install bear`\nstep with an unanswerable interactive prompt) to a single `docker run`\nscoped to just that step, merging its compile_commands.json into the\nexisting one instead of overwriting it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: use a ROS 2-preinstalled image tag for rmw_tickle\n\nCI's first real run (after the merge/fix commits) showed\n`/opt/ros/*/setup.bash: No such file or directory` -\nrostooling/setup-ros-docker:ubuntu-noble-latest turns out to be that\nproject's bare OS-base tag (APT repos configured, no actual ROS\npackages - see its own README), not a ROS-preinstalled one. Switch to\n...-ros-jazzy-ros-base-latest (ROS 2 Jazzy, the distro that targets\nUbuntu Noble) and install python3-colcon-common-extensions, which\nros-base doesn't pull in on its own.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: exclude colcon's build/ output from shellcheck\n\nThe rmw_tickle compile-db step's colcon build leaves ament-generated\nboilerplate under build/ (local_setup.{bash,sh,zsh},\ncolcon_command_prefix_build.sh, ...) - not ours to fix, and already\ngitignored. Extend shellcheck_ignore_paths the same way third_party\nalready is.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: also exclude colcon's install/ from shellcheck\n\nSame class of issue as the previous build/ fix, just under colcon's\nother generated-output directory (setup.{bash,sh,zsh},\nlocal_setup.{bash,sh,zsh}, package.{bash,sh}, ...).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: mount the rmw_tickle build container at $PWD, not /ws\n\nThe compile database that clang-tidy actually needed (rcutils/\nallocator.h et al. not found for rmw_init.c, reproducible on every\npull_request-triggered run) turned out to be a path mismatch: CMake\nbakes the build directory's absolute path into compile_commands.json,\nand colcon built at /ws inside the container while clang-tidy reads\nthe merged database back out on the *host*, where the runner's own\ncheckout lives at a completely different absolute path. Mounting the\ncontainer at the same absolute path the runner uses ($PWD) makes the\nrecorded paths resolve on both sides.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: install ROS 2 on the runner instead of via Docker\n\nThe path-mount fix wasn't enough - clang-tidy (run on the *host* by\ncpp-linter-action, after the rmw_tickle build step finishes) could\nresolve our own headers fine, but never rcutils/rmw/rosidl_runtime_c's:\nthose only ever existed inside the throwaway `--rm` container's\n/opt/ros, which is gone by the time linting happens on the bare\nrunner. Use ros-tooling/setup-ros@v0.7 (required-ros-distributions:\njazzy) instead - it installs ROS 2 straight onto the runner via apt,\nso colcon and clang-tidy see the exact same filesystem throughout, no\ncontainer/host split at all. It also brings colcon with it, so the\nmanual apt-get install step goes away too.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: source setup.bash before the rmw_tickle colcon build\n\nros-tooling/setup-ros@v0.7 installs ROS 2 onto the runner but doesn't\nput ament_cmake et al. on CMAKE_PREFIX_PATH for every subsequent step\nby itself - colcon build failed with \"Could not find a package\nconfiguration file provided by ament_cmake\". Source setup.bash first,\nsame as any fresh ROS 2 terminal would.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: pip install catkin_pkg for the rmw_tickle colcon build\n\nCMake's ament_package_xml.cmake shells out to `python3` for\ncatkin_pkg's package.xml parser - but actions/setup-python (run\nearlier, for the typesupport regen step) already put its own Python\n3.12 first on PATH, so that's the python3 CMake invokes, and it has\nno catkin_pkg (that's only installed for the system python3 that\nros-tooling/setup-ros's apt packages target). pip install it into\nwhichever python3 is currently active instead of reordering steps.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* rmw_tickle: fix include-cleaner and redundant-declaration findings\n\nNow that the compile database actually resolves (previous 4 commits),\nclang-tidy could finally see real findings instead of just failing to\nparse the file at all:\n\n- rmw_tickle.h re-declared rmw_get_implementation_identifier() and\n  rmw_get_zero_initialized_init_options() - both already declared in\n  rmw/rmw.h, which every .c file needing them already includes\n  directly (rmw_init.c does). Drop the redundant copies rather than\n  keep two declarations of the same function in sync by hand.\n- misc-include-cleaner: both files relied on rmw/rmw.h's own\n  transitive includes for rmw_node_t/rmw_publisher_t/rmw_context_t/\n  rmw_ret_t/rmw_init_options_t/rmw_security_options_t/etc. instead of\n  including each type's own defining header (rmw/types.h, rmw/init.h,\n  rmw/ret_types.h, rmw/init_options.h, rmw/security_options.h,\n  rcutils/allocator.h). rmw_tickle.h no longer needs rmw/rmw.h itself\n  at all once its own declarations are gone - it only ever used types,\n  never called an rmw_* function.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n---------\n\nCo-authored-by: github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>\nCo-authored-by: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T12:55:05+09:00",
          "tree_id": "b548eb0b8aad52fdcf3a44bab8b4b87979d7fb3b",
          "url": "https://github.com/tsnlab/tickle/commit/f1b5b77d9f0dc36818a051e2d1c3fc558ba722b8"
        },
        "date": 1789358156274,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.706,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 898.088,
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
          "id": "65214ca07bf8a6583a91c74d9a9ad9a25658de02",
          "message": "rmw_tickle: write the implementation plan (PLAN.md)\n\nCaptures the design philosophy (TickLE stays single-threaded and\nmalloc-free; rmw_tickle owns locking and allocation; the one core\nextension is a narrow blocking-poll wake primitive, not a workaround),\nthe supported subset (tools/typesupport's own out-of-scope list plus\nTickLE's single-datagram size ceiling), the QoS roadmap (local/rmw-only\nwork first, wire-protocol work last), and an 11-milestone build order\nfrom TickLE core extensions through packaging and a scoped test suite.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T13:30:31+09:00",
          "tree_id": "f700c58e456ac729694986c6ba3f166cf9e766de",
          "url": "https://github.com/tsnlab/tickle/commit/65214ca07bf8a6583a91c74d9a9ad9a25658de02"
        },
        "date": 1789360282718,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.61,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 890.833,
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
          "id": "560c55cb607196bd1475169f3f1a6f2c1051ca07",
          "message": "tt_Node_interrupt(): wake a blocking tt_Node_poll() from another thread\n\nrmw_tickle/PLAN.md's Milestone 0(a). Adds a private loopback UDP\nsocket (wake_sock/wake_addr) that tt_receive() polls alongside the\nreal one on both platforms, plus tt_wake_signal() (HAL) and its\npublic wrapper tt_Node_interrupt() (tickle.h) to write to it -\npoll()/select() wakes immediately, tt_receive() reports -3, and\ntt_Node_poll() returns the new tt_RET_INTERRUPTED. This is the one\nexception to a tt_Node's single-threaded rule (DESIGN.md's\n\"Concurrency\"): it adds no locking and lets no second thread touch\nnode-owned state, it only shortens how long a blocked tt_Node_poll()\ncall waits before yielding control back. rmw_tickle needs this so a\ndedicated thread can drive tt_Node_poll() in a loop while other calls\n(rmw_publish()) don't have to wait out its current timeout to get its\nattention.\n\nVerified with a standalone two-thread program against the real Linux\nHAL (not part of the permanent suite - tests/test_*.c is whitebox/\nmock-only by design, see platform/linux/Makefile's own comment): a\ntt_Node_poll() blocked on a 10s timeout returns tt_RET_INTERRUPTED\nwithin ~200ms of another thread calling tt_Node_interrupt(). Also\nconfirmed the signal is \"at least once, at or after the call\" rather\nthan \"only if currently blocked\" - one sent before anything is\nblocked still cuts short the very next tt_Node_poll() call, which the\ndoc comments and DESIGN.md now say explicitly.\n\ntests/test_node_interrupt.c covers handle_receive_result()'s dispatch\n(an interrupt ends the poll even when a scheduler entry was also about\nto fire, unlike a plain timeout) and tt_Node_interrupt()'s own\nargument validation via the mock HAL. Full platform/freertos/test.sh\n(uint64/set_bool/ping_pong/perf) and make test/sanitize/lint all green.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T13:53:42+09:00",
          "tree_id": "1a5661a4e60eeaa8e089e561f41204109d98764d",
          "url": "https://github.com/tsnlab/tickle/commit/560c55cb607196bd1475169f3f1a6f2c1051ca07"
        },
        "date": 1789361673699,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.642,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 895.684,
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
          "id": "1e76fb2ec4fc0ae40bc76cc13e966ed8b895caa9",
          "message": "Liveliness timeout: presume a silent peer gone after N missed UPDATEs\n\nrmw_tickle/PLAN.md's Milestone 0(b). Discovery previously only forgot\na remote node's peer-table entries when a *fresh* UPDATE said it no\nlonger hosts them, or when it sent tt_Node_destroy()'s own explicit\nfarewell UPDATE - a node that just stopped announcing at all (crash,\nnetwork partition, anything that skips the farewell) lingered in\nevery peer table forever, since process_update()'s own dedup early\nreturn (unchanged content) never touched any per-source timestamp.\n\nAdd tt_Node.update_last_seen[] (wall-clock time of the most recent\nannounce from that source, moved on *every* valid announce including\nthe dedup case - unlike update_last_modified[], which only moves on\nreal content change) and a new scheduled check_liveliness(), run every\ntt_NODE_UPDATE_INTERVAL alongside node_update()/node_flush(): a known\nnode with no announce heard for tt_LIVELINESS_MISS_THRESHOLD (config.h,\ndefault 3 - separate knob from the interval itself) consecutive\nintervals gets the same forget_peers_from_source() cleanup and update_\nseen[]/update_last_modified[] reset a farewell UPDATE would have\ntriggered, so a later announce from the same node id is treated as\nfirst contact again.\n\ntests/test_liveliness.c covers expiry past the threshold, no false\nexpiry before it, and the critical regression case: a repeated\n*unchanged* announce must still push update_last_seen[] forward, or a\nperfectly healthy node with static endpoints would eventually get\nfalsely expired despite never missing an announce.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T14:08:36+09:00",
          "tree_id": "3e2610cc027958f20924adaf299bd79a863a9d68",
          "url": "https://github.com/tsnlab/tickle/commit/1e76fb2ec4fc0ae40bc76cc13e966ed8b895caa9"
        },
        "date": 1789362567878,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.611,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 899.392,
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
          "id": "240fc4067b47adb7c2b0542cb61672aa451faa2f",
          "message": "Opt-in discovery API for graph introspection\n\nrmw_tickle/PLAN.md's Milestone 0(c) - the last piece of Milestone 0.\nEvery UPDATE decode_update_entities() sees was already discarded once\nit finished matching against local endpoints for peer tracking;\nnothing let a caller ask \"what remote entities exist at all\" for\n`ros2 topic list`-style introspection.\n\ntt_Node_set_discovery(node, discovery, callback, param) attaches a\ncaller-owned struct tt_Discovery (fixed capacity\ntt_MAX_DISCOVERED_ENTITIES, config.h, default 16, user-overridable) -\ndeliberately not embedded in struct tt_Node itself. Investigated\nmicro-ROS's own rmw_microxrcedds_c first: it's a thin XRCE-DDS client\nthat delegates essentially all real discovery/graph state to a\nseparate Agent process running full DDS elsewhere, never carrying that\nweight on the constrained device itself. If TickLE ever gets an\nanalogous split for FreeRTOS, the discovery cache belongs on whatever\nplays the Agent role (a full rmw_tickle node, presumably on Linux),\nnot on tt_Node - so tt_Node only holds a discovery pointer + callback\n+ param (3 pointers, +24 bytes measured), and a node nothing has\nattached to pays that alone regardless of platform.\n\ndecode_update_entities() upserts every remote entity it decodes\n(regardless of kind or whether a local endpoint matches) via the new\nupsert_discovered_entity(), firing the callback on appearance/refresh.\nforget_discovered_entities_from_source() mirrors forget_peers_from_\nsource() at both its existing call sites (a fresh, content-changed\nUPDATE; check_liveliness()'s timeout) to fire departed=true and clear\nthe entry. The callback itself is deliberately minimal (node_id,\nendpoint_id, kind, departed) - name/type are looked up separately via\ntt_Discovery_find() rather than paid for on every callback whether\nwanted or not.\n\ntests/test_discovery.c covers: no-op with nothing attached, record +\ncallback on a fresh announce, departure via both an explicit farewell\nUPDATE and the liveliness timeout, detaching stops future recording\nwithout clearing what's already there, and NULL-safety on the helpers.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T14:28:29+09:00",
          "tree_id": "2760bbe014c795dc024ce010c516160bcc007d82",
          "url": "https://github.com/tsnlab/tickle/commit/240fc4067b47adb7c2b0542cb61672aa451faa2f"
        },
        "date": 1789363760945,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.627,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 897.407,
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
          "id": "f0b39624ec2a103716929bf47aad0d538a9b2287",
          "message": "rmw_tickle Milestone 1(a): ros2_adapter.py converter generator\n\nAdds tickle_typesupport.ros2_adapter, generating a thin converter between a\nreal ROS 2 interface package's own rosidl_generator_c struct and TickLE's\nown, already-generated, already-tested struct/codec for that same message -\nfield-by-field copy plus bounds checks, calling the existing\n<Msg>_encode/_decode/_encode_size/_free codec completely unchanged rather\nthan regenerating CDR-4 logic a second time against ROS 2's struct shape.\nChosen specifically to minimize risk: this tool's own dev/test environment\nhas no ROS 2 install at all (no /opt/ros, rosidl_adapter not importable),\nso reusing TickLE's existing, CI-verified codec means only the converter\nitself - a much smaller surface - needs new verification.\n\nVerified fully offline via tests/fixtures_ros2_adapter/ (hand-written\nstand-ins for rosidl_generator_c/rosidl_runtime_c's public API shape) and\ntests/test_ros2_adapter.py, which compiles and round-trips the generated\nconverter against the real TickLE codec: scalars, fixed arrays,\nbounded/unbounded variable arrays (including over-capacity rejection), and\nbounded/unbounded strings (including over-capacity rejection). Zero\ncompiler warnings under -Wall -Wextra and zero clang-tidy findings under\nthe project's own .clang-tidy.\n\nBugs found and fixed during development:\n- TickLE header include was derived from the WireStruct's own c_name\n  (e.g. \"ArraysData.h\") instead of the interface-level generated filename\n  (e.g. \"Arrays.h\") that cli.generate_interface() actually produces.\n- ROS 2 header paths need snake_case filenames even though the struct name\n  keeps PascalCase (UInt64 -> u_int64.h) - added _camel_to_snake().\n- A struct with multiple variable-array element types emitted a duplicate\n  #include for primitives_sequence_functions.h, one per element type.\n- test_ros2_adapter.py's own independent render.render_topic() call hit\n  empy's global Interpreter._wasProxyInstalled state conflicting with\n  pytest's stdout capture across test modules (\"interpreter stdout proxy\n  lost\") when run as part of the full suite; fixed by reusing conftest.py's\n  shared, session-scoped generated_dir fixture instead of generating the\n  TickLE codec a second time.\n- clang-tidy (run with the project's real .clang-tidy config, not defaults)\n  flagged missing direct includes for bool/true/false, uint16_t, and both\n  paired struct headers, previously pulled in only transitively.\n\nrmw_tickle/PLAN.md's Milestone 1(b) (the CMake package + macro invoking\nthis generator as part of a real ROS 2 build) and 1(c) (automatic\nextension-point registration, stretch goal) remain pending - both need a\nreal ROS 2 CI environment to iterate against.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T15:46:08+09:00",
          "tree_id": "5d21c108a1c0e4f4230c6afc991487cb216ff242",
          "url": "https://github.com/tsnlab/tickle/commit/f0b39624ec2a103716929bf47aad0d538a9b2287"
        },
        "date": 1789368427638,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.65,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 902.262,
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
          "id": "3b8f4343a7f6cbd060fcab4240bb17589d80db4d",
          "message": "Fix check-all: clang-tidy can't lint fixtures_ros2_adapter/'s new headers\n\nf0b3962's tests/fixtures_ros2_adapter/ headers cross-include each other via\nROS 2's own relative-path convention (#include \"rosidl_runtime_c/string.h\"),\nbut being test fixtures rather than anything make all/bear actually\ncompiles, had no entry in compile_commands.json - cpp-linter's clang-tidy\nthen couldn't resolve those includes at all ('file not found'), failing\ncheck-all outright instead of just flagging a style issue.\n\n- check-all.yml: synthesise a compile_commands.json entry per fixture\n  header (same approach already used there for FreeRTOS-only sources),\n  giving clang-tidy the -I it needs to resolve the relative includes.\n- tests/fixtures_ros2_adapter/.clang-tidy: disable readability-identifier-\n  naming for this directory only, same rationale and precedent as tests/\n  golden/.clang-tidy - these are hand-written stand-ins for real ROS 2\n  headers, so they intentionally keep ROS 2's own PascalCase/dunder naming\n  rather than this project's lower_case convention.\n\nVerified locally: synthesising the same compile_commands.json entries and\nrunning clang-tidy -p against each of the 5 fixture headers now resolves\nevery include and reports zero warnings.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T15:54:43+09:00",
          "tree_id": "bce210e8078e603e04d5374082ef961d5756edc4",
          "url": "https://github.com/tsnlab/tickle/commit/3b8f4343a7f6cbd060fcab4240bb17589d80db4d"
        },
        "date": 1789368941967,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.615,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 896.781,
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
          "id": "c5c7acf0df0ebd99f31ef27d8d5d28d7236e0cdc",
          "message": "rmw_tickle Milestone 1(b)+(c): rosidl_typesupport_tickle_c, built together\n\nStarted 1(b) (the CMake package/macro wrapping ros2_adapter.py's converter)\nas planned - an explicit rosidl_typesupport_tickle_c_generate_interfaces()\nmacro call, with (c)'s automatic extension-point registration deferred as a\nstretch goal. Reading ros2/rosidl and ros2/rosidl_typesupport's own real\nCMake source (jazzy branch) turned up that this ordering doesn't work: the\nrosidl_message_type_support_t* handle rcl hands an rmw implementation is\nalways the one rosidl_typesupport_c builds for that specific message,\nlisting only whichever typesupport identifiers were registered as an\nament_index \"rosidl_typesupport_c\" resource (get_used_typesupports(),\nrosidl_typesupport_c/cmake/get_used_typesupports.cmake) *before* that\ninterface package's own rosidl_generate_interfaces() ran. Without that\nregistration, no amount of correct generated code is reachable from a real\nrmw_create_publisher() call - so (c) isn't an optional convenience on top\nof (b), it's a hard prerequisite, and both are built together here instead.\n\nNew rosidl_typesupport_tickle_c package:\n- CMakeLists.txt: ament_index_register_resource(\"rosidl_typesupport_c\")\n  (what get_used_typesupports() actually queries) + a small identifier.c\n  runtime library (rosidl_typesupport_tickle_c__identifier, same pattern as\n  rosidl_typesupport_introspection_c/src/identifier.c) + include/\n  message_type_support.h (this package's own private\n  rosidl_typesupport_tickle_c_message_callbacks_t - rosidl's typesupport\n  contract never inspects a handle's .data shape, so this only needs to\n  agree with rmw_tickle itself, reusing TickLE's own tt_DATA_ENCODE/\n  tt_DATA_DECODE/tt_DATA_ENCODE_SIZE/tt_DATA_FREE typedefs and the same\n  cast-a-per-type-function-to-a-generic-signature idiom examples/*/*.c's\n  own <Name>Topic definitions already use).\n- rosidl_typesupport_tickle_c-extras.cmake.in +\n  cmake/rosidl_typesupport_tickle_c_generate_interfaces.cmake:\n  ament_register_extension(\"rosidl_generate_idl_interfaces\", ...) - the\n  *current* extension point (rosidl_generate_interfaces.cmake's own\n  ament_execute_extensions() call; the identically-named-but-obsolete one\n  was replaced in Dashing) - registers a per-.msg add_custom_command\n  running the new `python3 -m tickle_typesupport.ros2_cli`, compiling\n  TickLE's own src/encoding.c/log.c straight into each interface package's\n  generated typesupport library (no installed ament/colcon TickLE package\n  exists to link against instead - same approach tools/typesupport/tests/\n  test_ros2_adapter.py's own offline round-trip test already uses).\n  rosidl_generate_interfaces_ABS_IDL_FILES turned out to hold rosidl_\n  adapter's *converted .idl* paths, not the original .msg tickle_typesupport\n  can actually parse - reconstructed from the known msg/<Name>.msg layout\n  instead of assumed to be usable directly.\n\ntickle_typesupport.ros2_adapter.render_type_support() (new): the\nrosidl_message_type_support_t wrapper + ROSIDL_TYPESUPPORT_INTERFACE__\nMESSAGE_SYMBOL_NAME-named accessor tying render_adapter()'s converter and\nTickLE's codec together. .typesupport_identifier is set lazily on first\naccess rather than in the static initializer - a plain extern const char*\nisn't a C constant expression, confirmed by hitting the same compiler error\nreal rosidl_typesupport_introspection_c-generated code works around the\nsame way (its own msg__type_support.c.em template, fetched from ros2/rosidl\n@ jazzy, doing exactly this). tickle_typesupport.ros2_cli (new): the CLI\nentry point the CMake extension invokes, tying cli.py's existing TickLE\ncodec generation together with ros2_adapter's two new pieces in one pass.\n\nNew rosidl_typesupport_tickle_c_tests package: a minimal real interface\n(msg/Simple.msg) whose test/test_dispatch.c proves reachability through the\n*exact* standard get_message_typesupport_handle() dispatch chain a real\nrmw_create_publisher() call would use, not just that generated code\ncompiles - the specific thing this whole detour was about. check-all.yml\nbuilds both new packages alongside rmw_tickle and runs this test as its own\nstep, before the lint pass.\n\nVerified everything not requiring a real ROS 2 install offline: ros2_cli.py\ngenerates all 5 files per message; the type-support wrapper's C syntax was\nchecked with clang -fsyntax-only against real rosidl_runtime_c/\nrosidl_typesupport_interface header text fetched from ros2/rosidl @ jazzy\n(not guessed) plus TickLE's own real tickle.h - zero warnings. The CMake\nextension-point mechanism itself (ament_register_extension/get_used_\ntypesupports/the .idl-vs-.msg path issue) could only be derived by reading\nros2/rosidl's and ros2/rosidl_typesupport's actual source, since this\nproject's own dev environment has no ROS 2 install at all - expect this to\nneed real CI iteration (ros-tooling/setup-ros) despite the research.\n\nKnown gaps for follow-on work, not solved here: .srv support (skipped with\na CMake warning), a nested message field's own converter/wrapper files, and\npackaging tickle_typesupport itself as an installable ament_cmake_python\npackage (a real end-user build currently needs `pip install <tickle repo>/\ntools/typesupport` done by hand ahead of time, same as check-all.yml's own\nCI already does).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:36:38+09:00",
          "tree_id": "fde5c52e758ce97f461d9ce774e7474ed8295a64",
          "url": "https://github.com/tsnlab/tickle/commit/c5c7acf0df0ebd99f31ef27d8d5d28d7236e0cdc"
        },
        "date": 1789371457038,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.613,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.623,
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
          "id": "100ba2323e1bb15efcd5b7d5d4d7144885b05f24",
          "message": "Fix check-all: rosidl_generator_py needs NumPy, not something we use\n\nrosidl_typesupport_tickle_c_tests declares <depend>rosidl_default_generators</depend>\n(matching how any real ROS 2 interface package declares its own generator\ndependency) - that transitively pulls in rosidl_generator_py too, unrelated\nto rosidl_typesupport_tickle_c itself, whose own CMake configure step\n(rosidl_generator_py_generate_interfaces.cmake) needs Python3's NumPy\nheaders and failed outright since this CI environment never installed it:\n\n  CMake Error ... Could NOT find Python3 (missing: Python3_NumPy_INCLUDE_DIRS NumPy)\n\npip install rather than `apt install python3-numpy`: actions/setup-python's\nown Python 3.12 is first on PATH (same reason catkin_pkg right below it is\npip-installed instead of apt-installed), so that's the python3 CMake's\nfind_package(Python3) resolves to - not the system one apt's package would\nland in.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:42:43+09:00",
          "tree_id": "55189154d1dfb2b6bbab7a57190861c20e33d36a",
          "url": "https://github.com/tsnlab/tickle/commit/100ba2323e1bb15efcd5b7d5d4d7144885b05f24"
        },
        "date": 1789371814439,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.608,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 895.522,
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
          "id": "f605a3324113a38f4d853019a5d12afe66537fc8",
          "message": "Fix check-all: return() inside our extension aborted every later one\n\nrosidl_typesupport_tickle_c_tests's colcon build failed with \"CMake Error:\nCannot determine link language\" for OTHER packages' own generated\ntypesupport targets (rosidl_typesupport_c, rosidl_typesupport_fastrtps_c/\n_cpp, rosidl_typesupport_introspection_cpp) - none of which our code\ntouches directly. Root cause: ament_execute_extensions() and\nrosidl_generate_interfaces() are both CMake macros, and include() inside a\nmacro runs in the *caller's* scope rather than a scope of its own (unlike a\nfunction's). Our extension's `if(NOT _generated_sources) return() endif()`\nearly-exit therefore didn't just exit our own file - it unwound the whole\nenclosing rosidl_generate_interfaces() call, silently skipping every\nextension registered after ours in the same run (whichever those happened\nto be for that package) without any error of its own. Their own\nadd_library() calls simply never ran, which is what actually surfaced as\n\"link language\" errors on their targets much later.\n\nFixed by wrapping the rest of the file's logic in `if(_generated_sources)`\ninstead of returning early - same effect, without the scope-unwinding\nhazard. (`continue()` inside the earlier foreach loop is unaffected - that\none is loop-scoped, not file/caller-scoped, by design.)\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:48:13+09:00",
          "tree_id": "8a7180e5bad908f2804683eef0ba9ee977786079",
          "url": "https://github.com/tsnlab/tickle/commit/f605a3324113a38f4d853019a5d12afe66537fc8"
        },
        "date": 1789372145337,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.609,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 894.759,
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
          "id": "812128ddc1a383868f57384ed4a192efa955f9db",
          "message": "Fix check-all: rosidl_typesupport_tickle_c_tests needs CXX too, not just C\n\nThe remaining \"CMake Error: Cannot determine link language\" failures (for\nrosidl_typesupport_c, rosidl_typesupport_fastrtps_c/_cpp, rosidl_typesupport_\nintrospection_cpp - none of them ours) persisted even after removing the\nscope-unwinding return() in our own extension. Root cause this time:\nproject(rosidl_typesupport_tickle_c_tests C) only enabled the C compiler/\nlinker, copying rmw_tickle/rmw_tickle/CMakeLists.txt's own C-only\nproject() - but that package never calls rosidl_generate_interfaces() at\nall, so it never needed CXX. Several of the *other* typesupport generators\nrosidl_generate_interfaces() invokes for our msg/Simple.msg generate .cpp\nsources (rosidl_typesupport_c's own dispatch file is .cpp despite its \"C\"\nname, and fastrtps_cpp/introspection_cpp are C++-only outright) - without\nCXX enabled, CMake can't determine a link language for any of their\ngenerated library targets, unrelated to our own rosidl_typesupport_tickle_c\nextension (C-only, and confirmed NOT in the failing-target list either\ntime). project(... C CXX) fixes it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:54:26+09:00",
          "tree_id": "ab8bd4ba201934430613136b63881050fa8b89ba",
          "url": "https://github.com/tsnlab/tickle/commit/812128ddc1a383868f57384ed4a192efa955f9db"
        },
        "date": 1789372517535,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.626,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.173,
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
          "id": "d70b26ce9a86f73f49059ffbabb949e81b6195d8",
          "message": "Fix check-all: trim rosidl_typesupport_tickle_c_tests deps, add lark\n\nrosidl_default_generators (what a real interface package normally depends\non) transitively pulls in rosidl_generator_py/_rs and ament_cmake_python's\nown egg-build step for this test package - none of which\nrosidl_typesupport_tickle_c itself needs, and which failed outright\n(ModuleNotFoundError: setuptools/lark) since this CI environment's\nactions/setup-python interpreter has neither. Swapped to depending only on\nwhat's actually needed: rosidl_generator_c (the message struct test/\ntest_dispatch.c includes) and rosidl_typesupport_c (ROSIDL_GET_MSG_TYPE_\nSUPPORT()'s own dispatch entry point).\n\nThat alone wasn't enough, though: rosidl_generator_c unconditionally\ndepends on rosidl_generator_type_description for every interface it\ngenerates regardless of which typesupports are involved, which needs both\nNumPy and lark - lark wasn't part of the earlier NumPy fix. pip install\nlark alongside it, same reasoning as numpy/catkin_pkg already there\n(actions/setup-python's own Python 3.12 is first on PATH, so that's the\npython3 CMake's find_package(Python3) resolves to - a system apt package\nwould land somewhere find_package(Python3) never looks).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:03:43+09:00",
          "tree_id": "16e4856bbdec713178e01ba8732d74d2a710aba9",
          "url": "https://github.com/tsnlab/tickle/commit/d70b26ce9a86f73f49059ffbabb949e81b6195d8"
        },
        "date": 1789373075753,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.61,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 900.327,
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
          "id": "7219c6d455ad7a31bde10170e81b8abf1f2c17c4",
          "message": "Fix check-all: rosidl_generate_interfaces() needs rosidl_cmake found too\n\nd70b26c dropped rosidl_default_generators in favor of a minimal dependency\nset (rosidl_generator_c + rosidl_typesupport_c), but rosidl_\ngenerate_interfaces() itself is provided by rosidl_cmake, which\nrosidl_default_generators had only been pulling in transitively - without\nit: \"Unknown CMake command rosidl_generate_interfaces\". find_package(\nrosidl_cmake REQUIRED) explicitly instead, and declare it as a\nbuildtool_depend in package.xml.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:07:59+09:00",
          "tree_id": "81a6a0e2411a9a6b24d93c1dc20a03bbc0e7a65b",
          "url": "https://github.com/tsnlab/tickle/commit/7219c6d455ad7a31bde10170e81b8abf1f2c17c4"
        },
        "date": 1789373330666,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.652,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 889.355,
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
          "id": "40a63fb1dabec592b2d4337f273f6bc696f0b012",
          "message": "Fix check-all: doubled include path, TickLE headers missing post-install\n\ntest_dispatch.c's build failed with \"tickle/tickle.h: No such file or\ndirectory\" while reading /home/runner/.../install/rosidl_typesupport_\ntickle_c/include/rosidl_typesupport_tickle_c/rosidl_typesupport_tickle_c/\nmessage_type_support.h - a doubled rosidl_typesupport_tickle_c/ segment.\n\nTwo separate bugs, both mine:\n- install(DIRECTORY include/ DESTINATION include/rosidl_typesupport_tickle_c)\n  duplicated the subdirectory: include/ already contains its own\n  rosidl_typesupport_tickle_c/ subfolder, so this landed headers at\n  .../include/rosidl_typesupport_tickle_c/rosidl_typesupport_tickle_c/*.h.\n  Fixed to DESTINATION include, matching rmw_tickle/rmw_tickle/CMakeLists.\n  txt's own working convention (-I root is plain \"include\", callers write\n  #include \"rosidl_typesupport_tickle_c/identifier.h\" with the\n  subdirectory) - and the matching target_include_directories()/\n  ament_export_include_directories()/install(TARGETS ... INCLUDES\n  DESTINATION ...) entries, all previously \"include/rosidl_typesupport_\n  tickle_c\" (the -I root itself, wrong) instead of plain \"include\".\n- Once that was fixed, TICKLE_ROOT/include was still missing for any\n  consumer reached through find_package() (like rosidl_typesupport_tickle_c_\n  tests/test/test_dispatch.c) rather than this package's own build: it was\n  only added under $<BUILD_INTERFACE:...>, and an installed/imported target\n  only ever sees its INSTALL_INTERFACE. TickLE has no installed package of\n  its own to find_package() instead (same reasoning as TICKLE_ROOT's own\n  baked-in-absolute-path design in the extras.cmake.in), so this now sits\n  outside any interface generator expression, unconditional either way.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:13:51+09:00",
          "tree_id": "d9d634a528a4a915787e487678c22b64794a66c2",
          "url": "https://github.com/tsnlab/tickle/commit/40a63fb1dabec592b2d4337f273f6bc696f0b012"
        },
        "date": 1789373681510,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.657,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.665,
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
          "id": "b9fb6606301d762f49ab724e8d0ed24992942aa0",
          "message": "Fix check-all: split colcon build so AMENT_PREFIX_PATH updates in between\n\nThe remaining failure was a real one, not a build/link problem: test_\ndispatch's own assert(ours != NULL) fired - get_message_typesupport_handle()\ncouldn't find rosidl_typesupport_tickle_c in rosidl_typesupport_c's own\ndispatch table for msg/Simple, meaning rmw_tickle/PLAN.md's Milestone 1(c)\nregistration wasn't actually visible at generate time for this specific\nmessage despite rosidl_typesupport_tickle_c having already built and\ninstalled successfully earlier in the very same colcon build.\n\nRoot cause, confirmed by reading ament_cmake's own CMake source (ament/\nament_cmake @ rolling): get_used_typesupports() (rosidl_typesupport_c/cmake/\nget_used_typesupports.cmake) calls ament_index_get_resources(), which reads\ncandidate prefixes from the AMENT_PREFIX_PATH *environment variable*\n(ament_index_get_prefix_path.cmake) - a completely different mechanism from\nCMAKE_PREFIX_PATH, which is what find_package() itself resolves against and\nwhich colcon *does* extend across packages within a single build run. AMENT_\nPREFIX_PATH only grows when install/setup.bash is sourced - which is why\nfind_package(rosidl_typesupport_tickle_c REQUIRED) succeeded (proving\nnothing about ament index visibility) while the resource registration\nitself stayed invisible to a package built in the same colcon invocation.\n\nSplit into two colcon build calls with `source install/setup.bash` between\nthem: rmw_tickle + rosidl_typesupport_tickle_c first, then rosidl_\ntypesupport_tickle_c_tests (the one that actually calls rosidl_generate_\ninterfaces() and needs rosidl_typesupport_tickle_c's registration to be\nlive) second.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:21:29+09:00",
          "tree_id": "749310f7c027a2d26db4d4e68293d5285cd7732f",
          "url": "https://github.com/tsnlab/tickle/commit/b9fb6606301d762f49ab724e8d0ed24992942aa0"
        },
        "date": 1789374140711,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.621,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 894.549,
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
          "id": "e82e4a4b3d157ffb6dcec9fec7ad23ea2b68aa8b",
          "message": "check-all: temporary diagnostics for the ament-index registration gap\n\nSplitting the colcon build call and sourcing install/setup.bash between\nthem (b9fb660) didn't fix test_dispatch's assert(ours != NULL) failure -\nsame failure, same line, even though that should have updated AMENT_PREFIX_\nPATH before rosidl_typesupport_tickle_c_tests's own build. Rather than\nguess again, print what's actually on disk and in the environment at that\npoint (marker file presence, AMENT_PREFIX_PATH value) so the next CI run's\nlog settles it directly. Will remove once the real cause is confirmed.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:26:34+09:00",
          "tree_id": "8f50c867b2abc9fa36ad99bfefbf09ef7be5fe13",
          "url": "https://github.com/tsnlab/tickle/commit/e82e4a4b3d157ffb6dcec9fec7ad23ea2b68aa8b"
        },
        "date": 1789374446004,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.653,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 902.238,
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
          "id": "4c358cacdec560103bc7dbddb973835cd84e20b3",
          "message": "Fix check-all: build our own typesupport libraries shared, not static\n\nThe diagnostic step (e82e4a4) confirmed the ament-index registration itself\nwas already correct: the marker file existed at install/rosidl_typesupport_\ntickle_c/share/ament_index/resource_index/rosidl_typesupport_c/rosidl_\ntypesupport_tickle_c, and AMENT_PREFIX_PATH included that install prefix.\nSo the split colcon build call wasn't the actual fix for the earlier\nfailure - but the same diagnostic output also showed install/rosidl_\ntypesupport_tickle_c/lib/librosidl_typesupport_tickle_c.a: a *static*\narchive, CMake's own default absent an explicit BUILD_SHARED_LIBS=ON.\n\nrosidl_typesupport_c's own runtime dispatch (rosidl_typesupport_c__get_\nmessage_typesupport_handle_function) loads a message's specific typesupport\nimplementation via dlopen() by convention name at *runtime*, once multiple\ntypesupports are registered (true here regardless of what we ourselves\nfind_package() - /opt/ros/jazzy's own apt-installed packages, e.g.\nintrospection_c/fastrtps_c, are still globally ament-index-discoverable) -\nimpossible for a plain .a. /opt/ros/jazzy's own packages are already shared\n(standard for apt-packaged ROS 2); only our own two packages, built fresh\nin this job, needed -DBUILD_SHARED_LIBS=ON told explicitly. Removed the\ntemporary diagnostic lines now that they've served their purpose.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:32:23+09:00",
          "tree_id": "d301e642814a50cdad73bd368b1d9c6940c981ba",
          "url": "https://github.com/tsnlab/tickle/commit/4c358cacdec560103bc7dbddb973835cd84e20b3"
        },
        "date": 1789374794531,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.657,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.866,
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
          "id": "8f87b196cfc4a1d44978e4350bb91b73550c431c",
          "message": "rmw_tickle Milestone 1(b)+(c): mark done, verified green in real CI\n\nCHANGELOG.md: record rosidl_typesupport_tickle_c as a real, working\ncapability now that check-all.yml's real ROS 2 CI (jazzy) has proven the\nwhole dispatch chain reachable end to end, not just offline-plausible.\n\nPLAN.md: Milestone 1 marked done. Recorded the handful of real CI round\ntrips it took beyond what reading rosidl's/CMake's own source predicted -\neach its own small ROS 2/CMake/colcon gotcha (rosidl_generator_type_\ndescription needing NumPy/lark regardless of typesupport choice; return()\ninside an ament_execute_extensions()-included file unwinding the caller's\nwhole macro scope; rosidl_generate_interfaces_ABS_IDL_FILES holding\nconverted .idl paths, not the original .msg; needing project(... C CXX);\ndoubled install include destinations; and rosidl_typesupport_c's own\ndlopen()-based runtime dispatch needing BUILD_SHARED_LIBS=ON, the least\nguessable one) - useful context for whoever tackles Milestone 2 next and\nhits similarly undocumented territory.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:41:45+09:00",
          "tree_id": "13c613278fa847983764b795e7f8ffd1fee75571",
          "url": "https://github.com/tsnlab/tickle/commit/8f87b196cfc4a1d44978e4350bb91b73550c431c"
        },
        "date": 1789375356660,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.609,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 900.477,
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
          "id": "c7a90b59eadb735e462aef67032b257d669329a9",
          "message": "rmw_tickle Milestone 2: rmw_create_node()/rmw_destroy_node()\n\nNew src/rmw_node.c: rmw_create_node()/rmw_destroy_node()/rmw_node_get_\ngraph_guard_condition(), implementing rmw_tickle/PLAN.md's threading model\n(TickLE row: \"single-threaded per tt_Node\"; rmw_tickle row: \"owns all lock/\nthread management - a background thread per node drives tt_Node_poll(); a\nper-node mutex serializes every other entry point against it\").\n\nrmw_tickle_node_t (rmw_tickle.h) gains poll_thread/mutex/poll_thread_\nrunning. poll_thread loops tt_Node_poll(&tickle_node, tt_RECEIVE_TIMEOUT),\nholding `mutex` only around each individual call - not across iterations,\nand not while blocked in the syscall underneath a single call for longer\nthan that short (100us, config.h) default timeout. Every other entry point\nthat will touch tickle_node (rmw_publish() et al., Milestone 3+) is meant to\ntt_Node_interrupt(&tickle_node) *then* lock `mutex` before doing so -\ntt_Node_interrupt() is the one tt_Node_* call Milestone 0(a) built\nspecifically to be safe from a different thread than whichever one is\nblocked in tt_Node_poll(). rmw_destroy_node() uses that exact pattern to\nstop poll_thread before tt_Node_destroy(): clear poll_thread_running,\ninterrupt, join, destroy.\n\n_tt_CONFIG (config.h) is a process-wide global, not per-node, so only one\ntt_Node can exist per process for now - a process-wide atomic flag rejects\na second rmw_create_node() call outright (RMW_RET-equivalent NULL + error\nmessage) rather than silently colliding with the first. \"Multiple ROS 2\nnodes per process\" stays the explicitly deferred PLAN.md item it already\nwas; this just makes the current limit fail loudly instead of silently.\n\nrmw_tickle/rmw_tickle/CMakeLists.txt: compiles TickLE's own src/tickle.c/\nencoding.c/log.c/hal_linux.c straight into librmw_tickle and links\nThreads::Threads - the first milestone that actually calls into TickLE's\nreal node lifecycle (tt_Node_create/_poll/_interrupt/_destroy); rmw_init.c\nonly ever touched the _tt_CONFIG global struct before this, needing none of\nTickLE's compiled code. No installed ament/colcon TickLE package exists to\nlink against instead - same reasoning already established for rosidl_\ntypesupport_tickle_c's own CMakeLists.txt.\n\nAlso filled in rmw_get_serialization_format() (rmw_init.c) - missing from\nthe #20-era scaffold despite RMW_TICKLE_SERIALIZATION_FORMAT/rmw_tickle_\nserialization_format already existing right next to it.\n\nNot yet verified against a real ROS 2 build - no local ROS 2 install to\ncheck rmw/rcutils header usage against directly, unlike Milestone 1's\nCMake-internals research (this one's API surface is small and copies\nrmw_init.c's own already-CI-verified macro/field usage directly, but the\nactual compile is still pending the next CI run).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:20:22+09:00",
          "tree_id": "ece24e51f6bc0a3f5c646ccafa3adc3e056b8115",
          "url": "https://github.com/tsnlab/tickle/commit/c7a90b59eadb735e462aef67032b257d669329a9"
        },
        "date": 1789377675591,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.612,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 901.935,
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
          "id": "dcf07a2854887ed57e951ace93e034a570c2d505",
          "message": "Fix check-all: mixed plain/keyword target_link_libraries on rmw_tickle\n\nament_target_dependencies() (called just above for rcutils/rmw/\nrosidl_runtime_c) internally uses the plain (non-keyword) target_link_\nlibraries() signature for this same target. My own target_link_libraries(\nrmw_tickle PUBLIC Threads::Threads) used the keyword form, which CMake\nrefuses to mix with plain-signature calls on the same target (\"All uses of\ntarget_link_libraries with a target must be either all-keyword or\nall-plain\"). Switched to the plain form to match.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:24:49+09:00",
          "tree_id": "180c3cf17511f3024ce0528a77b362b76b28f08c",
          "url": "https://github.com/tsnlab/tickle/commit/dcf07a2854887ed57e951ace93e034a570c2d505"
        },
        "date": 1789377940264,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "send throughput",
            "value": 937.664,
            "unit": "Mbps"
          },
          {
            "name": "recv throughput",
            "value": 902.393,
            "unit": "Mbps"
          }
        ]
      }
    ],
    "Latency jitter (ping/pong rtt mdev)": [
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
          "id": "2277851ba8ba7171b51c777e480af6327a6ee075",
          "message": "Lint cleanup (repo-wide, `make lint` now zero warnings) + HIL jitter alert split out\n\n1. Pre-existing lint findings, none of them caused by the typesupport work, just never cleaned\n   up:\n   - examples/linux/{uint64/{publisher,subscriber},set_bool/{client,server},ping_pong/{ping,pong}}.c:\n     removed unused <stdlib.h>/<string.h> (neither ever called anything from either - same\n     \"misc-include-cleaner\" finding perf_client.c/perf_server.c already had fixed for the same\n     reason during the Bulk cutover).\n   - examples/linux/common/cli_opts.h: `1u` -> `1U` (readability-uppercase-literal-suffix, 5\n     spots). cli_opts.c: added a direct <stdint.h> for uint32_t (previously only reached\n     transitively through cli_opts.h).\n   - src/hal_linux.c: tt_send_iov()'s `(void*)(uintptr_t)hdr`/`body` casts were a redundant\n     integer round-trip just to discard `const` on an already-a-pointer value (performance-no-\n     int-to-ptr) - a straight `(void*)hdr` does the same thing without it, same fix already\n     applied to the generated Bulk.c during the earlier cutover. Also added NOLINT(misc-include-\n     cleaner) on <sys/uio.h> and its own `struct iovec` use - clang-tidy's IWYU mapping doesn't\n     know this glibc symbol's real (portable, POSIX-specified) home and flags both sides of a\n     correct, portable include; same class of false positive this file already suppresses for\n     CLOCK_REALTIME/SOL_SOCKET a few lines up.\n\n2. performance.yml: rtt mdev (ping's own mean-deviation/jitter stat) is inherently noisy on real\n   hardware - it alone tripped the Performance Test's 200% alert-threshold three separate times\n   in one session, each time on a commit nowhere near the ping/pong latency path (a check-all.yml\n   fix, a doc-only CHANGELOG commit, ...). Split into its own benchmark group (run_perf.sh now\n   writes a separate latency-jitter-benchmark.json) with fail-on-alert: false - still tracked and\n   graphed (summary-always, auto-push, same as every other group), just never fails the build on\n   its own. rtt avg and packet loss - the two latency-benchmark.json still carries - stay at\n   200%/fail-on-alert: true, since neither showed this flakiness and both do reflect a real\n   regression when they move.\n\nVerified: `make lint` / `make -C platform/freertos lint` both zero warnings (previously ~16\npre-existing findings across 9 files); `make test` / `make sanitize` still pass; run_perf.sh's\nnew JSON-writing logic checked directly (valid JSON, correct split) since HIL itself isn't\nreachable from here to run the real script end to end.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-11T17:52:38+09:00",
          "tree_id": "76846519aefc3f0c44d64f359e346119e925a5ff",
          "url": "https://github.com/tsnlab/tickle/commit/2277851ba8ba7171b51c777e480af6327a6ee075"
        },
        "date": 1789116811289,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
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
          "id": "a517f871fcb345ec464ecabb62c465ebd33ebc0b",
          "message": "README.md: fix docs left stale by the typesupport flatten (M5), plus a \"no make install\" claim\n\n- examples/ layout paragraph: still described the pre-M5 shape (codec + driver together under\n  examples/linux/<protocol>/, examples/freertos/ cross-compiling straight out of that same\n  directory). Rewritten for the actual current layout: examples/<protocol>/ holds the .msg/.srv\n  + its generated codec; examples/linux/<protocol>/ and examples/freertos/<protocol>/ hold only\n  their own driver, both building against the same examples/<protocol>/ codec.\n- \"Integrating the library\": said \"There is no make install yet\" - it's existed since the 1.0\n  readiness pass (see CHANGELOG.md's own Added entry), this just never got mentioned here.\n- \"Message size: filling an Ethernet frame\": the numbers were for the hand-written Bulk (seq +\n  size, 8 bytes of its own header, BULK_MAX_PAYLOAD_SIZE = 1440) - stale since the cutover to the\n  generated codec, whose payload array carries a real uint16 wire length prefix instead (seq + a\n  2-byte count = 6 bytes of its own header, BULKDATA__PAYLOAD_CAPACITY = 1442, auto-derived by\n  tools/typesupport rather than hand-calculated). Recomputed and cross-checked against\n  perf_client's own -h output (`-s  payload bytes per message (default/max 1442: ...)`) and\n  BULKDATA__PAYLOAD_CAPACITY's real value before writing either down.\n\ntools/typesupport/PLAN.md: added the four adapt.py rejects that were never listed in \"Out of\nscope\" (arrays of strings, arrays of nested message types, defaults on a nested message field,\nand a bounded string's own capacity) - all four already raise a clear UnsupportedFieldError\nrather than generating something wrong, just weren't written down as deliberately out of scope\nversus simply forgotten.\n\nNo code changes; `make test` / `make lint` still pass (unaffected either way).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T09:05:47+09:00",
          "tree_id": "08db25c002b2b59d9722c1a39c038bac4cf21c25",
          "url": "https://github.com/tsnlab/tickle/commit/a517f871fcb345ec464ecabb62c465ebd33ebc0b"
        },
        "date": 1789344406527,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
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
          "id": "a6e5a8c0f33fa0ddac227e02da529e324c0524a4",
          "message": "typesupport: implement bounded string (string<=N / @capacity) capacity\n\nDESIGN.md's \"Capacity\" rule already documented this (\"a variable\narray's *or bounded string's* C buffer\"), but adapt.py silently\ngenerated the exact same alias-only char* for a bounded string as for\na plain one - no fixed buffer, no capacity check. Give a bounded\nstring (a ROS 2 upper bound `string<=N`, or a plain `string` with an\nexplicit `# @capacity <N>` annotation) a real char[N+1] buffer and a\ncapacity check on both encode and decode, mirroring how a variable\narray's capacity already works. A plain, unbounded string is\nunaffected - auto-derivation (the rule's priority-3 case) is\ndeliberately not extended to strings, so no existing generated\ninterface changes shape.\n\nNew tests/fixtures_own/BoundedString.msg (+ golden output) covers both\ncapacity sources plus a default value; test_capacity.py adds the\nover-capacity encode/decode rejection cases.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T10:05:25+09:00",
          "tree_id": "c2775288d7991f98e7e643523b35a29cd56f7af1",
          "url": "https://github.com/tsnlab/tickle/commit/a6e5a8c0f33fa0ddac227e02da529e324c0524a4"
        },
        "date": 1789348054724,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
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
          "id": "d3b430e59c70aa5ad168578097f66d9495afbdc2",
          "message": "CHANGELOG: record discovery-unicast + bounded strings, cut v1.0.0\n\nBoth features were already merged (peer-discovery unicast: 1dcb059,\n0fded4b; bounded string capacity: this session) but never made it\ninto the changelog - CONTRIBUTING.md's own rule is to record anything\nuser-visible there. Fold them into the Added section, then promote\nUnreleased to the first tagged release.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T10:29:06+09:00",
          "tree_id": "c2a6e840c30eb779541de75023d8e345ff4821a9",
          "url": "https://github.com/tsnlab/tickle/commit/d3b430e59c70aa5ad168578097f66d9495afbdc2"
        },
        "date": 1789349399897,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "halim9512@gmail.com",
            "name": "hseong",
            "username": "harimseong"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "f1b5b77d9f0dc36818a051e2d1c3fc558ba722b8",
          "message": "Integrate rmw_tickle initialization part (#20)\n\n* Integrate RMW partially with TickLE\n\n* global header\n* RMW initialization source\n\n* WIP: fix lint error\n\n* WIP: resolve ROS 2 dependency and generate compile_commands.json for github actions\n\n* rmw_tickle: fix build/config gaps found in review\n\nRebased onto main via the merge commit above; on top of that, fix\nwhat a review of this PR turned up:\n\n- Add package.xml/CMakeLists.txt: colcon had nothing to build -\n  check-all.yml's new \"Generate compile_commands.json for rmw_tickle\"\n  step ran `colcon build --packages-select rmw_tickle` against a\n  directory with no build manifest at all.\n- rmw_init() hardcoded `_tt_CONFIG.broadcast` to a specific /24. Read\n  it from TICKLE_BROADCAST_ADDR instead, defaulting to the HAL's own\n  compiled-in address when unset, so this doesn't silently break every\n  network that isn't 192.168.10.0/24.\n- Add the missing definitions for rmw_get_implementation_identifier(),\n  rmw_get_zero_initialized_init_options(), and the two identifier\n  externs the header declares but rmw_init.c never defined.\n- Add the standard TickLE license header to both new files\n  (CONTRIBUTING.md: every .c/.h needs one) and #include <stdbool.h>\n  explicitly for the bool field rmw_tickle.h uses instead of relying\n  on a transitive include.\n\ncheck-all.yml's own colcon step also moved from wrapping the whole\njob in a ROS Docker image (broke the existing `sudo apt install bear`\nstep with an unanswerable interactive prompt) to a single `docker run`\nscoped to just that step, merging its compile_commands.json into the\nexisting one instead of overwriting it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: use a ROS 2-preinstalled image tag for rmw_tickle\n\nCI's first real run (after the merge/fix commits) showed\n`/opt/ros/*/setup.bash: No such file or directory` -\nrostooling/setup-ros-docker:ubuntu-noble-latest turns out to be that\nproject's bare OS-base tag (APT repos configured, no actual ROS\npackages - see its own README), not a ROS-preinstalled one. Switch to\n...-ros-jazzy-ros-base-latest (ROS 2 Jazzy, the distro that targets\nUbuntu Noble) and install python3-colcon-common-extensions, which\nros-base doesn't pull in on its own.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: exclude colcon's build/ output from shellcheck\n\nThe rmw_tickle compile-db step's colcon build leaves ament-generated\nboilerplate under build/ (local_setup.{bash,sh,zsh},\ncolcon_command_prefix_build.sh, ...) - not ours to fix, and already\ngitignored. Extend shellcheck_ignore_paths the same way third_party\nalready is.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: also exclude colcon's install/ from shellcheck\n\nSame class of issue as the previous build/ fix, just under colcon's\nother generated-output directory (setup.{bash,sh,zsh},\nlocal_setup.{bash,sh,zsh}, package.{bash,sh}, ...).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: mount the rmw_tickle build container at $PWD, not /ws\n\nThe compile database that clang-tidy actually needed (rcutils/\nallocator.h et al. not found for rmw_init.c, reproducible on every\npull_request-triggered run) turned out to be a path mismatch: CMake\nbakes the build directory's absolute path into compile_commands.json,\nand colcon built at /ws inside the container while clang-tidy reads\nthe merged database back out on the *host*, where the runner's own\ncheckout lives at a completely different absolute path. Mounting the\ncontainer at the same absolute path the runner uses ($PWD) makes the\nrecorded paths resolve on both sides.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: install ROS 2 on the runner instead of via Docker\n\nThe path-mount fix wasn't enough - clang-tidy (run on the *host* by\ncpp-linter-action, after the rmw_tickle build step finishes) could\nresolve our own headers fine, but never rcutils/rmw/rosidl_runtime_c's:\nthose only ever existed inside the throwaway `--rm` container's\n/opt/ros, which is gone by the time linting happens on the bare\nrunner. Use ros-tooling/setup-ros@v0.7 (required-ros-distributions:\njazzy) instead - it installs ROS 2 straight onto the runner via apt,\nso colcon and clang-tidy see the exact same filesystem throughout, no\ncontainer/host split at all. It also brings colcon with it, so the\nmanual apt-get install step goes away too.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: source setup.bash before the rmw_tickle colcon build\n\nros-tooling/setup-ros@v0.7 installs ROS 2 onto the runner but doesn't\nput ament_cmake et al. on CMAKE_PREFIX_PATH for every subsequent step\nby itself - colcon build failed with \"Could not find a package\nconfiguration file provided by ament_cmake\". Source setup.bash first,\nsame as any fresh ROS 2 terminal would.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* check-all.yml: pip install catkin_pkg for the rmw_tickle colcon build\n\nCMake's ament_package_xml.cmake shells out to `python3` for\ncatkin_pkg's package.xml parser - but actions/setup-python (run\nearlier, for the typesupport regen step) already put its own Python\n3.12 first on PATH, so that's the python3 CMake invokes, and it has\nno catkin_pkg (that's only installed for the system python3 that\nros-tooling/setup-ros's apt packages target). pip install it into\nwhichever python3 is currently active instead of reordering steps.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n* rmw_tickle: fix include-cleaner and redundant-declaration findings\n\nNow that the compile database actually resolves (previous 4 commits),\nclang-tidy could finally see real findings instead of just failing to\nparse the file at all:\n\n- rmw_tickle.h re-declared rmw_get_implementation_identifier() and\n  rmw_get_zero_initialized_init_options() - both already declared in\n  rmw/rmw.h, which every .c file needing them already includes\n  directly (rmw_init.c does). Drop the redundant copies rather than\n  keep two declarations of the same function in sync by hand.\n- misc-include-cleaner: both files relied on rmw/rmw.h's own\n  transitive includes for rmw_node_t/rmw_publisher_t/rmw_context_t/\n  rmw_ret_t/rmw_init_options_t/rmw_security_options_t/etc. instead of\n  including each type's own defining header (rmw/types.h, rmw/init.h,\n  rmw/ret_types.h, rmw/init_options.h, rmw/security_options.h,\n  rcutils/allocator.h). rmw_tickle.h no longer needs rmw/rmw.h itself\n  at all once its own declarations are gone - it only ever used types,\n  never called an rmw_* function.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n\n---------\n\nCo-authored-by: github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>\nCo-authored-by: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T12:55:05+09:00",
          "tree_id": "b548eb0b8aad52fdcf3a44bab8b4b87979d7fb3b",
          "url": "https://github.com/tsnlab/tickle/commit/f1b5b77d9f0dc36818a051e2d1c3fc558ba722b8"
        },
        "date": 1789358153561,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.009,
            "unit": "ms"
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
          "id": "65214ca07bf8a6583a91c74d9a9ad9a25658de02",
          "message": "rmw_tickle: write the implementation plan (PLAN.md)\n\nCaptures the design philosophy (TickLE stays single-threaded and\nmalloc-free; rmw_tickle owns locking and allocation; the one core\nextension is a narrow blocking-poll wake primitive, not a workaround),\nthe supported subset (tools/typesupport's own out-of-scope list plus\nTickLE's single-datagram size ceiling), the QoS roadmap (local/rmw-only\nwork first, wire-protocol work last), and an 11-milestone build order\nfrom TickLE core extensions through packaging and a scoped test suite.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T13:30:31+09:00",
          "tree_id": "f700c58e456ac729694986c6ba3f166cf9e766de",
          "url": "https://github.com/tsnlab/tickle/commit/65214ca07bf8a6583a91c74d9a9ad9a25658de02"
        },
        "date": 1789360280016,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
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
          "id": "560c55cb607196bd1475169f3f1a6f2c1051ca07",
          "message": "tt_Node_interrupt(): wake a blocking tt_Node_poll() from another thread\n\nrmw_tickle/PLAN.md's Milestone 0(a). Adds a private loopback UDP\nsocket (wake_sock/wake_addr) that tt_receive() polls alongside the\nreal one on both platforms, plus tt_wake_signal() (HAL) and its\npublic wrapper tt_Node_interrupt() (tickle.h) to write to it -\npoll()/select() wakes immediately, tt_receive() reports -3, and\ntt_Node_poll() returns the new tt_RET_INTERRUPTED. This is the one\nexception to a tt_Node's single-threaded rule (DESIGN.md's\n\"Concurrency\"): it adds no locking and lets no second thread touch\nnode-owned state, it only shortens how long a blocked tt_Node_poll()\ncall waits before yielding control back. rmw_tickle needs this so a\ndedicated thread can drive tt_Node_poll() in a loop while other calls\n(rmw_publish()) don't have to wait out its current timeout to get its\nattention.\n\nVerified with a standalone two-thread program against the real Linux\nHAL (not part of the permanent suite - tests/test_*.c is whitebox/\nmock-only by design, see platform/linux/Makefile's own comment): a\ntt_Node_poll() blocked on a 10s timeout returns tt_RET_INTERRUPTED\nwithin ~200ms of another thread calling tt_Node_interrupt(). Also\nconfirmed the signal is \"at least once, at or after the call\" rather\nthan \"only if currently blocked\" - one sent before anything is\nblocked still cuts short the very next tt_Node_poll() call, which the\ndoc comments and DESIGN.md now say explicitly.\n\ntests/test_node_interrupt.c covers handle_receive_result()'s dispatch\n(an interrupt ends the poll even when a scheduler entry was also about\nto fire, unlike a plain timeout) and tt_Node_interrupt()'s own\nargument validation via the mock HAL. Full platform/freertos/test.sh\n(uint64/set_bool/ping_pong/perf) and make test/sanitize/lint all green.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T13:53:42+09:00",
          "tree_id": "1a5661a4e60eeaa8e089e561f41204109d98764d",
          "url": "https://github.com/tsnlab/tickle/commit/560c55cb607196bd1475169f3f1a6f2c1051ca07"
        },
        "date": 1789361670858,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.01,
            "unit": "ms"
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
          "id": "1e76fb2ec4fc0ae40bc76cc13e966ed8b895caa9",
          "message": "Liveliness timeout: presume a silent peer gone after N missed UPDATEs\n\nrmw_tickle/PLAN.md's Milestone 0(b). Discovery previously only forgot\na remote node's peer-table entries when a *fresh* UPDATE said it no\nlonger hosts them, or when it sent tt_Node_destroy()'s own explicit\nfarewell UPDATE - a node that just stopped announcing at all (crash,\nnetwork partition, anything that skips the farewell) lingered in\nevery peer table forever, since process_update()'s own dedup early\nreturn (unchanged content) never touched any per-source timestamp.\n\nAdd tt_Node.update_last_seen[] (wall-clock time of the most recent\nannounce from that source, moved on *every* valid announce including\nthe dedup case - unlike update_last_modified[], which only moves on\nreal content change) and a new scheduled check_liveliness(), run every\ntt_NODE_UPDATE_INTERVAL alongside node_update()/node_flush(): a known\nnode with no announce heard for tt_LIVELINESS_MISS_THRESHOLD (config.h,\ndefault 3 - separate knob from the interval itself) consecutive\nintervals gets the same forget_peers_from_source() cleanup and update_\nseen[]/update_last_modified[] reset a farewell UPDATE would have\ntriggered, so a later announce from the same node id is treated as\nfirst contact again.\n\ntests/test_liveliness.c covers expiry past the threshold, no false\nexpiry before it, and the critical regression case: a repeated\n*unchanged* announce must still push update_last_seen[] forward, or a\nperfectly healthy node with static endpoints would eventually get\nfalsely expired despite never missing an announce.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T14:08:36+09:00",
          "tree_id": "3e2610cc027958f20924adaf299bd79a863a9d68",
          "url": "https://github.com/tsnlab/tickle/commit/1e76fb2ec4fc0ae40bc76cc13e966ed8b895caa9"
        },
        "date": 1789362565223,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
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
          "id": "240fc4067b47adb7c2b0542cb61672aa451faa2f",
          "message": "Opt-in discovery API for graph introspection\n\nrmw_tickle/PLAN.md's Milestone 0(c) - the last piece of Milestone 0.\nEvery UPDATE decode_update_entities() sees was already discarded once\nit finished matching against local endpoints for peer tracking;\nnothing let a caller ask \"what remote entities exist at all\" for\n`ros2 topic list`-style introspection.\n\ntt_Node_set_discovery(node, discovery, callback, param) attaches a\ncaller-owned struct tt_Discovery (fixed capacity\ntt_MAX_DISCOVERED_ENTITIES, config.h, default 16, user-overridable) -\ndeliberately not embedded in struct tt_Node itself. Investigated\nmicro-ROS's own rmw_microxrcedds_c first: it's a thin XRCE-DDS client\nthat delegates essentially all real discovery/graph state to a\nseparate Agent process running full DDS elsewhere, never carrying that\nweight on the constrained device itself. If TickLE ever gets an\nanalogous split for FreeRTOS, the discovery cache belongs on whatever\nplays the Agent role (a full rmw_tickle node, presumably on Linux),\nnot on tt_Node - so tt_Node only holds a discovery pointer + callback\n+ param (3 pointers, +24 bytes measured), and a node nothing has\nattached to pays that alone regardless of platform.\n\ndecode_update_entities() upserts every remote entity it decodes\n(regardless of kind or whether a local endpoint matches) via the new\nupsert_discovered_entity(), firing the callback on appearance/refresh.\nforget_discovered_entities_from_source() mirrors forget_peers_from_\nsource() at both its existing call sites (a fresh, content-changed\nUPDATE; check_liveliness()'s timeout) to fire departed=true and clear\nthe entry. The callback itself is deliberately minimal (node_id,\nendpoint_id, kind, departed) - name/type are looked up separately via\ntt_Discovery_find() rather than paid for on every callback whether\nwanted or not.\n\ntests/test_discovery.c covers: no-op with nothing attached, record +\ncallback on a fresh announce, departure via both an explicit farewell\nUPDATE and the liveliness timeout, detaching stops future recording\nwithout clearing what's already there, and NULL-safety on the helpers.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T14:28:29+09:00",
          "tree_id": "2760bbe014c795dc024ce010c516160bcc007d82",
          "url": "https://github.com/tsnlab/tickle/commit/240fc4067b47adb7c2b0542cb61672aa451faa2f"
        },
        "date": 1789363758242,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.01,
            "unit": "ms"
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
          "id": "f0b39624ec2a103716929bf47aad0d538a9b2287",
          "message": "rmw_tickle Milestone 1(a): ros2_adapter.py converter generator\n\nAdds tickle_typesupport.ros2_adapter, generating a thin converter between a\nreal ROS 2 interface package's own rosidl_generator_c struct and TickLE's\nown, already-generated, already-tested struct/codec for that same message -\nfield-by-field copy plus bounds checks, calling the existing\n<Msg>_encode/_decode/_encode_size/_free codec completely unchanged rather\nthan regenerating CDR-4 logic a second time against ROS 2's struct shape.\nChosen specifically to minimize risk: this tool's own dev/test environment\nhas no ROS 2 install at all (no /opt/ros, rosidl_adapter not importable),\nso reusing TickLE's existing, CI-verified codec means only the converter\nitself - a much smaller surface - needs new verification.\n\nVerified fully offline via tests/fixtures_ros2_adapter/ (hand-written\nstand-ins for rosidl_generator_c/rosidl_runtime_c's public API shape) and\ntests/test_ros2_adapter.py, which compiles and round-trips the generated\nconverter against the real TickLE codec: scalars, fixed arrays,\nbounded/unbounded variable arrays (including over-capacity rejection), and\nbounded/unbounded strings (including over-capacity rejection). Zero\ncompiler warnings under -Wall -Wextra and zero clang-tidy findings under\nthe project's own .clang-tidy.\n\nBugs found and fixed during development:\n- TickLE header include was derived from the WireStruct's own c_name\n  (e.g. \"ArraysData.h\") instead of the interface-level generated filename\n  (e.g. \"Arrays.h\") that cli.generate_interface() actually produces.\n- ROS 2 header paths need snake_case filenames even though the struct name\n  keeps PascalCase (UInt64 -> u_int64.h) - added _camel_to_snake().\n- A struct with multiple variable-array element types emitted a duplicate\n  #include for primitives_sequence_functions.h, one per element type.\n- test_ros2_adapter.py's own independent render.render_topic() call hit\n  empy's global Interpreter._wasProxyInstalled state conflicting with\n  pytest's stdout capture across test modules (\"interpreter stdout proxy\n  lost\") when run as part of the full suite; fixed by reusing conftest.py's\n  shared, session-scoped generated_dir fixture instead of generating the\n  TickLE codec a second time.\n- clang-tidy (run with the project's real .clang-tidy config, not defaults)\n  flagged missing direct includes for bool/true/false, uint16_t, and both\n  paired struct headers, previously pulled in only transitively.\n\nrmw_tickle/PLAN.md's Milestone 1(b) (the CMake package + macro invoking\nthis generator as part of a real ROS 2 build) and 1(c) (automatic\nextension-point registration, stretch goal) remain pending - both need a\nreal ROS 2 CI environment to iterate against.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T15:46:08+09:00",
          "tree_id": "5d21c108a1c0e4f4230c6afc991487cb216ff242",
          "url": "https://github.com/tsnlab/tickle/commit/f0b39624ec2a103716929bf47aad0d538a9b2287"
        },
        "date": 1789368424196,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.01,
            "unit": "ms"
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
          "id": "3b8f4343a7f6cbd060fcab4240bb17589d80db4d",
          "message": "Fix check-all: clang-tidy can't lint fixtures_ros2_adapter/'s new headers\n\nf0b3962's tests/fixtures_ros2_adapter/ headers cross-include each other via\nROS 2's own relative-path convention (#include \"rosidl_runtime_c/string.h\"),\nbut being test fixtures rather than anything make all/bear actually\ncompiles, had no entry in compile_commands.json - cpp-linter's clang-tidy\nthen couldn't resolve those includes at all ('file not found'), failing\ncheck-all outright instead of just flagging a style issue.\n\n- check-all.yml: synthesise a compile_commands.json entry per fixture\n  header (same approach already used there for FreeRTOS-only sources),\n  giving clang-tidy the -I it needs to resolve the relative includes.\n- tests/fixtures_ros2_adapter/.clang-tidy: disable readability-identifier-\n  naming for this directory only, same rationale and precedent as tests/\n  golden/.clang-tidy - these are hand-written stand-ins for real ROS 2\n  headers, so they intentionally keep ROS 2's own PascalCase/dunder naming\n  rather than this project's lower_case convention.\n\nVerified locally: synthesising the same compile_commands.json entries and\nrunning clang-tidy -p against each of the 5 fixture headers now resolves\nevery include and reports zero warnings.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T15:54:43+09:00",
          "tree_id": "bce210e8078e603e04d5374082ef961d5756edc4",
          "url": "https://github.com/tsnlab/tickle/commit/3b8f4343a7f6cbd060fcab4240bb17589d80db4d"
        },
        "date": 1789368939143,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.014,
            "unit": "ms"
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
          "id": "c5c7acf0df0ebd99f31ef27d8d5d28d7236e0cdc",
          "message": "rmw_tickle Milestone 1(b)+(c): rosidl_typesupport_tickle_c, built together\n\nStarted 1(b) (the CMake package/macro wrapping ros2_adapter.py's converter)\nas planned - an explicit rosidl_typesupport_tickle_c_generate_interfaces()\nmacro call, with (c)'s automatic extension-point registration deferred as a\nstretch goal. Reading ros2/rosidl and ros2/rosidl_typesupport's own real\nCMake source (jazzy branch) turned up that this ordering doesn't work: the\nrosidl_message_type_support_t* handle rcl hands an rmw implementation is\nalways the one rosidl_typesupport_c builds for that specific message,\nlisting only whichever typesupport identifiers were registered as an\nament_index \"rosidl_typesupport_c\" resource (get_used_typesupports(),\nrosidl_typesupport_c/cmake/get_used_typesupports.cmake) *before* that\ninterface package's own rosidl_generate_interfaces() ran. Without that\nregistration, no amount of correct generated code is reachable from a real\nrmw_create_publisher() call - so (c) isn't an optional convenience on top\nof (b), it's a hard prerequisite, and both are built together here instead.\n\nNew rosidl_typesupport_tickle_c package:\n- CMakeLists.txt: ament_index_register_resource(\"rosidl_typesupport_c\")\n  (what get_used_typesupports() actually queries) + a small identifier.c\n  runtime library (rosidl_typesupport_tickle_c__identifier, same pattern as\n  rosidl_typesupport_introspection_c/src/identifier.c) + include/\n  message_type_support.h (this package's own private\n  rosidl_typesupport_tickle_c_message_callbacks_t - rosidl's typesupport\n  contract never inspects a handle's .data shape, so this only needs to\n  agree with rmw_tickle itself, reusing TickLE's own tt_DATA_ENCODE/\n  tt_DATA_DECODE/tt_DATA_ENCODE_SIZE/tt_DATA_FREE typedefs and the same\n  cast-a-per-type-function-to-a-generic-signature idiom examples/*/*.c's\n  own <Name>Topic definitions already use).\n- rosidl_typesupport_tickle_c-extras.cmake.in +\n  cmake/rosidl_typesupport_tickle_c_generate_interfaces.cmake:\n  ament_register_extension(\"rosidl_generate_idl_interfaces\", ...) - the\n  *current* extension point (rosidl_generate_interfaces.cmake's own\n  ament_execute_extensions() call; the identically-named-but-obsolete one\n  was replaced in Dashing) - registers a per-.msg add_custom_command\n  running the new `python3 -m tickle_typesupport.ros2_cli`, compiling\n  TickLE's own src/encoding.c/log.c straight into each interface package's\n  generated typesupport library (no installed ament/colcon TickLE package\n  exists to link against instead - same approach tools/typesupport/tests/\n  test_ros2_adapter.py's own offline round-trip test already uses).\n  rosidl_generate_interfaces_ABS_IDL_FILES turned out to hold rosidl_\n  adapter's *converted .idl* paths, not the original .msg tickle_typesupport\n  can actually parse - reconstructed from the known msg/<Name>.msg layout\n  instead of assumed to be usable directly.\n\ntickle_typesupport.ros2_adapter.render_type_support() (new): the\nrosidl_message_type_support_t wrapper + ROSIDL_TYPESUPPORT_INTERFACE__\nMESSAGE_SYMBOL_NAME-named accessor tying render_adapter()'s converter and\nTickLE's codec together. .typesupport_identifier is set lazily on first\naccess rather than in the static initializer - a plain extern const char*\nisn't a C constant expression, confirmed by hitting the same compiler error\nreal rosidl_typesupport_introspection_c-generated code works around the\nsame way (its own msg__type_support.c.em template, fetched from ros2/rosidl\n@ jazzy, doing exactly this). tickle_typesupport.ros2_cli (new): the CLI\nentry point the CMake extension invokes, tying cli.py's existing TickLE\ncodec generation together with ros2_adapter's two new pieces in one pass.\n\nNew rosidl_typesupport_tickle_c_tests package: a minimal real interface\n(msg/Simple.msg) whose test/test_dispatch.c proves reachability through the\n*exact* standard get_message_typesupport_handle() dispatch chain a real\nrmw_create_publisher() call would use, not just that generated code\ncompiles - the specific thing this whole detour was about. check-all.yml\nbuilds both new packages alongside rmw_tickle and runs this test as its own\nstep, before the lint pass.\n\nVerified everything not requiring a real ROS 2 install offline: ros2_cli.py\ngenerates all 5 files per message; the type-support wrapper's C syntax was\nchecked with clang -fsyntax-only against real rosidl_runtime_c/\nrosidl_typesupport_interface header text fetched from ros2/rosidl @ jazzy\n(not guessed) plus TickLE's own real tickle.h - zero warnings. The CMake\nextension-point mechanism itself (ament_register_extension/get_used_\ntypesupports/the .idl-vs-.msg path issue) could only be derived by reading\nros2/rosidl's and ros2/rosidl_typesupport's actual source, since this\nproject's own dev environment has no ROS 2 install at all - expect this to\nneed real CI iteration (ros-tooling/setup-ros) despite the research.\n\nKnown gaps for follow-on work, not solved here: .srv support (skipped with\na CMake warning), a nested message field's own converter/wrapper files, and\npackaging tickle_typesupport itself as an installable ament_cmake_python\npackage (a real end-user build currently needs `pip install <tickle repo>/\ntools/typesupport` done by hand ahead of time, same as check-all.yml's own\nCI already does).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:36:38+09:00",
          "tree_id": "fde5c52e758ce97f461d9ce774e7474ed8295a64",
          "url": "https://github.com/tsnlab/tickle/commit/c5c7acf0df0ebd99f31ef27d8d5d28d7236e0cdc"
        },
        "date": 1789371454152,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
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
          "id": "100ba2323e1bb15efcd5b7d5d4d7144885b05f24",
          "message": "Fix check-all: rosidl_generator_py needs NumPy, not something we use\n\nrosidl_typesupport_tickle_c_tests declares <depend>rosidl_default_generators</depend>\n(matching how any real ROS 2 interface package declares its own generator\ndependency) - that transitively pulls in rosidl_generator_py too, unrelated\nto rosidl_typesupport_tickle_c itself, whose own CMake configure step\n(rosidl_generator_py_generate_interfaces.cmake) needs Python3's NumPy\nheaders and failed outright since this CI environment never installed it:\n\n  CMake Error ... Could NOT find Python3 (missing: Python3_NumPy_INCLUDE_DIRS NumPy)\n\npip install rather than `apt install python3-numpy`: actions/setup-python's\nown Python 3.12 is first on PATH (same reason catkin_pkg right below it is\npip-installed instead of apt-installed), so that's the python3 CMake's\nfind_package(Python3) resolves to - not the system one apt's package would\nland in.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:42:43+09:00",
          "tree_id": "55189154d1dfb2b6bbab7a57190861c20e33d36a",
          "url": "https://github.com/tsnlab/tickle/commit/100ba2323e1bb15efcd5b7d5d4d7144885b05f24"
        },
        "date": 1789371811568,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
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
          "id": "f605a3324113a38f4d853019a5d12afe66537fc8",
          "message": "Fix check-all: return() inside our extension aborted every later one\n\nrosidl_typesupport_tickle_c_tests's colcon build failed with \"CMake Error:\nCannot determine link language\" for OTHER packages' own generated\ntypesupport targets (rosidl_typesupport_c, rosidl_typesupport_fastrtps_c/\n_cpp, rosidl_typesupport_introspection_cpp) - none of which our code\ntouches directly. Root cause: ament_execute_extensions() and\nrosidl_generate_interfaces() are both CMake macros, and include() inside a\nmacro runs in the *caller's* scope rather than a scope of its own (unlike a\nfunction's). Our extension's `if(NOT _generated_sources) return() endif()`\nearly-exit therefore didn't just exit our own file - it unwound the whole\nenclosing rosidl_generate_interfaces() call, silently skipping every\nextension registered after ours in the same run (whichever those happened\nto be for that package) without any error of its own. Their own\nadd_library() calls simply never ran, which is what actually surfaced as\n\"link language\" errors on their targets much later.\n\nFixed by wrapping the rest of the file's logic in `if(_generated_sources)`\ninstead of returning early - same effect, without the scope-unwinding\nhazard. (`continue()` inside the earlier foreach loop is unaffected - that\none is loop-scoped, not file/caller-scoped, by design.)\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:48:13+09:00",
          "tree_id": "8a7180e5bad908f2804683eef0ba9ee977786079",
          "url": "https://github.com/tsnlab/tickle/commit/f605a3324113a38f4d853019a5d12afe66537fc8"
        },
        "date": 1789372142345,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.008,
            "unit": "ms"
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
          "id": "812128ddc1a383868f57384ed4a192efa955f9db",
          "message": "Fix check-all: rosidl_typesupport_tickle_c_tests needs CXX too, not just C\n\nThe remaining \"CMake Error: Cannot determine link language\" failures (for\nrosidl_typesupport_c, rosidl_typesupport_fastrtps_c/_cpp, rosidl_typesupport_\nintrospection_cpp - none of them ours) persisted even after removing the\nscope-unwinding return() in our own extension. Root cause this time:\nproject(rosidl_typesupport_tickle_c_tests C) only enabled the C compiler/\nlinker, copying rmw_tickle/rmw_tickle/CMakeLists.txt's own C-only\nproject() - but that package never calls rosidl_generate_interfaces() at\nall, so it never needed CXX. Several of the *other* typesupport generators\nrosidl_generate_interfaces() invokes for our msg/Simple.msg generate .cpp\nsources (rosidl_typesupport_c's own dispatch file is .cpp despite its \"C\"\nname, and fastrtps_cpp/introspection_cpp are C++-only outright) - without\nCXX enabled, CMake can't determine a link language for any of their\ngenerated library targets, unrelated to our own rosidl_typesupport_tickle_c\nextension (C-only, and confirmed NOT in the failing-target list either\ntime). project(... C CXX) fixes it.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T16:54:26+09:00",
          "tree_id": "ab8bd4ba201934430613136b63881050fa8b89ba",
          "url": "https://github.com/tsnlab/tickle/commit/812128ddc1a383868f57384ed4a192efa955f9db"
        },
        "date": 1789372514632,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.01,
            "unit": "ms"
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
          "id": "d70b26ce9a86f73f49059ffbabb949e81b6195d8",
          "message": "Fix check-all: trim rosidl_typesupport_tickle_c_tests deps, add lark\n\nrosidl_default_generators (what a real interface package normally depends\non) transitively pulls in rosidl_generator_py/_rs and ament_cmake_python's\nown egg-build step for this test package - none of which\nrosidl_typesupport_tickle_c itself needs, and which failed outright\n(ModuleNotFoundError: setuptools/lark) since this CI environment's\nactions/setup-python interpreter has neither. Swapped to depending only on\nwhat's actually needed: rosidl_generator_c (the message struct test/\ntest_dispatch.c includes) and rosidl_typesupport_c (ROSIDL_GET_MSG_TYPE_\nSUPPORT()'s own dispatch entry point).\n\nThat alone wasn't enough, though: rosidl_generator_c unconditionally\ndepends on rosidl_generator_type_description for every interface it\ngenerates regardless of which typesupports are involved, which needs both\nNumPy and lark - lark wasn't part of the earlier NumPy fix. pip install\nlark alongside it, same reasoning as numpy/catkin_pkg already there\n(actions/setup-python's own Python 3.12 is first on PATH, so that's the\npython3 CMake's find_package(Python3) resolves to - a system apt package\nwould land somewhere find_package(Python3) never looks).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:03:43+09:00",
          "tree_id": "16e4856bbdec713178e01ba8732d74d2a710aba9",
          "url": "https://github.com/tsnlab/tickle/commit/d70b26ce9a86f73f49059ffbabb949e81b6195d8"
        },
        "date": 1789373072895,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.011,
            "unit": "ms"
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
          "id": "7219c6d455ad7a31bde10170e81b8abf1f2c17c4",
          "message": "Fix check-all: rosidl_generate_interfaces() needs rosidl_cmake found too\n\nd70b26c dropped rosidl_default_generators in favor of a minimal dependency\nset (rosidl_generator_c + rosidl_typesupport_c), but rosidl_\ngenerate_interfaces() itself is provided by rosidl_cmake, which\nrosidl_default_generators had only been pulling in transitively - without\nit: \"Unknown CMake command rosidl_generate_interfaces\". find_package(\nrosidl_cmake REQUIRED) explicitly instead, and declare it as a\nbuildtool_depend in package.xml.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:07:59+09:00",
          "tree_id": "81a6a0e2411a9a6b24d93c1dc20a03bbc0e7a65b",
          "url": "https://github.com/tsnlab/tickle/commit/7219c6d455ad7a31bde10170e81b8abf1f2c17c4"
        },
        "date": 1789373327837,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
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
          "id": "40a63fb1dabec592b2d4337f273f6bc696f0b012",
          "message": "Fix check-all: doubled include path, TickLE headers missing post-install\n\ntest_dispatch.c's build failed with \"tickle/tickle.h: No such file or\ndirectory\" while reading /home/runner/.../install/rosidl_typesupport_\ntickle_c/include/rosidl_typesupport_tickle_c/rosidl_typesupport_tickle_c/\nmessage_type_support.h - a doubled rosidl_typesupport_tickle_c/ segment.\n\nTwo separate bugs, both mine:\n- install(DIRECTORY include/ DESTINATION include/rosidl_typesupport_tickle_c)\n  duplicated the subdirectory: include/ already contains its own\n  rosidl_typesupport_tickle_c/ subfolder, so this landed headers at\n  .../include/rosidl_typesupport_tickle_c/rosidl_typesupport_tickle_c/*.h.\n  Fixed to DESTINATION include, matching rmw_tickle/rmw_tickle/CMakeLists.\n  txt's own working convention (-I root is plain \"include\", callers write\n  #include \"rosidl_typesupport_tickle_c/identifier.h\" with the\n  subdirectory) - and the matching target_include_directories()/\n  ament_export_include_directories()/install(TARGETS ... INCLUDES\n  DESTINATION ...) entries, all previously \"include/rosidl_typesupport_\n  tickle_c\" (the -I root itself, wrong) instead of plain \"include\".\n- Once that was fixed, TICKLE_ROOT/include was still missing for any\n  consumer reached through find_package() (like rosidl_typesupport_tickle_c_\n  tests/test/test_dispatch.c) rather than this package's own build: it was\n  only added under $<BUILD_INTERFACE:...>, and an installed/imported target\n  only ever sees its INSTALL_INTERFACE. TickLE has no installed package of\n  its own to find_package() instead (same reasoning as TICKLE_ROOT's own\n  baked-in-absolute-path design in the extras.cmake.in), so this now sits\n  outside any interface generator expression, unconditional either way.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:13:51+09:00",
          "tree_id": "d9d634a528a4a915787e487678c22b64794a66c2",
          "url": "https://github.com/tsnlab/tickle/commit/40a63fb1dabec592b2d4337f273f6bc696f0b012"
        },
        "date": 1789373678756,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
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
          "id": "b9fb6606301d762f49ab724e8d0ed24992942aa0",
          "message": "Fix check-all: split colcon build so AMENT_PREFIX_PATH updates in between\n\nThe remaining failure was a real one, not a build/link problem: test_\ndispatch's own assert(ours != NULL) fired - get_message_typesupport_handle()\ncouldn't find rosidl_typesupport_tickle_c in rosidl_typesupport_c's own\ndispatch table for msg/Simple, meaning rmw_tickle/PLAN.md's Milestone 1(c)\nregistration wasn't actually visible at generate time for this specific\nmessage despite rosidl_typesupport_tickle_c having already built and\ninstalled successfully earlier in the very same colcon build.\n\nRoot cause, confirmed by reading ament_cmake's own CMake source (ament/\nament_cmake @ rolling): get_used_typesupports() (rosidl_typesupport_c/cmake/\nget_used_typesupports.cmake) calls ament_index_get_resources(), which reads\ncandidate prefixes from the AMENT_PREFIX_PATH *environment variable*\n(ament_index_get_prefix_path.cmake) - a completely different mechanism from\nCMAKE_PREFIX_PATH, which is what find_package() itself resolves against and\nwhich colcon *does* extend across packages within a single build run. AMENT_\nPREFIX_PATH only grows when install/setup.bash is sourced - which is why\nfind_package(rosidl_typesupport_tickle_c REQUIRED) succeeded (proving\nnothing about ament index visibility) while the resource registration\nitself stayed invisible to a package built in the same colcon invocation.\n\nSplit into two colcon build calls with `source install/setup.bash` between\nthem: rmw_tickle + rosidl_typesupport_tickle_c first, then rosidl_\ntypesupport_tickle_c_tests (the one that actually calls rosidl_generate_\ninterfaces() and needs rosidl_typesupport_tickle_c's registration to be\nlive) second.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:21:29+09:00",
          "tree_id": "749310f7c027a2d26db4d4e68293d5285cd7732f",
          "url": "https://github.com/tsnlab/tickle/commit/b9fb6606301d762f49ab724e8d0ed24992942aa0"
        },
        "date": 1789374137920,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.015,
            "unit": "ms"
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
          "id": "e82e4a4b3d157ffb6dcec9fec7ad23ea2b68aa8b",
          "message": "check-all: temporary diagnostics for the ament-index registration gap\n\nSplitting the colcon build call and sourcing install/setup.bash between\nthem (b9fb660) didn't fix test_dispatch's assert(ours != NULL) failure -\nsame failure, same line, even though that should have updated AMENT_PREFIX_\nPATH before rosidl_typesupport_tickle_c_tests's own build. Rather than\nguess again, print what's actually on disk and in the environment at that\npoint (marker file presence, AMENT_PREFIX_PATH value) so the next CI run's\nlog settles it directly. Will remove once the real cause is confirmed.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:26:34+09:00",
          "tree_id": "8f50c867b2abc9fa36ad99bfefbf09ef7be5fe13",
          "url": "https://github.com/tsnlab/tickle/commit/e82e4a4b3d157ffb6dcec9fec7ad23ea2b68aa8b"
        },
        "date": 1789374443083,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.009,
            "unit": "ms"
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
          "id": "4c358cacdec560103bc7dbddb973835cd84e20b3",
          "message": "Fix check-all: build our own typesupport libraries shared, not static\n\nThe diagnostic step (e82e4a4) confirmed the ament-index registration itself\nwas already correct: the marker file existed at install/rosidl_typesupport_\ntickle_c/share/ament_index/resource_index/rosidl_typesupport_c/rosidl_\ntypesupport_tickle_c, and AMENT_PREFIX_PATH included that install prefix.\nSo the split colcon build call wasn't the actual fix for the earlier\nfailure - but the same diagnostic output also showed install/rosidl_\ntypesupport_tickle_c/lib/librosidl_typesupport_tickle_c.a: a *static*\narchive, CMake's own default absent an explicit BUILD_SHARED_LIBS=ON.\n\nrosidl_typesupport_c's own runtime dispatch (rosidl_typesupport_c__get_\nmessage_typesupport_handle_function) loads a message's specific typesupport\nimplementation via dlopen() by convention name at *runtime*, once multiple\ntypesupports are registered (true here regardless of what we ourselves\nfind_package() - /opt/ros/jazzy's own apt-installed packages, e.g.\nintrospection_c/fastrtps_c, are still globally ament-index-discoverable) -\nimpossible for a plain .a. /opt/ros/jazzy's own packages are already shared\n(standard for apt-packaged ROS 2); only our own two packages, built fresh\nin this job, needed -DBUILD_SHARED_LIBS=ON told explicitly. Removed the\ntemporary diagnostic lines now that they've served their purpose.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:32:23+09:00",
          "tree_id": "d301e642814a50cdad73bd368b1d9c6940c981ba",
          "url": "https://github.com/tsnlab/tickle/commit/4c358cacdec560103bc7dbddb973835cd84e20b3"
        },
        "date": 1789374791729,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
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
          "id": "8f87b196cfc4a1d44978e4350bb91b73550c431c",
          "message": "rmw_tickle Milestone 1(b)+(c): mark done, verified green in real CI\n\nCHANGELOG.md: record rosidl_typesupport_tickle_c as a real, working\ncapability now that check-all.yml's real ROS 2 CI (jazzy) has proven the\nwhole dispatch chain reachable end to end, not just offline-plausible.\n\nPLAN.md: Milestone 1 marked done. Recorded the handful of real CI round\ntrips it took beyond what reading rosidl's/CMake's own source predicted -\neach its own small ROS 2/CMake/colcon gotcha (rosidl_generator_type_\ndescription needing NumPy/lark regardless of typesupport choice; return()\ninside an ament_execute_extensions()-included file unwinding the caller's\nwhole macro scope; rosidl_generate_interfaces_ABS_IDL_FILES holding\nconverted .idl paths, not the original .msg; needing project(... C CXX);\ndoubled install include destinations; and rosidl_typesupport_c's own\ndlopen()-based runtime dispatch needing BUILD_SHARED_LIBS=ON, the least\nguessable one) - useful context for whoever tackles Milestone 2 next and\nhits similarly undocumented territory.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T17:41:45+09:00",
          "tree_id": "13c613278fa847983764b795e7f8ffd1fee75571",
          "url": "https://github.com/tsnlab/tickle/commit/8f87b196cfc4a1d44978e4350bb91b73550c431c"
        },
        "date": 1789375353260,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.014,
            "unit": "ms"
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
          "id": "c7a90b59eadb735e462aef67032b257d669329a9",
          "message": "rmw_tickle Milestone 2: rmw_create_node()/rmw_destroy_node()\n\nNew src/rmw_node.c: rmw_create_node()/rmw_destroy_node()/rmw_node_get_\ngraph_guard_condition(), implementing rmw_tickle/PLAN.md's threading model\n(TickLE row: \"single-threaded per tt_Node\"; rmw_tickle row: \"owns all lock/\nthread management - a background thread per node drives tt_Node_poll(); a\nper-node mutex serializes every other entry point against it\").\n\nrmw_tickle_node_t (rmw_tickle.h) gains poll_thread/mutex/poll_thread_\nrunning. poll_thread loops tt_Node_poll(&tickle_node, tt_RECEIVE_TIMEOUT),\nholding `mutex` only around each individual call - not across iterations,\nand not while blocked in the syscall underneath a single call for longer\nthan that short (100us, config.h) default timeout. Every other entry point\nthat will touch tickle_node (rmw_publish() et al., Milestone 3+) is meant to\ntt_Node_interrupt(&tickle_node) *then* lock `mutex` before doing so -\ntt_Node_interrupt() is the one tt_Node_* call Milestone 0(a) built\nspecifically to be safe from a different thread than whichever one is\nblocked in tt_Node_poll(). rmw_destroy_node() uses that exact pattern to\nstop poll_thread before tt_Node_destroy(): clear poll_thread_running,\ninterrupt, join, destroy.\n\n_tt_CONFIG (config.h) is a process-wide global, not per-node, so only one\ntt_Node can exist per process for now - a process-wide atomic flag rejects\na second rmw_create_node() call outright (RMW_RET-equivalent NULL + error\nmessage) rather than silently colliding with the first. \"Multiple ROS 2\nnodes per process\" stays the explicitly deferred PLAN.md item it already\nwas; this just makes the current limit fail loudly instead of silently.\n\nrmw_tickle/rmw_tickle/CMakeLists.txt: compiles TickLE's own src/tickle.c/\nencoding.c/log.c/hal_linux.c straight into librmw_tickle and links\nThreads::Threads - the first milestone that actually calls into TickLE's\nreal node lifecycle (tt_Node_create/_poll/_interrupt/_destroy); rmw_init.c\nonly ever touched the _tt_CONFIG global struct before this, needing none of\nTickLE's compiled code. No installed ament/colcon TickLE package exists to\nlink against instead - same reasoning already established for rosidl_\ntypesupport_tickle_c's own CMakeLists.txt.\n\nAlso filled in rmw_get_serialization_format() (rmw_init.c) - missing from\nthe #20-era scaffold despite RMW_TICKLE_SERIALIZATION_FORMAT/rmw_tickle_\nserialization_format already existing right next to it.\n\nNot yet verified against a real ROS 2 build - no local ROS 2 install to\ncheck rmw/rcutils header usage against directly, unlike Milestone 1's\nCMake-internals research (this one's API surface is small and copies\nrmw_init.c's own already-CI-verified macro/field usage directly, but the\nactual compile is still pending the next CI run).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:20:22+09:00",
          "tree_id": "ece24e51f6bc0a3f5c646ccafa3adc3e056b8115",
          "url": "https://github.com/tsnlab/tickle/commit/c7a90b59eadb735e462aef67032b257d669329a9"
        },
        "date": 1789377672502,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.013,
            "unit": "ms"
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
          "id": "dcf07a2854887ed57e951ace93e034a570c2d505",
          "message": "Fix check-all: mixed plain/keyword target_link_libraries on rmw_tickle\n\nament_target_dependencies() (called just above for rcutils/rmw/\nrosidl_runtime_c) internally uses the plain (non-keyword) target_link_\nlibraries() signature for this same target. My own target_link_libraries(\nrmw_tickle PUBLIC Threads::Threads) used the keyword form, which CMake\nrefuses to mix with plain-signature calls on the same target (\"All uses of\ntarget_link_libraries with a target must be either all-keyword or\nall-plain\"). Switched to the plain form to match.\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:24:49+09:00",
          "tree_id": "180c3cf17511f3024ce0528a77b362b76b28f08c",
          "url": "https://github.com/tsnlab/tickle/commit/dcf07a2854887ed57e951ace93e034a570c2d505"
        },
        "date": 1789377937419,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.012,
            "unit": "ms"
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
          "id": "606898eae9477ed91dcc36ea890787a27a18ad41",
          "message": "rmw_tickle Milestone 2: mark done, verified green in real CI\n\nCHANGELOG.md: record rmw_create_node()/rmw_destroy_node()/rmw_node_get_\ngraph_guard_condition() as real, working entry points now that check-all.yml\nhas proven them green.\n\nPLAN.md: Milestone 2 marked done, noting the one CI-only issue found beyond\nlocal review (mixing CMake's plain and keyword target_link_libraries()\nsignatures on the same target, from ament_target_dependencies() and a\ndirect Threads::Threads link both touching rmw_tickle).\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>",
          "timestamp": "2026-09-14T18:33:45+09:00",
          "tree_id": "de5fdf7bc044791ac16c35e4070319f6c5e39573",
          "url": "https://github.com/tsnlab/tickle/commit/606898eae9477ed91dcc36ea890787a27a18ad41"
        },
        "date": 1789378474043,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "rtt mdev",
            "value": 0.009,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}