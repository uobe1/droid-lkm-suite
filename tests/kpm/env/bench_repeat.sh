#!/bin/sh
# Run a benchmark binary N times and report median / p99 of ns_per_op.
#
#   bench_repeat.sh MODE REPS BIN [ARGS...]
#
# MODE is A/B/C/D/E/F as used by the KPM-vs-LKM evaluation.  The binary is
# expected to print a line containing "ns_per_op=<float>" (see
# tests/kpm/bench_getpid.c).  Raw per-run lines go to stdout so the caller can
# keep them in the job log; the final KPM_BENCH_STATS line is machine-grepped.
set -eu

mode=${1:?usage: bench_repeat.sh MODE REPS BIN [ARGS...]}
reps=${2:?usage: bench_repeat.sh MODE REPS BIN [ARGS...]}
shift 2
test "$#" -ge 1 || { echo "bench_repeat: no benchmark binary" >&2; exit 1; }
test "$reps" -ge 1 2>/dev/null || { echo "bench_repeat: bad reps: $reps" >&2; exit 1; }

raw=$(mktemp)
trap 'rm -f "$raw"' EXIT

i=1
while [ "$i" -le "$reps" ]; do
    "$@" | tee -a "$raw"
    i=$((i + 1))
done

vals=$(awk -F'ns_per_op=' '/ns_per_op=/{split($2, a, " "); print a[1]}' "$raw" | sort -n)
n=$(printf '%s\n' "$vals" | awk 'NF{c++} END{print c+0}')
test "$n" -gt 0 || { echo "bench_repeat: no ns_per_op samples" >&2; exit 1; }

# Nearest-rank median and p99 over the collected samples.
median=$(printf '%s\n' "$vals" | awk -v n="$n" 'NR==int((n+1)/2){print; exit}')
p99=$(printf '%s\n' "$vals" | awk -v n="$n" 'BEGIN{i=int(n*0.99)+1; if(i>n)i=n; print i}')
p99v=$(printf '%s\n' "$vals" | awk -v k="$p99" 'NR==k{print; exit}')
min=$(printf '%s\n' "$vals" | head -n 1)
max=$(printf '%s\n' "$vals" | tail -n 1)

echo "KPM_BENCH_STATS mode=$mode reps=$n median_ns_per_op=$median p99_ns_per_op=$p99v min_ns_per_op=$min max_ns_per_op=$max"
