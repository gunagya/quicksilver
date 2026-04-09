#!/usr/bin/env python3
"""
Benchmark comparison: singlepass prefetch (second pass) vs EIF and ideal-memory baselines.

Step 1 — compile memory ops:
  For each benchmark × intermediate capacity × min-layer-distance, run
  qs_memory_scheduler with:
    -s 0  (EIF, first pass)                               → one output binary per benchmark
    --enable-singlepass-prefetch (second pass)            → one binary per (policy, cap, mld)
      using:
        --singlepass-prefetch-eviction-policy {lru|rri}
        --singlepass-prefetch-layer-type weighted
  Collect compile-time singlepass-prefetch stats.

Step 2 — simulate:
  For the EIF binary:
    yoked_simulator ... --baseline 1 -i 0   → ideal-memory IPC
    yoked_simulator ... -i 0                → EIF no-prefetch IPC
  For each singlepass-prefetch binary (policy × cap × mld):
    yoked_simulator ... -i <cap>            → IPC, cold miss rate, 1D ready metrics

Step 3 — write CSV.
  One row per simulated configuration, including "ideal_memory" and "eif" rows.
"""

import subprocess
import os
import re
import csv
from pathlib import Path

############################################################
# Configuration — edit as needed
############################################################

BUILD_DIR   = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/"
                   "simulators/routing-simulator/deps/quicksilver/build")
RAW_BIN_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/"
                   "simulators/routing-simulator/deps/quicksilver/benchmarks/bin")
MEM_DIR     = RAW_BIN_DIR / "mem"
MEM_FIRSTPASS_DIR = MEM_DIR / "firstpass"
MEM_SECONDPASS_PREFETCH_DIR = MEM_DIR / "secondpass" / "prefetch"

COMPUTE_CAPACITIES  = [4]   # -c / -a values to sweep
SIM_INSTRUCTIONS    = 1_000_000   # second positional arg to yoked_simulator
SINGLEPASS_PREFETCH_POLICIES = ["lru", "rri"]
SINGLEPASS_PREFETCH_LAYER_TYPE = "weighted"
SECOND_PASS_INST_LIMIT_DELTA = 500_000

# For each intermediate storage size, the MLD values to sweep.
# Edit the lists here to control which (cap, mld) pairs are compiled + simulated.
INTERMEDIATE_MLD_MAP: dict[int, list[int]] = {
    8:  [0, 50, 100, 150, 200],
}

# Benchmarks: (raw_binary_filename, factory_phys_qubit_budget, label[, compile_inst_limit[, sim_instructions]])
# raw_binary_filename is relative to RAW_BIN_DIR.
BENCHMARKS = [
    ("BQ_bose_hubbard_q.xz",                     50_000, "bose_hubbard_q"),
    ("BQ_bose_hubbard_t.xz",                     50_000, "bose_hubbard_t"),
    ("BQ_c2h4o_ethylene_oxide_q_prepare.xz",     50_000, "ethylene_oxide_q_prepare"),
    ("BQ_c2h4o_ethylene_oxide_q_select.xz",      50_000, "ethylene_oxide_q_select"),
    ("BQ_c2h4o_ethylene_oxide_t.xz",             50_000, "ethylene_oxide_t"),
    ("BQ_chromium_q_prepare.xz",                 50_000, "chromium_q_prepare"),
    ("BQ_chromium_q_select.xz",                  50_000, "chromium_q_select"),
    ("BQ_chromium_t.xz",                         50_000, "chromium_t"),
    ("shor_modmult_N16777259_a3_pow0.bin",       50_000, "shor_rsa24"),
    ("BQ_grover_3sat_schoning_1710.xz",          50_000, "grover_3sat"),
]

# Memory-scheduler compile limits (instructions compiled per run)
MS_INST_LIMIT = 3_000_000

############################################################
# Helpers
############################################################

def run(cmd: list[str], desc: str) -> str | None:
    """Run a subprocess and return combined stdout/stderr, or None on failure."""
    print(f"\n>>> {desc}")
    print("    " + " ".join(str(x) for x in cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return (r.stdout or "") + (("\n" + r.stderr) if r.stderr else "")
    except subprocess.CalledProcessError as e:
        print(f"  FAILED (rc={e.returncode})")
        if e.stdout:
            print("  stdout:", e.stdout[-800:])
        if e.stderr:
            print("  stderr:", e.stderr[-800:])
        return None


def stat(output: str | None, key: str) -> str | None:
    """Extract a printed stat of the form  'KEY   <value>'  from output."""
    if output is None:
        return None
    m = re.search(rf'{re.escape(key)}\s*[=:\|]?\s*([0-9.eE+\-]+)', output)
    return m.group(1) if m else None


def fstat(output, key):
    v = stat(output, key)
    return float(v) if v is not None else None


def istat(output, key):
    v = stat(output, key)
    return int(float(v)) if v is not None else None


def parse_excessive_error_rates(output: str | None) -> str | None:
    if output is None:
        return None
    matches = re.findall(
        r"\[(YOKED_(?:COLD|1D)_STORAGE)\] logical-qubit round error rate exceeded threshold:\s*([0-9.eE+\-]+)",
        output,
    )
    if not matches:
        return None
    return "; ".join(f"{block}={rate}" for block, rate in matches)


def human_inst_limit(inst_limit: int) -> str:
    if inst_limit % 1_000_000 == 0:
        return f"{inst_limit // 1_000_000}M"
    if inst_limit % 1_000 == 0:
        return f"{inst_limit // 1_000}K"
    return str(inst_limit)


def eif_mem_binary(label: str, compute_cap: int, inst_limit: int) -> Path:
    limit_tag = human_inst_limit(inst_limit)
    return MEM_FIRSTPASS_DIR / label / f"{label}_c{compute_cap}_{limit_tag}.bin"


def sp_mem_binary(
    label: str,
    policy: str,
    cap: int,
    mld: int,
    compute_cap: int,
    inst_limit: int,
) -> Path:
    limit_tag = human_inst_limit(inst_limit)
    return MEM_SECONDPASS_PREFETCH_DIR / label / f"{label}_{policy}_i{cap}_mld{mld}_c{compute_cap}_{limit_tag}.bin"


############################################################
# Step 1 — compile memory ops
############################################################

def append_row(rows: list[dict], row: dict, writer=None, csv_file=None):
    rows.append(row)
    if writer is not None:
        writer.writerow(row)
        csv_file.flush()


def simulate_firstpass_outputs(
    bm,
    compute_cap: int,
    inst_limit: int,
    eif_mem_accesses,
    rows: list[dict],
    writer=None,
    csv_file=None,
):
    label = bm[2]
    factory_budget = bm[1]
    sim_instructions = bm[4] if len(bm) > 4 else SIM_INSTRUCTIONS
    eif_bin = eif_mem_binary(label, compute_cap, inst_limit)
    if not eif_bin.exists():
        print(f"[SKIP SIM] {label} c={compute_cap}: EIF binary missing")
        return

    common_sim = [
        str(sim_instructions),
        "-a", str(compute_cap),
        "-f", str(factory_budget),
        "-pp", "0",
    ]

    baseline_out = run(
        ["./yoked_simulator", str(eif_bin)] + common_sim + ["--baseline", "1", "-i", "0"],
        f"{label} c={compute_cap}: ideal-memory sim",
    )
    baseline_ipc = fstat(baseline_out, "IPC")
    baseline_miss_rate = fstat(baseline_out, "Miss rate (%) [cold+mplace / total]")
    append_row(rows, {
        "benchmark": label,
        "compute_capacity": compute_cap,
        "eviction_policy": "ideal_memory",
        "intermediate_capacity": 0,
        "min_layer_distance": 0,
        "compile_mem_accesses": eif_mem_accesses,
        "compile_prefetches_inserted": None,
        "compile_cache_hits": None,
        "compile_prefetch_misses": None,
        "compile_prefetch_miss_rate": None,
        "compile_median_candidate_distance": None,
        "ipc": baseline_ipc,
        "sim_cold_miss_rate_pct": baseline_miss_rate,
        "loads_1d_not_ready_pct": None,
        "avg_1d_not_ready_delay_cycles": None,
        "excessive_error_rates": parse_excessive_error_rates(baseline_out),
    }, writer, csv_file)

    eif_sim_out = run(
        ["./yoked_simulator", str(eif_bin)] + common_sim + ["-i", "0"],
        f"{label} c={compute_cap}: EIF yoked sim (no 1D)",
    )
    eif_ipc = fstat(eif_sim_out, "IPC")
    eif_miss_rate = fstat(eif_sim_out, "Miss rate (%) [cold+mplace / total]")
    append_row(rows, {
        "benchmark": label,
        "compute_capacity": compute_cap,
        "eviction_policy": "eif",
        "intermediate_capacity": 0,
        "min_layer_distance": 0,
        "compile_mem_accesses": eif_mem_accesses,
        "compile_prefetches_inserted": None,
        "compile_cache_hits": None,
        "compile_prefetch_misses": None,
        "compile_prefetch_miss_rate": None,
        "compile_median_candidate_distance": None,
        "ipc": eif_ipc,
        "sim_cold_miss_rate_pct": eif_miss_rate,
        "loads_1d_not_ready_pct": None,
        "avg_1d_not_ready_delay_cycles": None,
        "excessive_error_rates": parse_excessive_error_rates(eif_sim_out),
    }, writer, csv_file)


def simulate_secondpass_output(
    bm,
    compute_cap: int,
    second_pass_inst_limit: int,
    policy: str,
    cap: int,
    mld: int,
    compile_stats: dict,
    rows: list[dict],
    writer=None,
    csv_file=None,
):
    label = bm[2]
    factory_budget = bm[1]
    sim_instructions = bm[4] if len(bm) > 4 else SIM_INSTRUCTIONS
    sp_bin = sp_mem_binary(label, policy, cap, mld, compute_cap, second_pass_inst_limit)
    if not sp_bin.exists():
        print(
            f"[SKIP SIM] {label} c={compute_cap} policy={policy} cap={cap} mld={mld}: "
            "singlepass prefetch binary missing"
        )
        return

    common_sim = [
        str(sim_instructions),
        "-a", str(compute_cap),
        "-f", str(factory_budget),
        "-pp", "0",
    ]

    sp_sim_out = run(
        ["./yoked_simulator", str(sp_bin)] + common_sim + ["-i", str(cap)],
        f"{label} c={compute_cap}: SINGLEPASS-PREFETCH sim policy={policy} cap={cap} mld={mld}",
    )
    sp_ipc = fstat(sp_sim_out, "IPC")
    sp_miss_rate = fstat(sp_sim_out, "Miss rate (%) [cold+mplace / total]")
    loads_ready = istat(sp_sim_out, "1D loads: already verified at load time")
    loads_delayed = istat(sp_sim_out, "1D loads: needed to wait for verification")
    avg_delay = fstat(sp_sim_out, "Avg delay: 1D load to non-Clifford ready (cycles)")
    if loads_ready is not None and loads_delayed is not None:
        total_1d = loads_ready + loads_delayed
        loads_not_ready_pct = (100.0 * loads_delayed / total_1d) if total_1d > 0 else None
    else:
        loads_not_ready_pct = None

    append_row(rows, {
        "benchmark": label,
        "compute_capacity": compute_cap,
        "eviction_policy": policy,
        "intermediate_capacity": cap,
        "min_layer_distance": mld,
        "compile_mem_accesses": compile_stats.get("mem_accesses"),
        "compile_prefetches_inserted": compile_stats.get("prefetches_inserted"),
        "compile_cache_hits": compile_stats.get("cache_hits"),
        "compile_prefetch_misses": compile_stats.get("prefetch_misses"),
        "compile_prefetch_miss_rate": compile_stats.get("prefetch_miss_rate"),
        "compile_median_candidate_distance": compile_stats.get("median_candidate_distance"),
        "ipc": sp_ipc,
        "sim_cold_miss_rate_pct": sp_miss_rate,
        "loads_1d_not_ready_pct": loads_not_ready_pct,
        "avg_1d_not_ready_delay_cycles": avg_delay,
        "excessive_error_rates": parse_excessive_error_rates(sp_sim_out),
    }, writer, csv_file)


def compile_benchmark(bm, rows: list[dict], writer=None, csv_file=None):
    raw_file, _factory_budget, label = bm[0], bm[1], bm[2]
    inst_limit = bm[3] if len(bm) > 3 else MS_INST_LIMIT
    second_pass_inst_limit = max(1, inst_limit - SECOND_PASS_INST_LIMIT_DELTA)
    raw_path = RAW_BIN_DIR / raw_file
    if not raw_path.exists():
        print(f"[SKIP] raw binary not found: {raw_path}")
        return

    print(f"\n{'='*70}")
    print(f"[COMPILE] {label}")
    print(f"{'='*70}")

    for compute_cap in COMPUTE_CAPACITIES:
        eif_out = eif_mem_binary(label, compute_cap, inst_limit)
        eif_out.parent.mkdir(parents=True, exist_ok=True)
        if eif_out.exists():
            print(f"[REUSE] {label} c={compute_cap}: using existing first-pass output {eif_out}")
            eif_mem_accesses = None
        else:
            eif_cmd = [
                "./qs_memory_scheduler",
                str(raw_path), str(eif_out),
                "-c", str(compute_cap),
                "-s", "0",
                "-i", str(inst_limit),
                "-pp", "0",
            ]
            eif_output = run(eif_cmd, f"{label} c={compute_cap}: EIF compile")
            eif_mem_accesses = istat(eif_output, "MEMORY_ACCESSES")

            if eif_output is None or not eif_out.exists():
                print(f"[SKIP] {label} c={compute_cap}: EIF compile failed")
                continue

        simulate_firstpass_outputs(
            bm,
            compute_cap,
            inst_limit,
            eif_mem_accesses,
            rows,
            writer=writer,
            csv_file=csv_file,
        )

        stage_a_dummy_out = MEM_SECONDPASS_PREFETCH_DIR / label / f".{label}_c{compute_cap}_stagea_dummy.bin"
        stage_a_dummy_out.parent.mkdir(parents=True, exist_ok=True)
        for cap, mld_list in INTERMEDIATE_MLD_MAP.items():
            for mld in mld_list:
                for policy in SINGLEPASS_PREFETCH_POLICIES:
                    sp_out = sp_mem_binary(label, policy, cap, mld, compute_cap, inst_limit)
                    sp_out.parent.mkdir(parents=True, exist_ok=True)
                    sp_cmd = [
                        "./qs_memory_scheduler",
                        str(raw_path), str(stage_a_dummy_out),
                        "-c", str(compute_cap),
                        "-s", "0",
                        "-i", str(second_pass_inst_limit),
                        "-pp", "0",
                        "--enable-singlepass-prefetch",
                        "--singlepass-prefetch-input", str(eif_out),
                        "--singlepass-prefetch-output-file", str(sp_out),
                        "--singlepass-prefetch-intermediate-capacity", str(cap),
                        "--singlepass-prefetch-min-layer-distance", str(mld),
                        "--singlepass-prefetch-eviction-policy", policy,
                        "--singlepass-prefetch-layer-type", SINGLEPASS_PREFETCH_LAYER_TYPE,
                    ]
                    sp_output = run(
                        sp_cmd,
                        f"{label} c={compute_cap}: SINGLEPASS-PREFETCH policy={policy} cap={cap} mld={mld}",
                    )
                    compile_stats = {
                        "mem_accesses": istat(sp_output, "MEMORY_ACCESSES"),
                        "prefetches_inserted": istat(sp_output, "SP_PREFETCH_PREFETCHES_INSERTED"),
                        "cache_hits": istat(sp_output, "SP_PREFETCH_CACHE_HITS"),
                        "prefetch_misses": istat(sp_output, "SP_PREFETCH_COLD_MEMORY_ACCESSES"),
                        "prefetch_miss_rate": fstat(sp_output, "SP_PREFETCH_MISS_RATE"),
                        "median_candidate_distance": fstat(sp_output, "SP_PREFETCH_MEDIAN_CANDIDATE_DISTANCE"),
                    }
                    simulate_secondpass_output(
                        bm,
                        compute_cap,
                        inst_limit,
                        policy,
                        cap,
                        mld,
                        compile_stats,
                        rows,
                        writer=writer,
                        csv_file=csv_file,
                    )


############################################################
# Step 3 — write CSV
############################################################

FIELDNAMES = [
    "benchmark",
    "compute_capacity",
    "eviction_policy",
    "intermediate_capacity",
    "min_layer_distance",
    # compile
    "compile_mem_accesses",
    "compile_prefetches_inserted",
    "compile_cache_hits",
    "compile_prefetch_misses",
    "compile_prefetch_miss_rate",
    "compile_median_candidate_distance",
    # sim
    "ipc",
    "sim_cold_miss_rate_pct",
    "loads_1d_not_ready_pct",
    "avg_1d_not_ready_delay_cycles",
    "excessive_error_rates",
]


def write_csv(rows: list[dict], path: Path):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    print(f"\nResults written to {path}")


def print_summary(rows: list[dict]):
    """Print a compact summary table grouped by benchmark."""
    print("\n" + "=" * 140)
    print(f"{'Benchmark':<22} {'Policy':<14} {'Cmpt':>4} {'Cap':>4} {'MLD':>4}  "
          f"{'IPC':>8}  {'ColdMiss%':>10}  {'1DNotRdy%':>10}  {'AvgDelay':>10}")
    print("-" * 140)
    for r in rows:
        ipc_s = f"{r['ipc']:.4f}" if r.get("ipc") is not None else "—"
        mr_s  = f"{r['sim_cold_miss_rate_pct']:.1f}" if r.get("sim_cold_miss_rate_pct") is not None else "—"
        nr_s  = f"{r['loads_1d_not_ready_pct']:.1f}" if r.get("loads_1d_not_ready_pct") is not None else "—"
        ad_s  = f"{r['avg_1d_not_ready_delay_cycles']:.1f}" if r.get("avg_1d_not_ready_delay_cycles") is not None else "—"
        print(f"{r['benchmark']:<22} {r.get('eviction_policy', '—'):<14} {r['compute_capacity']:>4} "
              f"{r['intermediate_capacity']:>4} {r['min_layer_distance']:>4}  "
              f"{ipc_s:>8}  {mr_s:>10}  {nr_s:>10}  {ad_s:>10}")
    print("=" * 140)


############################################################
# Main
############################################################

def main():
    os.chdir(BUILD_DIR)
    MEM_DIR.mkdir(parents=True, exist_ok=True)
    MEM_FIRSTPASS_DIR.mkdir(parents=True, exist_ok=True)
    MEM_SECONDPASS_PREFETCH_DIR.mkdir(parents=True, exist_ok=True)

    csv_path = BUILD_DIR / "yoked_prefetch_comparison.csv"
    rows = []

    with open(csv_path, "w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=FIELDNAMES, extrasaction="ignore")
        writer.writeheader()
        csv_file.flush()

        for bm in BENCHMARKS:
            compile_benchmark(bm, rows, writer=writer, csv_file=csv_file)

    print_summary(rows)


if __name__ == "__main__":
    main()
