#!/usr/bin/env python3
"""Extract logical-qubit round error rates and runtime stats for csm194 runs.

This script collects one row per benchmark and parses three simulation components:
- firstpass: c4_baseline0_i0_csm194.log (fallback: c4_baseline_i0_csm194.log)
- cache: c4_i8_rri_csm194.log
- prefetch: c4_i8_lru_mld0_csm194.log
- baseline1 cycles source: c4_baseline1_i0_csm194.log

For each component, it extracts:
- 1D storage logical-qubit round error rate
- Sum of COLD storage logical-qubit round error rates
- TOTAL_SIMULATION_CYCLES
- UNROLLED_INSTRUCTIONS_DONE

Additionally, it extracts baseline1 TOTAL_SIMULATION_CYCLES.

It also adds scaled columns:
- 8 * 1D storage error rate
- 194 * sum cold storage error rate

Files containing "csd" are ignored by exact filename selection.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]
SIM_BASE = ROOT / "build" / "yoked_codes_run_all_workloads" / "logs" / "simulate"
FIRSTPASS_DIR = SIM_BASE / "firstpass"
CACHE_DIR = SIM_BASE / "cache"
PREFETCH_DIR = SIM_BASE / "prefetch"

OUT_CSV = ROOT / "results" / "logical_qubit_round_error_rates_csm194.csv"

TOTAL_CYCLES_RE = re.compile(r"^TOTAL_SIMULATION_CYCLES\s+([0-9]+)\s*$")
UNROLLED_INSTR_DONE_RE = re.compile(r"^UNROLLED_INSTRUCTIONS_DONE\s+([0-9]+)\s*$")
ERROR_RATE_RE = re.compile(r"Per logical-qubit round error rate:\s*([0-9.eE+-]+)")

SECTION_1D = "1d"
SECTION_COLD = "cold"


def parse_log(log_path: Path) -> dict[str, Optional[float | int]]:
    """Parse one simulation log and return metrics.

    Missing metrics are returned as None, except summed cold errors default to 0.0 when
    at least one file is parsed and no cold section appears.
    """
    result: dict[str, Optional[float | int]] = {
        "storage_1d_error_rate": None,
        "cold_storage_error_rate_sum": 0.0,
        "total_simulation_cycles": None,
        "unrolled_instructions_done": None,
    }

    if not log_path.exists():
        return result

    current_section: Optional[str] = None
    storage_1d_sum = 0.0
    storage_1d_seen = False
    cold_sum = 0.0

    with log_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\n")
            stripped = line.strip()

            m_cycles = TOTAL_CYCLES_RE.match(stripped)
            if m_cycles:
                result["total_simulation_cycles"] = int(m_cycles.group(1))
                continue

            m_instr = UNROLLED_INSTR_DONE_RE.match(stripped)
            if m_instr:
                result["unrolled_instructions_done"] = int(m_instr.group(1))
                continue

            if stripped.startswith("YOKED_1D_STORAGE Error Stats:"):
                current_section = SECTION_1D
                continue

            if stripped.startswith("YOKED_COLD_STORAGE Error Stats:"):
                current_section = SECTION_COLD
                continue

            m_err = ERROR_RATE_RE.search(stripped)
            if not m_err:
                continue

            value = float(m_err.group(1))
            if current_section == SECTION_1D:
                storage_1d_sum += value
                storage_1d_seen = True
            elif current_section == SECTION_COLD:
                cold_sum += value

    result["storage_1d_error_rate"] = storage_1d_sum if storage_1d_seen else None
    result["cold_storage_error_rate_sum"] = cold_sum
    return result


def pick_log(base_dir: Path, benchmark: str, filename: str, fallback: Optional[str] = None) -> Optional[Path]:
    """Return expected log path if present, optionally trying fallback filename."""
    path = base_dir / benchmark / filename
    if path.exists() and "csd" not in path.name.lower():
        return path

    if fallback is not None:
        fallback_path = base_dir / benchmark / fallback
        if fallback_path.exists() and "csd" not in fallback_path.name.lower():
            return fallback_path

    return None


def component_values(component_metrics: dict[str, Optional[float | int]]) -> dict[str, Optional[float | int]]:
    """Attach scaled error columns for one component metrics object."""
    one_d = component_metrics["storage_1d_error_rate"]
    cold = component_metrics["cold_storage_error_rate_sum"]

    scaled_8x_1d = (8.0 * float(one_d)) if one_d is not None else None
    scaled_194x_cold = (194.0 * float(cold)) if cold is not None else None

    out = dict(component_metrics)
    out["scaled_8x_1d_storage_error"] = scaled_8x_1d
    out["scaled_194x_cold_storage_error_sum"] = scaled_194x_cold
    return out


def get_benchmarks() -> list[str]:
    """Collect benchmark names from directory union across firstpass/cache/prefetch."""
    names: set[str] = set()
    for d in (FIRSTPASS_DIR, CACHE_DIR, PREFETCH_DIR):
        if not d.exists():
            continue
        for p in d.iterdir():
            if p.is_dir():
                names.add(p.name)
    return sorted(names)


def collect_rows() -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []

    for benchmark in get_benchmarks():
        firstpass_log = pick_log(
            FIRSTPASS_DIR,
            benchmark,
            "c4_baseline0_i0_csm194.log",
            fallback="c4_baseline_i0_csm194.log",
        )
        cache_log = pick_log(CACHE_DIR, benchmark, "c4_i8_rri_csm194.log")
        prefetch_log = pick_log(PREFETCH_DIR, benchmark, "c4_i8_lru_mld0_csm194.log")
        baseline1_log = pick_log(
            FIRSTPASS_DIR,
            benchmark,
            "c4_baseline1_i0_csm194.log",
            fallback="c4_baseline_i1_csm194.log",
        )

        firstpass_metrics = component_values(parse_log(firstpass_log)) if firstpass_log else component_values(parse_log(Path("/nonexistent")))
        cache_metrics = component_values(parse_log(cache_log)) if cache_log else component_values(parse_log(Path("/nonexistent")))
        prefetch_metrics = component_values(parse_log(prefetch_log)) if prefetch_log else component_values(parse_log(Path("/nonexistent")))
        baseline1_cycles = parse_log(baseline1_log)["total_simulation_cycles"] if baseline1_log else None

        row: dict[str, object] = {"benchmark": benchmark}

        for prefix, metrics in (
            ("firstpass", firstpass_metrics),
            ("cache", cache_metrics),
            ("prefetch", prefetch_metrics),
        ):
            row[f"{prefix}_storage_1d_error_rate"] = metrics["storage_1d_error_rate"]
            row[f"{prefix}_cold_storage_error_rate_sum"] = metrics["cold_storage_error_rate_sum"]
            row[f"{prefix}_total_simulation_cycles"] = metrics["total_simulation_cycles"]
            row[f"{prefix}_unrolled_instructions_done"] = metrics["unrolled_instructions_done"]
            row[f"{prefix}_8x_storage_1d_error"] = metrics["scaled_8x_1d_storage_error"]
            row[f"{prefix}_194x_cold_storage_error_sum"] = metrics["scaled_194x_cold_storage_error_sum"]

        row["baseline1_total_simulation_cycles"] = baseline1_cycles

        rows.append(row)

    return rows


def write_csv(rows: list[dict[str, object]]) -> None:
    fieldnames = [
        "benchmark",
        "firstpass_storage_1d_error_rate",
        "firstpass_cold_storage_error_rate_sum",
        "firstpass_total_simulation_cycles",
        "firstpass_unrolled_instructions_done",
        "firstpass_8x_storage_1d_error",
        "firstpass_194x_cold_storage_error_sum",
        "cache_storage_1d_error_rate",
        "cache_cold_storage_error_rate_sum",
        "cache_total_simulation_cycles",
        "cache_unrolled_instructions_done",
        "cache_8x_storage_1d_error",
        "cache_194x_cold_storage_error_sum",
        "prefetch_storage_1d_error_rate",
        "prefetch_cold_storage_error_rate_sum",
        "prefetch_total_simulation_cycles",
        "prefetch_unrolled_instructions_done",
        "prefetch_8x_storage_1d_error",
        "prefetch_194x_cold_storage_error_sum",
        "baseline1_total_simulation_cycles",
    ]

    with OUT_CSV.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    if not SIM_BASE.exists():
        raise FileNotFoundError(f"Missing simulation logs directory: {SIM_BASE}")

    rows = collect_rows()
    write_csv(rows)
    print(f"Wrote {len(rows)} rows to {OUT_CSV}")


if __name__ == "__main__":
    main()
