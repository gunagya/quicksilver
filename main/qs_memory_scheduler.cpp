/*
 *  author: Suhas Vittal
 *  date:   5 January 2026
 * */

#include "argparse.h"
#include "generic_io.h"
#include "compiler/memory_scheduler.h"
#include "compiler/memory_scheduler/impl.h"
#include "compiler/memory_scheduler/singlepass.h"
#include "compiler/prefetcher/prefetcher.h"
#include "compiler/prefetcher/lookahead.h"
#include "compiler/prefetcher/rri_placer.h"

#include <chrono>
#include <iomanip>
#include <cstdio>
#include <iostream>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

int
main(int argc, char* argv[])
{
    std::string                            input_trace_file;
    std::string                            output_trace_file;
    compile::memory_scheduler::config_type conf;
    int64_t                                scheduler_impl_id;
    bool                                   enable_prefetch{false};
    std::string                            prefetch_output_file;
    compile::prefetcher::config_type       prefetch_conf;
    bool                                   enable_rri_placer{false};
    std::string                            rri_output_file;
    compile::prefetcher::rri_placer_config_type rri_conf;

    ARGPARSE()
        .required("input-file", "The trace file (without memory instructions) to compile", input_trace_file)
        .required("output-file", "The output trace file path", output_trace_file)
        .optional("-c", "--active-set-capacity", "Number of program qubits in the active set", conf.active_set_capacity, 12)
        .optional("-i", "--inst-limit", "Number of instructions to compile", conf.inst_compile_limit, 15'000'000)
        .optional("-pp", "--print-progress", "Print progress frequency (#inst)", conf.print_progress_frequency, 1'000'000)
        .optional("", "--dag-capacity", "DAG instruction capacity", conf.dag_inst_capacity, 8192)
        .optional("-v", "--verbose", "Verbose flag", conf.verbose, false)
        .optional("-s", "--scheduler", "Scheduler ID (0 = EIF, 1 = HINT, 2 = SINGLEPASS-EIF, 3 = SINGLEPASS-HINT)", scheduler_impl_id, 0)

        /* HINT PARAMETERS START HERE */
        .optional("", "--hint-lookahead-depth", "HINT Lookahead Depth (layers)", conf.hint_lookahead_depth, 16)

        /* SINGLEPASS PARAMETERS START HERE */
        .optional("", "--singlepass-intermediate-capacity", "Intermediate storage capacity for singlepass scheduler", conf.intermediate_storage_capacity, 8)
        .optional("", "--singlepass-min-layer-distance", "Suppress prefetch if MSWAP DAG layer - candidate DAG layer < threshold (0 = disabled)", conf.prefetch_min_layer_distance, 0)

        /* PREFETCHER PARAMETERS START HERE */
        .optional("-p", "--enable-prefetch", "Run lookahead prefetcher as a second pass", enable_prefetch, false)
        .optional("", "--prefetch-output-file", "Output file for prefetcher pass", prefetch_output_file, std::string{})
        .optional("", "--prefetch-intermediate-capacity", "Intermediate storage capacity", prefetch_conf.intermediate_buffer_capacity, 8)
        .optional("", "--prefetch-lookahead-depth", "Prefetch lookahead depth (layers)", prefetch_conf.prefetch_lookahead_depth, 20)

        /* RRI PLACER PARAMETERS START HERE */
        .optional("-r", "--enable-rri-placer", "Run RRI-based data placement as a second pass", enable_rri_placer, false)
        .optional("", "--rri-output-file", "Output file for RRI placer pass", rri_output_file, std::string{})
        .optional("", "--rri-intermediate-capacity", "Intermediate (1D) storage capacity for RRI placer", rri_conf.intermediate_buffer_capacity, 8)
        .optional("", "--rri-window-size", "RRI sliding window size (layers)", rri_conf.window_size, 16)
        .optional("", "--rri-commit-zone", "RRI commit zone size (layers, must be <= window size)", rri_conf.commit_zone_size, 8)

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
    else if (scheduler_impl_id == 2)
    {
        compile::memory_scheduler::SINGLEPASS_SCHEDULER sp{
            compile::memory_scheduler::eif, conf};
        stats = compile::memory_scheduler::singlepass_run(ostrm, istrm, sp, conf);
    }
    else if (scheduler_impl_id == 3)
    {
        compile::memory_scheduler::SINGLEPASS_SCHEDULER sp{
            compile::memory_scheduler::hint, conf};
        stats = compile::memory_scheduler::singlepass_run(ostrm, istrm, sp, conf);
    }
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

    if (stats.prefetches_emitted > 0 || stats.prefetch_hits > 0 || stats.prefetch_misses > 0)
    {
        const uint64_t total_covered = stats.prefetches_emitted + stats.prefetch_hits;
        const uint64_t total_ops     = total_covered + stats.prefetch_misses;
        const double   coverage      = total_ops > 0
            ? static_cast<double>(total_covered) / total_ops
            : 0.0;
        print_stat_line(std::cout, "PREFETCHES_EMITTED",    stats.prefetches_emitted);
        print_stat_line(std::cout, "PREFETCH_HITS",          stats.prefetch_hits);
        print_stat_line(std::cout, "PREFETCH_MISSES",        stats.prefetch_misses);
        print_stat_line(std::cout, "PREFETCH_COVERAGE",      coverage);
        if (stats.prefetch_suppressed > 0)
            print_stat_line(std::cout, "PREFETCH_SUPPRESSED", stats.prefetch_suppressed);
    }

    if (enable_prefetch)
    {
        if (prefetch_output_file.empty())
        {
            auto dot = output_trace_file.rfind('.');
            if (dot == std::string::npos)
                prefetch_output_file = output_trace_file + "_prefetch";
            else
                prefetch_output_file = output_trace_file.substr(0, dot) + "_prefetch" + output_trace_file.substr(dot);
        }

        // carry over matching config from memory scheduler pass
        prefetch_conf.active_set_capacity      = conf.active_set_capacity;
        prefetch_conf.inst_compile_limit        = conf.inst_compile_limit / 3;
        prefetch_conf.print_progress_frequency  = conf.print_progress_frequency;
        prefetch_conf.dag_inst_capacity         = conf.dag_inst_capacity;
        prefetch_conf.verbose                   = conf.verbose;

        // read the memory-scheduled trace as input
        uint32_t num_qubits;
        {
            generic_strm_type tmp;
            generic_strm_open(tmp, output_trace_file, "rb");
            generic_strm_read(tmp, &num_qubits, sizeof(num_qubits));
            generic_strm_close(tmp);
        }

        generic_strm_type p_istrm, p_ostrm;
        generic_strm_open(p_istrm, output_trace_file, "rb");
        generic_strm_open(p_ostrm, prefetch_output_file, "wb");

        compile::prefetcher::LOOKAHEAD_PREFETCHER prefetcher{prefetch_conf, num_qubits};
        auto prefetch_start = std::chrono::high_resolution_clock::now();
        auto p_stats = compile::prefetcher::run(p_ostrm, p_istrm, prefetcher, prefetch_conf);
        auto prefetch_end = std::chrono::high_resolution_clock::now();

        generic_strm_close(p_istrm);
        generic_strm_close(p_ostrm);

        double prefetch_time_seconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                            prefetch_end - prefetch_start).count() / 1000000.0;
        double prefetch_hit_rate = p_stats.prefetch_operations + p_stats.cold_memory_accesses > 0
            ? static_cast<double>(p_stats.prefetch_operations)
              / (p_stats.prefetch_operations + p_stats.cold_memory_accesses)
            : 0.0;

        std::cout << "\n";
        print_stat_line(std::cout, "PREFETCH_INST_DONE", p_stats.unrolled_inst_done);
        print_stat_line(std::cout, "PREFETCH_LAYERS_PROCESSED", p_stats.layers_processed);
        print_stat_line(std::cout, "PREFETCH_OPERATIONS", p_stats.prefetch_operations);
        print_stat_line(std::cout, "COLD_MEMORY_ACCESSES", p_stats.cold_memory_accesses);
        print_stat_line(std::cout, "PREFETCH_HIT_RATE", prefetch_hit_rate);
        print_stat_line(std::cout, "PREFETCH_TIME_SECONDS", prefetch_time_seconds);
    }

    if (enable_rri_placer)
    {
        if (rri_output_file.empty())
        {
            auto dot = output_trace_file.rfind('.');
            if (dot == std::string::npos)
                rri_output_file = output_trace_file + "_rri";
            else
                rri_output_file = output_trace_file.substr(0, dot) + "_rri" + output_trace_file.substr(dot);
        }

        // carry over matching config from memory scheduler pass
        rri_conf.active_set_capacity      = conf.active_set_capacity;
        rri_conf.inst_compile_limit        = conf.inst_compile_limit;
        rri_conf.print_progress_frequency  = conf.print_progress_frequency;
        rri_conf.dag_inst_capacity         = conf.dag_inst_capacity;
        rri_conf.verbose                   = conf.verbose;

        if (rri_conf.commit_zone_size > rri_conf.window_size)
        {
            std::cerr << "[RRI_PLACER] commit_zone_size (" << rri_conf.commit_zone_size
                      << ") must be <= window_size (" << rri_conf.window_size << ")" << _die{};
        }

        // read num_qubits from the memory-scheduled output
        uint32_t num_qubits;
        {
            generic_strm_type tmp;
            generic_strm_open(tmp, output_trace_file, "rb");
            generic_strm_read(tmp, &num_qubits, sizeof(num_qubits));
            generic_strm_close(tmp);
        }

        generic_strm_type r_istrm, r_ostrm;
        generic_strm_open(r_istrm, output_trace_file, "rb");
        generic_strm_open(r_ostrm, rri_output_file, "wb");

        compile::prefetcher::RRI_PLACER rri_placer{rri_conf, num_qubits};
        auto rri_start = std::chrono::high_resolution_clock::now();
        auto rri_stats = compile::prefetcher::run_rri_placer(r_ostrm, r_istrm, rri_placer, rri_conf);
        auto rri_end   = std::chrono::high_resolution_clock::now();

        generic_strm_close(r_istrm);
        generic_strm_close(r_ostrm);

        double rri_time_seconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                      rri_end - rri_start).count() / 1000000.0;

        std::cout << "\n";
        rri_stats.print(std::cout);
        print_stat_line(std::cout, "RRI_TIME_SECONDS", rri_time_seconds);
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
