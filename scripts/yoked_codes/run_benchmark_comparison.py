#!/usr/bin/env python3
"""
Benchmark comparison script for yoked architecture.
Runs yoked_simulator with baseline and with optimal memory configuration.
"""

import subprocess
import os
import re
import csv
from pathlib import Path

# Configuration
BUILD_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/simulators/routing-simulator/deps/quicksilver/build")
BENCHMARK_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/simulators/routing-simulator/deps/quicksilver/benchmarks/bin")
MEM_COMPILED_OUTPUT_DIR = Path("/Users/gunagya/Desktop/Research Work/routing-space-sliding/simulators/routing-simulator/deps/quicksilver/benchmarks/bin/mem")
CAPACITY = 4
CYCLE_LIMIT = 1_000_000  # 1M cycles for simulation
COMPILE_MEMORY = False  # Set to False if input files already have memory instructions compiled

# List of benchmarks to process
# Format: (input_file, num_qubits, factory_size, description)
BENCHMARKS = [
    ("shor_modmult_N16777259_a3_pow0.bin", 111, 100000, "shor_rsa24"),
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

def extract_memory_physical_qubits(output):
    """Extract memory physical qubits from simulator output."""
    if output is None:
        return None
    
    # Look for MEMORY_PHYSICAL_QUBITS in output
    match = re.search(r'MEMORY_PHYSICAL_QUBITS[:\s]+([0-9]+)', output, re.IGNORECASE)
    if match:
        return int(match.group(1))
    
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
        baseline_mem_qubits = extract_memory_physical_qubits(baseline_output)
        result_row['baseline_ipc'] = baseline_ipc
        result_row['baseline_mem_qubits'] = baseline_mem_qubits
        print(f"Baseline IPC: {baseline_ipc}, Memory Qubits: {baseline_mem_qubits}")
        
        # Run with optimal memory configuration
        optimal_cmd = [
            "./yoked_simulator",
            str(output_binary),
            str(CYCLE_LIMIT),
            "-a", str(CAPACITY),
            "-f", str(factory_size),
            "--use-optimal-memory-config"
        ]
        
        optimal_output = run_command(optimal_cmd, f"Yoked simulator (optimal memory config)")
        optimal_ipc = extract_ipc(optimal_output)
        optimal_mem_qubits = extract_memory_physical_qubits(optimal_output)
        result_row['optimal_ipc'] = optimal_ipc
        result_row['optimal_mem_qubits'] = optimal_mem_qubits
        print(f"Optimal IPC: {optimal_ipc}, Memory Qubits: {optimal_mem_qubits}")
        
        # Run with only 2D blocks (no 1D intermediate storage)
        only_2d_cmd = [
            "./yoked_simulator",
            str(output_binary),
            str(CYCLE_LIMIT),
            "-a", str(CAPACITY),
            "-f", str(factory_size),
            "--only-2d"
        ]
        
        only_2d_output = run_command(only_2d_cmd, f"Yoked simulator (only 2D blocks)")
        only_2d_ipc = extract_ipc(only_2d_output)
        only_2d_mem_qubits = extract_memory_physical_qubits(only_2d_output)
        result_row['only_2d_ipc'] = only_2d_ipc
        result_row['only_2d_mem_qubits'] = only_2d_mem_qubits
        print(f"Only 2D IPC: {only_2d_ipc}, Memory Qubits: {only_2d_mem_qubits}")
        
        results.append(result_row)
    
    # Step 4: Write results to CSV
    csv_file = "benchmark_comparison.csv"
    print(f"\n{'='*80}")
    print(f"Writing results to {csv_file}")
    print(f"{'='*80}\n")
    
    if results:
        fieldnames = [
            'benchmark', 'input_file', 'num_qubits', 'capacity',
            'baseline_ipc', 'baseline_mem_qubits',
            'optimal_ipc', 'optimal_mem_qubits',
            'only_2d_ipc', 'only_2d_mem_qubits'
        ]
        
        with open(csv_file, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(results)
        
        print(f"Results saved to {csv_file}")
        
        # Print summary table
        print("\nSummary:")
        print("-" * 130)
        print(f"{'Benchmark':<20} {'Baseline IPC':<15} {'Baseline Mem':<15} {'Optimal IPC':<15} {'Optimal Mem':<15} {'Only2D IPC':<15} {'Only2D Mem':<15}")
        print("-" * 130)
        for row in results:
            print(f"{row['benchmark']:<20} {row.get('baseline_ipc', 'N/A')!s:<15} "
                  f"{row.get('baseline_mem_qubits', 'N/A')!s:<15} "
                  f"{row.get('optimal_ipc', 'N/A')!s:<15} "
                  f"{row.get('optimal_mem_qubits', 'N/A')!s:<15} "
                  f"{row.get('only_2d_ipc', 'N/A')!s:<15} "
                  f"{row.get('only_2d_mem_qubits', 'N/A')!s:<15}")
        print("-" * 130)
    else:
        print("No results to write!")

if __name__ == "__main__":
    main()
