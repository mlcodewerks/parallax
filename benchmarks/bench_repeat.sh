#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 CORE ROM [TARGET_VI=1800] [RUNS=5]" >&2
    exit 2
fi

CORE=$(realpath "$1")
ROM=$(realpath "$2")
TARGET=${3:-1800}
RUNS=${4:-5}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$HERE/out/perf"

for i in $(seq 1 "$RUNS"); do
    CSV="$HERE/out/perf/run_${i}.csv"
    RUN_DIR="${CSV%.csv}.run"
    mkdir -p "$RUN_DIR/bench"
    printf 'run %d: ' "$i"
    /usr/bin/time -f '%e seconds' env NO_RAM_HASH=1 M64P_BENCH_DIR="$RUN_DIR/bench" \
        "$HERE/benchmark" "$CORE" "$ROM" "$TARGET" "$TARGET" "$CSV" >/dev/null
 done
