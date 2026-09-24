#!/usr/bin/env python3
"""
Extract the percentage of PREFETCHES_INSERTED as a fraction of total memory accesses
(prefetch_inserted + cache_hits + cold_memory_accesses) for c4_i8_lru_mld0 across all benchmarks.
"""

import csv
from extraction_common import parse_args

def extract_prefetch_metrics(log_file):
    """
    Extract prefetch metrics from a log file.
    Returns a dict with the metric values, or None if metrics not found.
    """
    metrics = {
        'prefetches_inserted': None,
        'cache_hits': None,
        'cold_memory_accesses': None,
    }
    
    try:
        with open(log_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line.startswith('SP_PREFETCH_PREFETCHES_INSERTED'):
                    metrics['prefetches_inserted'] = int(line.split()[-1])
                elif line.startswith('SP_PREFETCH_CACHE_HITS'):
                    metrics['cache_hits'] = int(line.split()[-1])
                elif line.startswith('SP_PREFETCH_COLD_MEMORY_ACCESSES'):
                    metrics['cold_memory_accesses'] = int(line.split()[-1])
    except Exception as e:
        print(f"Error reading {log_file}: {e}")
        return None
    
    # Check if all metrics were found
    if all(v is not None for v in metrics.values()):
        return metrics
    return None

def calculate_prefetch_percentage(metrics):
    """
    Calculate the percentage of prefetches inserted relative to total memory accesses.
    """
    total = metrics['prefetches_inserted'] + metrics['cache_hits'] + metrics['cold_memory_accesses']
    if total == 0:
        return 0.0
    return (metrics['prefetches_inserted'] / total) * 100

def main():
    # Base path to log files
    args = parse_args(__doc__)
    log_base_path = args.log_dir / "compile" / "prefetch"
    
    # Find all benchmark directories with c4_i8_lru_mld0.log
    results = []
    
    for log_file in sorted(log_base_path.glob('*/c4_i8_lru_mld0.log')):
        benchmark_name = log_file.parent.name
        if args.benchmarks is not None and benchmark_name not in args.benchmarks:
            continue
        metrics = extract_prefetch_metrics(log_file)
        
        if metrics:
            percentage = calculate_prefetch_percentage(metrics)
            results.append({
                'benchmark': benchmark_name,
                'prefetches_inserted': metrics['prefetches_inserted'],
                'cache_hits': metrics['cache_hits'],
                'cold_memory_accesses': metrics['cold_memory_accesses'],
                'total_memory_accesses': metrics['prefetches_inserted'] + metrics['cache_hits'] + metrics['cold_memory_accesses'],
                'prefetch_percentage': percentage,
            })
    
    # Output results as CSV to stdout and to file
    output_file = args.output_dir / 'prefetch_percentage.csv'
    
    if results:
        # Print to stdout
        print("Prefetch Percentage for c4_i8_lru_mld0 across all benchmarks:")
        print("=" * 100)
        print(f"{'Benchmark':<40} {'Prefetches':<15} {'Cache Hits':<15} {'Cold Access':<15} {'Total':<15} {'Percentage':<12}")
        print("-" * 100)
        
        for r in results:
            print(f"{r['benchmark']:<40} {r['prefetches_inserted']:<15} {r['cache_hits']:<15} {r['cold_memory_accesses']:<15} {r['total_memory_accesses']:<15} {r['prefetch_percentage']:>10.2f}%")
        
        print("-" * 100)
        avg_percentage = sum(r['prefetch_percentage'] for r in results) / len(results)
        print(f"{'Average Percentage':<40} {' ':<15} {' ':<15} {' ':<15} {' ':<15} {avg_percentage:>10.2f}%")
        print()
        
        # Write to CSV file
        with open(output_file, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=['benchmark', 'prefetches_inserted', 'cache_hits', 'cold_memory_accesses', 'total_memory_accesses', 'prefetch_percentage'])
            writer.writeheader()
            writer.writerows(results)
        
        print(f"\nResults saved to: {output_file}")
    else:
        print("No log files found or metrics could not be extracted.")

if __name__ == '__main__':
    main()
