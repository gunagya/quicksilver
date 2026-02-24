#!/usr/bin/env python3
"""
Benchmark comparison script for yoked architecture with varying intermediate storage sizes.
Runs yoked_simulator with baseline once, then non-baseline with intermediate storage sizes 0, 8, 16, 24.
"""

import subprocess
import os
import re
import csv
from pathlib import Path

# Configuration
BUILD_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/simulators/routing-simulator/deps/quicksilver/build")
BENCHMARK_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/benchmarks")
MEM_COMPILED_OUTPUT_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/simulators/routing-simulator/deps/quicksilver/benchmarks/bin")
CAPACITY = 4
CYCLE_LIMIT = 1_000_000  # 1M cycles for simulation
COMPILE_MEMORY = False  # Set to False if input files already have memory instructions compiled
INTERMEDIATE_STORAGE_SIZES = [0, 8, 16, 24]

# List of benchmarks to process
# Format: (input_file, num_qubits, factory_size, description)
BENCHMARKS = [
    ("compressed/BQ_v_c2h4o_ethylene_oxide_240_d100_t1M_T15M.xz", 241, 100000, "ethylene_oxide"),
    ("compressed/BQ_v_hc3h2cn_288_d100_t1M_T63M.xz", 289, 100000, "hc3h2cn"),
    ("compressed/BQ_e_cr2_120_d100_t1M_T5M.xz", 121, 200000, "cr2"),
    ("binary/e_h60_121_td_1000by40.bin", 121, 250000, "e_h60"),
    ("compressed/BQ_shor_rsa256_iter_4.xz", 514, 2000000, "shor_rsa256"),
    ("binary/sat_grover_schoning_n784_new.bin", 784, 200000, "sat_grover_schoning"),
]

def run_command(cmd, description):
    """Run a command and return stdout."""
    print(f"Running: {description}")
    print(f"Command: {' '.join(cmd)}")
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=True
        )
        return result.stdout
    except subprocess.CalledProcessError as e:
        print(f"Error running {description}")
        print(f"Return code: {e.returncode}")
        print(f"Stdout: {e.stdout}")
        print(f"Stderr: {e.stderr}")
        return None

def extract_ipc(output):
    """Extract IPC from simulator output."""
    if output is None:
        return None
    
    # Look for IPC in output (format: "IPC: X.XX" or similar)
    match = re.search(r'IPC[:\s]+([0-9.]+)', output, re.IGNORECASE)
    if match:
        return float(match.group(1))
    
    # Alternative: look for "instructions per cycle"
    match = re.search(r'([0-9.]+)\s+instructions per cycle', output, re.IGNORECASE)
    if match:
        return float(match.group(1))
    
    return None

def get_benchmark_basename(input_file):
    """Get base name from input file path."""
    # Strip extension(s) and directory
    name = Path(input_file).stem
    # Remove BQ_ prefix if present
    if name.startswith("BQ_"):
        name = name[3:]
    return name

def main():
    os.chdir(BUILD_DIR)
    
    results = []
    
    for input_file, num_qubits, factory_size, description in BENCHMARKS:
        print(f"\n{'='*80}")
        print(f"Processing benchmark: {description}")
        print(f"{'='*80}\n")
        
        input_path = BENCHMARK_DIR / input_file
        if not input_path.exists():
            print(f"Warning: Input file not found: {input_path}")
            continue
        
        basename = get_benchmark_basename(input_file)
        
        result_row = {
            'benchmark': description,
            'input_file': input_file,
            'num_qubits': num_qubits,
            'capacity': CAPACITY,
        }
        
        # EIF scheduler only
        scheduler_name = "eif"
        print(f"\n--- Processing with {scheduler_name.upper()} scheduler ---")
        
        # Generate output binary name
        output_binary = MEM_COMPILED_OUTPUT_DIR / f"{basename}_{scheduler_name}_c{CAPACITY}.bin"
        
        if COMPILE_MEMORY:
            mem_sched_cmd = [
                "./qs_memory_scheduler",
                str(input_path),
                str(output_binary),
                "-c", str(CAPACITY),
                "-s", "0",
                "--hint-lookahead-depth", "512"
            ]
            
            mem_output = run_command(mem_sched_cmd, f"Memory scheduler ({scheduler_name})")
            if mem_output is None:
                print(f"Failed to run memory scheduler for {scheduler_name}")
                results.append(result_row)
                continue
        else:
            if not output_binary.exists():
                print(f"Warning: Expected pre-compiled file not found: {output_binary}")
                results.append(result_row)
                continue
            print(f"Using pre-compiled binary: {output_binary}")
        
        # Run baseline once
        baseline_cmd = [
            "./yoked_simulator",
            str(output_binary),
            str(CYCLE_LIMIT),
            "-a", str(CAPACITY),
            "-f", str(factory_size),
            "--baseline", "1"
        ]
        
        baseline_output = run_command(baseline_cmd, f"Yoked simulator baseline")
        baseline_ipc = extract_ipc(baseline_output)
        result_row['baseline_ipc'] = baseline_ipc
        print(f"Baseline IPC: {baseline_ipc}")
        
        # Run non-baseline with each intermediate storage size
        for i_size in INTERMEDIATE_STORAGE_SIZES:
            yoked_cmd = [
                "./yoked_simulator",
                str(output_binary),
                str(CYCLE_LIMIT),
                "-a", str(CAPACITY),
                "-f", str(factory_size),
                "-i", str(i_size)
            ]
            
            yoked_output = run_command(yoked_cmd, f"Yoked simulator (intermediate={i_size})")
            yoked_ipc = extract_ipc(yoked_output)
            result_row[f'yoked_i{i_size}_ipc'] = yoked_ipc
            print(f"Yoked IPC (intermediate={i_size}): {yoked_ipc}")
        
        results.append(result_row)
    
    # Step 4: Write results to CSV
    csv_file = "benchmark_comparison.csv"
    print(f"\n{'='*80}")
    print(f"Writing results to {csv_file}")
    print(f"{'='*80}\n")
    
    if results:
        fieldnames = [
            'benchmark', 'input_file', 'num_qubits', 'capacity',
            'baseline_ipc',
        ] + [f'yoked_i{s}_ipc' for s in INTERMEDIATE_STORAGE_SIZES]
        
        with open(csv_file, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(results)
        
        print(f"Results saved to {csv_file}")
        
        # Print summary table
        i_cols = ''.join(f"{'i='+str(s):<14}" for s in INTERMEDIATE_STORAGE_SIZES)
        print("\nSummary:")
        print("-" * (48 + 14 * (1 + len(INTERMEDIATE_STORAGE_SIZES))))
        print(f"{'Benchmark':<20} {'Baseline':<14} {i_cols}")
        print("-" * (48 + 14 * (1 + len(INTERMEDIATE_STORAGE_SIZES))))
        for row in results:
            i_vals = ''.join(f"{row.get(f'yoked_i{s}_ipc', 'N/A')!s:<14}" for s in INTERMEDIATE_STORAGE_SIZES)
            print(f"{row['benchmark']:<20} {row.get('baseline_ipc', 'N/A')!s:<14} {i_vals}")
        print("-" * (48 + 14 * (1 + len(INTERMEDIATE_STORAGE_SIZES))))
    else:
        print("No results to write!")

if __name__ == "__main__":
    main()
