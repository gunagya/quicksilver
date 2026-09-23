#!/usr/bin/env python3
"""Extract IPC and non-Clifford readiness delay for firstpass csm194 logs.

This script scans:
  build/yoked_codes_run_all_workloads/logs/simulate/firstpass/*/*csm194*.log

Filtering:
  - includes files with "csm194" in the filename
  - excludes files with "csd" in the filename

Output:
  results/firstpass_csm194_ipc_nonclifford_delay.csv

One row is emitted per benchmark.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent
FIRSTPASS_DIR = ROOT / "build" / "yoked_codes_run_all_workloads" / "logs" / "simulate" / "firstpass"
OUT_CSV = Path(__file__).resolve().parent / "firstpass_csm194_ipc_nonclifford_delay.csv"

IPC_RE = re.compile(r"^IPC\s+([0-9.eE+-]+)\s*$")
NONCLIFFORD_DELAY_RE = re.compile(
    r"^Avg instruction delay due only to non-Clifford readiness \(cycles\)\s+([0-9.eE+-]+)\s*$"
)


def parse_metrics(log_path: Path) -> tuple[Optional[float], Optional[float]]:
    ipc: Optional[float] = None
    nonclifford_delay_cycles: Optional[float] = None

    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for raw_line in handle:
                line = raw_line.rstrip("\n")

                if ipc is None:
                    m_ipc = IPC_RE.match(line)
                    if m_ipc:
                        ipc = float(m_ipc.group(1))
                        continue

                if nonclifford_delay_cycles is None:
                    m_delay = NONCLIFFORD_DELAY_RE.match(line)
                    if m_delay:
                        nonclifford_delay_cycles = float(m_delay.group(1))
                        continue

                if ipc is not None and nonclifford_delay_cycles is not None:
                    break
    except OSError:
        return None, None

    return ipc, nonclifford_delay_cycles


def collect_rows() -> list[dict[str, object]]:
    if not FIRSTPASS_DIR.exists():
        raise FileNotFoundError(f"Missing firstpass directory: {FIRSTPASS_DIR}")

    rows: list[dict[str, object]] = []

    benchmark_dirs = sorted(p for p in FIRSTPASS_DIR.iterdir() if p.is_dir())
    for benchmark_dir in benchmark_dirs:
        benchmark = benchmark_dir.name

        matches = sorted(
            p for p in benchmark_dir.glob("*csm194*.log") if "csd" not in p.name.lower()
        )
        if not matches:
            continue

        # Use the first deterministic match when multiple csm194 logs exist.
        log_path = matches[0]
        ipc, nonclifford_delay_cycles = parse_metrics(log_path)

        rows.append(
            {
                "benchmark": benchmark,
                "ipc": ipc,
                "avg_instruction_delay_nonclifford_readiness_cycles": nonclifford_delay_cycles,
                "log_file": log_path.name,
            }
        )

    return rows


def write_csv(rows: list[dict[str, object]]) -> None:
    fieldnames = [
        "benchmark",
        "ipc",
        "avg_instruction_delay_nonclifford_readiness_cycles",
        "log_file",
    ]

    with OUT_CSV.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    rows = collect_rows()
    write_csv(rows)
    print(f"Wrote {len(rows)} rows to {OUT_CSV}")


if __name__ == "__main__":
    main()
