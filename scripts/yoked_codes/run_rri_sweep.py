#!/usr/bin/env python3
"""
RRI eviction-policy sweep.

Step 1 — compile memory ops (RRI pass only):
    For each benchmark, reuse precompiled first-pass memory binary
    (benchmarks/bin/mem/firstpass/*_eif_c4.bin), then run RRI placer for each
    intermediate_capacity × eviction_policy combination using --rri-input.

Step 2 — simulate:
  EIF binary         → yoked_simulator ... -i 0   (no 1D storage, cold-only)
  RRI/LRU binary     → yoked_simulator ... -i <cap>
  Collect miss rate from simulator output:
      "Miss rate (%) [cold+mplace / total]"

Step 3 — write CSV.
  One row per (benchmark × intermediate_capacity × eviction_policy).
  Columns: benchmark, active_set_capacity, intermediate_capacity,
           eviction_policy, miss_rate, ipc.
"""

import subprocess
import os
import re
import csv
import argparse
from pathlib import Path

############################################################
# Configuration
############################################################

BUILD_DIR   = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/"
                   "simulators/routing-simulator/deps/quicksilver/build")
RAW_BIN_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/"
                   "simulators/routing-simulator/deps/quicksilver/benchmarks/bin")
MEM_FIRSTPASS_DIR = RAW_BIN_DIR / "mem" / "firstpass"
MEM_SECONDPASS_DIR = RAW_BIN_DIR / "mem" / "secondpass" / "cache"

ACTIVE_SET_CAPACITY = 4
SIM_INSTRUCTIONS    = 100_000_000
INTERMEDIATE_SIZES  = [8]
EVICTION_POLICIES   = ["rri", "lru"]

MS_INST_LIMIT       = 200_000_000

# Benchmarks: (raw_binary_filename, factory_phys_qubit_budget, label)
BENCHMARKS = [
    ("BQ_bose_hubbard_q.xz",                     50_000, "bose_hubbard_q"),
    ("BQ_bose_hubbard_t.xz",                     50_000, "bose_hubbard_t"),
    ("BQ_c2h4o_ethylene_oxide_q.xz",                       50_000, "ethylene_oxide_q"),
    ("BQ_c2h4o_ethylene_oxide_t.xz",                       50_000, "ethylene_oxide_t"),
    ("BQ_chromium_q.xz",                          50_000, "chromium_q"),
    ("BQ_chromium_t.xz",                          50_000, "chromium_t"),
    ("BQ_hc3h2cn_q.xz",                          50_000, "hc3h2cn_q"),
    ("BQ_hc3h2cn_t.xz",                          50_000, "hc3h2cn_t"),
    ("BQ_manganese_nitride_q.xz",              50_000, "manganese_nitride_q"),
    ("BQ_manganese_nitride_t.xz",              50_000, "manganese_nitride_t"),
    # ("shor_modmult_N16777259_a3_pow0.bin",               50_000, "shor_rsa24"),
    ("BQ_grover_3sat_schoning_1710.xz",                 50_000, "grover_3sat")
]

############################################################
# Path helpers
############################################################

def firstpass_mem_binary(label: str) -> Path:
    """Precompiled first-pass EIF output (shared across cap/policy variants)."""
    return MEM_FIRSTPASS_DIR / label / f"{label}_c{ACTIVE_SET_CAPACITY}.bin"


def rri_mem_binary(label: str, cap: int, policy: str) -> Path:
    """Second-pass RRI-placer output for a given capacity + eviction policy."""
    return MEM_SECONDPASS_DIR / label / f"{label}_i{cap}_c{ACTIVE_SET_CAPACITY}_{policy}.bin"


############################################################
# Subprocess helpers
############################################################

def run(cmd: list, desc: str) -> str | None:
    """Run a subprocess; return stdout string or None on failure."""
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
    """Extract  'KEY   <value>'  from output."""
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


############################################################
# Step 1 — compile
############################################################

def compile_all(benchmarks, rebuild_firstpass: bool = False) -> dict:
    """
    Returns:
      compile_stats[label] = {
          "eif_ok": bool,
          (cap, policy): {},
      }

        Strategy:
                        • Reuse first-pass EIF binaries from benchmarks/bin/mem/firstpass
                            (or rebuild them when --rebuild-firstpass is set).
            • For each (cap, policy), run only the RRI placer by providing --rri-input.
            • Write outputs to benchmarks/bin/mem/secondpass/cache/<benchmark>/.
            • If an output already exists, reuse it and skip recompilation.
    """
    stats = {}

    for bm in benchmarks:
        raw_file, _factory, label = bm[0], bm[1], bm[2]
        inst_limit = MS_INST_LIMIT
        raw_path = RAW_BIN_DIR / raw_file

        if not raw_path.exists():
            print(f"[SKIP] raw binary not found: {raw_path}")
            stats[label] = None
            continue

        print(f"\n{'='*70}\n[COMPILE] {label}\n{'='*70}")
        entry: dict = {"eif_ok": False}

        eif_out = firstpass_mem_binary(label)

        # Optional rebuild of first pass (EIF) from raw binary.
        if rebuild_firstpass:
            eif_out.parent.mkdir(parents=True, exist_ok=True)
            eif_cmd = [
                "./qs_memory_scheduler",
                str(raw_path), str(eif_out),
                "-c", str(ACTIVE_SET_CAPACITY),
                "-s", "0",
                "-i", str(inst_limit),
                "-pp", "0",
            ]
            eif_build_out = run(eif_cmd, f"{label}: rebuild firstpass EIF")
            if eif_build_out is None or not eif_out.exists():
                print(f"[SKIP] first-pass rebuild failed: {eif_out}")
                stats[label] = None
                continue

        if not eif_out.exists():
            print(f"[SKIP] first-pass memory binary not found: {eif_out}")
            stats[label] = None
            continue

        entry["eif_ok"] = True

        for cap in INTERMEDIATE_SIZES:
            for policy in EVICTION_POLICIES:
                rri_out = rri_mem_binary(label, cap, policy)
                rri_out.parent.mkdir(parents=True, exist_ok=True)

                if rri_out.exists() and not rebuild_firstpass:
                    print(f"  [REUSE] {rri_out.name}")
                    entry["eif_ok"] = True   # EIF binary must exist if RRI binary does
                    entry[(cap, policy)] = {}
                    continue

                # RRI-only pass, reusing precompiled first-pass memory binary.
                # input-file / output-file are required CLI arguments, but Stage A
                # is skipped when --enable-rri-placer and --rri-input are provided.
                stage_a_dummy_out = MEM_SECONDPASS_DIR / label / f".{label}_stagea_dummy.bin"
                stage_a_dummy_out.parent.mkdir(parents=True, exist_ok=True)
                cmd = [
                    "./qs_memory_scheduler",
                    str(raw_path), str(stage_a_dummy_out),
                    "-c", str(ACTIVE_SET_CAPACITY),
                    "-s", "0",
                    "-i", str(inst_limit),
                    "-pp", "0",
                    "-r",
                    "--rri-input",                 str(eif_out),
                    "--rri-output-file",           str(rri_out),
                    "--rri-intermediate-capacity", str(cap),
                    "--rri-eviction-policy",       policy,
                ]
                run(cmd, f"{label}: RRI cap={cap} policy={policy} (reuse firstpass)")

                entry[(cap, policy)] = {}

        stats[label] = entry

    return stats


def existing_compile_state(benchmarks) -> dict:
    """
    Construct a minimal compile-state view from files already on disk.
    This supports simulation-only reruns without invoking qs_memory_scheduler.
    """
    stats = {}

    for bm in benchmarks:
        _raw_file, _factory, label = bm[0], bm[1], bm[2]
        eif_ok = firstpass_mem_binary(label).exists()
        entry: dict = {"eif_ok": eif_ok}

        for cap in INTERMEDIATE_SIZES:
            for policy in EVICTION_POLICIES:
                if rri_mem_binary(label, cap, policy).exists():
                    entry[(cap, policy)] = {}

        stats[label] = entry if eif_ok else None

    return stats


############################################################
# Step 2 — simulate
############################################################

def simulate_all(benchmarks, compile_stats, csv_path: Path | None = None) -> list[dict]:
    rows = []

    csv_file = None
    writer = None
    if csv_path is not None:
        csv_file = open(csv_path, "w", newline="")
        writer = csv.DictWriter(csv_file, fieldnames=FIELDNAMES, extrasaction="ignore")
        writer.writeheader()
        csv_file.flush()

    try:
        for bm in benchmarks:
            raw_file, factory_budget, label = bm[0], bm[1], bm[2]

            if compile_stats.get(label) is None:
                print(f"[SKIP SIM] {label}: compile failed / raw binary missing")
                continue

            entry = compile_stats[label]
            if not entry.get("eif_ok"):
                print(f"[SKIP SIM] {label}: EIF compile failed")
                continue

            print(f"\n{'='*70}\n[SIMULATE] {label}\n{'='*70}")

            eif_bin = firstpass_mem_binary(label)

            common_sim = [
                str(SIM_INSTRUCTIONS),
                "-a", str(ACTIVE_SET_CAPACITY),
                "-f", str(factory_budget),
                "-pp", "0",
            ]

            # ── EIF baseline (no 1D storage) ──────────────────────────────────
            eif_out = run(
                ["./yoked_simulator", str(eif_bin)] + common_sim + ["-i", "0"],
                f"{label}: EIF sim (no 1D)",
            )
            eif_ipc = fstat(eif_out, "IPC")
            eif_miss_rate = fstat(eif_out, "Miss rate (%) [cold+mplace / total]")

            row = {
                "benchmark":              label,
                "active_set_capacity":    ACTIVE_SET_CAPACITY,
                "intermediate_capacity":  0,
                "eviction_policy":        "eif_baseline",
                "miss_rate":              eif_miss_rate,
                "ipc":                    eif_ipc,
            }
            rows.append(row)
            if writer is not None:
                writer.writerow(row)
                csv_file.flush()

            # ── RRI/LRU variants ──────────────────────────────────────────────
            for cap in INTERMEDIATE_SIZES:
                for policy in EVICTION_POLICIES:
                    rri_bin = rri_mem_binary(label, cap, policy)
                    if not rri_bin.exists():
                        print(f"[SKIP SIM] {label} cap={cap} policy={policy}: binary missing")
                        continue

                    sim_out = run(
                        ["./yoked_simulator", str(rri_bin)] + common_sim + ["-i", str(cap)],
                        f"{label}: RRI sim cap={cap} policy={policy}",
                    )
                    ipc = fstat(sim_out, "IPC")
                    miss_rate = fstat(sim_out, "Miss rate (%) [cold+mplace / total]")
                    row = {
                        "benchmark":              label,
                        "active_set_capacity":    ACTIVE_SET_CAPACITY,
                        "intermediate_capacity":  cap,
                        "eviction_policy":        policy,
                        "miss_rate":              miss_rate,
                        "ipc":                    ipc,
                    }
                    rows.append(row)
                    if writer is not None:
                        writer.writerow(row)
                        csv_file.flush()
    finally:
        if csv_file is not None:
            csv_file.close()

    return rows


############################################################
# Step 3 — write CSV + print summary
############################################################

FIELDNAMES = [
    "benchmark",
    "active_set_capacity",
    "intermediate_capacity",
    "eviction_policy",
    "miss_rate",
    "ipc",
]


def write_csv(rows: list[dict], path: Path):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    print(f"\nResults written to {path}")


def print_summary(rows: list[dict]):
    print("\n" + "=" * 90)
    print(f"{'Benchmark':<22} {'Policy':<14} {'Cap':>4}  "
          f"{'MissRt%':>8}  {'IPC':>8}")
    print("-" * 90)
    for r in rows:
        mr_s  = f"{r['miss_rate']:.4f}"           if r.get("miss_rate")          is not None else "—"
        ipc_s = f"{r['ipc']:.4f}"                 if r.get("ipc")               is not None else "—"
        print(f"{r['benchmark']:<22} {r['eviction_policy']:<14} {r['intermediate_capacity']:>4}  "
              f"{mr_s:>8}  {ipc_s:>8}")
    print("=" * 90)


############################################################
# Main
############################################################

def main():
    parser = argparse.ArgumentParser(description="Run RRI placer sweep on firstpass memory binaries")
    parser.add_argument(
        "--rebuild-firstpass",
        action="store_true",
        help="Rebuild first-pass EIF binaries before running second-pass RRI sweep",
    )
    parser.add_argument(
        "--simulate-only",
        action="store_true",
        help="Skip compilation and rerun simulation using existing firstpass/secondpass binaries",
    )
    args = parser.parse_args()

    os.chdir(BUILD_DIR)
    MEM_FIRSTPASS_DIR.mkdir(parents=True, exist_ok=True)
    MEM_SECONDPASS_DIR.mkdir(parents=True, exist_ok=True)

    csv_path = BUILD_DIR / "rri_sweep_results.csv"
    if args.simulate_only:
        compile_stats = existing_compile_state(BENCHMARKS)
    else:
        compile_stats = compile_all(BENCHMARKS, rebuild_firstpass=args.rebuild_firstpass)

    rows = simulate_all(BENCHMARKS, compile_stats, csv_path=csv_path)
    print_summary(rows)


if __name__ == "__main__":
    main()
