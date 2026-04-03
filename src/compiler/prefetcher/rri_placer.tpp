/*
 *  author: Gunagya Singh Mamak
 *  date:   9 March 2026
 * */

#include <algorithm>
#include <cassert>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * RRI_PLACER::operator()
 *
 * Called once per DAG front-layer retirement with the MSWAPs from that layer.
 * Uses the pre-computed UsageData inside eviction_policy_ for full-circuit
 * next-use / last-use lookups (replaces the old sliding-window scan).
 *
 * Returns a vector of replacement instructions:
 *   - Original MSWAP       for 1D hits and cold-miss passthroughs.
 *   - New MPLACE(ld,st,ev) for cold misses that were successfully placed.
 * */
std::vector<inst_ptr>
RRI_PLACER::operator()(const std::vector<inst_ptr>& commit_zone_mswaps,
                        const dag_ptr& /* dag */,
                        const rri_placer_config_type& conf) const
{
    // Advance the layer counter (used as current_layer for eviction queries).
    ++layer_counter_;

    // --------------------------------------------------------
    // Step 1: Classify every MSWAP as a 1D hit or cold miss.
    //
    // 1D hit  : ld is already in intermediate_qubits_
    // cold    : ld absent from both compute_qubits_ and intermediate_qubits_
    // --------------------------------------------------------
    struct cold_candidate
    {
        inst_ptr inst;
        size_t   rri;   // next_use_after(st, layer_counter_); UsageData::INF if no reuse
    };

    std::vector<cold_candidate>    cold_candidates;
    std::vector<inst_ptr>          hit_mswaps;
    // Track qubits that entered 1D as st of a 1D-hit MSWAP in this layer to
    // avoid the same-layer hazard (MPLACE must not evict a qubit that hasn't
    // actually arrived in 1D yet within this same front-layer batch).
    std::unordered_set<qubit_type> freshly_placed_in_1d;

    for (inst_ptr inst : commit_zone_mswaps)
    {
        if (!is_memory_access(inst->type))
            continue;

        const qubit_type ld = inst->qubits[0];
        const qubit_type st = inst->qubits[1];

        if (intermediate_qubits_.count(ld))
        {
            // 1D hit: ld is already staged in intermediate storage.
            s_stats_.mswap_1d_hits++;
            hit_mswaps.push_back(inst);

            // Slot-swap: ld leaves intermediate → compute; st enters intermediate.
            compute_qubits_.insert(ld);
            intermediate_qubits_.erase(ld);
            compute_qubits_.erase(st);
            intermediate_qubits_.insert(st);
            freshly_placed_in_1d.insert(st);

            if (conf.verbose)
                std::cout << "[RRI_PLACER]   MSWAP(ld=" << ld << ", st=" << st
                          << ") [1D HIT]\n";
        }
        else
        {
            // Cold miss: ld is in cold storage.
            s_stats_.mswap_cold_misses++;
            const size_t st_rri = eviction_policy_.usage_data.next_use_after(
                st, layer_counter_);
            cold_candidates.push_back({inst, st_rri});
        }
    }

    // --------------------------------------------------------
    // Step 2: Rank cold candidates by ascending next_use_after(st) so that
    // the ones with the most urgent reuse of st are placed first.
    // UsageData::INF candidates sort last (no future reuse → least benefit).
    // --------------------------------------------------------
    std::sort(cold_candidates.begin(), cold_candidates.end(),
        [](const cold_candidate& a, const cold_candidate& b)
        {
            return a.rri < b.rri;
        });

    const size_t k = static_cast<size_t>(conf.intermediate_buffer_capacity);

    // --------------------------------------------------------
    // Step 3: For each ranked candidate attempt MPLACE emission.
    // --------------------------------------------------------
    std::unordered_set<inst_ptr> placed_set;
    size_t placements_remaining = k;

    std::vector<inst_ptr> result;
    result.insert(result.end(), hit_mswaps.begin(), hit_mswaps.end());

    // Build the eviction candidate list once (same intermediate set for all
    // cold candidates in this layer).
    std::vector<qubit_type> evict_candidates(
        intermediate_qubits_.begin(), intermediate_qubits_.end());

    for (const cold_candidate& cand : cold_candidates)
    {
        if (placements_remaining == 0)
            break;

        if (cand.rri == UsageData::INF)
            break;  // all remaining also have INF (sorted); no point continuing

        auto evict_result = eviction_policy_.select_eviction_candidate(
            evict_candidates, layer_counter_, freshly_placed_in_1d);

        if (!evict_result.found)
        {
            if (conf.verbose)
            {
                const qubit_type ld = cand.inst->qubits[0];
                const qubit_type st = cand.inst->qubits[1];
                std::cout << "[RRI_PLACER]   MSWAP(ld=" << ld << ", st=" << st
                          << ") [NO VICTIM]\n";
            }
            break;  // no remaining intermediate slots
        }

        // RRI-policy guard: never evict a 1D resident whose next reuse is
        // sooner than the incoming st's next reuse.
        if (eviction_policy_.policy == EvictionPolicy::RRI
            && cand.rri >= evict_result.metric)
        {
            if (conf.verbose)
            {
                const qubit_type ld = cand.inst->qubits[0];
                const qubit_type st = cand.inst->qubits[1];
                std::cout << "[RRI_PLACER]   MSWAP(ld=" << ld << ", st=" << st
                          << ") [GUARD SKIP] cand_rri=" << cand.rri
                          << " evict_rri="
                          << (evict_result.metric == UsageData::INF
                              ? std::string("INF")
                              : std::to_string(evict_result.metric))
                          << "\n";
            }
            continue;
        }

        // Emit MPLACE(ld, st, evict_1d).
        const qubit_type ld       = cand.inst->qubits[0];
        const qubit_type st       = cand.inst->qubits[1];
        const qubit_type evict_1d = evict_result.qubit;

        inst_ptr mplace = new INSTRUCTION{INSTRUCTION::TYPE::MPLACE, {ld, st, evict_1d}};
        result.push_back(mplace);
        placed_set.insert(cand.inst);
        s_stats_.mplace_emitted++;
        placements_remaining--;

        // Update compile-time location model:
        //   ld       : cold → compute
        //   st       : compute → intermediate
        //   evict_1d : intermediate → cold
        compute_qubits_.insert(ld);
        compute_qubits_.erase(st);
        intermediate_qubits_.erase(evict_1d);
        intermediate_qubits_.insert(st);

        // Refresh evict_candidates to reflect the change.
        evict_candidates.erase(
            std::find(evict_candidates.begin(), evict_candidates.end(), evict_1d));
        evict_candidates.push_back(st);

        if (conf.verbose)
            std::cout << "[RRI_PLACER]   MPLACE(ld=" << ld << ", st=" << st
                      << ", evict=" << evict_1d << ")"
                      << " rri(st)=" << cand.rri
                      << " rri(evict)="
                      << (evict_result.metric == UsageData::INF
                          ? std::string("INF")
                          : std::to_string(evict_result.metric))
                      << "\n";
    }

    // --------------------------------------------------------
    // Step 4: Pass through all cold-miss MSWAPs that were NOT placed.
    // --------------------------------------------------------
    for (const cold_candidate& cand : cold_candidates)
    {
        if (placed_set.count(cand.inst))
            continue;

        const qubit_type ld = cand.inst->qubits[0];
        const qubit_type st = cand.inst->qubits[1];

        result.push_back(cand.inst);
        s_stats_.mswap_passthrough++;

        // ld: cold → compute; st: compute → cold (not tracked)
        compute_qubits_.insert(ld);
        compute_qubits_.erase(st);

        if (conf.verbose)
            std::cout << "[RRI_PLACER]   MSWAP(ld=" << ld << ", st=" << st
                      << ") [PASSTHROUGH] rri(st)="
                      << (cand.rri == UsageData::INF
                          ? std::string("INF")
                          : std::to_string(cand.rri))
                      << "\n";
    }

    return result;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * run_rri_placer
 *
 * Performs a full-circuit pre-pass (build_usage_data) on conf.input_file_path
 * to populate placer.eviction_policy_, then streams the main binary one
 * front layer at a time, writing the rewritten output to `ostrm`.
 * */
rri_placer_stats_type
run_rri_placer(generic_strm_type& ostrm,
               generic_strm_type& istrm,
               RRI_PLACER& placer,
               const rri_placer_config_type& conf)
{
    constexpr size_t OUTGOING_CAPACITY{16384};

    // --------------------------------------------------------
    // Pre-pass: build full-circuit UsageData from the input file.
    // --------------------------------------------------------
    if (!conf.input_file_path.empty())
    {
        placer.eviction_policy_.policy     = conf.eviction_policy;
        placer.eviction_policy_.usage_data = build_usage_data(
            conf.input_file_path,
            conf.layer_type,
            static_cast<size_t>(conf.dag_inst_capacity),
            conf.inst_compile_limit);
    }

    // Read and forward the qubit count header.
    uint32_t num_qubits;
    generic_strm_read(istrm, &num_qubits, sizeof(num_qubits));
    generic_strm_write(ostrm, &num_qubits, sizeof(num_qubits));

    dag_ptr               dag{new DAG{num_qubits}};
    std::vector<inst_ptr> outgoing_buffer;
    outgoing_buffer.reserve(OUTGOING_CAPACITY);

    int64_t inst_done{0};
    int64_t window_steps{0};

    while (inst_done < conf.inst_compile_limit)
    {
        const int64_t inst_done_before = inst_done;

        if (!generic_strm_eof(istrm))
            read_instructions_into_dag(dag, istrm, conf.dag_inst_capacity);

        // Collect MSWAPs from the current front layer.
        std::vector<inst_ptr> commit_zone_mswaps;
        for (inst_ptr inst : dag->get_front_layer())
            if (is_memory_access(inst->type))
                commit_zone_mswaps.push_back(inst);

        if (dag->get_front_layer().empty())
            break;

        // Run the placer — returns MPLACE/MSWAP replacements.
        auto replacements = placer(commit_zone_mswaps, dag, conf);

        // Map original MSWAP → replacement MPLACE by ld qubit.
        std::unordered_map<inst_ptr, inst_ptr> replacement_map;
        {
            std::unordered_set<inst_ptr> commit_set{commit_zone_mswaps.begin(),
                                                     commit_zone_mswaps.end()};
            for (inst_ptr r : replacements)
            {
                if (!commit_set.count(r))
                {
                    for (inst_ptr orig : commit_zone_mswaps)
                    {
                        if (orig->qubits[0] == r->qubits[0])
                        {
                            replacement_map[orig] = r;
                            break;
                        }
                    }
                }
            }
        }

        // Retire exactly one front layer.
        {
            auto front_layer = dag->get_front_layer();
            for (inst_ptr inst : front_layer)
            {
                const int64_t ucount = static_cast<int64_t>(inst->unrolled_inst_count());

                if (is_memory_access(inst->type))
                {
                    auto it = replacement_map.find(inst);
                    outgoing_buffer.push_back(
                        it != replacement_map.end() ? it->second : inst);
                }
                else
                {
                    outgoing_buffer.push_back(inst);
                }

                dag->remove_instruction_from_front_layer(inst);
                if (is_memory_access(inst->type) && replacement_map.count(inst))
                    delete inst;

                inst_done += ucount;
            }
        }

        window_steps++;

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
            std::cout << "\nRRI Placer =================================================="
                      << "\ninstructions done  = " << inst_done
                      << "\nwindow steps       = " << window_steps
                      << "\nDAG inst count     = " << dag->inst_count()
                          << " of " << conf.dag_inst_capacity
                      << "\nmplace emitted     = " << placer.rri_stats().mplace_emitted
                      << "\n1D hit rate        = "
                      << (placer.rri_stats().mswap_1d_hits + placer.rri_stats().mswap_cold_misses > 0
                          ? static_cast<double>(placer.rri_stats().mswap_1d_hits)
                            / (placer.rri_stats().mswap_1d_hits + placer.rri_stats().mswap_cold_misses)
                          : 0.0)
                      << "\n";
        }
    }

    drain_buffer_into_stream(outgoing_buffer.begin(), outgoing_buffer.end(), ostrm);
    outgoing_buffer.clear();

    return placer.rri_stats();
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
