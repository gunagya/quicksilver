#!/usr/bin/env python3
"""Find the largest logical error rate in yoked-code blocks across simulate logs.

Scans all .log files under:
  build/yoked_codes_run_all_workloads/logs/simulate

Looks for entries in the form:
  YOKED_* Error Stats:
  Per logical-qubit round error rate:<value>

Only considers files with "csm194" in the filename and ignores anything with "csd".

Prints the largest value found and its file location.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent
SIMULATE_DIR = ROOT / "build" / "yoked_codes_run_all_workloads" / "logs" / "simulate"

BLOCK_RE = re.compile(r"^(YOKED_[A-Z0-9_]+) Error Stats:\s*$")
LOGICAL_RATE_RE = re.compile(
    r"^Per logical-qubit round error rate\s*:\s*([0-9.eE+-]+)\s*$"
)


class MaxRecord:
    def __init__(self) -> None:
        self.value: Optional[float] = None
        self.file_path: Optional[Path] = None
        self.line_number: Optional[int] = None
        self.block_name: Optional[str] = None

    def update(self, value: float, file_path: Path, line_number: int, block_name: str) -> None:
        if self.value is None or value > self.value:
            self.value = value
            self.file_path = file_path
            self.line_number = line_number
            self.block_name = block_name


def scan_log_file(log_path: Path, max_record: MaxRecord) -> int:
    """Scan one log file and update max_record. Returns number of rates found."""
    found_in_file = 0
    current_block = "UNKNOWN_YOKED_BLOCK"

    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for i, raw_line in enumerate(handle, start=1):
                line = raw_line.rstrip("\n")

                block_match = BLOCK_RE.match(line)
                if block_match:
                    current_block = block_match.group(1)
                    continue

                rate_match = LOGICAL_RATE_RE.match(line)
                if rate_match:
                    value = float(rate_match.group(1))
                    max_record.update(value, log_path, i, current_block)
                    found_in_file += 1
    except OSError:
        return 0

    return found_in_file


def main() -> None:
    if not SIMULATE_DIR.exists():
        raise FileNotFoundError(f"Missing simulate directory: {SIMULATE_DIR}")

    max_record = MaxRecord()
    total_logs = 0
    total_rates = 0

    for log_path in sorted(SIMULATE_DIR.rglob("*.log")):
        if not log_path.is_file():
            continue

        name_lower = log_path.name.lower()
        if "csm194" not in name_lower:
            continue
        if "csd" in name_lower:
            continue

        total_logs += 1
        total_rates += scan_log_file(log_path, max_record)

    print(f"Scanned {total_logs} log files under {SIMULATE_DIR}")
    print(f"Found {total_rates} logical error-rate entries")

    if max_record.value is None:
        print("No logical error-rate entries found.")
        return

    print("Largest logical error rate found:")
    print(f"  value: {max_record.value:.6e}")
    print(f"  yoked_block: {max_record.block_name}")
    print(f"  file: {max_record.file_path}")
    print(f"  line: {max_record.line_number}")


if __name__ == "__main__":
    main()
