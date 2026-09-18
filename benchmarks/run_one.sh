#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 CORE ROM OUTPUT.csv [TARGET_VI=7200] [INTERVAL=60]" >&2
    exit 2
fi

CORE=$(realpath "$1")
ROM=$(realpath "$2")
OUT=$(realpath -m "$3")
TARGET=${4:-7200}
INTERVAL=${5:-60}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN_DIR="${OUT%.csv}.run"
mkdir -p "$RUN_DIR/bench" "$(dirname "$OUT")"

(
    cd "$RUN_DIR"
    M64P_BENCH_DIR="$PWD/bench" \
        "$HERE/benchmark" "$CORE" "$ROM" "$TARGET" "$INTERVAL" "$OUT"
)
