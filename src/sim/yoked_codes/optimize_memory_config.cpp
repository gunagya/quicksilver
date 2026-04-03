/*
 *  author: GitHub Copilot
 *  date:   25 February 2026
 *
 *  CLI tool for sweeping space-optimal yoked surface code memory configurations.
 *  Precomputes optimal single-block configurations once, then reuses them across
 *  all target logical-qubit counts and constraint settings.
 */

#include "sim/yoked_codes/memory_optimizer.h"

#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using namespace sim::yoked_codes;

namespace
{

constexpr size_t SWEEP_MIN_LOGICAL_QUBITS = 100;
constexpr size_t SWEEP_MAX_LOGICAL_QUBITS = 2500;
constexpr std::array<size_t, 5> MIN_1D_SIZES = {4, 8, 16, 24, 32};

void
write_constraint_rows(std::ofstream&           csv,
                      const std::string&       min_1d_size_label,
                      const std::vector<size_t>& physical_qubits_by_target)
{
    for (size_t logical_qubits = SWEEP_MIN_LOGICAL_QUBITS;
         logical_qubits <= SWEEP_MAX_LOGICAL_QUBITS;
         ++logical_qubits)
    {
        const size_t physical_qubits = physical_qubits_by_target[logical_qubits];
        csv << min_1d_size_label << "," << logical_qubits << ",";
        if (physical_qubits == std::numeric_limits<size_t>::max())
            csv << "";
        else
            csv << physical_qubits;
        csv << "\n";
    }
}

}  // namespace

int
main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "Usage: " << argv[0] << " <target_error_rate> [output_csv]\n";
        std::cerr << "  target_error_rate: Target error rate per logical qubit per round (e.g., 1e-6)\n";
        std::cerr << "  output_csv:        Optional CSV output path "
                     "(default: optimize_memory_config_sweep.csv)\n";
        return 1;
    }

    const double target_error_rate = std::stod(argv[1]);
    const std::string output_csv = (argc == 3) ? argv[2] : "optimize_memory_config_sweep.csv";

    std::cout << "Sweeping optimal memory configurations for logical qubits "
              << SWEEP_MIN_LOGICAL_QUBITS << " to " << SWEEP_MAX_LOGICAL_QUBITS << "...\n";
    std::cout << "Target error rate per logical qubit per round: " << target_error_rate << "\n";

    const size_t effective_code_distance = min_code_distance_for_error_rate(target_error_rate);
    std::cout << "Calculated effective code distance: " << effective_code_distance << "\n\n";

    std::cout << "Precomputing mixed 1D/2D optimal blocks once...\n";
    const auto mixed_optimal_blocks = precompute_optimal_blocks(
        SWEEP_MAX_LOGICAL_QUBITS,
        effective_code_distance,
        target_error_rate,
        false,
        false);

    std::cout << "Precomputing 2D-only optimal blocks once...\n";
    const auto only_2d_optimal_blocks = precompute_optimal_blocks(
        SWEEP_MAX_LOGICAL_QUBITS,
        effective_code_distance,
        target_error_rate,
        true,
        false);

    if (mixed_optimal_blocks.empty() || only_2d_optimal_blocks.empty())
    {
        std::cerr << "Could not precompute valid block configurations!\n";
        return 1;
    }

    std::ofstream csv{output_csv};
    if (!csv)
    {
        std::cerr << "Could not open output CSV: " << output_csv << "\n";
        return 1;
    }

    csv << "min_1d_size,logical_qubits,physical_qubits\n";

    std::cout << "Running DP sweep for only-2D storage...\n";
    const auto only_2d_physical_qubits = optimal_physical_qubits_by_target(
        SWEEP_MAX_LOGICAL_QUBITS,
        only_2d_optimal_blocks,
        0,
        false);
    write_constraint_rows(csv, "only_2d", only_2d_physical_qubits);

    for (size_t min_1d_size : MIN_1D_SIZES)
    {
        std::cout << "Running DP sweep for min_1d_size=" << min_1d_size << "...\n";
        const auto physical_qubits_by_target = optimal_physical_qubits_by_target(
            SWEEP_MAX_LOGICAL_QUBITS,
            mixed_optimal_blocks,
            min_1d_size,
            false);
        write_constraint_rows(csv, std::to_string(min_1d_size), physical_qubits_by_target);
    }

    csv.close();

    std::cout << "\nWrote sweep results to " << output_csv << "\n";
    return 0;
}
