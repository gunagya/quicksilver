/*
 *  author: GitHub Copilot
 *  date:   25 February 2026
 * 
 *  CLI tool for finding space-optimal yoked surface code memory configuration.
 *  Uses dynamic programming with precomputed optimal single-block configurations.
 */

#include "sim/yoked_codes/memory_optimizer.h"
#include <iostream>
#include <string>
#include <limits>

using namespace sim::yoked_codes;

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <logical_qubits> <target_error_rate> [--require-1d]\n";
        std::cerr << "  logical_qubits: Target number of logical qubits in memory\n";
        std::cerr << "  target_error_rate: Target error rate per logical qubit per round (e.g., 1e-6)\n";
        std::cerr << "  --require-1d: (optional) Require at least one 1D yoked block\n";
        return 1;
    }
    
    size_t target_logical_qubits = std::stoull(argv[1]);
    double target_error_rate = std::stod(argv[2]);
    bool require_1d_block = false;
    
    if (argc == 4 && std::string(argv[3]) == "--require-1d") {
        require_1d_block = true;
    }
    
    std::cout << "Finding optimal memory configuration for " << target_logical_qubits 
              << " logical qubits...\n";
    std::cout << "Target error rate per logical qubit per round: " << target_error_rate << "\n";
    if (require_1d_block) {
        std::cout << "Constraint: At least one 1D yoked block required\n";
    }
    
    // Calculate effective code distance from error rate
    size_t effective_code_distance = min_code_distance_for_error_rate(target_error_rate);
    std::cout << "Calculated effective code distance: " << effective_code_distance << "\n\n";
    
    // Find optimal configuration (with optional 1D constraint)
    auto optimal = optimize_memory_config(
        target_logical_qubits,
        target_error_rate,
        effective_code_distance,
        require_1d_block,
        true  // verbose
    );
    
    if (optimal.physical_qubits == std::numeric_limits<size_t>::max()) {
        std::cerr << "Could not find a valid configuration!\n";
        return 1;
    }
    
    // Optionally show comparison with unconstrained solution
    if (require_1d_block) {
        auto unconstrained = optimize_memory_config(
            target_logical_qubits,
            target_error_rate,
            effective_code_distance,
            false,  // no constraint
            false   // not verbose
        );
        
        std::cout << "\n========================================\n";
        std::cout << "COMPARISON: Unconstrained vs 1D-Required\n";
        std::cout << "========================================\n";
        
        std::cout << "\nUnconstrained optimal:\n";
        std::cout << "  Physical qubits: " << unconstrained.physical_qubits << "\n";
        
        std::cout << "\nWith 1D constraint:\n";
        std::cout << "  Physical qubits: " << optimal.physical_qubits << "\n";
        
        if (unconstrained.physical_qubits != std::numeric_limits<size_t>::max()) {
            double overhead_pct = 100.0 * (static_cast<double>(optimal.physical_qubits) - 
                                           static_cast<double>(unconstrained.physical_qubits)) / 
                                           static_cast<double>(unconstrained.physical_qubits);
            std::cout << "\nOverhead from 1D constraint: " << overhead_pct << "%\n";
        }
    }
    
    print_config(optimal, effective_code_distance, target_error_rate);
    
    return 0;
}
