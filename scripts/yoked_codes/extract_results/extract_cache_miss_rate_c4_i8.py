#!/usr/bin/env python3
"""Extract c4/i8 cache-policy metrics from csm194 simulate and compile logs.

Reads logs from:
  build/yoked_codes_run_all_workloads/logs/simulate/cache/<benchmark>/
    build/yoked_codes_run_all_workloads/logs/compile/cache/<benchmark>/
    build/yoked_codes_run_all_workloads/logs/compile/prefetch/<benchmark>/

Expected files per benchmark:
    - c4_i8_lru_csm194.log
    - c4_i8_rri_csm194.log

Output CSV:
  results/cache_miss_rate_c4_i8.csv

Rows are benchmarks, columns include policy-specific metrics.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Optional

SIM_LOGS_BASE = (
    Path(__file__).resolve().parent.parent
    / "build"
    / "yoked_codes_run_all_workloads"
    / "logs"
    / "simulate"
)
CACHE_DIR = SIM_LOGS_BASE / "cache"
COMPILE_BASE = SIM_LOGS_BASE.parent / "compile"
COMPILE_CACHE_DIR = COMPILE_BASE / "cache"
COMPILE_PREFETCH_DIR = COMPILE_BASE / "prefetch"
OUT_CSV = Path(__file__).resolve().parent / "cache_miss_rate_c4_i8.csv"
CSM_TAG = "csm194"

MISS_RATE_RE = re.compile(
    r"^Miss rate \(%\) \[cold\+mplace / total\]\s+([0-9.eE+-]+)\s*$"
)
COMPILE_HIT_RATE_RE = re.compile(r"^RRI_1D_HIT_RATE\s+([0-9.eE+-]+)\s*$")
PREFETCH_EXPECTED_WAIT_RE = re.compile(
    r"^SP_PREFETCH_EXPECTED_WAIT_TIME\s+([0-9.eE+-]+)\s*$"
)
PREFETCH_COLD_MEMORY_ACCESSES_RE = re.compile(
    r"^SP_PREFETCH_COLD_MEMORY_ACCESSES\s+([0-9.eE+-]+)\s*$"
)
PREFETCH_CACHE_HITS_RE = re.compile(r"^SP_PREFETCH_CACHE_HITS\s+([0-9.eE+-]+)\s*$")
PREFETCH_PREFETCHES_INSERTED_RE = re.compile(
    r"^SP_PREFETCH_PREFETCHES_INSERTED\s+([0-9.eE+-]+)\s*$"
)


def parse_number(value: str) -> float | int:
    """Return an int when the value is integral, otherwise a float."""
    if re.fullmatch(r"[0-9]+", value):
        return int(value)
    return float(value)


def extract_first_match(log_path: Path, pattern: re.Pattern[str]) -> Optional[float | int]:
    """Extract the first matching numeric value from a log file."""
    if not log_path.exists():
        return None

    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for raw_line in handle:
                line = raw_line.rstrip("\n")
                match = pattern.match(line)
                if match:
                    return parse_number(match.group(1))
    except OSError:
        return None

    return None


def extract_miss_rate(log_path: Path) -> Optional[float]:
    """Extract miss-rate percentage from one log file."""
    if not log_path.exists():
        return None

    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for raw_line in handle:
                line = raw_line.rstrip("\n")
                match = MISS_RATE_RE.match(line)
                if match:
                    return float(match.group(1))
    except OSError:
        return None

    return None


def extract_compile_hit_rate(log_path: Path) -> Optional[float]:
    """Extract compile-time hit rate from cache compile log."""
    return extract_first_match(log_path, COMPILE_HIT_RATE_RE)


def extract_prefetch_expected_wait(log_path: Path) -> Optional[float]:
    """Extract SP prefetch expected wait time from prefetch compile log."""
    return extract_first_match(log_path, PREFETCH_EXPECTED_WAIT_RE)


def get_benchmarks() -> list[str]:
    """Get benchmark names from cache log subdirectories."""
    if not CACHE_DIR.exists():
        raise FileNotFoundError(f"Cache directory not found: {CACHE_DIR}")
    return sorted(p.name for p in CACHE_DIR.iterdir() if p.is_dir())


def collect_rows() -> list[dict[str, object]]:
    """Build one row per benchmark with policy miss-rate columns."""
    rows: list[dict[str, object]] = []

    for benchmark in get_benchmarks():
        # Simulate logs are tagged with the CSM suffix.
        lru_log = CACHE_DIR / benchmark / f"c4_i8_lru_{CSM_TAG}.log"
        rri_log = CACHE_DIR / benchmark / f"c4_i8_rri_{CSM_TAG}.log"

        # Compile logs are NOT tagged with the CSM suffix.
        compile_lru_log = COMPILE_CACHE_DIR / benchmark / "c4_i8_lru.log"
        compile_rri_log = COMPILE_CACHE_DIR / benchmark / "c4_i8_rri.log"
        compile_prefetch_lru_log = COMPILE_PREFETCH_DIR / benchmark / "c4_i8_lru_mld0.log"
        compile_prefetch_rri_log = COMPILE_PREFETCH_DIR / benchmark / "c4_i8_rri_mld0.log"

        rows.append(
            {
                "benchmark": benchmark,
                "lru_miss_rate": extract_miss_rate(lru_log),
                "rri_miss_rate": extract_miss_rate(rri_log),
                "lru_compile_hit_rate": extract_compile_hit_rate(compile_lru_log),
                "rri_compile_hit_rate": extract_compile_hit_rate(compile_rri_log),
                "lru_prefetch_cold_memory_accesses": extract_first_match(
                    compile_prefetch_lru_log, PREFETCH_COLD_MEMORY_ACCESSES_RE
                ),
                "rri_prefetch_cold_memory_accesses": extract_first_match(
                    compile_prefetch_rri_log, PREFETCH_COLD_MEMORY_ACCESSES_RE
                ),
                "lru_prefetch_cache_hits": extract_first_match(
                    compile_prefetch_lru_log, PREFETCH_CACHE_HITS_RE
                ),
                "rri_prefetch_cache_hits": extract_first_match(
                    compile_prefetch_rri_log, PREFETCH_CACHE_HITS_RE
                ),
                "lru_prefetch_prefetches_inserted": extract_first_match(
                    compile_prefetch_lru_log, PREFETCH_PREFETCHES_INSERTED_RE
                ),
                "rri_prefetch_prefetches_inserted": extract_first_match(
                    compile_prefetch_rri_log, PREFETCH_PREFETCHES_INSERTED_RE
                ),
                "lru_prefetch_expected_wait_time": extract_prefetch_expected_wait(
                    compile_prefetch_lru_log
                ),
                "rri_prefetch_expected_wait_time": extract_prefetch_expected_wait(
                    compile_prefetch_rri_log
                ),
            }
        )

    return rows


def write_csv(rows: list[dict[str, object]]) -> None:
    """Write rows to CSV with benchmark as row index and policy columns."""
    fieldnames = [
        "benchmark",
        "lru_miss_rate",
        "rri_miss_rate",
        "lru_compile_hit_rate",
        "rri_compile_hit_rate",
        "lru_prefetch_cold_memory_accesses",
        "rri_prefetch_cold_memory_accesses",
        "lru_prefetch_cache_hits",
        "rri_prefetch_cache_hits",
        "lru_prefetch_prefetches_inserted",
        "rri_prefetch_prefetches_inserted",
        "lru_prefetch_expected_wait_time",
        "rri_prefetch_expected_wait_time",
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
