#!/usr/bin/env python3
"""Turn ffbench's CSV output into a Markdown table.

    scripts/bench_to_md.py bench/results/bench.csv
"""
import csv
import sys
from pathlib import Path

ENHANCER = {
    "none": "—",
    "gfpgan": "GFPGAN 1.4",
    "esrgan2": "Real-ESRGAN x2",
    "esrgan4": "Real-ESRGAN x4",
}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"missing {path}", file=sys.stderr)
        return 1

    rows = list(csv.DictReader(path.open(newline="")))
    if not rows:
        print("empty CSV", file=sys.stderr)
        return 1

    print("| Stage | Provider | Enhancer | Faces | Seq | Cold (ms) | Median (ms) | p95 (ms) | VRAM base → peak (MiB) | RSS (MiB) |")
    print("|---|---|---|---:|:---:|---:|---:|---:|---:|---:|")
    for r in rows:
        stage = r["stage"]
        if stage == "full":
            stage = "**pipeline**"
        elif stage == "enhance":
            stage = "enhance*"
        enhancer = "—" if r["stage"] in ("detect", "embed", "swap") else ENHANCER.get(r["enhancer"], r["enhancer"])
        print(
            f"| {stage} | {r['provider']} | {enhancer} "
            f"| {r['faces']} | {'yes' if r['sequential'] == '1' else 'no'} "
            f"| {float(r['cold_ms']):.1f} | {float(r['median_ms']):.1f} "
            f"| {float(r['p95_ms']):.1f} | {float(r['vram_baseline_mb']):.0f} → "
            f"{float(r['vram_peak_mb']):.0f} | {float(r['rss_mb']):.0f} |"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
