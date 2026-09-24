#!/usr/bin/env python3
"""Compile, simulate, extract, and optionally plot the paper's exact experiment set."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

from scripts.yoked_codes import run_all_workloads as workloads

ROOT = Path(__file__).resolve().parent
EXTRACTOR_DIR = ROOT / "scripts" / "yoked_codes" / "extract_results"
EXTRACTORS = {
    "extract_ipc_c4_i8.py": ("main_results.csv",),
    "extract_nonclifford_readiness_stats_c4_i8.py": (
        "readiness_latency.csv", "verification_stalls_impact.csv",
    ),
    "extract_csm_csd_memory_stats.py": ("sensitivity_block_size.csv",),
    "extract_ipc_matrix_c4.py": ("sensitivity_buffer_capacity.csv",),
    "extract_ipc_matrix_i8_by_c.py": ("sensitivity_compute.csv",),
    "extract_cache_miss_rate_c4_i8.py": ("cache_prefetch_policies.csv",),
    "extract_prefetch_percentage.py": ("prefetch_percentage.csv",),
    "extract_logical_qubit_round_error_rates_csm194.py": (
        "logical_qubit_round_error_rates_csm194.csv",
    ),
}


def positive_int(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--extract-only", action="store_true", help="read existing logs; do not compile/simulate")
    modes.add_argument("--run-only", action="store_true", help="compile/simulate without extracting CSVs")
    parser.add_argument("--instructions", type=positive_int, help="short simulation limit; compilation adds lookahead headroom; isolated outputs by default")
    parser.add_argument("--benchmarks", nargs="+", choices=[b.label for b in workloads.BENCHMARKS], help="default: all ten paper benchmarks")
    parser.add_argument("--jobs", type=positive_int, default=1, help="parallel benchmarks (default: 1)")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build", help="directory containing built executables")
    parser.add_argument("--binary-dir", type=Path, default=ROOT / "benchmarks/bin", help="directory containing raw .xz inputs")
    parser.add_argument("--run-dir", type=Path, help="compiled traces and logs; default build/yoked_codes_run_all_workloads")
    parser.add_argument("--log-dir", type=Path, help="existing logs root containing compile/ and simulate/ (--extract-only)")
    parser.add_argument("--results-dir", type=Path, help="CSV output directory (default: results/)")
    parser.add_argument("--dry-run", action="store_true", help="print the exact commands without running or writing anything")
    parser.add_argument("--plots", action="store_true", help="execute the paper notebook after extraction")
    parser.add_argument("--plot-dir", type=Path, help="PDF destination (default: results directory)")
    parser.add_argument("--plot-timeout", type=positive_int, default=600, help="timeout per notebook cell in seconds")
    args = parser.parse_args(argv)
    if args.log_dir is not None and not args.extract_only:
        parser.error("--log-dir requires --extract-only; use --run-dir for new runs")
    if args.run_only and args.plots:
        parser.error("--plots requires extraction; omit --run-only")
    args.build_dir = args.build_dir.resolve()
    args.binary_dir = args.binary_dir.resolve()
    smoke_name = f"artifact_smoke_{args.instructions}"
    args.run_dir = (args.run_dir or args.build_dir / (smoke_name if args.instructions else "yoked_codes_run_all_workloads")).resolve()
    args.log_dir = (args.log_dir or args.run_dir / "logs").resolve()
    default_results = ROOT / "results"
    if args.instructions:
        default_results /= f"smoke_{args.instructions}"
    args.results_dir = (args.results_dir or default_results).resolve()
    args.plot_dir = (args.plot_dir or args.results_dir).resolve()
    selected = set(args.benchmarks or [b.label for b in workloads.BENCHMARKS])
    args.benchmarks = [b for b in workloads.BENCHMARKS if b.label in selected]
    return args


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, default=str) + "\n")
    temporary.replace(path)


def check_prerequisites(args):
    missing = [args.binary_dir / b.raw_binary for b in args.benchmarks if not (args.binary_dir / b.raw_binary).is_file()]
    missing += [args.build_dir / name for name in ("qs_memory_scheduler", "yoked_simulator")
                if not os.access(args.build_dir / name, os.X_OK)]
    if missing:
        raise RuntimeError("Missing workload inputs or executables; see README.md:\n" + "\n".join(map(str, missing)))


def execute_plan(plan, args):
    check_prerequisites(args)
    manifest_path = args.run_dir / "artifact_manifest.json"
    manifest = {
        "started_at": datetime.now(timezone.utc).isoformat(),
        "status": "running", "instruction_limit": args.instructions,
        "benchmarks": [asdict(b) for b in args.benchmarks],
        "steps": [asdict(step) for step in plan],
    }
    write_json(manifest_path, manifest)
    stop = threading.Event()

    def run_benchmark(benchmark):
        steps = [step for step in plan if step.benchmark == benchmark.label]
        for index, step in enumerate(steps, 1):
            if stop.is_set():
                return
            step.log_path.parent.mkdir(parents=True, exist_ok=True)
            if step.output_path is not None:
                step.output_path.parent.mkdir(parents=True, exist_ok=True)
            pending_log = step.log_path.with_suffix(".log.partial")
            print(f"[{benchmark.label} {index}/{len(steps)}] {step.stage}: {step.log_path.name}", flush=True)
            with pending_log.open("w") as handle:
                result = subprocess.run(step.command, cwd=args.build_dir, stdout=handle, stderr=subprocess.STDOUT)
            if result.returncode:
                stop.set()
                raise RuntimeError(f"Command failed (exit {result.returncode}); inspect {pending_log}\n{shlex.join(step.command)}")
            if step.output_path is not None and not step.output_path.is_file():
                stop.set()
                raise RuntimeError(f"Compiler did not produce {step.output_path}; inspect {pending_log}")
            pending_log.replace(step.log_path)

    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            futures = [executor.submit(run_benchmark, b) for b in args.benchmarks]
            for future in as_completed(futures):
                try:
                    future.result()
                except BaseException:
                    stop.set()
                    for pending in futures:
                        pending.cancel()
                    raise
    except BaseException:
        manifest["status"] = "failed"
        write_json(manifest_path, manifest)
        raise
    manifest["status"] = "complete"
    manifest["finished_at"] = datetime.now(timezone.utc).isoformat()
    write_json(manifest_path, manifest)


def validate_logs(plan, args):
    """Require every plotted simulation and the compile counters used for coverage."""
    manifest_path = args.log_dir.parent / "artifact_manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        if manifest.get("status") != "complete":
            raise RuntimeError(f"The recorded run is {manifest.get('status')}: {manifest_path}; finish the run before extracting")
    required = [args.log_dir / step.log_path.relative_to(args.run_dir / "logs")
                for step in plan if step.stage == "simulate"]
    required += [args.log_dir / "compile/prefetch" / b.label / "c4_i8_lru_mld0.log" for b in args.benchmarks]
    missing = [path for path in required if not path.is_file() or path.stat().st_size == 0]
    if missing:
        raise RuntimeError(f"Missing {len(missing)} required logs; no CSVs were replaced:\n" + "\n".join(map(str, missing)))
    for path in required:
        if path.with_suffix(".log.partial").exists():
            raise RuntimeError(f"An unfinished replacement exists for {path}; finish that run before extracting")
        if "simulate" not in path.relative_to(args.log_dir).parts:
            continue
        text = path.read_text()
        for metric in ("IPC", "TOTAL_SIMULATION_CYCLES", "UNROLLED_INSTRUCTIONS_DONE"):
            match = re.search(rf"^{metric}\s+([0-9.eE+-]+)\s*$", text, re.MULTILINE)
            if match is None or not math.isfinite(float(match[1])) or float(match[1]) <= 0:
                raise RuntimeError(f"Incomplete simulation log {path}: missing/invalid {metric}")


def read_csv(path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def validate_csvs(directory, labels, short_run=False):
    """Reject missing plotted observations before publishing any replacement CSVs.

    Short samples can have no measured error rates or readiness delays. Such
    missing values may be shown as N/A, but present values must always be finite.
    """
    expected = set(labels)
    tables = {}

    def number(filename, row, column, *, positive=False, percent=False, optional=False):
        raw = row.get(column)
        identity = row.get("benchmark", f"{row.get('policy')} at {row.get('compute_capacity', row.get('intermediate_capacity_i'))}")
        if raw in (None, "") and optional:
            return None
        try:
            value = float(raw)
        except (TypeError, ValueError):
            raise RuntimeError(f"{filename}: missing/invalid {column} for {identity}: {raw!r}") from None
        if not math.isfinite(value) or value < 0 or (positive and value == 0) or (percent and value > 100):
            raise RuntimeError(f"{filename}: invalid {column} for {identity}: {raw!r}")
        return value

    for filename in (name for names in EXTRACTORS.values() for name in names):
        rows = read_csv(directory / filename)
        tables[filename] = rows
        if not rows:
            raise RuntimeError(f"Extractor produced no rows in {filename}")
        if "benchmark" in rows[0]:
            found = {row["benchmark"] for row in rows}
            if found != expected:
                raise RuntimeError(f"{filename}: benchmark mismatch (expected {sorted(expected)}, found {sorted(found)})")
            if filename != "sensitivity_block_size.csv" and len(rows) != len(expected):
                raise RuntimeError(f"{filename}: duplicate benchmark rows")
        else:
            compute = filename == "sensitivity_compute.csv"
            dimension = "compute_capacity" if compute else "intermediate_capacity_i"
            if set(rows[0]) - {dimension, "policy"} != expected:
                raise RuntimeError(f"{filename}: missing or extra benchmark columns")
            configurations = (
                {(str(c), policy) for c in (4, 8, 12, 16)
                 for policy in ("baseline", "ideal_memory", "cache_rri", "prefetch_lru")}
                if compute else
                {("0", "baseline"), ("0", "ideal")} |
                {(str(i), policy) for i in (4, 8, 16, 24) for policy in ("cache_rri", "prefetch_lru")}
            )
            found = {(row.get(dimension), row.get("policy")) for row in rows}
            if found != configurations or len(rows) != len(configurations):
                raise RuntimeError(f"{filename}: missing, extra, or duplicate paper configurations")
            for row in rows:
                for label in labels:
                    number(filename, row, label, positive=True)

    filename = "sensitivity_block_size.csv"
    rows = tables[filename]
    configurations = {(label, "firstpass", "194") for label in labels} | {
        (label, mode, str(csm)) for label in labels for mode in ("cache", "prefetch")
        for csm in (34, 98, 194, 322, 482)
    }
    found = {(row.get("benchmark"), row.get("simulation_mode"), row.get("csm")) for row in rows}
    if found != configurations or len(rows) != len(configurations):
        raise RuntimeError(f"{filename}: missing, extra, or duplicate paper configurations")
    for row in rows:
        number(filename, row, "ipc", positive=True)

    for row in tables["main_results.csv"]:
        for column in ("baseline_ipc", "ideal_ipc", "cache_lru_ipc", "cache_rri_ipc", "prefetch_lru_ipc"):
            number("main_results.csv", row, column, positive=True)
    for row in tables["cache_prefetch_policies.csv"]:
        for column in ("lru_miss_rate", "rri_miss_rate"):
            number("cache_prefetch_policies.csv", row, column, percent=True, optional=short_run)
    for row in tables["prefetch_percentage.csv"]:
        number("prefetch_percentage.csv", row, "prefetch_percentage", percent=True)

    for filename in ("readiness_latency.csv", "verification_stalls_impact.csv"):
        for row in tables[filename]:
            for prefix in ("baseline", "cache", "prefetch"):
                number(filename, row, f"{prefix}_cycles_stalled_non_clifford_readiness_percent", percent=True)
            for prefix in ("cache", "prefetch"):
                cold = number(filename, row, f"{prefix}_loads_2d_cold_reached_non_clifford_ready")
                waited = number(filename, row, f"{prefix}_loads_1d_waited_for_verification")
                verified = number(filename, row, f"{prefix}_loads_1d_already_verified")
                if not short_run and cold + waited + verified == 0:
                    raise RuntimeError(f"{filename}: no {prefix} readiness observations for {row['benchmark']}")
                number(filename, row, f"{prefix}_avg_delay_2d_cold_non_clifford_ready_cycles",
                       optional=short_run or cold == 0)
                number(filename, row, f"{prefix}_avg_delay_1d_non_clifford_ready_cycles",
                       optional=short_run or waited == 0)

    filename = "logical_qubit_round_error_rates_csm194.csv"
    for row in tables[filename]:
        for prefix in ("firstpass", "cache", "prefetch"):
            for metric in ("total_simulation_cycles", "unrolled_instructions_done"):
                number(filename, row, f"{prefix}_{metric}", positive=True)
            number(filename, row, f"{prefix}_cold_storage_error_rate_sum", optional=short_run)
            if prefix != "firstpass":
                number(filename, row, f"{prefix}_storage_1d_error_rate", optional=short_run)


def extractor_command(script, args, output_dir):
    return [sys.executable, str(EXTRACTOR_DIR / script), "--log-dir", str(args.log_dir),
            "--output-dir", str(output_dir), "--benchmarks", *[b.label for b in args.benchmarks]]


def extract_results(plan, args):
    validate_logs(plan, args)
    source_manifest_path = args.log_dir.parent / "artifact_manifest.json"
    source_manifest = json.loads(source_manifest_path.read_text()) if source_manifest_path.exists() else None
    args.results_dir.mkdir(parents=True, exist_ok=True)
    # Publish only after all extractors and completeness checks succeed.
    with tempfile.TemporaryDirectory(prefix=".artifact-extract-", dir=args.results_dir) as staging:
        staging_dir = Path(staging)
        for script in EXTRACTORS:
            subprocess.run(extractor_command(script, args, staging_dir), cwd=ROOT, check=True)
        validate_csvs(
            staging_dir, [b.label for b in args.benchmarks],
            short_run=bool(source_manifest.get("instruction_limit")) if source_manifest is not None else bool(args.instructions),
        )
        for filenames in EXTRACTORS.values():
            for filename in filenames:
                (staging_dir / filename).replace(args.results_dir / filename)
    write_json(args.results_dir / "artifact_results.json", {
        "extracted_at": datetime.now(timezone.utc).isoformat(),
        "log_dir": args.log_dir,
        "benchmarks": [b.label for b in args.benchmarks],
        "requested_instruction_limit": args.instructions,
        "source_run_instruction_limit": source_manifest.get("instruction_limit") if source_manifest else "unknown (legacy logs)",
        "csv_files": [name for names in EXTRACTORS.values() for name in names],
        "static_inputs": [ROOT / "results/application_fidelity.csv.numbers", ROOT / "results/space_overhead.numbers"],
    })


def render_plots(args):
    try:
        import nbformat
        from nbclient import NotebookClient
        from jupyter_client import KernelManager
    except ImportError as exc:
        raise RuntimeError("Plotting needs requirements-artifact.txt: python3 -m pip install -r requirements-artifact.txt") from exc
    notebook = nbformat.read(ROOT / "results/paper_plots.ipynb", as_version=4)
    args.plot_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, QUICKSILVER_REPO_ROOT=str(ROOT),
               QUICKSILVER_RESULTS_DIR=str(args.results_dir), QUICKSILVER_PLOT_DIR=str(args.plot_dir),
               MPLBACKEND="Agg")
    manager = KernelManager(kernel_name="python3")
    # Use this interpreter's dependencies, even if a global kernel is registered.
    manager.kernel_spec.argv = [sys.executable, "-m", "ipykernel_launcher", "-f", "{connection_file}"]
    client = NotebookClient(notebook, km=manager, timeout=args.plot_timeout,
                            resources={"metadata": {"path": str(ROOT)}})
    try:
        client.execute(cwd=str(ROOT), env=env)
    finally:
        if manager.has_kernel:
            manager.shutdown_kernel(now=True)
    output = args.results_dir / "paper_plots.executed.ipynb"
    nbformat.write(notebook, output)
    print(f"Executed notebook: {output}\nPDFs: {args.plot_dir}")


def main(argv=None):
    args = parse_args(argv)
    plan = workloads.build_paper_plan(args.benchmarks, build_dir=args.build_dir,
                                      raw_bin_dir=args.binary_dir, run_dir=args.run_dir,
                                      instruction_limit=args.instructions)
    compiles = sum(step.stage == "compile" for step in plan)
    print(f"Paper plan: {len(args.benchmarks)} benchmarks, {compiles} compilation passes, {len(plan) - compiles} simulations.", flush=True)
    print(f"Logs: {args.log_dir}\nResults: {args.results_dir}")
    if args.instructions:
        print(f"Short validation run: simulation target {args.instructions}; compilation includes lookahead headroom. Not paper results.")
    if args.dry_run:
        if not args.extract_only:
            for step in plan:
                print(f"{shlex.join(step.command)} > {shlex.quote(str(step.log_path))} 2>&1")
        if not args.run_only:
            for script in EXTRACTORS:
                print(shlex.join(extractor_command(script, args, args.results_dir)))
        if args.plots:
            print(f"Execute {ROOT / 'results/paper_plots.ipynb'} -> {args.results_dir / 'paper_plots.executed.ipynb'}")
        return 0
    try:
        if args.plots:
            missing = [name for name in ("matplotlib", "numbers_parser", "nbformat", "nbclient", "ipykernel", "jupyter_client")
                       if importlib.util.find_spec(name) is None]
            if missing:
                raise RuntimeError("Missing plotting dependencies: " + ", ".join(missing)
                                   + "; run python3 -m pip install -r requirements-artifact.txt")
        if not args.extract_only:
            execute_plan(plan, args)
        if not args.run_only:
            extract_results(plan, args)
        if args.plots:
            render_plots(args)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print("Artifact workflow completed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
