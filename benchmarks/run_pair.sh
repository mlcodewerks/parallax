#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 PURE_CORE CACHED_CORE ROM [TARGET_VI=7200] [INTERVAL=60]" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TARGET=${4:-7200}
INTERVAL=${5:-60}
mkdir -p "$HERE/out"

"$HERE/run_one.sh" "$1" "$3" "$HERE/out/pure.csv" "$TARGET" "$INTERVAL"
"$HERE/run_one.sh" "$2" "$3" "$HERE/out/cached.csv" "$TARGET" "$INTERVAL"
python3 "$HERE/compare_csv.py" --ignore emumode "$HERE/out/pure.csv" "$HERE/out/cached.csv"
