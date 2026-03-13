#!/usr/bin/env python3
"""
Factory-qubit sweep across pre-compiled memory binaries.

Usage
-----
Edit the BINARIES list at the top with the paths to your already-compiled
memory binaries (EIF, singlepass, RRI, LRU, or any mix).  The script infers
simulation parameters directly from each filename, then sweeps across
FACTORY_BUDGETS and runs yoked_simulator for every (binary × factory_budget)
combination.

Filename conventions understood
--------------------------------
  *_c{N}.*          → compute (active-set) capacity -a N
  *_sp{N}_*         → intermediate storage -i N  (singlepass binaries)
  *_cap{N}_*        → intermediate storage -i N  (rri/lru binaries)
  *_eif_*           → intermediate storage -i 0  (EIF / no 1D storage)

If a pattern is not matched the script falls back to the defaults defined in
FALLBACK_COMPUTE_CAP and FALLBACK_INTERMEDIATE_CAP.

Output
------
  build/factory_sweep_results.csv
"""

import subprocess
import re
import csv
import os
from pathlib import Path

############################################################
# Configuration — edit here
############################################################

BUILD_DIR        = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/"
                        "simulators/routing-simulator/deps/quicksilver/build")

SIM_INSTRUCTIONS = 1_000_000
FACTORY_BUDGETS  = [10_000, 25_000, 50_000, 100_000]

# Fallbacks used when the filename does not encode a parameter.
FALLBACK_COMPUTE_CAP      = 4
FALLBACK_INTERMEDIATE_CAP = 0

# List of pre-compiled memory binaries to sweep.
# Paths may be absolute or relative to BUILD_DIR.
# Format: path_string  OR  (path_string, label_override)
BINARIES: list = [
    "../benchmarks/bin/mem/cr2_eif_c8.bin",
    # "../benchmarks/bin/mem/cr2_sp4_mld50_c8.bin",
    # "../benchmarks/bin/mem/cr2_sp8_mld100_c8.bin",
    # "../benchmarks/bin/mem/cr2_sp16_mld175_c8.bin",
    # "../benchmarks/bin/mem/cr2_sp24_mld225_c8.bin",
    # "../benchmarks/bin/mem/cr2_sp32_mld275_c8.bin",

    "../benchmarks/bin/mem/ethylene_oxide_eif_c8.bin",
    # "../benchmarks/bin/mem/ethylene_oxide_sp4_mld50_c8.bin",
    # "../benchmarks/bin/mem/ethylene_oxide_sp8_mld100_c8.bin",
    # "../benchmarks/bin/mem/ethylene_oxide_sp16_mld175_c8.bin",
    # "../benchmarks/bin/mem/ethylene_oxide_sp24_mld225_c8.bin",
    # "../benchmarks/bin/mem/ethylene_oxide_sp32_mld275_c8.bin",

    "../benchmarks/bin/mem/grover_sat_eif_c8.bin",
    # "../benchmarks/bin/mem/grover_sat_sp4_mld50_c8.bin",
    # "../benchmarks/bin/mem/grover_sat_sp8_mld100_c8.bin",
    # "../benchmarks/bin/mem/grover_sat_sp16_mld175_c8.bin",
    # "../benchmarks/bin/mem/grover_sat_sp24_mld225_c8.bin",
    # "../benchmarks/bin/mem/grover_sat_sp32_mld275_c8.bin",

    "../benchmarks/bin/mem/h60_eif_c8.bin",
    # "../benchmarks/bin/mem/h60_sp4_mld50_c8.bin",
    # "../benchmarks/bin/mem/h60_sp8_mld100_c8.bin",
    # "../benchmarks/bin/mem/h60_sp16_mld175_c8.bin",
    # "../benchmarks/bin/mem/h60_sp24_mld225_c8.bin",
    # "../benchmarks/bin/mem/h60_sp32_mld275_c8.bin",

    "../benchmarks/bin/mem/hc3h2cn_eif_c8.bin",
    # "../benchmarks/bin/mem/hc3h2cn_sp4_mld50_c8.bin",
    # "../benchmarks/bin/mem/hc3h2cn_sp8_mld100_c8.bin",
    # "../benchmarks/bin/mem/hc3h2cn_sp16_mld175_c8.bin",
    # "../benchmarks/bin/mem/hc3h2cn_sp24_mld225_c8.bin",
    # "../benchmarks/bin/mem/hc3h2cn_sp32_mld275_c8.bin",

    "../benchmarks/bin/mem/shor_rsa24_eif_c8.bin",
    # "../benchmarks/bin/mem/shor_rsa24_sp4_mld50_c8.bin",
    # "../benchmarks/bin/mem/shor_rsa24_sp8_mld100_c8.bin",
    # "../benchmarks/bin/mem/shor_rsa24_sp16_mld175_c8.bin",
    # "../benchmarks/bin/mem/shor_rsa24_sp24_mld225_c8.bin",
    # "../benchmarks/bin/mem/shor_rsa24_sp32_mld275_c8.bin",
]

############################################################
# Parameter parsing
############################################################

def parse_params(path: Path) -> dict:
    """
    Infer yoked_simulator parameters from the filename.

    Returns a dict with keys:
      label               : human-readable name (stem without extensions)
      compute_cap         : value for -a
      intermediate_cap    : value for -i
      scheduler_hint      : short string describing the scheduler ("eif", "sp", "rri", "lru", …)
    """
    stem = path.name
    # strip all extensions (handles .bin, .xz, .gz, etc.)
    for _ in range(5):
        new = Path(stem).stem
        if new == stem:
            break
        stem = new

    label = stem

    # ── compute capacity: _c{N} at end or before a dot ──────────────────
    m = re.search(r'_c(\d+)$', stem)
    compute_cap = int(m.group(1)) if m else FALLBACK_COMPUTE_CAP

    # ── intermediate capacity ────────────────────────────────────────────
    # singlepass:  _sp{N}_   or   _sp{N} at end
    m_sp = re.search(r'_sp(\d+)(?:_|$)', stem)
    # rri/lru:     _cap{N}_  or   _cap{N} at end
    m_cap = re.search(r'_cap(\d+)(?:_|$)', stem)
    # eif: no 1D storage
    is_eif = bool(re.search(r'_eif_', stem) or stem.endswith('_eif'))

    if m_sp:
        intermediate_cap = int(m_sp.group(1))
        scheduler_hint   = "singlepass"
    elif m_cap:
        intermediate_cap = int(m_cap.group(1))
        # distinguish rri vs lru
        if re.search(r'_lru', stem):
            scheduler_hint = "lru"
        else:
            scheduler_hint = "rri"
    elif is_eif:
        intermediate_cap = 0
        scheduler_hint   = "eif"
    else:
        intermediate_cap = FALLBACK_INTERMEDIATE_CAP
        scheduler_hint   = "unknown"

    # ── mld (informational only, not passed to simulator) ────────────────
    m_mld = re.search(r'_mld(\d+)', stem)
    mld = int(m_mld.group(1)) if m_mld else None

    return {
        "label":            label,
        "compute_cap":      compute_cap,
        "intermediate_cap": intermediate_cap,
        "scheduler_hint":   scheduler_hint,
        "mld":              mld,
    }


############################################################
# Subprocess helpers
############################################################

def run_sim(cmd: list, desc: str) -> str | None:
    print(f"\n>>> {desc}")
    print("    " + " ".join(str(x) for x in cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return r.stdout
    except subprocess.CalledProcessError as e:
        print(f"  FAILED (rc={e.returncode})")
        if e.stdout:
            print("  stdout:", e.stdout[-600:])
        if e.stderr:
            print("  stderr:", e.stderr[-600:])
        return None


def fstat(output: str | None, key: str) -> float | None:
    if output is None:
        return None
    m = re.search(rf'{re.escape(key)}\s*[=:|\s]\s*([0-9.eE+\-]+)', output)
    return float(m.group(1)) if m else None


def istat(output: str | None, key: str) -> int | None:
    v = fstat(output, key)
    return int(v) if v is not None else None


############################################################
# Main sweep
############################################################

FIELDNAMES = [
    "binary",
    "label",
    "scheduler_hint",
    "compute_cap",
    "intermediate_cap",
    "mld",
    "factory_budget",
    "baseline_ipc",
    "ipc",
    "mem_phys_qubits",
    "sim_miss_rate_pct",
]


def main():
    os.chdir(BUILD_DIR)

    if not BINARIES:
        print("ERROR: BINARIES list is empty — add paths to compiled binaries at the top of this script.")
        return

    rows = []

    for entry in BINARIES:
        # Support plain string or (path, label_override) tuple
        if isinstance(entry, (str, Path)):
            raw_path = Path(entry)
            label_override = None
        else:
            raw_path, label_override = entry[0], entry[1]
            raw_path = Path(raw_path)

        # Resolve relative paths against BUILD_DIR
        if not raw_path.is_absolute():
            raw_path = BUILD_DIR / raw_path

        if not raw_path.exists():
            print(f"[SKIP] binary not found: {raw_path}")
            continue

        params = parse_params(raw_path)
        if label_override:
            params["label"] = label_override

        print(f"\n{'='*70}")
        print(f"[BINARY]  {raw_path.name}")
        print(f"  label={params['label']}  scheduler={params['scheduler_hint']}"
              f"  compute_cap={params['compute_cap']}  intermediate_cap={params['intermediate_cap']}"
              + (f"  mld={params['mld']}" if params["mld"] is not None else ""))
        print(f"{'='*70}")

        is_eif = params["scheduler_hint"] == "eif"

        for factory in FACTORY_BUDGETS:
            base_cmd = [
                "./yoked_simulator",
                str(raw_path),
                str(SIM_INSTRUCTIONS),
                "-a", str(params["compute_cap"]),
                "-f", str(factory),
                "-pp", "0",
            ]

            # Baseline run (--baseline 1, no 1D storage) — EIF binaries only.
            baseline_ipc = None
            if is_eif:
                baseline_out = run_sim(
                    base_cmd + ["--baseline", "1", "-i", "0"],
                    f"{params['label']}  factory={factory:,}  [baseline]",
                )
                baseline_ipc = fstat(baseline_out, "IPC")

            # Yoked run.
            out = run_sim(
                base_cmd + ["-i", str(params["intermediate_cap"])],
                f"{params['label']}  factory={factory:,}",
            )

            ipc      = fstat(out, "IPC")
            mem_qub  = istat(out, "MEMORY_PHYSICAL_QUBITS")
            miss_pct = fstat(out, "Miss rate")

            rows.append({
                "binary":            raw_path.name,
                "label":             params["label"],
                "scheduler_hint":    params["scheduler_hint"],
                "compute_cap":       params["compute_cap"],
                "intermediate_cap":  params["intermediate_cap"],
                "mld":               params["mld"],
                "factory_budget":    factory,
                "baseline_ipc":      baseline_ipc,
                "ipc":               ipc,
                "mem_phys_qubits":   mem_qub,
                "sim_miss_rate_pct": miss_pct,
            })

    # ── Write CSV ─────────────────────────────────────────────────────────
    csv_path = BUILD_DIR / "factory_sweep_results.csv"
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    print(f"\nResults written to {csv_path}")

    # ── Print summary ─────────────────────────────────────────────────────
    print("\n" + "=" * 125)
    print(f"{'Label':<30} {'Sched':<12} {'Cmpt':>4} {'1D':>4} {'Factory':>8}  "
          f"{'BaseIPC':>8}  {'IPC':>8}  {'MemQub':>8}  {'MissRt%':>8}")
    print("-" * 125)
    for r in rows:
        bipc_s = f"{r['baseline_ipc']:.4f}"     if r.get("baseline_ipc")      is not None else "—"
        ipc_s  = f"{r['ipc']:.4f}"              if r.get("ipc")               is not None else "—"
        mq_s   = str(r.get("mem_phys_qubits") or "—")
        mr_s   = f"{r['sim_miss_rate_pct']:.1f}" if r.get("sim_miss_rate_pct") is not None else "—"
        print(f"{r['label']:<30} {r['scheduler_hint']:<12} {r['compute_cap']:>4} "
              f"{r['intermediate_cap']:>4} {r['factory_budget']:>8,}  "
              f"{bipc_s:>8}  {ipc_s:>8}  {mq_s:>8}  {mr_s:>8}")
    print("=" * 125)


if __name__ == "__main__":
    main()
