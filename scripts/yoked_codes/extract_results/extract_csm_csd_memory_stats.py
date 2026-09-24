#!/usr/bin/env python3
"""Extract IPC and memory sizing stats from csm simulation logs.

Scans build/yoked_codes_run_all_workloads/logs/simulate for a restricted set of
runs (all must be csm-tagged and must NOT include "csd" in the filename):
- firstpass baseline: c4_baseline0_i0_csm194.log
- cache policy:       c4_i8_rri_csmX.log
- prefetch policy:    c4_i8_lru_mld0_csmX.log

X is restricted to the paper block sizes 34, 98, 194, 322, and 482.

For each matching log, extracts:
- IPC
- MEMORY_PHYSICAL_QUBITS
- Sum logical qubits in memory, computed as:
    intermediate_capacity_i + (num_2d_blocks * csm)
  where num_2d_blocks is inferred from the number of
  "YOKED_COLD_STORAGE Error Stats:" sections in the log.

Writes one row per simulation log. Benchmarks are represented by a dedicated
"benchmark" column and rows are sorted by benchmark/config.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Optional
from extraction_common import DEFAULT_LOG_DIR, DEFAULT_OUTPUT_DIR, parse_args

SIM_DIR = DEFAULT_LOG_DIR / "simulate"
OUT_CSV = DEFAULT_OUTPUT_DIR / "sensitivity_block_size.csv"
BENCHMARK_FILTER = None
PAPER_BLOCK_SIZES = {34, 98, 194, 322, 482}

BASELINE_RE = re.compile(r"^c4_baseline0_i0_csm(?P<csm>\d+)\.log$")
CACHE_RRI_RE = re.compile(r"^c4_i8_rri_csm(?P<csm>\d+)\.log$")
PREFETCH_LRU_RE = re.compile(r"^c4_i8_lru_mld0_csm(?P<csm>\d+)\.log$")

I_RE = re.compile(r"_i(?P<i>\d+)(?:_|\.)")
IPC_RE = re.compile(r"^IPC\s+([0-9.eE+-]+)\s*$")
MEM_PHYS_RE = re.compile(r"^MEMORY_PHYSICAL_QUBITS\s+([0-9]+)\s*$")


def extract_i_from_filename(filename: str) -> Optional[int]:
    match = I_RE.search(filename)
    if not match:
        return None
    return int(match.group("i"))


def extract_ipc_and_memory_physical(log_path: Path) -> tuple[Optional[float], Optional[int], int]:
    ipc: Optional[float] = None
    memory_physical_qubits: Optional[int] = None
    num_2d_blocks = 0

    with log_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\n")

            if line == "YOKED_COLD_STORAGE Error Stats:":
                num_2d_blocks += 1
                continue

            if ipc is None:
                ipc_match = IPC_RE.match(line)
                if ipc_match:
                    ipc = float(ipc_match.group(1))
                    continue

            if memory_physical_qubits is None:
                mem_phys_match = MEM_PHYS_RE.match(line)
                if mem_phys_match:
                    memory_physical_qubits = int(mem_phys_match.group(1))

    return ipc, memory_physical_qubits, num_2d_blocks


def collect_rows() -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []

    for log_path in sorted(SIM_DIR.rglob("*.log")):
        rel = log_path.relative_to(SIM_DIR)
        rel_str = str(rel)

        name_lower = log_path.name.lower()
        if "csd" in name_lower:
            continue

        if len(rel.parts) < 3:
            continue

        simulation_mode = rel.parts[0]   # firstpass/cache/prefetch
        benchmark = rel.parts[1]
        if BENCHMARK_FILTER is not None and benchmark not in BENCHMARK_FILTER:
            continue

        # Restrict to c4 baseline i0, c4_i8 cache rri, and c4_i8 prefetch lru.
        csm_match: Optional[re.Match[str]] = None
        if simulation_mode == "firstpass":
            csm_match = BASELINE_RE.match(log_path.name)
        elif simulation_mode == "cache":
            csm_match = CACHE_RRI_RE.match(log_path.name)
        elif simulation_mode == "prefetch":
            csm_match = PREFETCH_LRU_RE.match(log_path.name)

        if not csm_match:
            continue

        csm = int(csm_match.group("csm"))
        if csm not in PAPER_BLOCK_SIZES or (simulation_mode == "firstpass" and csm != 194):
            continue
        csd: Optional[int] = None
        intermediate_capacity_i = extract_i_from_filename(log_path.name)

        ipc, memory_physical_qubits, num_2d_blocks = extract_ipc_and_memory_physical(log_path)

        sum_logical_qubits_in_memory: Optional[int]
        if intermediate_capacity_i is None:
            sum_logical_qubits_in_memory = None
        else:
            sum_logical_qubits_in_memory = intermediate_capacity_i + (num_2d_blocks * csm)

        rows.append(
            {
                "benchmark": benchmark,
                "simulation_mode": simulation_mode,
                "log_file": rel_str,
                "intermediate_capacity_i": intermediate_capacity_i,
                "csm": csm,
                "csd": csd,
                "num_2d_blocks": num_2d_blocks,
                "sum_logical_qubits_in_memory": sum_logical_qubits_in_memory,
                "memory_physical_qubits": memory_physical_qubits,
                "ipc": ipc,
            }
        )

    rows.sort(
        key=lambda r: (
            str(r["benchmark"]),
            str(r["simulation_mode"]),
            int(r["intermediate_capacity_i"]) if r["intermediate_capacity_i"] is not None else -1,
            int(r["csm"]),
            int(r["csd"]) if r["csd"] is not None else -1,
            str(r["log_file"]),
        )
    )
    return rows


def write_csv(rows: list[dict[str, object]]) -> None:
    fieldnames = [
        "benchmark",
        "simulation_mode",
        "log_file",
        "intermediate_capacity_i",
        "csm",
        "csd",
        "num_2d_blocks",
        "sum_logical_qubits_in_memory",
        "memory_physical_qubits",
        "ipc",
    ]

    with OUT_CSV.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    global SIM_DIR, OUT_CSV, BENCHMARK_FILTER
    args = parse_args(__doc__)
    SIM_DIR = args.log_dir / "simulate"
    OUT_CSV = args.output_dir / "sensitivity_block_size.csv"
    BENCHMARK_FILTER = args.benchmarks
    rows = collect_rows()
    write_csv(rows)
    print(f"Wrote {len(rows)} rows to {OUT_CSV}")


if __name__ == "__main__":
    main()
