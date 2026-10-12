#!/usr/bin/env bash
# hybrid_gaps_perf_counts.sh - per-symbol sample COUNTS (not only shares) from hybrid_gaps_perf.sh's perf.data files,
# which perf wrote as root inside a namespace: read back as root through a throwaway namespace (`sudo ip` is the only
# root this PC grants). Writes <name>.nsyms beside each <name>.data. Read by hybrid_gaps_perf_read.py.
# Usage: hybrid_gaps_perf_counts.sh DIR [DIR...]
set -u
NS=hgread-$$
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; return 0; }
trap cleanup EXIT
sudo -n ip netns add "$NS" || exit 1
for dir in "$@"; do
    for data in "$dir"/*.data; do
        [ -e "$data" ] || continue
        sudo -n ip netns exec "$NS" perf report -i "$data" -n --no-children --sort comm,dso,sym --stdio -g none \
            --percent-limit 0 2>/dev/null | grep -E '^ +[0-9]|^# (Samples|Event count)' >"${data%.data}.nsyms"
        echo "$data: $(grep -c . "${data%.data}.nsyms") rows"
    done
done
