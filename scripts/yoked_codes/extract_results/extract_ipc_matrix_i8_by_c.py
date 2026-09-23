#!/usr/bin/env python3
"""Extract IPC matrix for i8 runs across compute capacities.

Output format:
- One row per (compute_capacity, policy)
- Columns are benchmarks
- Policies include baseline, ideal_memory, cache_lru, cache_rri, prefetch_lru, prefetch_rri

Notes:
- Baseline rows use firstpass logs: c{c}_baseline0_i0_csm194.log
- Ideal memory rows use firstpass logs: c{c}_baseline1_i0_csm194.log (baseline1 from firstpass)
- Cache rows use cache logs: c{c}_i8_{lru|rri}_csm194.log
- Prefetch rows use prefetch logs: c{c}_i8_{lru|rri}_mld0_csm194.log
- Logs with csm/csd suffixes are intentionally ignored.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent
SIM_DIR = ROOT / "build" / "yoked_codes_run_all_workloads" / "logs" / "simulate"
FIRSTPASS_DIR = SIM_DIR / "firstpass"
CACHE_DIR = SIM_DIR / "cache"
PREFETCH_DIR = SIM_DIR / "prefetch"

OUT_CSV = Path(__file__).resolve().parent / "ipc_matrix_i8_by_c.csv"
CSM_TAG = "csm194"

IPC_RE = re.compile(r"^IPC\s+([0-9.eE+-]+)\s*$")
CACHE_I8_RE = re.compile(rf"^c(?P<c>\d+)_i8_(?:lru|rri)_{CSM_TAG}\.log$")
PREFETCH_I8_RE = re.compile(rf"^c(?P<c>\d+)_i8_(?:lru|rri)_mld0_{CSM_TAG}\.log$")


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
    return sorted(p.name for p in FIRSTPASS_DIR.iterdir() if p.is_dir())


def discover_compute_capacities(benchmarks: list[str]) -> list[int]:
    values: set[int] = set()

    for benchmark in benchmarks:
        cache_bench_dir = CACHE_DIR / benchmark
        if cache_bench_dir.exists():
            for f in cache_bench_dir.iterdir():
                if not f.is_file():
                    continue
                m = CACHE_I8_RE.match(f.name)
                if m:
                    values.add(int(m.group("c")))

        prefetch_bench_dir = PREFETCH_DIR / benchmark
        if prefetch_bench_dir.exists():
            for f in prefetch_bench_dir.iterdir():
                if not f.is_file():
                    continue
                m = PREFETCH_I8_RE.match(f.name)
                if m:
                    values.add(int(m.group("c")))

    return sorted(values)


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

    policies = ["baseline", "ideal_memory", "cache_lru", "cache_rri", "prefetch_lru", "prefetch_rri"]
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
    rows, benchmarks, compute_capacities = collect_rows()
    write_csv(rows, benchmarks)
    print(
        f"Wrote {len(rows)} rows ({len(compute_capacities)} compute capacities x 6 policies) "
        f"for {len(benchmarks)} benchmarks to {OUT_CSV}"
    )


if __name__ == "__main__":
    main()
