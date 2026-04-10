/*
 *  author: GitHub Copilot
 *  date:   26 February 2026
 * 
 *  Library implementation for space-optimal yoked memory configuration.
 */

#include "sim/yoked_codes/memory_optimizer.h"
#include <iostream>
#include <cmath>

namespace sim
{
namespace yoked_codes
{

namespace {

constexpr size_t kFixed1DBlockRows = 2;

}  // namespace

////////////////////////////////////////////////////////////
// Error rate functions
////////////////////////////////////////////////////////////

double surface_code_error_rate(size_t d) {
    return std::pow(3.5, -static_cast<double>(d)) / 40.0;
}

double yoked_1d_error_rate(size_t yoke_cycle_rounds, size_t row_length, size_t d) {
    double r = static_cast<double>(yoke_cycle_rounds);
    double n = static_cast<double>(row_length);
    return (r) * (n * n) / (n-2) * std::pow(15.0, -static_cast<double>(d)) / 100.0;
}

double yoked_2d_error_rate(size_t yoke_cycle_rounds, size_t grid_length, size_t d) {
    double r = static_cast<double>(yoke_cycle_rounds);
    double n = static_cast<double>(grid_length);
    return std::pow(r, 3.0) * std::pow(n, 4.0) / ((n-2) * (n-2)-2) * std::pow(150.0, -static_cast<double>(d)) / 50000.0;
}

size_t min_code_distance_for_error_rate(double target_error_rate) {
    for (size_t d = 3; d <= 100; d += 2) {
        if (surface_code_error_rate(d) < target_error_rate) {
            return d;
        }
    }
    return 100;
}

////////////////////////////////////////////////////////////
// Yoke cycle round calculations
////////////////////////////////////////////////////////////

size_t yoked_1d_yoke_cycle_rounds(size_t rows, size_t inner_code_distance) {
    return 2 * (8 * rows + 2) * inner_code_distance;
}

size_t yoked_2d_yoke_cycle_rounds(size_t grid_length, size_t inner_code_distance) {
    return round(1.2 * (25 * grid_length + 4) * inner_code_distance);
}

////////////////////////////////////////////////////////////
// Minimum inner code distance calculations
////////////////////////////////////////////////////////////

size_t yoked_1d_min_inner_distance(size_t rows, size_t row_length, double target_error_rate) {
    for (size_t d_inner = 3; d_inner <= 100; d_inner += 2) {
        size_t yoke_rounds = yoked_1d_yoke_cycle_rounds(rows, d_inner);
        double total_error = yoked_1d_error_rate(yoke_rounds, row_length, d_inner);
        if (total_error < target_error_rate) {
            return d_inner;
        }
    }
    return 100;
}

size_t yoked_2d_min_inner_distance(size_t grid_length, double target_error_rate) {
    for (size_t d_inner = 3; d_inner <= 100; d_inner += 2) {
        size_t yoke_rounds = yoked_2d_yoke_cycle_rounds(grid_length, d_inner);
        double total_error = yoked_2d_error_rate(yoke_rounds, grid_length, d_inner);
        if (total_error < target_error_rate) {
            return d_inner;
        }
    }
    return 100;
}

////////////////////////////////////////////////////////////
// Core optimization functions
////////////////////////////////////////////////////////////

std::map<size_t, OptimalBlock> precompute_optimal_blocks(
    size_t max_logical_qubits,
    size_t effective_code_distance,
    double target_error_rate,
    bool only_2d,
    bool verbose) {
    
    std::map<size_t, OptimalBlock> optimal_blocks;
    
    if (verbose) {
        std::cout << "Precomputing optimal blocks for 1 to " << max_logical_qubits << " logical qubits";
        if (only_2d) {
            std::cout << " (only 2D blocks)";
        }
        std::cout << "...\n";
    }
    
    for (size_t l = 1; l <= max_logical_qubits; ++l) {
        OptimalBlock best;
        
        // Try 1D configurations (skip if only_2d is true)
        if (!only_2d) {
        const size_t rows = kFixed1DBlockRows;
            // row_length >= 2
            for (size_t row_length = 2; rows*row_length <= l + 2; row_length++) {
                size_t actual_logical = rows * (row_length - 1);
                if (actual_logical != l) continue;
                
                size_t d_inner = yoked_1d_min_inner_distance(rows, rows*row_length, target_error_rate);
                size_t yoke_rounds = yoked_1d_yoke_cycle_rounds(rows, d_inner);
                double error_rate = yoked_1d_error_rate(yoke_rounds, rows*row_length, d_inner);
                
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
        }  // end if (!only_2d)
        
        // Try 2D configuration
        for (size_t grid_length = 4; grid_length <= 200; grid_length += 4) {
            size_t actual_logical = (grid_length - 2) * (grid_length - 2) - 2;
            if (actual_logical != l) continue;
            
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
    
    if (verbose) {
        std::cout << "Precomputed " << optimal_blocks.size() << " optimal block configurations.\n";
    }
    
    return optimal_blocks;
}

std::pair<DPState, DPState> find_optimal_config_dp(
    size_t target_logical_qubits,
    const std::map<size_t, OptimalBlock>& optimal_blocks,
    size_t min_1d_size,
    bool verbose) {
    
    if (verbose) {
        std::cout << "Running DP to find optimal configuration...\n";
    }
    
    size_t max_dp_size = target_logical_qubits + 500;
    
    std::vector<DPState> dp(max_dp_size + 1);
    std::vector<DPState> dp_with_1d(max_dp_size + 1);
    
    dp[0].physical_qubits = 0;
    
    for (size_t l = 1; l <= max_dp_size; ++l) {
        for (const auto& [block_logical, block_config] : optimal_blocks) {
            if (block_logical > l) continue;
            
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
            
            // Update min-1D-size constrained DP
            if (block_config.is_1d && block_config.logical_qubits >= min_1d_size) {
                if (dp[remaining].physical_qubits != std::numeric_limits<size_t>::max()) {
                    size_t total_phys = dp[remaining].physical_qubits + block_config.physical_qubits;
                    
                    if (total_phys < dp_with_1d[l].physical_qubits) {
                        dp_with_1d[l].physical_qubits = total_phys;
                        dp_with_1d[l].blocks = dp[remaining].blocks;
                        dp_with_1d[l].blocks.push_back(block_config);
                    }
                }
            }

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
    
    if (verbose) {
        std::cout << "Found optimal configuration with " << best_logical_count 
                  << " logical qubits (target was " << target_logical_qubits << ")\n";
        
        if (min_1d_size > 0) {
            std::cout << "Found optimal configuration with min-1D-size constraint: "
                      << best_logical_count_with_1d
                      << " logical qubits\n";
        }
    }
    
    return {best_config, best_config_with_1d};
}

std::vector<size_t> optimal_physical_qubits_by_target(
    size_t max_target_logical_qubits,
    const std::map<size_t, OptimalBlock>& optimal_blocks,
    size_t min_1d_size,
    bool verbose) {

    constexpr size_t TARGET_OVERSHOOT = 1000;
    const size_t max_dp_size = max_target_logical_qubits + TARGET_OVERSHOOT;
    const size_t INF = std::numeric_limits<size_t>::max();

    if (verbose) {
        std::cout << "Running DP sweep up to " << max_target_logical_qubits
                  << " logical qubits";
        if (min_1d_size > 0) {
            std::cout << " with min_1d_size=" << min_1d_size;
        }
        std::cout << "...\n";
    }

    std::vector<size_t> dp(max_dp_size + 1, INF);
    std::vector<size_t> dp_with_1d(max_dp_size + 1, INF);
    dp[0] = 0;

    for (size_t l = 1; l <= max_dp_size; ++l) {
        for (const auto& [block_logical, block_config] : optimal_blocks) {
            if (block_logical > l) continue;

            const size_t remaining = l - block_logical;

            if (dp[remaining] != INF) {
                dp[l] = std::min(dp[l], dp[remaining] + block_config.physical_qubits);
            }

            if (block_config.is_1d && block_config.logical_qubits >= min_1d_size && dp[remaining] != INF) {
                dp_with_1d[l] = std::min(dp_with_1d[l], dp[remaining] + block_config.physical_qubits);
            }

            if (dp_with_1d[remaining] != INF) {
                dp_with_1d[l] = std::min(dp_with_1d[l], dp_with_1d[remaining] + block_config.physical_qubits);
            }
        }
    }

    const auto& source = (min_1d_size > 0) ? dp_with_1d : dp;

    std::vector<size_t> best_from(source.size(), INF);
    size_t running_best = INF;
    for (size_t l = source.size(); l-- > 0; ) {
        running_best = std::min(running_best, source[l]);
        best_from[l] = running_best;
    }

    if (verbose) {
        std::cout << "Completed DP sweep.\n";
    }

    return best_from;
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

////////////////////////////////////////////////////////////
// High-level API
////////////////////////////////////////////////////////////

DPState optimize_memory_config(
    size_t target_logical_qubits,
    double target_error_rate,
    size_t effective_code_distance,
    size_t min_1d_size,
    bool only_2d,
    bool verbose) {
    
    size_t max_precompute = std::min(target_logical_qubits + 500, size_t(2500));
    auto optimal_blocks = precompute_optimal_blocks(max_precompute, effective_code_distance, target_error_rate, only_2d, verbose);
    
    if (optimal_blocks.empty()) {
        return DPState();
    }
    
    auto [optimal, optimal_with_1d] =
        find_optimal_config_dp(target_logical_qubits, optimal_blocks, min_1d_size, verbose);
    
    return min_1d_size > 0 ? optimal_with_1d : optimal;
}

}  // namespace yoked_codes
}  // namespace sim
