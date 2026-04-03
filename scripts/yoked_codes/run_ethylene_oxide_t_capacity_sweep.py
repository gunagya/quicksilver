#!/usr/bin/env python3
"""
Compile and simulate the first 200,000 instructions of ethylene_oxide_t
across a small compute-capacity sweep, then store memory-instruction %
and IPC in a CSV.

Capacities swept:
  - 4
  - 8
  - 12
  - 16
  - program qubit count
"""

from __future__ import annotations

import csv
import re
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build"
RAW_BENCHMARK = REPO_ROOT / "benchmarks" / "bin" / "BQ_c2h4o_ethylene_oxide_t.xz"
OUTPUT_DIR = REPO_ROOT / "benchmarks" / "bin" / "mem" / "firstpass" / "ethylene_oxide_t"
CSV_PATH = REPO_ROOT / "build" / "ethylene_oxide_t_capacity_sweep.csv"

COMPILE_LIMIT = 2_000_000
SIM_INSTRUCTIONS = 1_000_000
FACTORY_BUDGET = 50_000


def run(cmd: list[str], desc: str) -> str:
    print(f"\n>>> {desc}")
    print("    " + " ".join(str(x) for x in cmd))
    result = subprocess.run(
        cmd,
        cwd=BUILD_DIR,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def stat(output: str, key: str) -> str | None:
    match = re.search(rf"{re.escape(key)}\s*[=:\|]?\s*([0-9.eE+\-]+)", output)
    return match.group(1) if match else None


def fstat(output: str, key: str) -> float | None:
    value = stat(output, key)
    return float(value) if value is not None else None


def istat(output: str, key: str) -> int | None:
    value = stat(output, key)
    return int(float(value)) if value is not None else None


def compiled_binary(capacity: int) -> Path:
    return OUTPUT_DIR / f"ethylene_oxide_t_c{capacity}_limit{COMPILE_LIMIT}.bin"


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    raw_report = run(
        ["./qs_report", str(RAW_BENCHMARK), "-i", "1"],
        "Read program qubit count from raw benchmark",
    )
    program_qubits = istat(raw_report, "PROGRAM_QUBITS")
    if program_qubits is None:
        raise RuntimeError("Could not determine program qubit count from qs_report output")

    capacities = []
    for capacity in [4, 8, 12, 16, program_qubits]:
        if capacity not in capacities:
            capacities.append(capacity)

    rows: list[dict[str, object]] = []

    for capacity in capacities:
        output_bin = compiled_binary(capacity)

        compile_out = run(
            [
                "./qs_memory_scheduler",
                str(RAW_BENCHMARK),
                str(output_bin),
                "-s",
                "0",
                "-c",
                str(capacity),
                "-i",
                str(COMPILE_LIMIT),
                "-pp",
                "0",
            ],
            f"Compile ethylene_oxide_t with compute capacity {capacity}",
        )

        report_out = run(
            ["./qs_report", str(output_bin)],
            f"Report compiled binary stats for compute capacity {capacity}",
        )
        memory_instruction_percentage = fstat(report_out, "MEMORY_INSTRUCTION_PERCENTAGE")
        memory_accesses = istat(compile_out, "MEMORY_ACCESSES")

        sim_out = run(
            [
                "./yoked_simulator",
                str(output_bin),
                str(SIM_INSTRUCTIONS),
                "-a",
                str(capacity),
                "-i",
                "0",
                "-f",
                str(FACTORY_BUDGET),
                "--baseline", "1",
                "-pp",
                "0",
            ],
            f"Simulate compiled binary with compute capacity {capacity}",
        )
        ipc = fstat(sim_out, "IPC")

        rows.append(
            {
                "compute_capacity": capacity,
                "program_qubits": program_qubits,
                "compile_limit": COMPILE_LIMIT,
                "sim_instructions": SIM_INSTRUCTIONS,
                "memory_accesses": memory_accesses,
                "memory_instruction_percentage": memory_instruction_percentage,
                "ipc": ipc,
                "compiled_binary": str(output_bin),
            }
        )

    with CSV_PATH.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(
            csv_file,
            fieldnames=[
                "compute_capacity",
                "program_qubits",
                "compile_limit",
                "sim_instructions",
                "memory_accesses",
                "memory_instruction_percentage",
                "ipc",
                "compiled_binary",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    print(f"\nWrote {CSV_PATH}")


if __name__ == "__main__":
    main()
