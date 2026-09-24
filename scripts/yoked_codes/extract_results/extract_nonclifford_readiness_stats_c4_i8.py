#!/usr/bin/env python3
"""Extract non-Clifford readiness metrics for c4/i8 runs.

Reads logs under build/yoked_codes_run_all_workloads/logs/simulate and writes a CSV
with one row per benchmark. For each requested metric, this script emits three
columns: baseline (firstpass baseline0 i0), cache (cache rri), and prefetch
(prefetch lru).
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from extraction_common import DEFAULT_LOG_DIR, parse_args, select_benchmarks

SIM_LOGS_BASE = DEFAULT_LOG_DIR / "simulate"
BENCHMARK_FILTER = None

FIRSTPASS_DIR = SIM_LOGS_BASE / "firstpass"
CACHE_DIR = SIM_LOGS_BASE / "cache"
PREFETCH_DIR = SIM_LOGS_BASE / "prefetch"

CSM_TAG = "csm194"

METRIC_PATTERNS = {
    "avg_delay_2d_cold_non_clifford_ready_cycles": re.compile(
        r"^Avg delay: 2D/cold load to non-Clifford ready \(cycles\)\s+([0-9.eE+-]+)\s*$"
    ),
    "avg_delay_1d_non_clifford_ready_cycles": re.compile(
        r"^Avg delay: 1D load to non-Clifford ready \(cycles\)\s+([0-9.eE+-]+)\s*$"
    ),
    "loads_1d_already_verified": re.compile(
        r"^1D loads: already verified at load time\s+([0-9.eE+-]+)\s*$"
    ),
    "loads_1d_waited_for_verification": re.compile(
        r"^1D loads: needed to wait for verification\s+([0-9.eE+-]+)\s*$"
    ),
    "loads_2d_cold_reached_non_clifford_ready": re.compile(
        r"^2D/cold loads: reached non-Clifford ready\s+([0-9.eE+-]+)\s*$"
    ),
    "instruction_front_layer_delay_non_clifford_readiness_percent": re.compile(
        r"^Instruction front-layer delay due only to non-Clifford readiness \(%\)\s+([0-9.eE+-]+)\s*$"
    ),
    "cycles_stalled_non_clifford_readiness_percent": re.compile(
        r"^Cycles stalled only because of non-Clifford readiness \(%\)\s+([0-9.eE+-]+)\s*$"
    ),
}


def parse_number(value: str) -> float | int:
    """Return int when possible, otherwise float."""
    if re.fullmatch(r"[0-9]+", value):
        return int(value)
    return float(value)


def extract_metrics(log_path: Path) -> dict[str, float | int | None]:
    """Extract all requested metrics from one log file."""
    metrics: dict[str, float | int | None] = {k: None for k in METRIC_PATTERNS}
    if not log_path.exists():
        return metrics

    with log_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\n")
            for metric_name, pattern in METRIC_PATTERNS.items():
                if metrics[metric_name] is not None:
                    continue
                match = pattern.match(line)
                if match:
                    metrics[metric_name] = parse_number(match.group(1))

    return metrics


def get_benchmarks() -> list[str]:
    """Get benchmark directory names from firstpass logs."""
    if not FIRSTPASS_DIR.exists():
        raise FileNotFoundError(f"Directory not found: {FIRSTPASS_DIR}")
    return select_benchmarks((p.name for p in FIRSTPASS_DIR.iterdir() if p.is_dir()), BENCHMARK_FILTER)


def collect_rows() -> list[dict[str, object]]:
    """Collect one CSV row per benchmark."""
    rows: list[dict[str, object]] = []
    for benchmark in get_benchmarks():
        baseline_file = FIRSTPASS_DIR / benchmark / f"c4_baseline0_i0_{CSM_TAG}.log"
        if not baseline_file.exists():
            baseline_file = FIRSTPASS_DIR / benchmark / "c4_baseline0_i0.log"

        cache_file = CACHE_DIR / benchmark / f"c4_i8_rri_{CSM_TAG}.log"
        if not cache_file.exists():
            cache_file = CACHE_DIR / benchmark / "c4_i8_rri.log"

        prefetch_file = PREFETCH_DIR / benchmark / f"c4_i8_lru_mld0_{CSM_TAG}.log"
        if not prefetch_file.exists():
            prefetch_file = PREFETCH_DIR / benchmark / "c4_i8_lru_mld0.log"

        baseline = extract_metrics(baseline_file)
        cache = extract_metrics(cache_file)
        prefetch = extract_metrics(prefetch_file)

        row: dict[str, object] = {"benchmark": benchmark}
        for metric_name in METRIC_PATTERNS:
            row[f"baseline_{metric_name}"] = baseline[metric_name]
            row[f"cache_{metric_name}"] = cache[metric_name]
            row[f"prefetch_{metric_name}"] = prefetch[metric_name]

        rows.append(row)

    return rows


def write_csv(rows: list[dict[str, object]], output_file: Path) -> None:
    """Write rows to CSV with stable column ordering."""
    fieldnames = ["benchmark"]
    for metric_name in METRIC_PATTERNS:
        fieldnames.extend(
            [
                f"baseline_{metric_name}",
                f"cache_{metric_name}",
                f"prefetch_{metric_name}",
            ]
        )

    with output_file.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    global SIM_LOGS_BASE, FIRSTPASS_DIR, CACHE_DIR, PREFETCH_DIR, BENCHMARK_FILTER
    args = parse_args(__doc__)
    SIM_LOGS_BASE = args.log_dir / "simulate"
    FIRSTPASS_DIR = SIM_LOGS_BASE / "firstpass"
    CACHE_DIR = SIM_LOGS_BASE / "cache"
    PREFETCH_DIR = SIM_LOGS_BASE / "prefetch"
    BENCHMARK_FILTER = args.benchmarks
    rows = collect_rows()
    for filename in ("readiness_latency.csv", "verification_stalls_impact.csv"):
        output_file = args.output_dir / filename
        write_csv(rows, output_file)
        print(f"Wrote {len(rows)} rows to {output_file}")


if __name__ == "__main__":
    main()
