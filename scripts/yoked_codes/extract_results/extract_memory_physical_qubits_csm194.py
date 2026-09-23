#!/usr/bin/env python3
"""Extract MEMORY_PHYSICAL_QUBITS for baseline i0 vs c4/i8 from csm194 simulate logs.

For each benchmark under:
  build/yoked_codes_run_all_workloads/logs/simulate

This script extracts MEMORY_PHYSICAL_QUBITS from:
  - firstpass/<benchmark>/c4_baseline0_i0_csm194.log
  - cache/<benchmark>/c4_i8_rri_csm194.log

Files that contain "csd" in the filename are ignored.

Output CSV:
  results/memory_physical_qubits_baseline_i0_vs_c4_i8_csm194.csv

One row per benchmark.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[2]
SIM_DIR = ROOT / "build" / "yoked_codes_run_all_workloads" / "logs" / "simulate"
FIRSTPASS_DIR = SIM_DIR / "firstpass"
CACHE_DIR = SIM_DIR / "cache"

CSM_TAG = "csm194"
OUT_CSV = ROOT / "results" / "memory_physical_qubits_baseline_i0_vs_c4_i8_csm194.csv"

MEM_PHYS_RE = re.compile(r"^MEMORY_PHYSICAL_QUBITS\s+([0-9]+)\s*$")


def extract_memory_physical_qubits(log_path: Path) -> Optional[int]:
    if not log_path.exists():
        return None

    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for raw_line in handle:
                m = MEM_PHYS_RE.match(raw_line.rstrip("\n"))
                if m:
                    return int(m.group(1))
    except OSError:
        return None

    return None


def get_benchmarks() -> list[str]:
    if not FIRSTPASS_DIR.exists():
        raise FileNotFoundError(f"Missing firstpass directory: {FIRSTPASS_DIR}")
    return sorted(p.name for p in FIRSTPASS_DIR.iterdir() if p.is_dir())


def pick_firstpass_baseline_log(benchmark: str) -> Optional[Path]:
    bench_dir = FIRSTPASS_DIR / benchmark

    primary = bench_dir / f"c4_baseline0_i0_{CSM_TAG}.log"
    if primary.exists() and "csd" not in primary.name.lower():
        return primary

    candidates = sorted(
        p
        for p in bench_dir.glob(f"c4_baseline0_i0_{CSM_TAG}*.log")
        if p.is_file() and "csd" not in p.name.lower()
    )
    return candidates[0] if candidates else None


def pick_cache_c4_i8_log(benchmark: str) -> Optional[Path]:
    bench_dir = CACHE_DIR / benchmark

    primary = bench_dir / f"c4_i8_rri_{CSM_TAG}.log"
    if primary.exists() and "csd" not in primary.name.lower():
        return primary

    # Fallback: if the rri file is missing, accept lru.
    fallback = bench_dir / f"c4_i8_lru_{CSM_TAG}.log"
    if fallback.exists() and "csd" not in fallback.name.lower():
        return fallback

    candidates = sorted(
        p
        for p in bench_dir.glob(f"c4_i8_*_{CSM_TAG}*.log")
        if p.is_file() and "csd" not in p.name.lower()
    )
    return candidates[0] if candidates else None


def collect_rows() -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []

    for benchmark in get_benchmarks():
        baseline_log = pick_firstpass_baseline_log(benchmark)
        cache_log = pick_cache_c4_i8_log(benchmark)

        baseline_mem = extract_memory_physical_qubits(baseline_log) if baseline_log else None
        cache_mem = extract_memory_physical_qubits(cache_log) if cache_log else None

        rows.append(
            {
                "benchmark": benchmark,
                "baseline_i0_csm194_memory_physical_qubits": baseline_mem,
                "c4_i8_csm194_memory_physical_qubits": cache_mem,
            }
        )

    return rows


def write_csv(rows: list[dict[str, object]]) -> None:
    fieldnames = [
        "benchmark",
        "baseline_i0_csm194_memory_physical_qubits",
        "c4_i8_csm194_memory_physical_qubits",
    ]

    with OUT_CSV.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    if not SIM_DIR.exists():
        raise FileNotFoundError(f"Missing simulate directory: {SIM_DIR}")

    rows = collect_rows()
    write_csv(rows)
    print(f"Wrote {len(rows)} rows to {OUT_CSV}")


if __name__ == "__main__":
    main()
