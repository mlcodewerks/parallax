#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 pure|cached [output.so]" >&2
    exit 2
fi

MODE=$1
case "$MODE" in
    pure|cached) ;;
    *) echo "CPU mode must be 'pure' or 'cached'" >&2; exit 2 ;;
esac

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
OUT=${2:-"$HERE/core_${MODE}.so"}

make -C "$ROOT" clean
make -C "$ROOT" -j"${JOBS:-2}" DEBUG=0 HEADLESS_BENCHMARK=1 HEADLESS_BENCH_CPU="$MODE"

CORE=$(find "$ROOT" -maxdepth 2 -type f \( -name '*.so' -o -name '*.dylib' \) -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n1 | cut -d' ' -f2-)
if [ -z "${CORE:-}" ]; then
    echo "could not locate built core shared library" >&2
    exit 3
fi
cp "$CORE" "$OUT"
echo "$OUT"
