#!/usr/bin/env python3
"""Build an IPC matrix for c4 runs across policies and intermediate capacities.

Rows are (intermediate_capacity_i, policy) for:
- cache_rri
- prefetch_lru

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
from extraction_common import DEFAULT_LOG_DIR, parse_args, select_benchmarks

SIM_DIR = DEFAULT_LOG_DIR / "simulate"
BENCHMARK_FILTER = None
FIRSTPASS_DIR = SIM_DIR / "firstpass"
CACHE_DIR = SIM_DIR / "cache"
PREFETCH_DIR = SIM_DIR / "prefetch"

IPC_RE = re.compile(r"^IPC\s+([0-9.eE+-]+)\s*$")
CSM_TAG = "csm194"


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
    return select_benchmarks((p.name for p in FIRSTPASS_DIR.iterdir() if p.is_dir()), BENCHMARK_FILTER)


def discover_intermediate_capacities(benchmarks: list[str]) -> list[int]:
    """Return the paper's capacities, independent of unrelated logs on disk."""
    return [4, 8, 16, 24]


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
        rows.append(build_row(i_value, "cache_rri", benchmarks))
        rows.append(build_row(i_value, "prefetch_lru", benchmarks))

    return rows


def write_csv(rows: list[dict[str, object]], benchmarks: list[str], output_file: Path) -> None:
    fieldnames = ["intermediate_capacity_i", "policy", *benchmarks]

    with output_file.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    global SIM_DIR, FIRSTPASS_DIR, CACHE_DIR, PREFETCH_DIR, BENCHMARK_FILTER
    args = parse_args(__doc__)
    SIM_DIR = args.log_dir / "simulate"
    FIRSTPASS_DIR = SIM_DIR / "firstpass"
    CACHE_DIR = SIM_DIR / "cache"
    PREFETCH_DIR = SIM_DIR / "prefetch"
    BENCHMARK_FILTER = args.benchmarks
    benchmarks = get_benchmarks()
    i_values = discover_intermediate_capacities(benchmarks)
    rows = build_rows(benchmarks, i_values)

    output_file = args.output_dir / "sensitivity_buffer_capacity.csv"
    write_csv(rows, benchmarks, output_file)

    print(f"Wrote {len(rows)} rows for {len(benchmarks)} benchmarks to {output_file}")
    print(f"Paper buffer capacities: {i_values}")


if __name__ == "__main__":
    main()
