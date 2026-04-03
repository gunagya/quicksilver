/*
 *  author: Gunagya Singh Mamak
 *  date:   12 March 2026
 * */

#include <iostream>
#include <unordered_map>
#include <vector>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * run_singlepass_prefetch
 *
 * Stage-B run loop.  Reads the Stage-A binary (MSWAP + compute interleaved)
 * from `istrm`, inserts MPREFETCH instructions, and writes the result to
 * `ostrm`.
 *
 * For LRU/RRI eviction modes, builds usage-data from `conf.input_file_path`
 * using `conf.layer_type` before the main run loop.
 *
 * Per front-layer retirement order:
 *   [MPREFETCH*]  prefetch instructions for the MSWAP batch
 *   [MSWAP*]      original MSWAPs from the front layer
 *   [compute*]    compute instructions from the front layer
 * */
stats_type
run_singlepass_prefetch(generic_strm_type&                     ostrm,
                        generic_strm_type&                     istrm,
                        SINGLEPASS_PREFETCH&                   scheduler,
                        const singlepass_prefetch_config_type& conf)
{
    constexpr size_t OUTGOING_CAPACITY{16384};

    if (conf.eviction_mode != singlepass_prefetch_eviction_mode::LOCAL_DISTANCE)
    {
        if (conf.input_file_path.empty())
            std::cerr << "singlepass_prefetch: input_file_path is required for LRU/RRI mode"
                      << _die{};

        scheduler.eviction_policy_.policy =
            (conf.eviction_mode == singlepass_prefetch_eviction_mode::RRI)
                ? EvictionPolicy::RRI
                : EvictionPolicy::LRU;
        scheduler.eviction_policy_.usage_data = build_usage_data(
            conf.input_file_path,
            conf.layer_type,
            static_cast<size_t>(conf.dag_inst_capacity),
            conf.inst_compile_limit);
    }

    stats_type stats;

    // Read and forward the qubit-count header.
    uint32_t num_qubits;
    generic_strm_read(istrm, &num_qubits, sizeof(num_qubits));
    generic_strm_write(ostrm, &num_qubits, sizeof(num_qubits));

    dag_ptr               dag{new DAG{num_qubits}};
    std::vector<inst_ptr> outgoing_buffer;
    outgoing_buffer.reserve(OUTGOING_CAPACITY);

    int64_t inst_done{0};
    size_t front_layer_counter{0};
    std::unordered_map<qubit_type, size_t> qubit_layer;
    size_t max_weighted_layer{0};

    while (inst_done < conf.inst_compile_limit)
    {
        const int64_t inst_done_before = inst_done;

        if (!generic_strm_eof(istrm))
            read_instructions_into_dag(dag, istrm, conf.dag_inst_capacity);

        auto front_layer = dag->get_front_layer();
        if (front_layer.empty())
            break;

        // --------------------------------------------------------
        // Separate front layer into MSWAPs and compute instructions.
        // Compute absolute layer per instruction according to conf.layer_type.
        // --------------------------------------------------------
        std::vector<inst_ptr> mswaps;
        std::vector<size_t>   mswap_layers;
        std::vector<inst_ptr> computes;

        for (inst_ptr inst : front_layer)
        {
            size_t inst_layer = 0;
            if (conf.layer_type == LayerType::UNWEIGHTED)
            {
                inst_layer = front_layer_counter;
            }
            else  // LayerType::WEIGHTED
            {
                size_t max_pred = 0;
                for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
                {
                    auto jt = qubit_layer.find(*it);
                    if (jt != qubit_layer.end() && jt->second > max_pred)
                        max_pred = jt->second;
                }
                inst_layer = max_pred + instruction_depth_weight(*inst);
                if (inst_layer > max_weighted_layer)
                    max_weighted_layer = inst_layer;
            }

            if (is_memory_access(inst->type))
            {
                mswaps.push_back(inst);
                mswap_layers.push_back(inst_layer);
            }
            else
                computes.push_back(inst);

            if (conf.layer_type == LayerType::WEIGHTED)
            {
                for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
                {
                    size_t& ql = qubit_layer[*it];
                    if (inst_layer > ql)
                        ql = inst_layer;
                }
            }
        }

        // --------------------------------------------------------
        // Emit MPREFETCHes for this MSWAP batch.
        // --------------------------------------------------------
        std::vector<inst_ptr> prefetches;
        if (!mswaps.empty())
            prefetches = scheduler(mswaps, mswap_layers);

        // --------------------------------------------------------
        // Observe emitted instructions for dependency tracking
        // (keeps dependent_qubit_layers up-to-date for the next epoch).
        // --------------------------------------------------------
        scheduler.observe_compute_instructions(prefetches);
        scheduler.observe_compute_instructions(mswaps);
        scheduler.observe_compute_instructions(computes);

        // --------------------------------------------------------
        // Write to output: [MPREFETCH*] [MSWAP*] [compute*]
        // --------------------------------------------------------
        for (inst_ptr pf : prefetches) outgoing_buffer.push_back(pf);
        for (inst_ptr m  : mswaps)     outgoing_buffer.push_back(m);
        for (inst_ptr c  : computes)   outgoing_buffer.push_back(c);

        // Retire front layer from the DAG.
        for (inst_ptr inst : front_layer)
        {
            dag->remove_instruction_from_front_layer(inst);
            inst_done += inst->uop_count();
        }

        if (conf.layer_type == LayerType::UNWEIGHTED)
            ++front_layer_counter;

        stats.layers_processed++;

        // Flush the output buffer when it grows too large.
        if (static_cast<size_t>(outgoing_buffer.size()) >= OUTGOING_CAPACITY)
        {
            const size_t flush_count = OUTGOING_CAPACITY / 2;
            drain_buffer_into_stream(outgoing_buffer.begin(),
                                     outgoing_buffer.begin() + flush_count,
                                     ostrm);
            outgoing_buffer.erase(outgoing_buffer.begin(),
                                  outgoing_buffer.begin() + flush_count);
        }

        if (conf.print_progress_frequency > 0
            && (inst_done % conf.print_progress_frequency)
               < (inst_done_before % conf.print_progress_frequency))
        {
            std::cout << "\nSinglepass Prefetch ============================================="
                      << "\ninstructions done  = " << inst_done
                      << "\nlayers processed   = " << stats.layers_processed
                      << "\nprefetches emitted = " << scheduler.s_prefetches_emitted_
                      << "\nDAG inst count     = " << dag->inst_count()
                          << " of " << conf.dag_inst_capacity
                      << "\n";
        }
    }

    drain_buffer_into_stream(outgoing_buffer.begin(), outgoing_buffer.end(), ostrm);
    outgoing_buffer.clear();

    stats.unrolled_inst_done = inst_done;
    stats.weighted_layers_processed = max_weighted_layer;
    scheduler.collect_stats(stats);
    return stats;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
