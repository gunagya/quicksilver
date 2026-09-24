#!/usr/bin/env python3
"""Extract IPC matrix for i8 runs across compute capacities.

Output format:
- One row per (compute_capacity, policy)
- Columns are benchmarks
- Policies include baseline, ideal_memory, cache_rri, and prefetch_lru

Notes:
- Baseline rows use firstpass logs: c{c}_baseline0_i0_csm194.log
- Ideal memory rows use firstpass logs: c{c}_baseline1_i0_csm194.log (baseline1 from firstpass)
- Cache rows use cache logs: c{c}_i8_rri_csm194.log
- Prefetch rows use prefetch logs: c{c}_i8_lru_mld0_csm194.log
- Only csm194 logs without code-distance overrides are included.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Optional
from extraction_common import DEFAULT_LOG_DIR, DEFAULT_OUTPUT_DIR, parse_args, select_benchmarks

SIM_DIR = DEFAULT_LOG_DIR / "simulate"
BENCHMARK_FILTER = None
FIRSTPASS_DIR = SIM_DIR / "firstpass"
CACHE_DIR = SIM_DIR / "cache"
PREFETCH_DIR = SIM_DIR / "prefetch"

OUT_CSV = DEFAULT_OUTPUT_DIR / "sensitivity_compute.csv"
CSM_TAG = "csm194"

IPC_RE = re.compile(r"^IPC\s+([0-9.eE+-]+)\s*$")


def parse_ipc(log_path: Path) -> Optional[float]:
    if not log_path.exists():
        return None

    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for raw_line in handle:
                m = IPC_RE.match(raw_line.rstrip("\n"))
                if m:
                    return float(m.group(1))
    except OSError:
        return None

    return None


def get_benchmarks() -> list[str]:
    if not FIRSTPASS_DIR.exists():
        raise FileNotFoundError(f"Missing firstpass directory: {FIRSTPASS_DIR}")
    return select_benchmarks((p.name for p in FIRSTPASS_DIR.iterdir() if p.is_dir()), BENCHMARK_FILTER)


def discover_compute_capacities(benchmarks: list[str]) -> list[int]:
    """Return the paper's capacities, independent of unrelated logs on disk."""
    return [4, 8, 12, 16]


def build_row(compute_capacity: int, policy: str, benchmarks: list[str]) -> dict[str, object]:
    row: dict[str, object] = {
        "compute_capacity": compute_capacity,
        "policy": policy,
    }

    for benchmark in benchmarks:
        if policy == "baseline":
            log = FIRSTPASS_DIR / benchmark / f"c{compute_capacity}_baseline0_i0_{CSM_TAG}.log"
        elif policy == "ideal_memory":
            log = FIRSTPASS_DIR / benchmark / f"c{compute_capacity}_baseline1_i0_{CSM_TAG}.log"
        elif policy == "cache_lru":
            log = CACHE_DIR / benchmark / f"c{compute_capacity}_i8_lru_{CSM_TAG}.log"
        elif policy == "cache_rri":
            log = CACHE_DIR / benchmark / f"c{compute_capacity}_i8_rri_{CSM_TAG}.log"
        elif policy == "prefetch_lru":
            log = PREFETCH_DIR / benchmark / f"c{compute_capacity}_i8_lru_mld0_{CSM_TAG}.log"
        elif policy == "prefetch_rri":
            log = PREFETCH_DIR / benchmark / f"c{compute_capacity}_i8_rri_mld0_{CSM_TAG}.log"
        else:
            raise ValueError(f"Unknown policy: {policy}")

        row[benchmark] = parse_ipc(log)

    return row


def collect_rows() -> tuple[list[dict[str, object]], list[str], list[int]]:
    benchmarks = get_benchmarks()
    compute_capacities = discover_compute_capacities(benchmarks)

    policies = ["baseline", "ideal_memory", "cache_rri", "prefetch_lru"]
    rows: list[dict[str, object]] = []

    for c in compute_capacities:
        for policy in policies:
            rows.append(build_row(c, policy, benchmarks))

    return rows, benchmarks, compute_capacities


def write_csv(rows: list[dict[str, object]], benchmarks: list[str]) -> None:
    fieldnames = ["compute_capacity", "policy", *benchmarks]

    with OUT_CSV.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    global SIM_DIR, FIRSTPASS_DIR, CACHE_DIR, PREFETCH_DIR, OUT_CSV, BENCHMARK_FILTER
    args = parse_args(__doc__)
    SIM_DIR = args.log_dir / "simulate"
    FIRSTPASS_DIR = SIM_DIR / "firstpass"
    CACHE_DIR = SIM_DIR / "cache"
    PREFETCH_DIR = SIM_DIR / "prefetch"
    OUT_CSV = args.output_dir / "sensitivity_compute.csv"
    BENCHMARK_FILTER = args.benchmarks
    rows, benchmarks, compute_capacities = collect_rows()
    write_csv(rows, benchmarks)
    print(
        f"Wrote {len(rows)} rows ({len(compute_capacities)} compute capacities x 4 policies) "
        f"for {len(benchmarks)} benchmarks to {OUT_CSV}"
    )


if __name__ == "__main__":
    main()
