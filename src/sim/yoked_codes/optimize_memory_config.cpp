/*
 *  author: GitHub Copilot
 *  date:   25 February 2026
 *
 *  CLI tool for either:
 *    (a) computing one optimal yoked memory configuration for a target number
 *        of logical memory qubits, or
 *    (b) sweeping many targets when --sweep is requested.
 */

#include "sim/yoked_codes/memory_optimizer.h"

#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace sim::yoked_codes;

namespace
{

constexpr size_t SWEEP_MIN_LOGICAL_QUBITS = 100;
constexpr size_t SWEEP_MAX_LOGICAL_QUBITS = 2500;
constexpr std::array<size_t, 5> MIN_1D_SIZES = {4, 8, 16, 24, 32};
constexpr size_t DEFAULT_MIN_1D_BLOCK_LOGICAL_QUBITS = 0;
constexpr size_t DEFAULT_MAX_2D_GRID_LENGTH = 200;

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

void
print_usage(const char* argv0)
{
    std::cerr << "Usage:\n";
    std::cerr << "  " << argv0 << " [--min-1d-size N] [--only-2d] <target_error_rate> <memory_logical_qubits>\n";
    std::cerr << "  " << argv0 << " --sweep <target_error_rate> [output_csv]\n\n";
    std::cerr << "Single-run mode:\n";
    std::cerr << "  target_error_rate:     Target error rate per logical qubit per round (e.g., 1e-6)\n";
    std::cerr << "  memory_logical_qubits: Number of logical memory qubits to optimize for\n\n";
    std::cerr << "Sweep mode:\n";
    std::cerr << "  output_csv:            Optional CSV output path "
                 "(default: optimize_memory_config_sweep.csv)\n";
    std::cerr << "Optional flags:\n";
    std::cerr << "  --min-1d-size N:       Minimum logical size required for a 1D block "
                 "(default: " << DEFAULT_MIN_1D_BLOCK_LOGICAL_QUBITS
              << ", single-run mode only)\n";
    std::cerr << "  --effective-code-distance N: Use a fixed effective code distance instead of deriving it from the target error rate\n";
    std::cerr << "  --max-2d-grid-length N: Maximum 2D grid length considered during optimization "
                 "(default: " << DEFAULT_MAX_2D_GRID_LENGTH << ")\n";
    std::cerr << "  --only-2d:             Restrict single-run mode to 2D blocks only\n";
}

}  // namespace

int
main(int argc, char* argv[])
{
    try
    {
        if (argc < 2)
        {
            print_usage(argv[0]);
            return 1;
        }

        bool do_sweep = false;
        bool only_2d = false;
        bool min_1d_size_overridden = false;
        size_t min_1d_size = DEFAULT_MIN_1D_BLOCK_LOGICAL_QUBITS;
        size_t effective_code_distance_override = 0;
        size_t max_2d_grid_length = DEFAULT_MAX_2D_GRID_LENGTH;
        std::vector<std::string> positional_args;

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--sweep")
            {
                do_sweep = true;
            }
            else if (arg == "--only-2d")
            {
                only_2d = true;
            }
            else if (arg == "--min-1d-size")
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Error: --min-1d-size requires a value\n";
                    print_usage(argv[0]);
                    return 1;
                }
                min_1d_size = std::stoull(argv[++i]);
                min_1d_size_overridden = true;
            }
            else if (arg == "--max-2d-grid-length")
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Error: --max-2d-grid-length requires a value\n";
                    print_usage(argv[0]);
                    return 1;
                }
                max_2d_grid_length = std::stoull(argv[++i]);
            }
            else if (arg == "--effective-code-distance")
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Error: --effective-code-distance requires a value\n";
                    print_usage(argv[0]);
                    return 1;
                }
                effective_code_distance_override = std::stoull(argv[++i]);
            }
            else
            {
                positional_args.push_back(arg);
            }
        }

        if (effective_code_distance_override > 0 && effective_code_distance_override < 3)
        {
            std::cerr << "Error: --effective-code-distance must be at least 3\n";
            print_usage(argv[0]);
            return 1;
        }
        if (max_2d_grid_length < 4)
        {
            std::cerr << "Error: --max-2d-grid-length must be at least 4\n";
            print_usage(argv[0]);
            return 1;
        }

        if (do_sweep)
        {
            if (positional_args.empty() || positional_args.size() > 2)
            {
                print_usage(argv[0]);
                return 1;
            }
            if (min_1d_size_overridden)
            {
                std::cerr << "Error: --min-1d-size is only supported in single-run mode\n";
                print_usage(argv[0]);
                return 1;
            }
            if (only_2d)
            {
                std::cerr << "Error: --only-2d is only supported in single-run mode\n";
                print_usage(argv[0]);
                return 1;
            }

            const double target_error_rate = std::stod(positional_args[0]);
            const std::string output_csv =
                (positional_args.size() == 2) ? positional_args[1] : "optimize_memory_config_sweep.csv";

            std::cout << "Sweeping optimal memory configurations for logical qubits "
                      << SWEEP_MIN_LOGICAL_QUBITS << " to " << SWEEP_MAX_LOGICAL_QUBITS << "...\n";
            std::cout << "Target error rate per logical qubit per round: " << target_error_rate << "\n";

            const size_t effective_code_distance =
                effective_code_distance_override > 0
                    ? effective_code_distance_override
                    : min_code_distance_for_error_rate(target_error_rate);
            if (effective_code_distance_override > 0)
                std::cout << "Fixed effective code distance: " << effective_code_distance << "\n\n";
            else
                std::cout << "Calculated effective code distance: " << effective_code_distance << "\n\n";

            std::cout << "Precomputing mixed 1D/2D optimal blocks once...\n";
            const auto mixed_optimal_blocks = precompute_optimal_blocks(
                SWEEP_MAX_LOGICAL_QUBITS,
                effective_code_distance,
                target_error_rate,
                max_2d_grid_length,
                false,
                false);

            std::cout << "Precomputing 2D-only optimal blocks once...\n";
            const auto only_2d_optimal_blocks = precompute_optimal_blocks(
                SWEEP_MAX_LOGICAL_QUBITS,
                effective_code_distance,
                target_error_rate,
                max_2d_grid_length,
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

            std::cout << "Running DP sweep for min_1d_size=0...\n";
            const auto unconstrained_physical_qubits = optimal_physical_qubits_by_target(
                SWEEP_MAX_LOGICAL_QUBITS,
                mixed_optimal_blocks,
                0,
                false);
            write_constraint_rows(csv, "0", unconstrained_physical_qubits);

            for (size_t sweep_min_1d_size : MIN_1D_SIZES)
            {
                std::cout << "Running DP sweep for min_1d_size=" << sweep_min_1d_size << "...\n";
                const auto physical_qubits_by_target = optimal_physical_qubits_by_target(
                    SWEEP_MAX_LOGICAL_QUBITS,
                    mixed_optimal_blocks,
                    sweep_min_1d_size,
                    false);
                write_constraint_rows(csv, std::to_string(sweep_min_1d_size), physical_qubits_by_target);
            }

            csv.close();

            std::cout << "\nWrote sweep results to " << output_csv << "\n";
            return 0;
        }

        if (positional_args.size() != 2)
        {
            print_usage(argv[0]);
            return 1;
        }

        const double target_error_rate = std::stod(positional_args[0]);
        const size_t memory_logical_qubits = std::stoull(positional_args[1]);
        if (only_2d && min_1d_size_overridden)
        {
            std::cerr << "Error: --only-2d cannot be combined with --min-1d-size\n";
            print_usage(argv[0]);
            return 1;
        }

        std::cout << "Computing optimal memory configuration...\n";
        std::cout << "Target logical memory qubits: " << memory_logical_qubits << "\n";
        std::cout << "Target error rate per logical qubit per round: " << target_error_rate << "\n";

        const size_t effective_code_distance =
            effective_code_distance_override > 0
                ? effective_code_distance_override
                : min_code_distance_for_error_rate(target_error_rate);
        if (effective_code_distance_override > 0)
            std::cout << "Fixed effective code distance: " << effective_code_distance << "\n";
        else
            std::cout << "Calculated effective code distance: " << effective_code_distance << "\n";
        if (only_2d)
            std::cout << "Mode: only 2D blocks\n";
        else
            std::cout << "Constraint: require a 1D block with logical size >= "
                      << min_1d_size << "\n";

        auto optimal_config = optimize_memory_config(
            memory_logical_qubits,
            target_error_rate,
            effective_code_distance,
            only_2d ? 0 : min_1d_size,
            max_2d_grid_length,
            only_2d,
            false);

        if (optimal_config.physical_qubits == std::numeric_limits<size_t>::max())
        {
            std::cerr << "Could not find valid memory configuration!\n";
            return 1;
        }

        print_config(optimal_config, effective_code_distance, target_error_rate);
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n";
        print_usage(argv[0]);
        return 1;
    }
}
