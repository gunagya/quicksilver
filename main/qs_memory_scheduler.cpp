/*
 *  author: Suhas Vittal
 *  date:   5 January 2026
 * */

#include "argparse.h"
#include "generic_io.h"
#include "compiler/memory_scheduler.h"
#include "compiler/memory_scheduler/impl.h"

#include <chrono>
#include <iomanip>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

int
main(int argc, char* argv[])
{
    std::string                            input_trace_file;
    std::string                            output_trace_file;
    compile::memory_scheduler::config_type conf;
    int64_t                                scheduler_impl_id;

    ARGPARSE()
        .required("input-file", "The trace file (without memory instructions) to compile", input_trace_file)
        .required("output-file", "The output trace file path", output_trace_file)
        .optional("-c", "--active-set-capacity", "Number of program qubits in the active set", conf.active_set_capacity, 12)
        .optional("-i", "--inst-limit", "Number of instructions to compile", conf.inst_compile_limit, 15'000'000)
        .optional("-pp", "--print-progress", "Print progress frequency (#inst)", conf.print_progress_frequency, 1'000'000)
        .optional("", "--dag-capacity", "DAG instruction capacity", conf.dag_inst_capacity, 8192)
        .optional("-v", "--verbose", "Verbose flag", conf.verbose, false)
        .optional("-s", "--scheduler", "Scheduler ID (0 = EIF, 1 = HINT)", scheduler_impl_id, 0)

        /* HINT PARAMETERS START HERE */
        .optional("", "--hint-lookahead-depth", "HINT Lookahead Depth (layers)", conf.hint_lookahead_depth, 16)

        .parse(argc, argv);

    // GL_USE_RPC_ISA = 1;

    generic_strm_type istrm, ostrm;
    generic_strm_open(istrm, input_trace_file, "rb");
    generic_strm_open(ostrm, output_trace_file, "wb");

    compile::memory_scheduler::stats_type stats;
    auto compile_start = std::chrono::high_resolution_clock::now();
    if (scheduler_impl_id == 0)
        stats = run(ostrm, istrm, compile::memory_scheduler::eif, conf);
    else if (scheduler_impl_id == 1)
        stats = run(ostrm, istrm, compile::memory_scheduler::hint, conf);
    else
        std::cerr << "unknown memory scheduler id: " << scheduler_impl_id << _die{};
    auto compile_end = std::chrono::high_resolution_clock::now();

    generic_strm_close(istrm);
    generic_strm_close(ostrm);

    // print stats:
    auto compile_duration = std::chrono::duration_cast<std::chrono::microseconds>(compile_end - compile_start);
    double compile_time_seconds = compile_duration.count() / 1000000.0;

    double compute_intensity = mean(stats.unrolled_inst_done, stats.memory_accesses);
    double mean_unused_bw = mean(stats.total_unused_bandwidth, stats.scheduler_epochs);

    print_stat_line(std::cout, "INST_DONE", stats.unrolled_inst_done);
    print_stat_line(std::cout, "MEMORY_ACCESSES", stats.memory_accesses);
    print_stat_line(std::cout, "SCHEDULING_EPOCHS", stats.scheduler_epochs);
    print_stat_line(std::cout, "COMPUTE_INTENSITY", compute_intensity);
    print_stat_line(std::cout, "MEAN_UNUSED_BANDWIDTH", mean_unused_bw);
    print_stat_line(std::cout, "COMPILATION_TIME_SECONDS", compile_time_seconds);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
