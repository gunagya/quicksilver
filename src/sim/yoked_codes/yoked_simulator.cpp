/*
 *  author: Suhas Vittal
 *  date:   16 January 2026
 * */

#include "globals.h"
#include "sim.h"
#include "sim/configuration/resource_estimation.h"
#include "sim/configuration/allocator.h"
#include "sim/yoked_codes/yoked_1d_storage.h"
#include "sim/yoked_codes/yoked_architecture.h"
#include "sim/yoked_codes/yoked_cold_storage.h"
#include "sim/yoked_codes/memory_optimizer.h"
#include "sim/memory_subsystem.h"
#include "sim/factory.h"

#include "compiler/memory_scheduler.h"
#include "compiler/memory_scheduler/impl.h"

#include "argparse.h"

#include <algorithm>
#include <sys/stat.h>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace 
{

constexpr size_t COMPUTE_CODE_DISTANCE{25};
constexpr size_t MEMORY_CODE_DISTANCE{25};
constexpr double TARGET_MEMORY_ERROR_RATE{1e-15};

constexpr uint64_t compute_syndrome_extraction_round_time_ns = 1200;
constexpr uint64_t memory_syndrome_extraction_round_time_ns = 1200;

/*
 * Compiles the given trace by performing memory access scheduler. The `trace`
 * reference is then overwritten with the new trace.
 * */
void jit_compile(std::string& trace, int64_t inst_sim, int64_t active_set_capacity);

/*
 * Retrieves the number of qubits for the given trace:
 * */
size_t get_number_of_qubits(std::string_view);
bool has_option(int argc, char* argv[], std::string_view option_name);

} // anon

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

int
main(int argc, char* argv[])
{
    std::string trace_file;
    int64_t     inst_sim;

    int64_t print_progress;
    bool    jit;
    int64_t    baseline;
    bool    use_optimal_memory_config;
    bool    only_2d;

    int64_t compute_local_memory_capacity;
    int64_t intermediate_storage_capacity;
    int64_t cold_storage_memory_block_capacity;
    int64_t cold_storage_inner_code_distance;

    int64_t factory_l2_buffer_capacity;
    int64_t factory_physical_qubit_budget;
    ARGPARSE()
        .required("trace file", "Path to trace file", trace_file)
        .required("simulation instructions", "Number of instructions to simulate", inst_sim)

        .optional("-pp", "--print-progress", "Progress print frequency (in compute cycles)", print_progress, 0)
        .optional("-jit", "", "Just-in-time compilation for an input source file", jit, false)
        .optional("", "--baseline", "Use baseline STORAGE instead of YOKED_COLD_STORAGE for memory blocks", baseline, 0)
        .optional("", "--use-optimal-memory-config", "Use optimal memory configuration (ignores -i flag)", use_optimal_memory_config, false)
        .optional("", "--only-2d", "Use optimal memory configuration with only 2D blocks (no 1D blocks)", only_2d, false)

        .optional("-a", "--compute-local-memory-capacity", "Number of active qubits in the compute subsystem's local memory", 
                      compute_local_memory_capacity, 12)
        .optional("-i", "--intermediate-storage-capacity", "Number of qubits in intermediate (1D) storage (0 = no 1D storage)",
                      intermediate_storage_capacity, 0)
        .optional("", "--cold-storage-memory-block-capacity",
                      "Logical qubit capacity per cold-storage block for manual yoked configs",
                      cold_storage_memory_block_capacity, 194)
        .optional("", "--cold-storage-inner-code-distance",
                      "Inner code distance for cold-storage blocks in manual yoked configs",
                      cold_storage_inner_code_distance, 11)

        .optional("", "--factory-l2-buffer-capacity", "Number of magic states stored in an L2 factory buffer",
                      factory_l2_buffer_capacity, 4)
        .optional("-f", "--factory-physical-qubit-budget", "Number of physical qubits allocated to factory allocator", 
                      factory_physical_qubit_budget, 50000)

        .parse(argc, argv);

    /* JIT compilation if needed */

    if (jit)
        jit_compile(trace_file, inst_sim, compute_local_memory_capacity);

    /* initialize magic state factories */

    sim::configuration::FACTORY_SPECIFICATION l1_spec
    {
        .is_cultivation=true,
        .syndrome_extraction_round_time_ns=compute_syndrome_extraction_round_time_ns,
        .buffer_capacity=1,
        .output_error_rate=1e-6,
        .escape_distance=13,
        .round_length=18,
        .probability_of_success=0.2
    };

    sim::configuration::FACTORY_SPECIFICATION l2_spec
    {
        .is_cultivation=false,
        .syndrome_extraction_round_time_ns=compute_syndrome_extraction_round_time_ns,
        .buffer_capacity=factory_l2_buffer_capacity,
        .output_error_rate=1e-12,
        .dx=25,
        .dz=11,
        .dm=11,
        .input_count=4,
        .output_count=1,
        .rotations=11
    };

    sim::configuration::FACTORY_ALLOCATION alloc =
        sim::configuration::throughput_aware_factory_allocation(factory_physical_qubit_budget, l1_spec, l2_spec);

    /* initialize memory subsystem */

    const bool cold_storage_memory_block_capacity_explicit =
        has_option(argc, argv, "--cold-storage-memory-block-capacity");
    if (cold_storage_memory_block_capacity <= 0) {
        std::cerr << "ERROR: --cold-storage-memory-block-capacity must be positive\n";
        return 1;
    }
    if (cold_storage_inner_code_distance <= 0) {
        std::cerr << "ERROR: --cold-storage-inner-code-distance must be positive\n";
        return 1;
    }

    // determine number of qubits for trace:
    size_t total_qubits = get_number_of_qubits(trace_file);
    size_t main_memory_qubits = total_qubits - compute_local_memory_capacity;
    const double m_freq_khz = sim::compute_freq_khz(MEMORY_CODE_DISTANCE * memory_syndrome_extraction_round_time_ns);
    std::vector<sim::STORAGE*> memory_blocks;
    size_t num_blocks;

    if (baseline) {
        // Baseline: use standard STORAGE class
        const int memory_block_capacity = 200;
        num_blocks = main_memory_qubits == 0 ? 0 : (main_memory_qubits-1) / memory_block_capacity + 1;
        memory_blocks.resize(num_blocks);

        for (size_t i = 0; i < num_blocks; i++)
        {
            memory_blocks[i] = new sim::STORAGE(m_freq_khz, 
                                            /*memory_block_physical_qubits=*/0,
                                            memory_block_capacity,
                                            MEMORY_CODE_DISTANCE,
                                            1, // num adapters
                                            8, // load latency
                                            4 // store latency
                                            );
        }
    } else if (use_optimal_memory_config || only_2d) {
        // Yoked: use optimal memory configuration
        constexpr size_t MIN_1D_BLOCK_LOGICAL_QUBITS = 8;

        std::cout << "\n=== Computing Optimal Memory Configuration ===\n";
        std::cout << "Target logical qubits: " << main_memory_qubits << "\n";
        std::cout << "Target error rate: " << TARGET_MEMORY_ERROR_RATE << "\n";
        std::cout << "Effective code distance: " << MEMORY_CODE_DISTANCE << "\n";
        if (only_2d) {
            std::cout << "Mode: Only 2D blocks (no 1D blocks)\n";
        } else {
            std::cout << "Mode: Require a 1D block with logical size >= "
                      << MIN_1D_BLOCK_LOGICAL_QUBITS << "\n";
        }
        std::cout << "\n";
        
        auto optimal_config = sim::yoked_codes::optimize_memory_config(
            main_memory_qubits,
            TARGET_MEMORY_ERROR_RATE,
            MEMORY_CODE_DISTANCE,
            only_2d ? 0 : MIN_1D_BLOCK_LOGICAL_QUBITS,
            only_2d,   // only_2d
            false      // verbose
        );
        
        if (optimal_config.physical_qubits == std::numeric_limits<size_t>::max()) {
            std::cerr << "ERROR: Could not find valid memory configuration!\n";
            return 1;
        }
        
        // Count and validate blocks
        size_t num_1d_blocks = 0;
        size_t max_1d_logical_qubits = 0;
        for (const auto& block : optimal_config.blocks) {
            if (block.is_1d) {
                num_1d_blocks++;
                max_1d_logical_qubits = std::max(max_1d_logical_qubits, block.logical_qubits);
            }
        }
        
        if (use_optimal_memory_config && max_1d_logical_qubits < MIN_1D_BLOCK_LOGICAL_QUBITS) {
            std::cerr << "ERROR: Expected at least one 1D block with logical size >= "
                      << MIN_1D_BLOCK_LOGICAL_QUBITS
                      << ", found maximum 1D block size "
                      << max_1d_logical_qubits << "\n";
            return 1;
        }
        
        if (only_2d && num_1d_blocks != 0) {
            std::cerr << "ERROR: Expected no 1D blocks with --only-2d flag, found " << num_1d_blocks << "\n";
            return 1;
        }
        
        num_blocks = optimal_config.blocks.size();
        memory_blocks.resize(num_blocks);
        
        std::cout << "=== Optimal Configuration ===\n";
        std::cout << "Total blocks: " << num_blocks << "\n";
        std::cout << "Total physical qubits: " << optimal_config.physical_qubits << "\n\n";
        
        // Create memory blocks from optimal configuration
        size_t idx = 0;
        for (auto block : optimal_config.blocks) {
            if (block.is_1d) {
                std::cout << "  Creating 1D block: " << block.logical_qubits << " logical qubits, "
                          << block.rows << " rows, " << block.row_length << " row_length, "
                          << "d_inner=" << block.inner_code_distance << "\n";
                          
                memory_blocks[idx++] = new sim::YOKED_1D_STORAGE(
                    m_freq_khz,
                    block.rows,
                    block.logical_qubits,
                    block.inner_code_distance,
                    MEMORY_CODE_DISTANCE
                );
            } else {
                std::cout << "  Creating 2D block: " << block.logical_qubits << " logical qubits, "
                          << "grid_length=" << block.grid_length << ", "
                          << "d_inner=" << block.inner_code_distance << "\n";
                          
                memory_blocks[idx++] = new sim::YOKED_COLD_STORAGE(
                    m_freq_khz,
                    block.logical_qubits,
                    block.inner_code_distance,
                    MEMORY_CODE_DISTANCE
                );
            }
        }
        
        std::cout << "==============================\n\n";
    } else {
        // Yoked: use YOKED_COLD_STORAGE class, with optional 1D intermediate storage (manual config)
        const size_t memory_block_capacity = static_cast<size_t>(cold_storage_memory_block_capacity);
        size_t cold_storage_qubits = main_memory_qubits;
        size_t has_1d_storage = 0;

        if (intermediate_storage_capacity > 0) {
            cold_storage_qubits -= intermediate_storage_capacity;
            has_1d_storage = 1;
        }

        size_t num_cold_blocks = cold_storage_qubits == 0 ? 0 : (cold_storage_qubits - 1) / memory_block_capacity + 1;
        num_blocks = has_1d_storage + num_cold_blocks;
        memory_blocks.resize(num_blocks);

        size_t idx = 0;
        if (has_1d_storage) {
            memory_blocks[idx++] = new sim::YOKED_1D_STORAGE(m_freq_khz,
                                    2, // rows
                                    intermediate_storage_capacity,
                                    15, // inner code distance
                                    MEMORY_CODE_DISTANCE);
        }
        for (size_t i = idx; i < num_blocks; i++) {
            size_t block_capacity = memory_block_capacity;
            if (!cold_storage_memory_block_capacity_explicit) {
                const size_t block_index = i - idx;
                const size_t assigned_qubits = block_index * memory_block_capacity;
                const size_t remaining_cold_qubits = cold_storage_qubits - assigned_qubits;
                block_capacity = std::min(remaining_cold_qubits, memory_block_capacity);
            }
            memory_blocks[i] = new sim::YOKED_COLD_STORAGE(m_freq_khz,
                                                block_capacity,
                                                static_cast<size_t>(cold_storage_inner_code_distance),
                                                MEMORY_CODE_DISTANCE);
        }
    }   

    sim::MEMORY_SUBSYSTEM* memory_subsystem = new sim::MEMORY_SUBSYSTEM(std::move(memory_blocks));

    /* initialize yoked architecture (compute region) */

    const double c_freq_khz = sim::compute_freq_khz(COMPUTE_CODE_DISTANCE * compute_syndrome_extraction_round_time_ns);
    sim::yoked_codes::YOKED_ARCHITECTURE* yoked_arch = 
        new sim::yoked_codes::YOKED_ARCHITECTURE(c_freq_khz,
                                                  compute_local_memory_capacity,
                                                  inst_sim,
                                                  memory_subsystem,
                                                  alloc.second_level,
                                                  trace_file);

    /* initialize simulation */

    std::vector<sim::OPERABLE*> all_operables;
    all_operables.push_back(yoked_arch);
    std::copy(memory_subsystem->storages().begin(), memory_subsystem->storages().end(), std::back_inserter(all_operables));
    std::copy(alloc.first_level.begin(), alloc.first_level.end(), std::back_inserter(all_operables));
    std::copy(alloc.second_level.begin(), alloc.second_level.end(), std::back_inserter(all_operables));

    sim::coordinate_clock_scale(all_operables);

    std::cout << "simulation parameters:"
                << "\n\tqubits in local memory = " << compute_local_memory_capacity
                << "\n\tqubits in main memory (blocks) = " << main_memory_qubits << " (" << num_blocks << ")"
                << "\n\tL1 factories = " << alloc.first_level.size()
                << "\n\tL2 factories = " << alloc.second_level.size()
                << "\n";

    /* run simulation */

    sim::GL_SIM_WALL_START = std::chrono::steady_clock::now();
    uint64_t last_print_cycle{0};
    do
    {
        if (print_progress > 0)
        {
            bool do_print = (yoked_arch->current_cycle() % print_progress == 0)
                            && yoked_arch->current_cycle() > last_print_cycle;
            if (do_print)
            {
                std::cout << "cycle " << yoked_arch->current_cycle() << "\n";
                last_print_cycle = yoked_arch->current_cycle();
            }
        }

        for (auto* x : all_operables)
            x->tick();
    }
    while (!yoked_arch->done());

    /* print stats */
    size_t compute_physical_qubits = sim::configuration::surface_code_physical_qubit_count(COMPUTE_CODE_DISTANCE)
                                            * compute_local_memory_capacity;
    size_t memory_physical_qubits = std::transform_reduce(memory_subsystem->storages().begin(), 
                                                            memory_subsystem->storages().end(),
                                                            size_t{0},
                                                            std::plus<size_t>{},
                                                            [] (auto* s) { return s->physical_qubit_count; });
    size_t factory_physical_qubits = alloc.physical_qubit_count;

    print_stat_line(std::cout, "TOTAL_SIMULATION_CYCLES", yoked_arch->current_cycle());
    print_stat_line(std::cout, "INSTRUCTIONS_DONE", yoked_arch->client_.s_inst_done);
    print_stat_line(std::cout, "UNROLLED_INSTRUCTIONS_DONE", yoked_arch->client_.s_unrolled_inst_done);
    print_stat_line(std::cout, "IPC", yoked_arch->client_.ipc());

    sim::print_stats_for_factories(std::cout, "L1_FACTORY", alloc.first_level);
    sim::print_stats_for_factories(std::cout, "L2_FACTORY", alloc.second_level);

    // Print yoked architecture storage statistics (includes error stats for all storages)
    yoked_arch->print_yoked_storage_stats();

    print_stat_line(std::cout, "COMPUTE_PHYSICAL_QUBITS", compute_physical_qubits);
    print_stat_line(std::cout, "MEMORY_PHYSICAL_QUBITS", memory_physical_qubits);
    print_stat_line(std::cout, "FACTORY_PHYSICAL_QUBITS", factory_physical_qubits);
    /* cleanup simulation */

    delete yoked_arch;
    delete memory_subsystem;
    for (auto* f : alloc.first_level)
        delete f;
    for (auto* f : alloc.second_level)
        delete f;

    return 0;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
jit_compile(std::string& trace, int64_t inst_sim, int64_t active_set_capacity)
{
    constexpr auto MEMORY_ACCESS_SCHEDULER{compile::memory_scheduler::eif};

    std::string trace_dir = trace.substr(0, trace.find_last_of("/\\") + 1) + "jit/";
    std::string trace_filename = trace.substr(trace.find_last_of("/\\") + 1);

    mkdir(trace_dir.c_str(), 0777);

    auto ext_it = trace_filename.find(".gz");
    if (ext_it == std::string::npos)
        ext_it = trace_filename.find(".xz");
    std::string base_name = trace_filename.substr(0, ext_it);
    std::string active_set_capacity_str = std::to_string(active_set_capacity);
    std::string inst_str = std::to_string(inst_sim/1'000'000) + "M";

    std::string compiled_trace = trace_dir + base_name + "_a" + active_set_capacity_str + "_" + inst_str + ".gz";
    
    std::cout << "********* (jit) running memory access scheduler for " << trace 
                << " -> " << compiled_trace << " *********\n";

    generic_strm_type istrm, ostrm;
    generic_strm_open(istrm, trace, "rb");
    generic_strm_open(ostrm, compiled_trace, "wb");

    compile::memory_scheduler::config_type conf;
    conf.active_set_capacity = active_set_capacity;
    conf.inst_compile_limit = static_cast<int64_t>(1.2 * inst_sim);
    conf.print_progress_frequency = 0;
    conf.dag_inst_capacity = 100000;
    conf.hint_lookahead_depth = 256;

    run(ostrm, istrm, MEMORY_ACCESS_SCHEDULER, conf);

    generic_strm_close(istrm);
    generic_strm_close(ostrm);

    trace = compiled_trace;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

size_t
get_number_of_qubits(std::string_view trace)
{
    generic_strm_type istrm;
    generic_strm_open(istrm, std::string{trace}, "rb");
    uint32_t num_qubits;
    generic_strm_read(istrm, &num_qubits, 4);
    return num_qubits;
}

bool
has_option(int argc, char* argv[], std::string_view option_name)
{
    return std::any_of(argv + 1, argv + argc,
                        [option_name] (const char* arg) { return option_name == arg; });
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

} // anon

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
