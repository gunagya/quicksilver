/*
 *  author: GitHub Copilot
 *  date:   25 February 2026
 * 
 *  Space-optimal configuration finder for yoked surface code memory.
 *  Uses dynamic programming with precomputed optimal single-block configurations.
 */

#include <iostream>
#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>
#include <map>

// Error rate calculation for surface codes: 3.5^(-d) / 40
double surface_code_error_rate(size_t d) {
    return std::pow(3.5, -static_cast<double>(d)) / 40.0;
}

// Error calculation for 1D yoked blocks: r * n^2 / (n-2) * 15^(-d) / 100
double yoked_1d_error_rate(size_t yoke_cycle_rounds, size_t row_length, size_t d) {
    double r = static_cast<double>(yoke_cycle_rounds);
    double n = static_cast<double>(row_length);
    return (r) * (n * n) / (n-2) * std::pow(15.0, -static_cast<double>(d)) / 100.0;
}

// Error calculation for 2D yoked blocks: r^3 * n^4 / ((n-2)^2-2) * 150^(-d) / 50000
double yoked_2d_error_rate(size_t yoke_cycle_rounds, size_t grid_length, size_t d) {
    double r = static_cast<double>(yoke_cycle_rounds);
    double n = static_cast<double>(grid_length);
    return std::pow(r, 3.0) * std::pow(n, 4.0) / ((n-2) * (n-2)-2) * std::pow(150.0, -static_cast<double>(d)) / 50000.0;
}

// Find minimum code distance to meet error rate requirement
size_t min_code_distance_for_error_rate(double target_error_rate) {
    for (size_t d = 3; d <= 100; ++d) {
        if (surface_code_error_rate(d) < target_error_rate) {
            return d;
        }
    }
    return 100;
}

// Physical qubit calculation for surface codes
constexpr size_t surface_code_physical_qubit_count(size_t d) {
    return 2 * d * (d + 1);
}

constexpr size_t surface_code_physical_qubit_count(size_t dx, size_t dz) {
    return 2 * dx * (dz + 1);
}

// Calculate yoke cycle rounds for 1D storage
size_t yoked_1d_yoke_cycle_rounds(size_t rows, size_t inner_code_distance) {
    return (8 * rows + 2) * inner_code_distance;
}

// Calculate yoke cycle rounds for 2D storage
size_t yoked_2d_yoke_cycle_rounds(size_t grid_length, size_t inner_code_distance) {
    return (25 * grid_length + 4) * inner_code_distance;
}

// Calculate minimum inner code distance for 1D storage to meet error rate
size_t yoked_1d_min_inner_distance(size_t rows, size_t row_length, double target_error_rate) {
    for (size_t d_inner = 3; d_inner <= 100; ++d_inner) {
        size_t yoke_rounds = yoked_1d_yoke_cycle_rounds(rows, d_inner);
        double total_error = yoked_1d_error_rate(yoke_rounds, row_length, d_inner);
        if (total_error < target_error_rate) {
            return d_inner;
        }
    }
    return 100;
}

// Calculate minimum inner code distance for 2D storage to meet error rate
size_t yoked_2d_min_inner_distance(size_t grid_length, double target_error_rate) {
    for (size_t d_inner = 3; d_inner <= 100; ++d_inner) {
        size_t yoke_rounds = yoked_2d_yoke_cycle_rounds(grid_length, d_inner);
        double total_error = yoked_2d_error_rate(yoke_rounds, grid_length, d_inner);
        if (total_error < target_error_rate) {
            return d_inner;
        }
    }
    return 100;
}

struct OptimalBlock {
    bool is_1d;
    size_t physical_qubits;
    size_t logical_qubits;
    // 1D specific
    size_t rows;
    size_t row_length;
    // 2D specific
    size_t grid_length;
    // Common
    size_t inner_code_distance;
    double achieved_error_rate;
    size_t yoke_cycle_rounds;
    
    OptimalBlock() : physical_qubits(std::numeric_limits<size_t>::max()), logical_qubits(0) {}
};

struct DPState {
    size_t physical_qubits;
    std::vector<OptimalBlock> blocks;
    
    DPState() : physical_qubits(std::numeric_limits<size_t>::max()) {}
};

// Precompute optimal single-block configurations
std::map<size_t, OptimalBlock> precompute_optimal_blocks(
    size_t max_logical_qubits,
    size_t effective_code_distance,
    double target_error_rate) {
    
    std::map<size_t, OptimalBlock> optimal_blocks;
    
    std::cout << "Precomputing optimal blocks for 1 to " << max_logical_qubits << " logical qubits...\n";
    
    for (size_t l = 1; l <= max_logical_qubits; ++l) {
        OptimalBlock best;
        
        // Try 1D configurations
        // Exhaustively search all rows × row_length geometries
        size_t max_rows = std::min(l + 2, size_t(100)); // Reasonable upper bound
        
        for (size_t rows = 1; rows <= max_rows; ++rows) {
            // row_length must be even and >= 4
            for (size_t row_length = 4; row_length <= l + 2; row_length += 2) {
                size_t actual_logical = rows * (row_length - 2);
                if (actual_logical != l) continue;
                
                // This geometry produces exactly l logical qubits
                size_t d_inner = yoked_1d_min_inner_distance(rows, row_length, target_error_rate);
                size_t yoke_rounds = yoked_1d_yoke_cycle_rounds(rows, d_inner);
                double error_rate = yoked_1d_error_rate(yoke_rounds, row_length, d_inner);
                
                size_t phys = surface_code_physical_qubit_count(d_inner) * rows * row_length 
                            + surface_code_physical_qubit_count(effective_code_distance, d_inner) * (rows + row_length)
                            + surface_code_physical_qubit_count(effective_code_distance);
                
                if (phys < best.physical_qubits) {
                    best.is_1d = true;
                    best.physical_qubits = phys;
                    best.logical_qubits = actual_logical;
                    best.rows = rows;
                    best.row_length = row_length;
                    best.inner_code_distance = d_inner;
                    best.achieved_error_rate = error_rate;
                    best.yoke_cycle_rounds = yoke_rounds;
                }
            }
        }
        
        // Try 2D configuration
        // Grid length must be a multiple of 4 and >= 4
        for (size_t grid_length = 4; grid_length <= 200; grid_length += 4) {
            size_t actual_logical = (grid_length - 2) * (grid_length - 2) - 2;
            if (actual_logical != l) continue;
            
            // This grid produces exactly l logical qubits
            size_t d_inner = yoked_2d_min_inner_distance(grid_length, target_error_rate);
            size_t yoke_rounds = yoked_2d_yoke_cycle_rounds(grid_length, d_inner);
            double error_rate = yoked_2d_error_rate(yoke_rounds, grid_length, d_inner);
            
            size_t phys = surface_code_physical_qubit_count(d_inner) * grid_length * grid_length 
                        + 2 * surface_code_physical_qubit_count(effective_code_distance, d_inner) * grid_length
                        + surface_code_physical_qubit_count(effective_code_distance);
            
            if (phys < best.physical_qubits) {
                best.is_1d = false;
                best.physical_qubits = phys;
                best.logical_qubits = actual_logical;
                best.grid_length = grid_length;
                best.inner_code_distance = d_inner;
                best.achieved_error_rate = error_rate;
                best.yoke_cycle_rounds = yoke_rounds;
            }
        }
        
        if (best.physical_qubits != std::numeric_limits<size_t>::max()) {
            optimal_blocks[l] = best;
        }
    }
    
    std::cout << "Precomputed " << optimal_blocks.size() << " optimal block configurations.\n";
    return optimal_blocks;
}

// Dynamic programming to find optimal memory configuration
std::pair<DPState, DPState> find_optimal_config_dp(
    size_t target_logical_qubits,
    const std::map<size_t, OptimalBlock>& optimal_blocks,
    bool require_1d_block = false) {
    
    std::cout << "Running DP to find optimal configuration...\n";
    
    // Compute DP for states beyond target to find configurations with more logical qubits
    // that might use fewer physical qubits
    size_t max_dp_size = target_logical_qubits + 500; // Reasonable margin
    
    // dp[l] = minimum physical qubits to store exactly l logical qubits
    std::vector<DPState> dp(max_dp_size + 1);
    // dp_with_1d[l] = minimum physical qubits to store exactly l logical qubits with at least one 1D block
    std::vector<DPState> dp_with_1d(max_dp_size + 1);
    
    dp[0].physical_qubits = 0; // Base case: 0 logical qubits needs 0 physical qubits
    // dp_with_1d[0] starts as max (can't have 1D block with 0 logical qubits)
    
    for (size_t l = 1; l <= max_dp_size; ++l) {
        // Try adding each precomputed block to achieve l logical qubits
        for (const auto& [block_logical, block_config] : optimal_blocks) {
            if (block_logical > l) continue; // Can't use this block
            
            size_t remaining = l - block_logical;
            
            // Update unconstrained DP
            if (dp[remaining].physical_qubits != std::numeric_limits<size_t>::max()) {
                size_t total_phys = dp[remaining].physical_qubits + block_config.physical_qubits;
                
                if (total_phys < dp[l].physical_qubits) {
                    dp[l].physical_qubits = total_phys;
                    dp[l].blocks = dp[remaining].blocks;
                    dp[l].blocks.push_back(block_config);
                }
            }
            
            // Update 1D-constrained DP
            if (block_config.is_1d) {
                // Adding a 1D block satisfies the constraint
                if (dp[remaining].physical_qubits != std::numeric_limits<size_t>::max()) {
                    size_t total_phys = dp[remaining].physical_qubits + block_config.physical_qubits;
                    
                    if (total_phys < dp_with_1d[l].physical_qubits) {
                        dp_with_1d[l].physical_qubits = total_phys;
                        dp_with_1d[l].blocks = dp[remaining].blocks;
                        dp_with_1d[l].blocks.push_back(block_config);
                    }
                }
            } else {
                // Adding a 2D block, so previous state must already have 1D block
                if (dp_with_1d[remaining].physical_qubits != std::numeric_limits<size_t>::max()) {
                    size_t total_phys = dp_with_1d[remaining].physical_qubits + block_config.physical_qubits;
                    
                    if (total_phys < dp_with_1d[l].physical_qubits) {
                        dp_with_1d[l].physical_qubits = total_phys;
                        dp_with_1d[l].blocks = dp_with_1d[remaining].blocks;
                        dp_with_1d[l].blocks.push_back(block_config);
                    }
                }
            }
        }
    }
    
    // Find the configuration with minimum physical qubits among all states
    // that have at least target_logical_qubits
    DPState best_config;
    DPState best_config_with_1d;
    size_t best_logical_count = 0;
    size_t best_logical_count_with_1d = 0;
    
    for (size_t l = target_logical_qubits; l <= max_dp_size; ++l) {
        if (dp[l].physical_qubits < best_config.physical_qubits) {
            best_config = dp[l];
            best_logical_count = l;
        }
        if (dp_with_1d[l].physical_qubits < best_config_with_1d.physical_qubits) {
            best_config_with_1d = dp_with_1d[l];
            best_logical_count_with_1d = l;
        }
    }
    
    std::cout << "Found optimal configuration with " << best_logical_count 
              << " logical qubits (target was " << target_logical_qubits << ")\n";
    
    if (require_1d_block) {
        std::cout << "Found optimal configuration with 1D constraint: " << best_logical_count_with_1d 
                  << " logical qubits\n";
    }
    
    return {best_config, best_config_with_1d};
}

void print_config(const DPState& config, size_t effective_code_distance, double target_error_rate) {
    std::cout << "\n=== Optimal Memory Configuration ===\n";
    std::cout << "Target error rate per logical qubit per round: " << target_error_rate << "\n";
    std::cout << "Effective code distance: " << effective_code_distance << "\n\n";
    
    size_t num_1d = 0, num_2d = 0;
    size_t total_logical_1d = 0, total_logical_2d = 0;
    size_t total_phys_1d = 0, total_phys_2d = 0;
    
    for (const auto& block : config.blocks) {
        if (block.is_1d) num_1d++;
        else num_2d++;
    }
    
    std::cout << "1D Yoked Storage:\n";
    std::cout << "  Number of blocks: " << num_1d << "\n";
    
    if (num_1d > 0) {
        size_t block_num = 1;
        for (const auto& block : config.blocks) {
            if (!block.is_1d) continue;
            
            std::cout << "\n  Block " << block_num++ << ":\n";
            std::cout << "    Logical qubits: " << block.logical_qubits << "\n";
            std::cout << "    Rows: " << block.rows << "\n";
            std::cout << "    Row length: " << block.row_length << "\n";
            std::cout << "    Inner code distance: " << block.inner_code_distance << "\n";
            std::cout << "    Yoke cycle rounds: " << block.yoke_cycle_rounds << "\n";
            std::cout << "    Achieved error rate: " << block.achieved_error_rate << "\n";
            std::cout << "    Physical qubits: " << block.physical_qubits << "\n";
            
            total_logical_1d += block.logical_qubits;
            total_phys_1d += block.physical_qubits;
        }
        std::cout << "\n  Total physical qubits (1D): " << total_phys_1d << "\n";
        std::cout << "  Total logical qubits (1D): " << total_logical_1d << "\n";
    }
    
    std::cout << "\n2D Yoked Cold Storage:\n";
    std::cout << "  Number of blocks: " << num_2d << "\n";
    
    if (num_2d > 0) {
        size_t block_num = 1;
        for (const auto& block : config.blocks) {
            if (block.is_1d) continue;
            
            std::cout << "\n  Block " << block_num++ << ":\n";
            std::cout << "    Logical qubits: " << block.logical_qubits << "\n";
            std::cout << "    Grid length: " << block.grid_length << "\n";
            std::cout << "    Inner code distance: " << block.inner_code_distance << "\n";
            std::cout << "    Yoke cycle rounds: " << block.yoke_cycle_rounds << "\n";
            std::cout << "    Achieved error rate: " << block.achieved_error_rate << "\n";
            std::cout << "    Physical qubits: " << block.physical_qubits << "\n";
            
            total_logical_2d += block.logical_qubits;
            total_phys_2d += block.physical_qubits;
        }
        std::cout << "\n  Total physical qubits (2D): " << total_phys_2d << "\n";
        std::cout << "  Total logical qubits (2D): " << total_logical_2d << "\n";
    }
    
    size_t total_logical = total_logical_1d + total_logical_2d;
    std::cout << "\nTotal logical qubits: " << total_logical << "\n";
    std::cout << "Total physical qubits: " << config.physical_qubits << "\n";
    std::cout << "Physical/Logical ratio: " 
              << static_cast<double>(config.physical_qubits) / total_logical << "\n";
}

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
    
    // Precompute optimal blocks (extend beyond target to explore larger configurations)
    size_t max_precompute = std::min(target_logical_qubits + 500, size_t(1500));
    auto optimal_blocks = precompute_optimal_blocks(max_precompute, effective_code_distance, target_error_rate);
    
    if (optimal_blocks.empty()) {
        std::cerr << "No valid block configurations found!\n";
        return 1;
    }
    
    // Run DP to find optimal configuration
    auto [optimal, optimal_with_1d] = find_optimal_config_dp(target_logical_qubits, optimal_blocks, require_1d_block);
    
    if (optimal.physical_qubits == std::numeric_limits<size_t>::max()) {
        std::cerr << "Could not find a valid configuration!\n";
        return 1;
    }
    
    if (require_1d_block) {
        if (optimal_with_1d.physical_qubits == std::numeric_limits<size_t>::max()) {
            std::cerr << "Could not find a valid configuration with at least one 1D block!\n";
            return 1;
        }
        
        // Print comparison
        std::cout << "\n========================================\n";
        std::cout << "COMPARISON: Unconstrained vs 1D-Required\n";
        std::cout << "========================================\n";
        
        std::cout << "\nUnconstrained optimal:\n";
        std::cout << "  Physical qubits: " << optimal.physical_qubits << "\n";
        
        std::cout << "\nWith 1D constraint:\n";
        std::cout << "  Physical qubits: " << optimal_with_1d.physical_qubits << "\n";
        
        double overhead_pct = 100.0 * (static_cast<double>(optimal_with_1d.physical_qubits) - 
                                       static_cast<double>(optimal.physical_qubits)) / 
                                       static_cast<double>(optimal.physical_qubits);
        
        std::cout << "\nOverhead from 1D constraint: " << overhead_pct << "%\n";
        
        print_config(optimal_with_1d, effective_code_distance, target_error_rate);
    } else {
        print_config(optimal, effective_code_distance, target_error_rate);
    }
    
    return 0;
}
