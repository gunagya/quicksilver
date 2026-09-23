#!/usr/bin/env python3
"""Build an IPC matrix for c4 runs across policies and intermediate capacities.

Rows are (intermediate_capacity_i, policy) for:
- cache_lru
- cache_rri
- prefetch_lru
- prefetch_rri

Plus extra rows for baseline and ideal from firstpass:
- (0, baseline)
- (0, ideal)

Columns are benchmarks.
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

IPC_RE = re.compile(r"^IPC\s+([0-9.eE+-]+)\s*$")
CSM_TAG = "csm194"
I_FROM_CACHE_RE = re.compile(rf"^c4_i(\d+)_(lru|rri)_{CSM_TAG}\.log$")
I_FROM_PREFETCH_RE = re.compile(rf"^c4_i(\d+)_(lru|rri)_mld0_{CSM_TAG}\.log$")


def parse_ipc(log_file: Path) -> Optional[float]:
    """Return IPC from a log file, or None if missing/unparseable."""
    if not log_file.exists():
        return None

    try:
        with log_file.open("r", encoding="utf-8") as handle:
            for line in handle:
                match = IPC_RE.match(line.rstrip("\n"))
                if match:
                    return float(match.group(1))
    except OSError:
        return None

    return None


def get_benchmarks() -> list[str]:
    if not FIRSTPASS_DIR.exists():
        raise FileNotFoundError(f"Missing directory: {FIRSTPASS_DIR}")
    return sorted(p.name for p in FIRSTPASS_DIR.iterdir() if p.is_dir())


def discover_intermediate_capacities(benchmarks: list[str]) -> list[int]:
    """Find all i values that appear in cache/prefetch c4 csm194 filenames."""
    values: set[int] = set()

    for benchmark in benchmarks:
        cache_bench_dir = CACHE_DIR / benchmark
        if cache_bench_dir.exists():
            for p in cache_bench_dir.glob(f"c4_i*_*_{CSM_TAG}.log"):
                m = I_FROM_CACHE_RE.match(p.name)
                if m:
                    values.add(int(m.group(1)))

        prefetch_bench_dir = PREFETCH_DIR / benchmark
        if prefetch_bench_dir.exists():
            for p in prefetch_bench_dir.glob(f"c4_i*_*_mld0_{CSM_TAG}.log"):
                m = I_FROM_PREFETCH_RE.match(p.name)
                if m:
                    values.add(int(m.group(1)))

    return sorted(values)


def build_row(i_value: int, policy: str, benchmarks: list[str]) -> dict[str, object]:
    """Construct one output row for a given (i, policy)."""
    row: dict[str, object] = {
        "intermediate_capacity_i": i_value,
        "policy": policy,
    }

    for benchmark in benchmarks:
        ipc: Optional[float]

        if policy == "baseline":
            ipc = parse_ipc(FIRSTPASS_DIR / benchmark / f"c4_baseline0_i0_{CSM_TAG}.log")
        elif policy == "ideal":
            ipc = parse_ipc(FIRSTPASS_DIR / benchmark / f"c4_baseline1_i0_{CSM_TAG}.log")
        elif policy == "cache_lru":
            ipc = parse_ipc(CACHE_DIR / benchmark / f"c4_i{i_value}_lru_{CSM_TAG}.log")
        elif policy == "cache_rri":
            ipc = parse_ipc(CACHE_DIR / benchmark / f"c4_i{i_value}_rri_{CSM_TAG}.log")
        elif policy == "prefetch_lru":
            ipc = parse_ipc(PREFETCH_DIR / benchmark / f"c4_i{i_value}_lru_mld0_{CSM_TAG}.log")
        elif policy == "prefetch_rri":
            ipc = parse_ipc(PREFETCH_DIR / benchmark / f"c4_i{i_value}_rri_mld0_{CSM_TAG}.log")
        else:
            raise ValueError(f"Unknown policy: {policy}")

        row[benchmark] = ipc

    return row


def build_rows(benchmarks: list[str], i_values: list[int]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []

    rows.append(build_row(0, "baseline", benchmarks))
    rows.append(build_row(0, "ideal", benchmarks))

    for i_value in i_values:
        rows.append(build_row(i_value, "cache_lru", benchmarks))
        rows.append(build_row(i_value, "cache_rri", benchmarks))
        rows.append(build_row(i_value, "prefetch_lru", benchmarks))
        rows.append(build_row(i_value, "prefetch_rri", benchmarks))

    return rows


def write_csv(rows: list[dict[str, object]], benchmarks: list[str], output_file: Path) -> None:
    fieldnames = ["intermediate_capacity_i", "policy", *benchmarks]

    with output_file.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    benchmarks = get_benchmarks()
    i_values = discover_intermediate_capacities(benchmarks)
    rows = build_rows(benchmarks, i_values)

    output_file = Path(__file__).resolve().parent / "ipc_matrix_c4.csv"
    write_csv(rows, benchmarks, output_file)

    print(f"Wrote {len(rows)} rows for {len(benchmarks)} benchmarks to {output_file}")
    print(f"Discovered i values: {i_values}")


if __name__ == "__main__":
    main()
