# Thin cross-platform entry point - see platform/linux/Makefile for the actual native (Linux)
# build (library, examples, unit tests, lint) and platform/freertos/Makefile for the FreeRTOS
# cross-build. Neither platform's own build system lives at the repo root anymore - Linux is
# treated as one platform among others here, exactly like FreeRTOS (see platform/linux/ vs
# platform/freertos/). Everything below except test-linux/test-freertos/test-all (which are
# inherently cross-platform) forwards to platform/linux/Makefile via .DEFAULT, so
# `make`/`make test`/`make all`/... still work exactly as before from the repo root - only where
# the resulting objects/binaries land changes (platform/linux/... instead of here).

.DEFAULT_GOAL := all

.PHONY: all library examples set_bool uint64 ping_pong perf test lint clean test-linux test-freertos test-all

all library examples set_bool uint64 ping_pong perf test lint clean:
	$(MAKE) -C platform/linux $@

# Anything not listed above (createns, deletens, runclient, runping, dump1, ... - see
# platform/linux/netns.mk) also forwards, without needing to be individually kept in sync here.
.DEFAULT:
	$(MAKE) -C platform/linux $@

# A real round trip over Linux's own UDP sockets, both sides run as plain processes on loopback -
# see platform/linux/test.sh (and its own comment on why this needs no root/namespaces, unlike
# platform/linux/netns.mk's veth-based manual dev targets). Named for the platform under test, not
# the mechanism - `test-<platform>` always runs platform/<platform>/test.sh.
test-linux:
	platform/linux/test.sh

# A real round trip under QEMU for the FreeRTOS platform - see platform/freertos/test.sh. Needs
# the RISC-V toolchain (see platform/freertos/Makefile's own lint target for the exact packages).
test-freertos:
	platform/freertos/test.sh

# Every test tier that's fully self-contained (no real hardware needed) in one target: unit
# tests (mock HAL), a real Linux-HAL round trip over loopback (test-linux), and a real
# FreeRTOS-HAL round trip over emulated virtio-net (test-freertos). The two Raspberry Pi HIL
# performance test (.github/workflows/performance.yml) is deliberately not part of this - it
# needs the two real, exclusively-held Pis, so there's no "run it anywhere" version of it to add
# here.
test-all: test test-linux test-freertos
