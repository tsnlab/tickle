#!/usr/bin/env bash
# shellcheck disable=SC2086,SC2029
# Builds `large_trace` on the client Pi (the large bench with -DR4_TRACE: one "LAT index= sent_ns= arrived_ns=" line per delivered sample on the
# subscriber's stdout) from the tree t1pi_build.sh shipped to ~/t1pi_src, and copies it to the server Pi. Harness-side only, no library change.
set -eu
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}; SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes"
CLI=${CLI_SSH:-ci@10.1.1.214}; SRV=${SRV_SSH:-ci@10.1.1.213}
ssh $SSHO "$CLI" 'cd $HOME/t1pi_src && cc -O2 -DNDEBUG -Wall -Dtt_RATE_TRACE -DR4_TRACE -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -Iinclude -Isrc -o $HOME/t1pi_bin/large_trace examples/perf_hil/experiments/large_sample_bench.c src/tickle.c src/encoding.c src/log.c src/hal_linux.c -lpthread -lm' </dev/null
ssh $SSHO "$CLI" 'cat $HOME/t1pi_bin/large_trace' </dev/null | ssh $SSHO "$SRV" 'cat > $HOME/t1pi_bin/large_trace && chmod +x $HOME/t1pi_bin/large_trace'
echo LARGE_TRACE_OK
