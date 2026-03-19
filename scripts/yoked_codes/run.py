#!/usr/bin/env python3
"""
Benchmark comparison: singlepass prefetch (second pass) vs baseline EIF.

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
    yoked_simulator ... --baseline 1 -i 0   → baseline IPC
    yoked_simulator ... -i 0                → EIF no-prefetch IPC
  For each singlepass-prefetch binary (policy × cap × mld):
    yoked_simulator ... -i <cap>            → IPC, miss rate, prefetch readiness

Step 3 — write CSV.
  Rows:  intermediate capacity
  Columns: min-layer-distance variants, each carrying IPC / miss-rate / ready%,
           with eviction policy as an additional sweep dimension.
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

# For each intermediate storage size, the MLD values to sweep.
# Edit the lists here to control which (cap, mld) pairs are compiled + simulated.
INTERMEDIATE_MLD_MAP: dict[int, list[int]] = {
    4:  [0, 25, 50, 75, 87, 100, 125, 150],
    8:  [0, 75, 87, 100, 112, 125, 150],
    16: [0, 125, 137, 150, 187, 175, 200],
    24: [0, 150, 175, 200, 225, 250],
    32: [0, 100, 150, 200, 250, 300],
}

# Benchmarks: (raw_binary_filename, factory_phys_qubit_budget, label)
# raw_binary_filename is relative to RAW_BIN_DIR.
BENCHMARKS = [
    ("BQ_e_cr2_120_d100_t1M_T5M.xz",                     50_000, "cr2"),
    ("e_h60_121_td_1000by40.bin",                       50_000, "h60"),
    ("BQ_v_c2h4o_ethylene_oxide_240_d100_t1M_T15M.xz",   50_000, "ethylene_oxide"),
    ("BQ_v_hc3h2cn_288_d100_t1M_T63M.xz",                50_000, "hc3h2cn"),
    ("shor_modmult_N16777259_a3_pow0.bin",               50_000, "shor_rsa24", 5_000_000),
    ("sat_grover_schoning_n784_new.bin",                 50_000, "grover_sat"),
]

# Memory-scheduler compile limits (instructions compiled per run)
MS_INST_LIMIT = 5_000_000

############################################################
# Helpers
############################################################

def run(cmd: list[str], desc: str) -> str | None:
    """Run a subprocess and return stdout, or None on failure."""
    print(f"\n>>> {desc}")
    print("    " + " ".join(str(x) for x in cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return r.stdout
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


def eif_mem_binary(label: str, compute_cap: int) -> Path:
    return MEM_FIRSTPASS_DIR / f"{label}_eif_c{compute_cap}.bin"


def sp_mem_binary(label: str, policy: str, cap: int, mld: int, compute_cap: int) -> Path:
    return MEM_SECONDPASS_PREFETCH_DIR / f"{label}_spf_{policy}_i{cap}_mld{mld}_c{compute_cap}.bin"


############################################################
# Step 1 — compile memory ops
############################################################

def compile_benchmarks(benchmarks) -> dict:
    """
    Returns compile_stats[label][compute_cap] = {
        'eif_mem_accesses': int,
        (policy, cap, mld): {
            'mem_accesses': int,
            'prefetches_emitted': int,   # SP_PREFETCH_OPERATIONS
            'prefetch_hits': int,
            'prefetch_misses': int,      # SP_PREFETCH_COLD_MEMORY_ACCESSES
            'prefetch_coverage': float,
            'prefetch_miss_rate': float,
        }
    }
    """
    compile_stats = {}

    for bm in benchmarks:
        raw_file, _factory_budget, label = bm[0], bm[1], bm[2]
        inst_limit = bm[3] if len(bm) > 3 else MS_INST_LIMIT
        raw_path = RAW_BIN_DIR / raw_file
        if not raw_path.exists():
            print(f"[SKIP] raw binary not found: {raw_path}")
            compile_stats[label] = None
            continue

        print(f"\n{'='*70}")
        print(f"[COMPILE] {label}")
        print(f"{'='*70}")

        compile_stats[label] = {}

        for compute_cap in COMPUTE_CAPACITIES:
            entry = {}

            # ── EIF (no prefetch) ──────────────────────────────────────────────
            eif_out = eif_mem_binary(label, compute_cap)
            eif_cmd = [
                "./qs_memory_scheduler",
                str(raw_path), str(eif_out),
                "-c", str(compute_cap),
                "-s", "0",
                "-i", str(inst_limit),
                "-pp", "0",
            ]
            eif_output = run(eif_cmd, f"{label} c={compute_cap}: EIF compile")
            entry["eif_mem_accesses"] = istat(eif_output, "MEMORY_ACCESSES")

            if eif_output is None or not eif_out.exists():
                print(f"[SKIP] {label} c={compute_cap}: EIF compile failed")
                compile_stats[label][compute_cap] = entry
                continue

            # ── SINGLEPASS PREFETCH (second pass) for each (policy, cap, mld) ─
            stage_a_dummy_out = MEM_SECONDPASS_PREFETCH_DIR / f".{label}_c{compute_cap}_stagea_dummy.bin"
            for cap, mld_list in INTERMEDIATE_MLD_MAP.items():
                for mld in mld_list:
                    for policy in SINGLEPASS_PREFETCH_POLICIES:
                        sp_out = sp_mem_binary(label, policy, cap, mld, compute_cap)
                        sp_cmd = [
                            "./qs_memory_scheduler",
                            str(raw_path), str(stage_a_dummy_out),
                            "-c", str(compute_cap),
                            "-s", "0",
                            "-i", str(inst_limit),
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

                        mem_acc      = istat(sp_output, "MEMORY_ACCESSES")
                        sp_ops       = istat(sp_output, "SP_PREFETCH_OPERATIONS")
                        sp_hits      = istat(sp_output, "SP_PREFETCH_HITS")
                        sp_cold      = istat(sp_output, "SP_PREFETCH_COLD_MEMORY_ACCESSES")
                        sp_miss_rate = fstat(sp_output, "SP_PREFETCH_MISS_RATE")
                        pf_cov       = (1.0 - sp_miss_rate) if sp_miss_rate is not None else None

                        entry[(policy, cap, mld)] = {
                            "mem_accesses":       mem_acc,
                            "prefetches_emitted": sp_ops,
                            "prefetch_hits":      sp_hits,
                            "prefetch_misses":    sp_cold,
                            "prefetch_coverage":  pf_cov,
                            "prefetch_miss_rate": sp_miss_rate,
                        }

            compile_stats[label][compute_cap] = entry

    return compile_stats


############################################################
# Step 2 — simulate
############################################################

def simulate_benchmarks(benchmarks, compile_stats) -> list[dict]:
    rows = []

    for bm in benchmarks:
        raw_file, factory_budget, label = bm[0], bm[1], bm[2]
        if compile_stats.get(label) is None:
            print(f"[SKIP SIM] {label}: compile failed")
            continue

        print(f"\n{'='*70}")
        print(f"[SIMULATE] {label}")
        print(f"{'='*70}")

        for compute_cap in COMPUTE_CAPACITIES:
            eif_bin = eif_mem_binary(label, compute_cap)
            if not eif_bin.exists():
                print(f"[SKIP SIM] {label} c={compute_cap}: EIF binary missing")
                continue

            common_sim = [
                str(SIM_INSTRUCTIONS),
                "-a", str(compute_cap),
                "-f", str(factory_budget),
                "-pp", "0",
            ]

            # ── Baseline (STORAGE, no yoked) ──────────────────────────────────
            baseline_out = run(
                ["./yoked_simulator", str(eif_bin)] + common_sim + ["--baseline", "1", "-i", "0"],
                f"{label} c={compute_cap}: baseline sim",
            )
            baseline_ipc     = fstat(baseline_out, "IPC")
            baseline_mem_qub = istat(baseline_out, "MEMORY_PHYSICAL_QUBITS")

            # ── EIF no prefetch (yoked cold storage, -i 0) ────────────────────
            eif_sim_out = run(
                ["./yoked_simulator", str(eif_bin)] + common_sim + ["-i", "0"],
                f"{label} c={compute_cap}: EIF yoked sim (no 1D)",
            )
            eif_ipc       = fstat(eif_sim_out, "IPC")
            eif_mem_qub   = istat(eif_sim_out, "MEMORY_PHYSICAL_QUBITS")
            eif_miss_rate = fstat(eif_sim_out, "Miss rate")

            rows.append({
                "benchmark":              label,
                "raw_file":               raw_file,
                "compute_capacity":       compute_cap,
                "sim_instructions":       SIM_INSTRUCTIONS,
                "scheduler":              "eif_baseline",
                "intermediate_capacity":  0,
                "min_layer_distance":     0,
                # compile stats
                "compile_mem_accesses":      compile_stats[label][compute_cap].get("eif_mem_accesses"),
                "compile_prefetch_coverage": None,
                # sim stats
                "baseline_ipc":             baseline_ipc,
                "baseline_mem_phys_qubits": baseline_mem_qub,
                "ipc":                      eif_ipc,
                "mem_phys_qubits":          eif_mem_qub,
                "sim_miss_rate_pct":        eif_miss_rate,
                "prefetch_ready_on_load_pct": None,
            })

            # ── SINGLEPASS PREFETCH variants ──────────────────────────────────
            for cap, mld_list in INTERMEDIATE_MLD_MAP.items():
                for mld in mld_list:
                    for policy in SINGLEPASS_PREFETCH_POLICIES:
                        sp_bin = sp_mem_binary(label, policy, cap, mld, compute_cap)
                        if not sp_bin.exists():
                            print(
                                f"[SKIP SIM] {label} c={compute_cap} policy={policy} cap={cap} mld={mld}: "
                                "singlepass prefetch binary missing"
                            )
                            continue

                        sp_sim_out = run(
                            ["./yoked_simulator", str(sp_bin)] + common_sim + ["-i", str(cap)],
                            f"{label} c={compute_cap}: SINGLEPASS-PREFETCH sim policy={policy} cap={cap} mld={mld}",
                        )

                        sp_ipc       = fstat(sp_sim_out, "IPC")
                        sp_mem_qub   = istat(sp_sim_out, "MEMORY_PHYSICAL_QUBITS")
                        sp_miss_rate = fstat(sp_sim_out, "Miss rate")

                        # Prefetch readiness: % of 1D loads already verified at load time
                        loads_ready   = istat(sp_sim_out, "1D loads: already verified at load time")
                        loads_delayed = istat(sp_sim_out, "1D loads: needed to wait for verification")
                        if loads_ready is not None and loads_delayed is not None:
                            total_1d = loads_ready + loads_delayed
                            pf_ready_pct = (100.0 * loads_ready / total_1d) if total_1d > 0 else None
                        else:
                            pf_ready_pct = None

                        cap_compile = compile_stats[label][compute_cap].get((policy, cap, mld), {})

                        rows.append({
                            "benchmark":              label,
                            "raw_file":               raw_file,
                            "compute_capacity":       compute_cap,
                            "sim_instructions":       SIM_INSTRUCTIONS,
                            "scheduler":              "singlepass_prefetch",
                            "eviction_policy":        policy,
                            "layer_type":             SINGLEPASS_PREFETCH_LAYER_TYPE,
                            "intermediate_capacity":  cap,
                            "min_layer_distance":     mld,
                            # compile stats
                            "compile_mem_accesses":        cap_compile.get("mem_accesses"),
                            "compile_prefetch_ops":        cap_compile.get("prefetches_emitted"),
                            "compile_prefetch_hits":       cap_compile.get("prefetch_hits"),
                            "compile_prefetch_misses":     cap_compile.get("prefetch_misses"),
                            "compile_prefetch_coverage":   cap_compile.get("prefetch_coverage"),
                            "compile_prefetch_miss_rate":  cap_compile.get("prefetch_miss_rate"),
                            # sim stats
                            "baseline_ipc":             baseline_ipc,
                            "baseline_mem_phys_qubits": baseline_mem_qub,
                            "ipc":                      sp_ipc,
                            "mem_phys_qubits":          sp_mem_qub,
                            "sim_miss_rate_pct":        sp_miss_rate,
                            "prefetch_ready_on_load_pct": pf_ready_pct,
                        })

    return rows


############################################################
# Step 3 — write CSV
############################################################

FIELDNAMES = [
    "benchmark",
    "raw_file",
    "compute_capacity",
    "sim_instructions",
    "scheduler",
    "eviction_policy",
    "layer_type",
    "intermediate_capacity",
    "min_layer_distance",
    # compile
    "compile_mem_accesses",
    "compile_prefetch_ops",
    "compile_prefetch_hits",
    "compile_prefetch_misses",
    "compile_prefetch_coverage",
    "compile_prefetch_miss_rate",
    # sim
    "baseline_ipc",
    "baseline_mem_phys_qubits",
    "ipc",
    "mem_phys_qubits",
    "sim_miss_rate_pct",
    "prefetch_ready_on_load_pct",
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
    print(f"{'Benchmark':<22} {'Sched':<18} {'Policy':<5} {'Cmpt':>4} {'Cap':>4} {'MLD':>4}  "
          f"{'CmpCov%':>8}  {'IPC':>8}  {'MemQub':>8}  "
          f"{'MissRt%':>8}  {'PfReady%':>9}")
    print("-" * 140)
    for r in rows:
        cov = r.get("compile_prefetch_coverage")
        cov_s = f"{cov*100:.1f}" if cov is not None else "—"
        ipc_s = f"{r['ipc']:.4f}" if r.get("ipc") is not None else "—"
        mq_s  = str(r.get("mem_phys_qubits") or "—")
        mr_s  = f"{r['sim_miss_rate_pct']:.1f}" if r.get("sim_miss_rate_pct") is not None else "—"
        pr_s  = f"{r['prefetch_ready_on_load_pct']:.1f}" if r.get("prefetch_ready_on_load_pct") is not None else "—"
        print(f"{r['benchmark']:<22} {r['scheduler']:<18} {r.get('eviction_policy', '—'):<5} {r['compute_capacity']:>4} "
              f"{r['intermediate_capacity']:>4} {r['min_layer_distance']:>4}  "
              f"{cov_s:>8}  {ipc_s:>8}  {mq_s:>8}  {mr_s:>8}  {pr_s:>9}")
    print("=" * 140)


############################################################
# Main
############################################################

def main():
    os.chdir(BUILD_DIR)
    MEM_DIR.mkdir(parents=True, exist_ok=True)
    MEM_FIRSTPASS_DIR.mkdir(parents=True, exist_ok=True)
    MEM_SECONDPASS_PREFETCH_DIR.mkdir(parents=True, exist_ok=True)

    compile_stats = compile_benchmarks(BENCHMARKS)
    rows = simulate_benchmarks(BENCHMARKS, compile_stats)

    csv_path = BUILD_DIR / "yoked_prefetch_comparison.csv"
    write_csv(rows, csv_path)
    print_summary(rows)


if __name__ == "__main__":
    main()
