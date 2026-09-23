#!/usr/bin/env python3
"""
Extract IPC values for c4_i8 runs from simulation logs.
Creates a CSV with baseline, ideal, cache lru/rri, and prefetch lru/rri IPCs per benchmark.
"""

import os
import re
import csv
from pathlib import Path
from typing import Optional, Dict

# Base path to simulation logs
SIM_LOGS_BASE = Path(__file__).parent.parent / "build" / "yoked_codes_run_all_workloads" / "logs" / "simulate"
CSM_TAG = "csm194"

def extract_ipc(log_path: Path) -> Optional[float]:
    """Extract IPC value from a log file."""
    if not log_path.exists():
        return None
    
    try:
        with open(log_path, 'r') as f:
            for line in f:
                if line.startswith('IPC'):
                    # Extract the numeric value after multiple spaces
                    match = re.search(r'IPC\s+([0-9.]+)', line)
                    if match:
                        return float(match.group(1))
    except Exception as e:
        print(f"Error reading {log_path}: {e}")
    
    return None

def get_benchmarks() -> list:
    """Get list of all benchmarks from firstpass directory."""
    firstpass_dir = SIM_LOGS_BASE / "firstpass"
    if not firstpass_dir.exists():
        raise FileNotFoundError(f"Firstpass directory not found: {firstpass_dir}")
    
    benchmarks = []
    for item in firstpass_dir.iterdir():
        if item.is_dir():
            benchmarks.append(item.name)
    
    return sorted(benchmarks)

def collect_ipc_data() -> list:
    """Collect IPC data for all benchmarks."""
    benchmarks = get_benchmarks()
    results = []
    
    for benchmark in benchmarks:
        benchmark_data = {'benchmark': benchmark}
        
        # Baseline (c4_baseline0_i0_csm194)
        baseline_log = SIM_LOGS_BASE / "firstpass" / benchmark / f"c4_baseline0_i0_{CSM_TAG}.log"
        baseline_ipc = extract_ipc(baseline_log)
        benchmark_data['baseline_ipc'] = baseline_ipc
        
        # Ideal (c4_baseline1_i0_csm194)
        ideal_log = SIM_LOGS_BASE / "firstpass" / benchmark / f"c4_baseline1_i0_{CSM_TAG}.log"
        ideal_ipc = extract_ipc(ideal_log)
        benchmark_data['ideal_ipc'] = ideal_ipc
        
        # Cache LRU (c4_i8_lru_csm194)
        cache_lru_log = SIM_LOGS_BASE / "cache" / benchmark / f"c4_i8_lru_{CSM_TAG}.log"
        cache_lru_ipc = extract_ipc(cache_lru_log)
        benchmark_data['cache_lru_ipc'] = cache_lru_ipc
        
        # Cache RRI (c4_i8_rri_csm194)
        cache_rri_log = SIM_LOGS_BASE / "cache" / benchmark / f"c4_i8_rri_{CSM_TAG}.log"
        cache_rri_ipc = extract_ipc(cache_rri_log)
        benchmark_data['cache_rri_ipc'] = cache_rri_ipc
        
        # Prefetch LRU (c4_i8_lru_mld0_csm194)
        prefetch_lru_log = SIM_LOGS_BASE / "prefetch" / benchmark / f"c4_i8_lru_mld0_{CSM_TAG}.log"
        prefetch_lru_ipc = extract_ipc(prefetch_lru_log)
        benchmark_data['prefetch_lru_ipc'] = prefetch_lru_ipc
        
        # Prefetch RRI (c4_i8_rri_mld0_csm194)
        prefetch_rri_log = SIM_LOGS_BASE / "prefetch" / benchmark / f"c4_i8_rri_mld0_{CSM_TAG}.log"
        prefetch_rri_ipc = extract_ipc(prefetch_rri_log)
        benchmark_data['prefetch_rri_ipc'] = prefetch_rri_ipc
        
        results.append(benchmark_data)
    
    return results

def write_csv(data: list, output_path: Path) -> None:
    """Write collected IPC data to CSV file."""
    fieldnames = [
        'benchmark',
        'baseline_ipc',
        'ideal_ipc',
        'cache_lru_ipc',
        'cache_rri_ipc',
        'prefetch_lru_ipc',
        'prefetch_rri_ipc'
    ]
    
    with open(output_path, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(data)

def main():
    """Main entry point."""
    print(f"Reading logs from: {SIM_LOGS_BASE}")
    
    # Collect data
    data = collect_ipc_data()
    
    # Determine output path
    output_path = Path(__file__).parent / "ipc_c4_i8.csv"
    
    # Write CSV
    write_csv(data, output_path)
    
    print(f"Successfully wrote {len(data)} benchmarks to {output_path}")
    
    # Print summary
    for row in data:
        print(f"{row['benchmark']}: baseline={row['baseline_ipc']}, ideal={row['ideal_ipc']}, "
              f"cache_lru={row['cache_lru_ipc']}, cache_rri={row['cache_rri_ipc']}, "
              f"prefetch_lru={row['prefetch_lru_ipc']}, prefetch_rri={row['prefetch_rri_ipc']}")

if __name__ == '__main__':
    main()
