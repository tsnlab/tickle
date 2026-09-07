window.BENCHMARK_DATA = {
  "lastUpdate": 1788771377082,
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
      }
    ]
  }
}