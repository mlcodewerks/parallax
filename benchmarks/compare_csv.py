#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path


def read_csv(path: Path):
    lines = path.read_text(errors="replace").splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.startswith("vi,"))
    except StopIteration as exc:
        raise SystemExit(f"{path}: no CSV header beginning with 'vi,'") from exc
    return list(csv.DictReader(lines[start:]))


def main():
    ap = argparse.ArgumentParser(description="Compare two regress_60vi CSV logs checkpoint-by-checkpoint")
    ap.add_argument("reference", type=Path)
    ap.add_argument("candidate", type=Path)
    ap.add_argument("--show", type=int, default=8, help="maximum examples per differing field")
    ap.add_argument("--ignore", action="append", default=[], help="field to ignore (repeatable or comma-separated)")
    args = ap.parse_args()

    ref = read_csv(args.reference)
    cand = read_csv(args.candidate)
    n = min(len(ref), len(cand))
    if not n:
        raise SystemExit("no comparable rows")

    ignored = {name.strip() for item in args.ignore for name in item.split(",") if name.strip()}
    fields = [field for field in ref[0].keys() if field not in ignored]
    print(f"reference rows={len(ref)} candidate rows={len(cand)} compared={n}")
    if len(ref) != len(cand):
        print("WARNING: row counts differ")

    total = 0
    for field in fields:
        diffs = []
        for r, c in zip(ref[:n], cand[:n]):
            if r.get(field) != c.get(field):
                diffs.append((r.get("vi", "?"), r.get(field), c.get(field)))
        total += len(diffs)
        print(f"{field:16s}: {len(diffs):5d} differences", end="")
        if diffs:
            print("  first:", "; ".join(f"VI {vi}: {a} -> {b}" for vi, a, b in diffs[:args.show]))
        else:
            print()

    raise SystemExit(1 if total else 0)


if __name__ == "__main__":
    main()
