# Contributing to TickLE

## License and copyright

TickLE is dual-licensed: GPL-3.0-or-later, or a proprietary license on request (see
[README.md](README.md#license)). By submitting a change, you agree it's contributed under those
same terms.

Every source file (`.c`/`.h`, including new ones) starts with this exact header:

```c
/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */
```

Vendored code under `third_party/` (FreeRTOS-Kernel, lwIP) is exempt - those keep their own
upstream licensing as-is. Code adapted from another GPL-2.0-or-later/GPL-3.0-or-later project
(e.g. `platform/freertos/board/virtio_net.c`, ported from U-Boot) gets this same header plus an
attribution comment naming the source and its original license.

## Building and testing

See [README.md](README.md#build) for the day-to-day commands. Before opening a PR, at minimum:

```sh
$ make all    # library + every example, native (Linux) build
$ make test   # unit tests
$ make lint   # clang-format --dry-run + clang-tidy
```

If your change touches `platform/freertos/` or anything in the HAL contract
(`include/tickle/hal.h` and friends), also run the FreeRTOS cross-build and its own lint pass:

```sh
$ make -C platform/freertos lint
$ make test-freertos
```

`make test-linux` needs `sudo` (it creates real network namespaces) and isn't required for every
PR, but run it if you touched `src/hal_linux.c` or the core protocol path in `src/tickle.c`.
`make test-all` runs all three self-contained tiers (unit + linux + freertos) in one command - see
[README.md](README.md#tests) - and is what CI runs on every push/PR
([test-all.yml](.github/workflows/test-all.yml)). The two-Raspberry-Pi hardware-in-the-loop
performance benchmark ([performance.yml](.github/workflows/performance.yml)) only runs on `main`
after merge - you can't trigger it from a PR, and don't need to.

## Code style

- `clang-format` (config in [.clang-format](.clang-format)) and `clang-tidy` (config in
  [.clang-tidy](.clang-tidy)) are both enforced by `make lint` / CI
  ([check-all.yml](.github/workflows/check-all.yml)). Run `clang-format -i` on files you touch
  rather than hand-formatting.
- Naming: `tt_`-prefixed `CamelCase` for public struct/type names (`tt_Node`, `tt_Client`),
  `lower_case` for functions/variables/struct field names, `UPPER_CASE` for macros and enum
  constants - see `.clang-tidy`'s `readability-identifier-naming.*` options for the exact rules
  (and their exemptions for generated codec types like `SetBool`/`UInt64`/`Bulk`).
- Comments explain **why**, not **what** - a non-obvious constraint, a workaround, a reason a
  design decision was made a particular way. If removing a comment wouldn't confuse a future
  reader, don't add it. See DESIGN.md's "Logging conventions" section for the log-level
  conventions specifically (when to use `TT_LOG_ERROR` vs `WARNING`, never `perror()`, etc.).
- Don't leave `TODO`/`FIXME` markers for work you could do now. If something genuinely needs a
  follow-up (a value that needs real-world tuning, a known limitation), say so in a comment with
  enough context for someone else to pick it up - not just `// TODO`.
- No new abstractions, config flags, or generality beyond what the change actually needs (see the
  top-level project conventions this repo follows). A bug fix doesn't need a refactor bundled in;
  a one-off script doesn't need a plugin system.

## Tests

- `tests/test_*.c` are framework-free whitebox unit tests: each one `#include`s `src/tickle.c`
  directly (to reach its `static` functions) and links against the mock HAL in
  `tests/test_mock.h` - see any existing `tests/test_*.c` for the pattern (`test_common.h`'s
  `EXPECT_*` macros, `TEST_MOCK_DEFINE_STORAGE`/`TEST_COMMON_DEFINE_STORAGE`). New ones are
  picked up automatically by the `test` Makefile target (it globs `tests/test_*.c`).
- If you add a code path (a new submessage type, a new HAL function, a new example), add a test
  for it in the same PR rather than leaving it to a follow-up - see DESIGN.md for the protocol
  shape if you're not sure what "a test for it" looks like at the wire level.

## Submitting a change

- Open a PR against `main` at <https://github.com/tsnlab/tickle>. `check-all.yml` and
  `test-all.yml` both run automatically; make sure they're green before requesting review.
- Commit messages: a short, imperative-mood subject line (`Fix indefinite recv hang on a short
  scheduler-driven timeout`, not `Fixed a bug` or `Updates`) - see `git log` for the existing
  convention. Explain *why* in the body when the subject alone doesn't make it obvious.
- Keep PRs scoped to one change. If you find something unrelated worth fixing while you're in
  there, mention it or open a separate issue instead of folding it in.
