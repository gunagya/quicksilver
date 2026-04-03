/*
 *  author: GitHub Copilot
 *  date:   26 February 2026
 * 
 *  Library header for space-optimal yoked memory configuration.
 *  Provides functions to compute optimal block configurations for yoked storage.
 */

#ifndef SIM_YOKED_CODES_MEMORY_OPTIMIZER_h
#define SIM_YOKED_CODES_MEMORY_OPTIMIZER_h

#include <cmath>
#include <vector>
#include <map>
#include <limits>
#include <algorithm>

namespace sim
{
namespace yoked_codes
{

////////////////////////////////////////////////////////////
// Error rate functions
////////////////////////////////////////////////////////////

// Error rate calculation for surface codes: 3.5^(-d) / 40
double surface_code_error_rate(size_t d);

// Error calculation for 1D yoked blocks: r * n^2 / (n-2) * 15^(-d) / 100
double yoked_1d_error_rate(size_t yoke_cycle_rounds, size_t row_length, size_t d);

// Error calculation for 2D yoked blocks: r^3 * n^4 / ((n-2)^2-2) * 150^(-d) / 50000
double yoked_2d_error_rate(size_t yoke_cycle_rounds, size_t grid_length, size_t d);

// Find minimum code distance to meet error rate requirement
size_t min_code_distance_for_error_rate(double target_error_rate);

////////////////////////////////////////////////////////////
// Physical qubit calculations
////////////////////////////////////////////////////////////

// Physical qubit calculation for surface codes
constexpr size_t surface_code_physical_qubit_count(size_t d) {
    return 2 * d * (d + 1);
}

constexpr size_t surface_code_physical_qubit_count(size_t dx, size_t dz) {
    return 2 * dx * (dz + 1);
}

////////////////////////////////////////////////////////////
// Yoke cycle round calculations
////////////////////////////////////////////////////////////

// Calculate yoke cycle rounds for 1D storage
size_t yoked_1d_yoke_cycle_rounds(size_t rows, size_t inner_code_distance);

// Calculate yoke cycle rounds for 2D storage
size_t yoked_2d_yoke_cycle_rounds(size_t grid_length, size_t inner_code_distance);

////////////////////////////////////////////////////////////
// Minimum inner code distance calculations
////////////////////////////////////////////////////////////

// Calculate minimum inner code distance for 1D storage to meet error rate
size_t yoked_1d_min_inner_distance(size_t rows, size_t row_length, double target_error_rate);

// Calculate minimum inner code distance for 2D storage to meet error rate
size_t yoked_2d_min_inner_distance(size_t grid_length, double target_error_rate);

////////////////////////////////////////////////////////////
// Data structures
////////////////////////////////////////////////////////////

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
    
    OptimalBlock() : is_1d(false), physical_qubits(std::numeric_limits<size_t>::max()), 
                     logical_qubits(0), rows(0), row_length(0), grid_length(0),
                     inner_code_distance(0), achieved_error_rate(0.0), yoke_cycle_rounds(0) {}
};

struct DPState {
    size_t physical_qubits;
    std::vector<OptimalBlock> blocks;
    
    DPState() : physical_qubits(std::numeric_limits<size_t>::max()) {}
};

////////////////////////////////////////////////////////////
// Core optimization functions
////////////////////////////////////////////////////////////

// Precompute optimal single-block configurations
std::map<size_t, OptimalBlock> precompute_optimal_blocks(
    size_t max_logical_qubits,
    size_t effective_code_distance,
    double target_error_rate,
    bool only_2d = false,
    bool verbose = true);

// Dynamic programming to find optimal memory configuration.
// Returns pair of (unconstrained_optimal, min_1d_size_constrained_optimal).
std::pair<DPState, DPState> find_optimal_config_dp(
    size_t target_logical_qubits,
    const std::map<size_t, OptimalBlock>& optimal_blocks,
    size_t min_1d_size = 0,
    bool verbose = true);

// Compute the best physical-qubit count for every target logical-qubit count in
// [0, max_target_logical_qubits], allowing the same overshoot policy as
// optimize_memory_config().
std::vector<size_t> optimal_physical_qubits_by_target(
    size_t max_target_logical_qubits,
    const std::map<size_t, OptimalBlock>& optimal_blocks,
    size_t min_1d_size = 0,
    bool verbose = false);

// Print configuration details
void print_config(const DPState& config, size_t effective_code_distance, double target_error_rate);

////////////////////////////////////////////////////////////
// High-level API
////////////////////////////////////////////////////////////

// Find optimal memory configuration for given parameters.
// If min_1d_size > 0, require at least one 1D block with logical size >= min_1d_size.
// If only_2d is true, only 2D blocks are considered.
DPState optimize_memory_config(
    size_t target_logical_qubits,
    double target_error_rate,
    size_t effective_code_distance,
    size_t min_1d_size = 0,
    bool only_2d = false,
    bool verbose = false);

}  // namespace yoked_codes
}  // namespace sim

#endif  // SIM_YOKED_CODES_MEMORY_OPTIMIZER_h
