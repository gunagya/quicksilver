#!/usr/bin/env python3
"""
Run yoked-code workloads across first-pass and second-pass memory compilation.

This script is intentionally organized around small command-construction
functions so that CLI interface changes in the binaries are easy to update in
one place.

Modes:
  firstpass   Compile first-pass EIF memory binaries only.
  secondpass  Simulate first-pass baselines, then compile second-pass cache + prefetch binaries and simulate each one immediately.
  simulate    Run yoked_simulator on existing first-pass and second-pass binaries.
  all         Run firstpass and then secondpass.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
from dataclasses import dataclass, replace
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build"
RAW_BIN_DIR = REPO_ROOT / "benchmarks" / "bin"
MEM_DIR = RAW_BIN_DIR
MEM_FIRSTPASS_DIR = MEM_DIR / "firstpass"
MEM_SECONDPASS_DIR = MEM_DIR / "secondpass"
MEM_SECONDPASS_CACHE_DIR = MEM_SECONDPASS_DIR / "cache"
MEM_SECONDPASS_PREFETCH_DIR = MEM_SECONDPASS_DIR / "prefetch"
RUN_OUTPUT_DIR = BUILD_DIR / "yoked_codes_run_all_workloads"
LOG_DIR = RUN_OUTPUT_DIR / "logs"

DEFAULT_COMPILE_INST_LIMIT = 250_000_000
DEFAULT_SIM_INSTRUCTIONS = 100_000_000
SECOND_PASS_INST_LIMIT_DELTA = 2_000_000
COMPUTE_CAPACITIES = [4]
INTERMEDIATE_CAPACITIES = [8]
EVICTION_POLICIES = ["lru", "rri"]
CACHE_EVICTION_POLICY = "rri"
PREFETCH_EVICTION_POLICY = "lru"
SINGLEPASS_LAYER_TYPE = "weighted"
PREFETCH_MIN_LAYER_DISTANCE = 0
# Optional simulation sweep for manual simulator settings.
# Each entry can override any subset of:
#   - cold_storage_memory_block_capacity
#   - cold_storage_inner_code_distance
#   - intermediate_storage_inner_code_distance
#   - effective_code_distance
# Leave empty to run simulations without adding override flags.


@dataclass(frozen=True)
class SimulatorConfig:
    cold_storage_memory_block_capacity: int | None = None
    cold_storage_inner_code_distance: int | None = None
    intermediate_storage_inner_code_distance: int | None = None
    effective_code_distance: int | None = None


SIMULATION_SWEEP: list[SimulatorConfig] = [SimulatorConfig(98, None, None, None), SimulatorConfig(322, None, None, None), SimulatorConfig(482, None, None, None)]  


@dataclass(frozen=True)
class BenchmarkConfig:
    raw_binary: str
    factory_budget: int
    label: str
    compile_inst_limit: int = DEFAULT_COMPILE_INST_LIMIT
    sim_instructions: int = DEFAULT_SIM_INSTRUCTIONS


BENCHMARKS = [
    BenchmarkConfig("BQ_bose_hubbard_q.xz",                 50_000, "bose_hubbard_q"),
    BenchmarkConfig("BQ_bose_hubbard_t.xz",                 50_000, "bose_hubbard_t", 200_000_000),
    BenchmarkConfig("BQ_c2h4o_ethylene_oxide_q_prepare.xz", 50_000, "ethylene_oxide_q_prepare"),
    BenchmarkConfig("BQ_c2h4o_ethylene_oxide_q_select.xz",  50_000, "ethylene_oxide_q_select"),
    BenchmarkConfig("BQ_c2h4o_ethylene_oxide_t.xz",         50_000, "ethylene_oxide_t"),
    BenchmarkConfig("BQ_chromium_q_prepare.xz",             50_000, "chromium_q_prepare"),
    BenchmarkConfig("BQ_chromium_q_select.xz",              50_000, "chromium_q_select"),
    BenchmarkConfig("BQ_chromium_t.xz",                     50_000, "chromium_t"),
    BenchmarkConfig("shor_modmult_N16777259_a3_pow0.xz",   50_000, "shor_rsa24"),
    BenchmarkConfig("BQ_grover_3sat_schoning_1710.xz",      50_000, "grover_3sat"),
]

def raw_binary_path(benchmark: BenchmarkConfig) -> Path:
    return RAW_BIN_DIR / benchmark.raw_binary

def firstpass_binary_path(benchmark: BenchmarkConfig, compute_capacity: int) -> Path:
    return MEM_FIRSTPASS_DIR / benchmark.label / f"{benchmark.label}_c{compute_capacity}.bin"

def cache_binary_path(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    eviction_policy: str,
) -> Path:
    filename = (
        f"{benchmark.label}_{eviction_policy}_i{intermediate_capacity}_"
        f"c{compute_capacity}.bin"
    )
    return MEM_SECONDPASS_CACHE_DIR / benchmark.label / filename


def prefetch_binary_path(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    eviction_policy: str,
) -> Path:
    filename = (
        f"{benchmark.label}_{eviction_policy}_i{intermediate_capacity}_"
        f"mld{PREFETCH_MIN_LAYER_DISTANCE}_c{compute_capacity}.bin"
    )
    return MEM_SECONDPASS_PREFETCH_DIR / benchmark.label / filename


def firstpass_log_path(benchmark: BenchmarkConfig, compute_capacity: int) -> Path:
    return LOG_DIR / "compile" / "firstpass" / benchmark.label / f"c{compute_capacity}.log"

def firstpass_simulation_log_path(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    baseline_enabled: bool,
    sim_config: SimulatorConfig | None = None,
) -> Path:
    baseline_tag = "baseline1" if baseline_enabled else "baseline0"
    suffix = simulator_config_log_suffix(sim_config or SimulatorConfig())
    return LOG_DIR / "simulate" / "firstpass" / benchmark.label / f"c{compute_capacity}_{baseline_tag}_i0{suffix}.log"

def cache_log_path(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    eviction_policy: str,
) -> Path:
    filename = f"c{compute_capacity}_i{intermediate_capacity}_{eviction_policy}.log"
    return LOG_DIR / "compile" / "cache" / benchmark.label / filename

def prefetch_log_path(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    eviction_policy: str,
) -> Path:
    filename = (
        f"c{compute_capacity}_i{intermediate_capacity}_{eviction_policy}_"
        f"mld{PREFETCH_MIN_LAYER_DISTANCE}.log"
    )
    return LOG_DIR / "compile" / "prefetch" / benchmark.label / filename


def simulation_log_path(
    compiler_name: str,
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    eviction_policy: str,
    sim_config: SimulatorConfig | None = None,
) -> Path:
    mld_suffix = "" if compiler_name == "cache" else f"_mld{PREFETCH_MIN_LAYER_DISTANCE}"
    cold_storage_suffix = simulator_config_log_suffix(sim_config or SimulatorConfig())
    filename = (
        f"c{compute_capacity}_i{intermediate_capacity}_{eviction_policy}{mld_suffix}{cold_storage_suffix}.log"
    )
    return LOG_DIR / "simulate" / compiler_name / benchmark.label / filename


def iter_simulation_configs() -> list[SimulatorConfig]:
    if not SIMULATION_SWEEP:
        return [SimulatorConfig()]
    return list(SIMULATION_SWEEP)


def simulator_config_log_suffix(sim_config: SimulatorConfig) -> str:
    parts: list[str] = []
    if sim_config.cold_storage_memory_block_capacity is not None:
        parts.append(f"csm{sim_config.cold_storage_memory_block_capacity}")
    if sim_config.cold_storage_inner_code_distance is not None:
        parts.append(f"csd{sim_config.cold_storage_inner_code_distance}")
    if sim_config.intermediate_storage_inner_code_distance is not None:
        parts.append(f"i1d{sim_config.intermediate_storage_inner_code_distance}")
    if sim_config.effective_code_distance is not None:
        parts.append(f"ed{sim_config.effective_code_distance}")
    if not parts:
        return ""
    return "_" + "_".join(parts)


def secondpass_compile_inst_limit(benchmark: BenchmarkConfig) -> int:
    return max(1, benchmark.compile_inst_limit - SECOND_PASS_INST_LIMIT_DELTA)


def should_run_cache(eviction_policy: str) -> bool:
    return eviction_policy == CACHE_EVICTION_POLICY


def should_run_prefetch(eviction_policy: str) -> bool:
    return eviction_policy == PREFETCH_EVICTION_POLICY


def build_firstpass_compile_command(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
) -> list[str]:
    return [
        "./qs_memory_scheduler",
        str(raw_binary_path(benchmark)),
        str(firstpass_binary_path(benchmark, compute_capacity)),
        "-c",
        str(compute_capacity),
        "-s",
        "0",
        "-i",
        str(benchmark.compile_inst_limit),
        "-pp",
        "0",
    ]


def build_cache_secondpass_command(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    eviction_policy: str,
    *,
    instruction_limit: int | None = None,
) -> list[str]:
    stage_a_dummy_output = (
        MEM_SECONDPASS_CACHE_DIR
        / benchmark.label
        / f".{benchmark.label}_c{compute_capacity}_stagea_dummy.bin"
    )
    return [
        "./qs_memory_scheduler",
        str(raw_binary_path(benchmark)),
        str(stage_a_dummy_output),
        "-c",
        str(compute_capacity),
        "-s",
        "0",
        "-i",
        str(
            secondpass_compile_inst_limit(benchmark)
            if instruction_limit is None
            else instruction_limit
        ),
        "-pp",
        "0",
        "--enable-rri-placer",
        "--rri-input",
        str(firstpass_binary_path(benchmark, compute_capacity)),
        "--rri-output-file",
        str(cache_binary_path(benchmark, compute_capacity, intermediate_capacity, eviction_policy)),
        "--rri-intermediate-capacity",
        str(intermediate_capacity),
        "--rri-eviction-policy",
        eviction_policy,
    ]


def build_prefetch_secondpass_command(
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    eviction_policy: str,
    *,
    instruction_limit: int | None = None,
) -> list[str]:
    stage_a_dummy_output = (
        MEM_SECONDPASS_PREFETCH_DIR
        / benchmark.label
        / f".{benchmark.label}_c{compute_capacity}_stagea_dummy.bin"
    )
    return [
        "./qs_memory_scheduler",
        str(raw_binary_path(benchmark)),
        str(stage_a_dummy_output),
        "-c",
        str(compute_capacity),
        "-s",
        "0",
        "-i",
        str(
            secondpass_compile_inst_limit(benchmark)
            if instruction_limit is None
            else instruction_limit
        ),
        "-pp",
        "0",
        "--enable-singlepass-prefetch",
        "--singlepass-prefetch-input",
        str(firstpass_binary_path(benchmark, compute_capacity)),
        "--singlepass-prefetch-output-file",
        str(
            prefetch_binary_path(
                benchmark,
                compute_capacity,
                intermediate_capacity,
                eviction_policy,
            )
        ),
        "--singlepass-prefetch-intermediate-capacity",
        str(intermediate_capacity),
        "--singlepass-prefetch-min-layer-distance",
        str(PREFETCH_MIN_LAYER_DISTANCE),
        "--singlepass-prefetch-eviction-policy",
        eviction_policy,
        "--singlepass-prefetch-layer-type",
        SINGLEPASS_LAYER_TYPE,
    ]


def build_yoked_simulator_command(
    trace_path: Path,
    benchmark: BenchmarkConfig,
    compute_capacity: int,
    intermediate_capacity: int,
    baseline: int | None = None,
    cold_storage_memory_block_capacity: int | None = None,
    cold_storage_inner_code_distance: int | None = None,
    intermediate_storage_inner_code_distance: int | None = None,
    effective_code_distance: int | None = None,
) -> list[str]:
    cmd = [
        "./yoked_simulator",
        str(trace_path),
        str(benchmark.sim_instructions),
        "-a",
        str(compute_capacity),
        "-f",
        str(benchmark.factory_budget),
        "-pp",
        "0",
        "-i",
        str(intermediate_capacity),
    ]
    if baseline is not None:
        cmd.extend(["--baseline", str(baseline)])
    if cold_storage_memory_block_capacity is not None:
        cmd.extend(
            [
                "--cold-storage-memory-block-capacity",
                str(cold_storage_memory_block_capacity),
            ]
        )
    if cold_storage_inner_code_distance is not None:
        cmd.extend(
            [
                "--cold-storage-inner-code-distance",
                str(cold_storage_inner_code_distance),
            ]
        )
    if intermediate_storage_inner_code_distance is not None:
        cmd.extend(
            [
                "--intermediate-storage-inner-code-distance",
                str(intermediate_storage_inner_code_distance),
            ]
        )
    if effective_code_distance is not None:
        cmd.extend(["--effective-code-distance", str(effective_code_distance)])
    return cmd


@dataclass(frozen=True)
class PaperRunStep:
    """One command in the paper workflow, in dependency order."""

    benchmark: str
    stage: str
    command: tuple[str, ...]
    log_path: Path
    output_path: Path | None = None


@dataclass(frozen=True)
class _PaperSimulation:
    kind: str
    compute_capacity: int
    intermediate_capacity: int = 0
    eviction_policy: str = ""
    baseline: int | None = None
    simulator: SimulatorConfig = SimulatorConfig(194)


def _paper_simulation_points() -> list[_PaperSimulation]:
    """The union of simulation settings consumed by paper_plots.ipynb."""
    points: list[_PaperSimulation] = []
    secondpass_modes = (("cache", "rri"), ("prefetch", "lru"))

    # Main IPC figure and compute-capacity sensitivity (i8_by_c).
    for compute_capacity in (4, 8, 12, 16):
        for baseline in (0, 1):
            points.append(
                _PaperSimulation("firstpass", compute_capacity, baseline=baseline)
            )
        for kind, policy in secondpass_modes:
            points.append(_PaperSimulation(kind, compute_capacity, 8, policy))

    # The main IPC figure also compares Trident-D with LRU.
    points.append(_PaperSimulation("cache", 4, 8, "lru"))

    # Intermediate-capacity sensitivity (c4); i8 was added above.
    for intermediate_capacity in (4, 16, 24):
        for kind, policy in secondpass_modes:
            points.append(_PaperSimulation(kind, 4, intermediate_capacity, policy))

    # Cold-storage capacity sensitivity (c4/i8); csm194 was added above.
    for capacity in (34, 98, 322, 482):
        for kind, policy in secondpass_modes:
            points.append(
                _PaperSimulation(
                    kind, 4, 8, policy, simulator=SimulatorConfig(capacity)
                )
            )

    # Other notebook tables and diagnostics reuse these simulations, plus the
    # c4/i8/lru prefetch compilation log. No extra experiment is required.
    return points


def build_paper_plan(
    benchmarks: list[BenchmarkConfig],
    *,
    build_dir: Path,
    raw_bin_dir: Path,
    run_dir: Path,
    instruction_limit: int | None = None,
) -> list[PaperRunStep]:
    """Build the exact paper workflow without running commands or writing files.

    Each benchmark needs 19 compilations and 31 simulations. Compiled traces
    live under run_dir/binaries and logs under run_dir/logs. The existing
    command builders remain the single source of compiler/simulator flags.

    An instruction_limit sets the simulation limit for short validation runs.
    Second-pass compilation retains two million instructions of lookahead and
    first-pass compilation retains another two million. These margins keep the
    existing DAG readers away from EOF without changing compiler/simulator code.
    Without it the benchmark's original limits and second-pass subtraction are
    preserved.

    This function briefly overrides and then restores the legacy path globals.
    Build plans before starting workers; concurrent calls to this function or
    the legacy runner in the same process are not supported.
    """
    if instruction_limit is not None and instruction_limit <= 0:
        raise ValueError("instruction_limit must be a positive integer")

    build_dir = Path(build_dir).expanduser().resolve()
    raw_bin_dir = Path(raw_bin_dir).expanduser().resolve()
    run_dir = Path(run_dir).expanduser().resolve()
    binary_dir = run_dir / "binaries"
    paths = {
        "BUILD_DIR": build_dir,
        "RAW_BIN_DIR": raw_bin_dir,
        "MEM_DIR": binary_dir,
        "MEM_FIRSTPASS_DIR": binary_dir / "firstpass",
        "MEM_SECONDPASS_DIR": binary_dir / "secondpass",
        "MEM_SECONDPASS_CACHE_DIR": binary_dir / "secondpass" / "cache",
        "MEM_SECONDPASS_PREFETCH_DIR": binary_dir / "secondpass" / "prefetch",
        "RUN_OUTPUT_DIR": run_dir,
        "LOG_DIR": run_dir / "logs",
    }
    previous_paths = {name: globals()[name] for name in paths}
    plan: list[PaperRunStep] = []
    points = _paper_simulation_points()

    def append_step(
        benchmark: BenchmarkConfig,
        stage: str,
        command: list[str],
        log_path: Path,
        output_path: Path | None = None,
    ) -> None:
        command[0] = str(build_dir / Path(command[0]).name)
        plan.append(
            PaperRunStep(
                benchmark.label, stage, tuple(command), log_path, output_path
            )
        )

    def secondpass_paths(
        point: _PaperSimulation, benchmark: BenchmarkConfig
    ) -> tuple[Path, Path]:
        binary_builder = (
            cache_binary_path if point.kind == "cache" else prefetch_binary_path
        )
        log_builder = cache_log_path if point.kind == "cache" else prefetch_log_path
        args = (
            benchmark,
            point.compute_capacity,
            point.intermediate_capacity,
            point.eviction_policy,
        )
        return binary_builder(*args), log_builder(*args)

    globals().update(paths)
    try:
        for original_benchmark in benchmarks:
            benchmark = (
                original_benchmark
                if instruction_limit is None
                else replace(
                    original_benchmark,
                    compile_inst_limit=instruction_limit + 2 * SECOND_PASS_INST_LIMIT_DELTA,
                    sim_instructions=instruction_limit,
                )
            )
            compute_capacities = sorted({p.compute_capacity for p in points})
            for compute_capacity in compute_capacities:
                append_step(
                    benchmark,
                    "compile",
                    build_firstpass_compile_command(benchmark, compute_capacity),
                    firstpass_log_path(benchmark, compute_capacity),
                    firstpass_binary_path(benchmark, compute_capacity),
                )

            # A trace can serve several simulation settings; compile it once.
            compiled: set[tuple[str, int, int, str]] = set()
            for point in points:
                if point.kind == "firstpass":
                    continue
                key = (
                    point.kind,
                    point.compute_capacity,
                    point.intermediate_capacity,
                    point.eviction_policy,
                )
                if key in compiled:
                    continue
                compiled.add(key)
                binary_path, log_path = secondpass_paths(point, benchmark)
                command_builder = (
                    build_cache_secondpass_command
                    if point.kind == "cache"
                    else build_prefetch_secondpass_command
                )
                append_step(
                    benchmark,
                    "compile",
                    command_builder(
                        benchmark,
                        point.compute_capacity,
                        point.intermediate_capacity,
                        point.eviction_policy,
                        instruction_limit=(
                            None if instruction_limit is None
                            else instruction_limit + SECOND_PASS_INST_LIMIT_DELTA
                        ),
                    ),
                    log_path,
                    binary_path,
                )

            for point in points:
                if point.kind == "firstpass":
                    binary_path = firstpass_binary_path(
                        benchmark, point.compute_capacity
                    )
                    log_path = firstpass_simulation_log_path(
                        benchmark,
                        point.compute_capacity,
                        bool(point.baseline),
                        point.simulator,
                    )
                else:
                    binary_path, _ = secondpass_paths(point, benchmark)
                    log_path = simulation_log_path(
                        point.kind,
                        benchmark,
                        point.compute_capacity,
                        point.intermediate_capacity,
                        point.eviction_policy,
                        point.simulator,
                    )
                append_step(
                    benchmark,
                    "simulate",
                    build_yoked_simulator_command(
                        binary_path,
                        benchmark,
                        point.compute_capacity,
                        point.intermediate_capacity,
                        baseline=point.baseline,
                        cold_storage_memory_block_capacity=point.simulator.cold_storage_memory_block_capacity,
                        cold_storage_inner_code_distance=point.simulator.cold_storage_inner_code_distance,
                        intermediate_storage_inner_code_distance=point.simulator.intermediate_storage_inner_code_distance,
                        effective_code_distance=point.simulator.effective_code_distance,
                    ),
                    log_path,
                )
    finally:
        globals().update(previous_paths)

    return plan


def run_command(cmd: list[str], desc: str, log_path: Path | None = None) -> str | None:
    print(f"\n>>> {desc}")
    print("    " + " ".join(str(x) for x in cmd))
    try:
        result = subprocess.run(
            cmd,
            cwd=BUILD_DIR,
            capture_output=True,
            text=True,
            check=True,
        )
        output = (result.stdout or "") + (("\n" + result.stderr) if result.stderr else "")
    except subprocess.CalledProcessError as exc:
        output = (exc.stdout or "") + (("\n" + exc.stderr) if exc.stderr else "")
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(output)
        print(f"  FAILED (rc={exc.returncode})")
        if exc.stdout:
            print("  stdout:", exc.stdout[-800:])
        if exc.stderr:
            print("  stderr:", exc.stderr[-800:])
        return None

    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(output)

    return output


def command_to_shell(cmd: list[str]) -> str:
    return shlex.join(str(arg) for arg in cmd)


def build_logged_shell_command(cmd: list[str], log_path: Path) -> str:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    return f"{shlex.join(str(arg) for arg in cmd)} > {shlex.quote(str(log_path))} 2>&1"


def run_shell_command(command: str, desc: str) -> bool:
    print(f"\n>>> {desc}")
    print("    " + command)
    result = subprocess.run(
        command,
        cwd=BUILD_DIR,
        shell=True,
        executable="/bin/zsh",
    )
    if result.returncode != 0:
        print(f"  FAILED (rc={result.returncode})")
        return False
    return True

def ensure_directories() -> None:
    MEM_FIRSTPASS_DIR.mkdir(parents=True, exist_ok=True)
    MEM_SECONDPASS_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    MEM_SECONDPASS_PREFETCH_DIR.mkdir(parents=True, exist_ok=True)
    RUN_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)


def ensure_benchmark_output_directories(benchmark: BenchmarkConfig) -> None:
    (MEM_FIRSTPASS_DIR / benchmark.label).mkdir(parents=True, exist_ok=True)
    (MEM_SECONDPASS_CACHE_DIR / benchmark.label).mkdir(parents=True, exist_ok=True)
    (MEM_SECONDPASS_PREFETCH_DIR / benchmark.label).mkdir(parents=True, exist_ok=True)


def print_all_commands(mode_filter: str | None = None) -> None:
    print_firstpass = mode_filter in (None, "firstpass")
    print_secondpass = mode_filter in (None, "secondpass")
    print_simulate = mode_filter in (None, "simulate")

    for benchmark in BENCHMARKS:
        if not raw_binary_path(benchmark).exists():
            continue

        ensure_benchmark_output_directories(benchmark)

        simulation_configs = iter_simulation_configs()

        if print_firstpass:
            for compute_capacity in COMPUTE_CAPACITIES:
                firstpass_compile_cmd = build_firstpass_compile_command(benchmark, compute_capacity)
                firstpass_parts = [
                    build_logged_shell_command(
                        firstpass_compile_cmd,
                        firstpass_log_path(benchmark, compute_capacity),
                    )
                ]
                for sim_config in simulation_configs:
                    firstpass_ideal_sim_cmd = build_yoked_simulator_command(
                        firstpass_binary_path(benchmark, compute_capacity),
                        benchmark,
                        compute_capacity,
                        0,
                        baseline=1,
                        cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                        cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                        intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                        effective_code_distance=sim_config.effective_code_distance,
                    )
                    firstpass_eif_sim_cmd = build_yoked_simulator_command(
                        firstpass_binary_path(benchmark, compute_capacity),
                        benchmark,
                        compute_capacity,
                        0,
                        baseline=0,
                        cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                        cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                        intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                        effective_code_distance=sim_config.effective_code_distance,
                    )
                    firstpass_parts.append(
                        build_logged_shell_command(
                            firstpass_ideal_sim_cmd,
                            firstpass_simulation_log_path(
                                benchmark,
                                compute_capacity,
                                True,
                                sim_config,
                            ),
                        )
                    )
                    firstpass_parts.append(
                        build_logged_shell_command(
                            firstpass_eif_sim_cmd,
                            firstpass_simulation_log_path(
                                benchmark,
                                compute_capacity,
                                False,
                                sim_config,
                            ),
                        )
                    )
                print(" && ".join(firstpass_parts))

        for compute_capacity in COMPUTE_CAPACITIES:
            for intermediate_capacity in INTERMEDIATE_CAPACITIES:
                for eviction_policy in EVICTION_POLICIES:
                    cache_output_path = cache_binary_path(
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                        eviction_policy,
                    )
                    cache_sim_cmd = build_yoked_simulator_command(
                        cache_output_path,
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                    )

                    prefetch_output_path = prefetch_binary_path(
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                        eviction_policy,
                    )
                    prefetch_sim_cmd = build_yoked_simulator_command(
                        prefetch_output_path,
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                    )

                    if print_secondpass:
                        cache_compile_cmd = build_cache_secondpass_command(
                            benchmark,
                            compute_capacity,
                            intermediate_capacity,
                            eviction_policy,
                        )
                        cache_compile_log = cache_log_path(
                            benchmark,
                            compute_capacity,
                            intermediate_capacity,
                            eviction_policy,
                        )
                        cache_parts = [build_logged_shell_command(cache_compile_cmd, cache_compile_log)]
                        for sim_config in simulation_configs:
                            cache_sim_cfg_cmd = build_yoked_simulator_command(
                                cache_output_path,
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                effective_code_distance=sim_config.effective_code_distance,
                            )
                            cache_parts.append(
                                build_logged_shell_command(
                                    cache_sim_cfg_cmd,
                                    simulation_log_path(
                                        "cache",
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        eviction_policy,
                                        sim_config,
                                    ),
                                )
                            )
                        print(" && ".join(cache_parts))

                        prefetch_compile_cmd = build_prefetch_secondpass_command(
                            benchmark,
                            compute_capacity,
                            intermediate_capacity,
                            eviction_policy,
                        )
                        prefetch_compile_log = prefetch_log_path(
                            benchmark,
                            compute_capacity,
                            intermediate_capacity,
                            eviction_policy,
                        )
                        prefetch_parts = [build_logged_shell_command(prefetch_compile_cmd, prefetch_compile_log)]
                        for sim_config in simulation_configs:
                            prefetch_sim_cfg_cmd = build_yoked_simulator_command(
                                prefetch_output_path,
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                effective_code_distance=sim_config.effective_code_distance,
                            )
                            prefetch_parts.append(
                                build_logged_shell_command(
                                    prefetch_sim_cfg_cmd,
                                    simulation_log_path(
                                        "prefetch",
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        eviction_policy,
                                        sim_config,
                                    ),
                                )
                            )
                        print(" && ".join(prefetch_parts))

                    if print_simulate:
                        for sim_config in simulation_configs:
                            cache_sim_cfg_cmd = build_yoked_simulator_command(
                                cache_output_path,
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                effective_code_distance=sim_config.effective_code_distance,
                            )
                            print(
                                build_logged_shell_command(
                                    cache_sim_cfg_cmd,
                                    simulation_log_path(
                                        "cache",
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        eviction_policy,
                                        sim_config,
                                    ),
                                )
                            )
                            prefetch_sim_cfg_cmd = build_yoked_simulator_command(
                                prefetch_output_path,
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                effective_code_distance=sim_config.effective_code_distance,
                            )
                            print(
                                build_logged_shell_command(
                                    prefetch_sim_cfg_cmd,
                                    simulation_log_path(
                                        "prefetch",
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        eviction_policy,
                                        sim_config,
                                    ),
                                )
                            )


def run_firstpass() -> None:
    for benchmark in BENCHMARKS:
        if not raw_binary_path(benchmark).exists():
            print(f"[SKIP] raw binary not found: {raw_binary_path(benchmark)}")
            continue

        print(f"\n{'=' * 70}\n[FIRSTPASS] {benchmark.label}\n{'=' * 70}")
        simulation_configs = iter_simulation_configs()
        for compute_capacity in COMPUTE_CAPACITIES:
            output_path = firstpass_binary_path(benchmark, compute_capacity)
            output_path.parent.mkdir(parents=True, exist_ok=True)
            firstpass_compile_cmd = build_firstpass_compile_command(benchmark, compute_capacity)
            chained_parts = [
                build_logged_shell_command(
                    firstpass_compile_cmd,
                    firstpass_log_path(benchmark, compute_capacity),
                )
            ]
            for sim_config in simulation_configs:
                firstpass_ideal_sim_cmd = build_yoked_simulator_command(
                    output_path,
                    benchmark,
                    compute_capacity,
                    0,
                    baseline=1,
                    cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                    cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                    intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                    effective_code_distance=sim_config.effective_code_distance,
                )
                firstpass_eif_sim_cmd = build_yoked_simulator_command(
                    output_path,
                    benchmark,
                    compute_capacity,
                    0,
                    baseline=0,
                    cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                    cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                    intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                    effective_code_distance=sim_config.effective_code_distance,
                )
                chained_parts.append(
                    build_logged_shell_command(
                        firstpass_ideal_sim_cmd,
                        firstpass_simulation_log_path(
                            benchmark,
                            compute_capacity,
                            True,
                            sim_config,
                        ),
                    )
                )
                chained_parts.append(
                    build_logged_shell_command(
                        firstpass_eif_sim_cmd,
                        firstpass_simulation_log_path(
                            benchmark,
                            compute_capacity,
                            False,
                            sim_config,
                        ),
                    )
                )
            chained_firstpass_cmd = " && ".join(chained_parts)

            run_shell_command(
                chained_firstpass_cmd,
                f"{benchmark.label}: firstpass compile+baseline sims c={compute_capacity}",
            )


def run_secondpass(rebuild_existing: bool = False) -> None:
    for benchmark in BENCHMARKS:
        if not raw_binary_path(benchmark).exists():
            print(f"[SKIP] raw binary not found: {raw_binary_path(benchmark)}")
            continue

        print(f"\n{'=' * 70}\n[SECONDPASS] {benchmark.label}\n{'=' * 70}")
        simulation_configs = iter_simulation_configs()
        for compute_capacity in COMPUTE_CAPACITIES:
            firstpass_path = firstpass_binary_path(benchmark, compute_capacity)
            if not firstpass_path.exists():
                print(f"[SKIP] first-pass output missing: {firstpass_path}")
                continue

            for intermediate_capacity in INTERMEDIATE_CAPACITIES:
                for eviction_policy in EVICTION_POLICIES:
                    cache_output_path = cache_binary_path(
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                        eviction_policy,
                    )
                    if should_run_cache(eviction_policy):
                        cache_output_path.parent.mkdir(parents=True, exist_ok=True)
                        if cache_output_path.exists() and not rebuild_existing:
                            print(f"[REUSE] {cache_output_path}")
                            for sim_config in simulation_configs:
                                cache_sim_cmd = build_yoked_simulator_command(
                                    cache_output_path,
                                    benchmark,
                                    compute_capacity,
                                    intermediate_capacity,
                                    cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                    cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                    intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                    effective_code_distance=sim_config.effective_code_distance,
                                )
                                cache_sim_log = simulation_log_path(
                                    "cache",
                                    benchmark,
                                    compute_capacity,
                                    intermediate_capacity,
                                    eviction_policy,
                                    sim_config,
                                )
                                run_command(
                                    cache_sim_cmd,
                                    (
                                        f"{benchmark.label}: simulate cache c={compute_capacity} "
                                        f"i={intermediate_capacity} policy={eviction_policy}"
                                    ),
                                    log_path=cache_sim_log,
                                )
                        else:
                            cache_compile_cmd = build_cache_secondpass_command(
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                eviction_policy,
                            )
                            cache_compile_log = cache_log_path(
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                eviction_policy,
                            )
                            compile_ok = run_shell_command(
                                build_logged_shell_command(cache_compile_cmd, cache_compile_log),
                                (
                                    f"{benchmark.label}: cache compile "
                                    f"c={compute_capacity} i={intermediate_capacity} "
                                    f"policy={eviction_policy}"
                                ),
                            )
                            if compile_ok:
                                for sim_config in simulation_configs:
                                    cache_sim_cmd = build_yoked_simulator_command(
                                        cache_output_path,
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                        cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                        intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                        effective_code_distance=sim_config.effective_code_distance,
                                    )
                                    cache_sim_log = simulation_log_path(
                                        "cache",
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        eviction_policy,
                                        sim_config,
                                    )
                                    run_command(
                                        cache_sim_cmd,
                                        (
                                            f"{benchmark.label}: simulate cache c={compute_capacity} "
                                            f"i={intermediate_capacity} policy={eviction_policy}"
                                        ),
                                        log_path=cache_sim_log,
                                    )

                    prefetch_output_path = prefetch_binary_path(
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                        eviction_policy,
                    )
                    if should_run_prefetch(eviction_policy):
                        prefetch_output_path.parent.mkdir(parents=True, exist_ok=True)
                        if prefetch_output_path.exists() and not rebuild_existing:
                            print(f"[REUSE] {prefetch_output_path}")
                            for sim_config in simulation_configs:
                                prefetch_sim_cmd = build_yoked_simulator_command(
                                    prefetch_output_path,
                                    benchmark,
                                    compute_capacity,
                                    intermediate_capacity,
                                    cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                    cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                    intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                    effective_code_distance=sim_config.effective_code_distance,
                                )
                                prefetch_sim_log = simulation_log_path(
                                    "prefetch",
                                    benchmark,
                                    compute_capacity,
                                    intermediate_capacity,
                                    eviction_policy,
                                    sim_config,
                                )
                                run_command(
                                    prefetch_sim_cmd,
                                    (
                                        f"{benchmark.label}: simulate prefetch "
                                        f"c={compute_capacity} i={intermediate_capacity} "
                                        f"policy={eviction_policy} mld={PREFETCH_MIN_LAYER_DISTANCE}"
                                    ),
                                    log_path=prefetch_sim_log,
                                )
                        else:
                            prefetch_compile_cmd = build_prefetch_secondpass_command(
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                eviction_policy,
                            )
                            prefetch_compile_log = prefetch_log_path(
                                benchmark,
                                compute_capacity,
                                intermediate_capacity,
                                eviction_policy,
                            )
                            compile_ok = run_shell_command(
                                build_logged_shell_command(prefetch_compile_cmd, prefetch_compile_log),
                                (
                                    f"{benchmark.label}: prefetch compile "
                                    f"c={compute_capacity} i={intermediate_capacity} "
                                    f"policy={eviction_policy} mld={PREFETCH_MIN_LAYER_DISTANCE}"
                                ),
                            )
                            if compile_ok:
                                for sim_config in simulation_configs:
                                    prefetch_sim_cmd = build_yoked_simulator_command(
                                        prefetch_output_path,
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                        cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                        intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                        effective_code_distance=sim_config.effective_code_distance,
                                    )
                                    prefetch_sim_log = simulation_log_path(
                                        "prefetch",
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        eviction_policy,
                                        sim_config,
                                    )
                                    run_command(
                                        prefetch_sim_cmd,
                                        (
                                            f"{benchmark.label}: simulate prefetch "
                                            f"c={compute_capacity} i={intermediate_capacity} "
                                            f"policy={eviction_policy} mld={PREFETCH_MIN_LAYER_DISTANCE}"
                                        ),
                                        log_path=prefetch_sim_log,
                                    )


def simulate_existing_secondpass_binaries() -> None:
    for benchmark in BENCHMARKS:
        print(f"\n{'=' * 70}\n[SIMULATE] {benchmark.label}\n{'=' * 70}")
        simulation_configs = iter_simulation_configs()
        for compute_capacity in COMPUTE_CAPACITIES:
            for intermediate_capacity in INTERMEDIATE_CAPACITIES:
                for eviction_policy in EVICTION_POLICIES:
                    cache_path = cache_binary_path(
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                        eviction_policy,
                    )
                    if should_run_cache(eviction_policy):
                        if cache_path.exists():
                            for sim_config in simulation_configs:
                                run_command(
                                    build_yoked_simulator_command(
                                        cache_path,
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                        cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                        intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                        effective_code_distance=sim_config.effective_code_distance,
                                    ),
                                    (
                                        f"{benchmark.label}: simulate cache c={compute_capacity} "
                                        f"i={intermediate_capacity} policy={eviction_policy}"
                                    ),
                                    log_path=simulation_log_path(
                                        "cache",
                                        benchmark,
                                        compute_capacity,
                                        intermediate_capacity,
                                        eviction_policy,
                                        sim_config,
                                    ),
                                )
                        else:
                            print(f"[SKIP SIM] missing binary: {cache_path}")

                    prefetch_path = prefetch_binary_path(
                        benchmark,
                        compute_capacity,
                        intermediate_capacity,
                        eviction_policy,
                    )
                    if should_run_prefetch(eviction_policy):
                        if not prefetch_path.exists():
                            print(f"[SKIP SIM] missing binary: {prefetch_path}")
                            continue

                        for sim_config in simulation_configs:
                            run_command(
                                build_yoked_simulator_command(
                                    prefetch_path,
                                    benchmark,
                                    compute_capacity,
                                    intermediate_capacity,
                                    cold_storage_memory_block_capacity=sim_config.cold_storage_memory_block_capacity,
                                    cold_storage_inner_code_distance=sim_config.cold_storage_inner_code_distance,
                                    intermediate_storage_inner_code_distance=sim_config.intermediate_storage_inner_code_distance,
                                    effective_code_distance=sim_config.effective_code_distance,
                                ),
                                (
                                    f"{benchmark.label}: simulate prefetch "
                                    f"c={compute_capacity} i={intermediate_capacity} "
                                    f"policy={eviction_policy} mld={PREFETCH_MIN_LAYER_DISTANCE}"
                                ),
                                log_path=simulation_log_path(
                                    "prefetch",
                                    benchmark,
                                    compute_capacity,
                                    intermediate_capacity,
                                    eviction_policy,
                                    sim_config,
                                ),
                            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run yoked-code workloads across compile/sim stages")
    parser.add_argument(
        "mode",
        choices=["firstpass", "secondpass", "simulate", "all", "print"],
        help="Which stage(s) to run",
    )
    parser.add_argument(
        "print_mode",
        nargs="?",
        choices=["firstpass", "secondpass", "simulate"],
        help="Optional stage filter when mode is print",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    os.chdir(BUILD_DIR)
    ensure_directories()

    if args.mode in {"firstpass", "all"}:
        run_firstpass()

    if args.mode == "secondpass":
        run_secondpass(rebuild_existing=False)

    if args.mode == "all":
        run_secondpass(rebuild_existing=True)

    if args.mode == "simulate":
        simulate_existing_secondpass_binaries()

    if args.mode == "print":
        print_all_commands(args.print_mode)


if __name__ == "__main__":
    main()
