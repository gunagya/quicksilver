"""Portable command-line options shared by the paper-data extractors."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_LOG_DIR = REPO_ROOT / "build" / "yoked_codes_run_all_workloads" / "logs"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "results"


def parse_args(description: str) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "--log-dir", type=Path, default=DEFAULT_LOG_DIR,
        help="Log root containing compile/ and simulate/ (default: repository build logs).",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR,
        help="Destination for notebook-ready CSV files (default: repository results/).",
    )
    parser.add_argument(
        "--benchmarks", nargs="+", metavar="LABEL",
        help="Extract only these benchmark directory names; default: all discovered benchmarks.",
    )
    args = parser.parse_args()
    args.log_dir = args.log_dir.expanduser().resolve()
    args.output_dir = args.output_dir.expanduser().resolve()
    if not args.log_dir.is_dir():
        parser.error(f"Log directory does not exist: {args.log_dir}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    return args


def select_benchmarks(names: Iterable[str], selected: list[str] | None) -> list[str]:
    """Keep selected labels explicit so missing logs produce visible empty values."""
    return sorted(set(selected if selected is not None else names))
