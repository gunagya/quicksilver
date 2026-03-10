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
 * Called once per window step with the MSWAPs from the commit zone
 * (layers 0 to commit_zone_size-1). Also receives the full DAG so it
 * can scan window_size layers ahead to compute RRIs.
 *
 * Returns a vector of replacement instructions. Each returned instruction
 * is either:
 *   - The original MSWAP (unchanged) for 1D hits and non-placed cold misses.
 *   - A new MPLACE(ld, st, evict_1d) replacing the cold-fetch MSWAP.
 *
 * The caller (run_rri_placer) writes returned instructions to the output
 * stream in place of the original MSWAPs.
 * */
std::vector<inst_ptr>
RRI_PLACER::operator()(const std::vector<inst_ptr>& commit_zone_mswaps,
                        const dag_ptr& dag,
                        const rri_placer_config_type& conf) const
{
    // --------------------------------------------------------
    // Step 1: Build qubit → next_ld_layer lookup across full window.
    //
    // Scan all MSWAPs in [0, window_size) layers from the current DAG front.
    // For each qubit that appears as ld, record the smallest layer index at
    // which it is first needed (first appearance as ld wins).
    // --------------------------------------------------------
    std::unordered_map<qubit_type, size_t> next_ld_layer;
    {
        const auto window_insts = dag->get_memory_instructions_upto_layers(
            static_cast<size_t>(conf.window_size));
        for (const auto& [layer, inst] : window_insts)
        {
            const qubit_type ld = inst->qubits[0];
            // only record the first (earliest) layer
            if (!next_ld_layer.count(ld))
                next_ld_layer.emplace(ld, layer);
        }
    }

    // --------------------------------------------------------
    // Step 2: Classify every MSWAP in the commit zone as a 1D hit or cold miss.
    //
    // 1D hit  : ld ∈ intermediate_qubits_
    // cold    : ld absent from both compute_qubits_ and intermediate_qubits_
    //
    // Collect cold-miss candidates for MPLACE consideration.
    // --------------------------------------------------------
    struct cold_candidate
    {
        inst_ptr inst;
        size_t   rri;   // RRI(st) — INF_RRI if st not visible in window
    };

    std::vector<cold_candidate> cold_candidates;
    // MSWAPs confirmed as 1D hits (emitted unchanged)
    std::vector<inst_ptr>       hit_mswaps;
    // Track qubits that just entered 1D as `st` of a 1D hit in THIS front layer.
    std::unordered_set<qubit_type> freshly_placed_in_1d;

    for (inst_ptr inst : commit_zone_mswaps)
    {
        if (!is_memory_access(inst->type))
            continue;

        const qubit_type ld = inst->qubits[0];
        const qubit_type st = inst->qubits[1];

        if (intermediate_qubits_.count(ld))
        {
            // 1D hit: ld is already in intermediate storage.
            s_stats_.mswap_1d_hits++;
            hit_mswaps.push_back(inst);

            // Slot-swap: ld leaves 1D → compute; st enters 1D.
            // RRI for st is taken from window lookup, or INF if not visible.
            const size_t st_rri = next_ld_layer.count(st)
                ? next_ld_layer.at(st)
                : INF_RRI;

            compute_qubits_.insert(ld);
            intermediate_qubits_.erase(ld);
            compute_qubits_.erase(st);
            intermediate_qubits_.emplace(st, st_rri);
            freshly_placed_in_1d.insert(st);

            if (conf.verbose)
                std::cout << "[RRI_PLACER]   MSWAP(ld=" << ld << ", st=" << st << ")"
                          << " [1D HIT] st_rri="
                          << (st_rri == INF_RRI ? std::string("INF") : std::to_string(st_rri))
                          << "\n";
        }
        else
        {
            // Cold miss: ld is in cold storage.
            s_stats_.mswap_cold_misses++;
            const size_t st_rri = next_ld_layer.count(st)
                ? next_ld_layer.at(st)
                : INF_RRI;
            cold_candidates.push_back({inst, st_rri});
        }
    }

    // --------------------------------------------------------
    // Step 3: Rank cold candidates by ascending RRI(st) and select the
    // top-k with finite RRI, where k = intermediate_buffer_capacity.
    // --------------------------------------------------------
    // Sort by ascending RRI; INF_RRI candidates sort last.
    std::sort(cold_candidates.begin(), cold_candidates.end(),
        [] (const cold_candidate& a, const cold_candidate& b)
        {
            return a.rri < b.rri;
        });

    const size_t k = static_cast<size_t>(conf.intermediate_buffer_capacity);

    // --------------------------------------------------------
    // Step 4: For each candidate (in rank order), attempt MPLACE emission.
    //
    // Guard: skip if RRI(st) >= RRI(evict_1d) — never evict a better resident.
    // A candidate with INF_RRI is always skipped (no visible reuse).
    // --------------------------------------------------------
    // Track which candidates were placed (to determine passthrough later).
    std::unordered_set<inst_ptr> placed_set;
    size_t placements_remaining = k;

    std::vector<inst_ptr> result;

    // Pre-populate result with hit MSWAPs (already processed above).
    result.insert(result.end(), hit_mswaps.begin(), hit_mswaps.end());

    for (const cold_candidate& cand : cold_candidates)
    {
        if (placements_remaining == 0)
            break;

        if (cand.rri == INF_RRI)
            break;  // all remaining candidates also have INF_RRI (sorted)

        // Find the 1D resident with the highest RRI as eviction victim.
        // Exclude qubits that were placed into 1D by a 1D-hit MSWAP in THIS
        // same front layer: both instructions would share the same DAG layer
        // depth, so the serial order in the binary is arbitrary.  If MPLACE
        // appears before its companion MSWAP in the file the simulator's DAG
        // will let MPLACE execute first — before the qubit is actually in 1D.
        qubit_type best_evict  = -1;
        size_t     best_evict_rri = 0;  // will be replaced by actual max
        bool       found_victim = false;

        for (const auto& [q, q_rri] : intermediate_qubits_)
        {
            if (freshly_placed_in_1d.count(q))
                continue;  // same-layer hazard: skip
            if (!found_victim || q_rri > best_evict_rri)
            {
                best_evict     = q;
                best_evict_rri = q_rri;
                found_victim   = true;
            }
        }

        // Guard: do not place if no victim exists or the candidate's RRI
        // is not strictly better than the worst resident's RRI.
        if (!found_victim || cand.rri >= best_evict_rri)
        {
            if (conf.verbose)
            {
                const qubit_type ld = cand.inst->qubits[0];
                const qubit_type st = cand.inst->qubits[1];
                std::cout << "[RRI_PLACER]   MSWAP(ld=" << ld << ", st=" << st << ")"
                          << " [GUARD SKIP] cand_rri=" << cand.rri
                          << " best_evict_rri="
                          << (found_victim
                              ? (best_evict_rri == INF_RRI ? std::string("INF") : std::to_string(best_evict_rri))
                              : std::string("none"))
                          << "\n";
            }
            continue;
        }

        // Emit MPLACE(ld, st, evict_1d).
        const qubit_type ld       = cand.inst->qubits[0];
        const qubit_type st       = cand.inst->qubits[1];
        const qubit_type evict_1d = best_evict;

        inst_ptr mplace = new INSTRUCTION{INSTRUCTION::TYPE::MPLACE, {ld, st, evict_1d}};
        result.push_back(mplace);
        placed_set.insert(cand.inst);
        s_stats_.mplace_emitted++;
        placements_remaining--;

        // Update compile-time location model:
        //   ld  : cold → compute
        //   st  : compute → 1D (with cand.rri)
        //   evict_1d : 1D → cold (remove from intermediate)
        compute_qubits_.insert(ld);
        compute_qubits_.erase(st);
        intermediate_qubits_.erase(evict_1d);
        intermediate_qubits_.emplace(st, cand.rri);

        if (conf.verbose)
            std::cout << "[RRI_PLACER]   MPLACE(ld=" << ld << ", st=" << st
                      << ", evict=" << evict_1d << ")"
                      << " rri(st)=" << cand.rri
                      << " rri(evict)="
                      << (best_evict_rri == INF_RRI ? std::string("INF") : std::to_string(best_evict_rri))
                      << "\n";
    }

    // --------------------------------------------------------
    // Step 5: Pass through all cold-miss MSWAPs that were NOT placed.
    // Update location model: ld → compute, st → cold (not tracked).
    // --------------------------------------------------------
    for (const cold_candidate& cand : cold_candidates)
    {
        if (placed_set.count(cand.inst))
            continue;  // already replaced by MPLACE

        const qubit_type ld = cand.inst->qubits[0];
        const qubit_type st = cand.inst->qubits[1];

        result.push_back(cand.inst);
        s_stats_.mswap_passthrough++;

        // ld: cold → compute; st: compute → cold (not tracked)
        compute_qubits_.insert(ld);
        compute_qubits_.erase(st);

        if (conf.verbose)
            std::cout << "[RRI_PLACER]   MSWAP(ld=" << ld << ", st=" << st << ")"
                      << " [PASSTHROUGH] rri(st)="
                      << (cand.rri == INF_RRI ? std::string("INF") : std::to_string(cand.rri))
                      << "\n";
    }

    return result;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * run_rri_placer
 *
 * Main loop for the RRI placer pass. Reads a MSWAP-only binary from `istrm`,
 * processes it in sliding windows of `conf.window_size` layers (advancing
 * `conf.commit_zone_size` layers per step), and writes the rewritten binary
 * (mix of MSWAP and MPLACE instructions) to `ostrm`.
 * */
rri_placer_stats_type
run_rri_placer(generic_strm_type& ostrm,
               generic_strm_type& istrm,
               RRI_PLACER& placer,
               const rri_placer_config_type& conf)
{
    constexpr size_t OUTGOING_CAPACITY{16384};

    assert(conf.commit_zone_size > 0);
    assert(conf.window_size >= conf.commit_zone_size);

    // read and forward the qubit count header
    uint32_t num_qubits;
    generic_strm_read(istrm, &num_qubits, sizeof(num_qubits));
    generic_strm_write(ostrm, &num_qubits, sizeof(num_qubits));

    dag_ptr              dag{new DAG{num_qubits}};
    std::vector<inst_ptr> outgoing_buffer;
    outgoing_buffer.reserve(OUTGOING_CAPACITY);

    int64_t inst_done{0};
    int64_t window_steps{0};

    while (inst_done < conf.inst_compile_limit)
    {
        const int64_t inst_done_before = inst_done;

        // Fill DAG up to window_size layers if possible.
        if (!generic_strm_eof(istrm))
            read_instructions_into_dag(dag, istrm, conf.dag_inst_capacity);

        // Collect MSWAPs from the front layer only (layer 0).
        // The full window (window_size layers) is still scanned inside operator()
        // for the RRI lookup; commit_zone_size remains the candidate horizon for ranking.
        std::vector<inst_ptr> commit_zone_mswaps;
        {
            for (inst_ptr inst : dag->get_front_layer())
                if (is_memory_access(inst->type))
                    commit_zone_mswaps.push_back(inst);
        }

        if (dag->get_front_layer().empty())
            break;

        // Run the placer
        // Returns replacement instructions (MPLACE or original MSWAP) for each
        // MSWAP in the commit zone.
        auto replacements = placer(commit_zone_mswaps, dag, conf);

        // Build a lookup: original MSWAP inst_ptr → replacement MPLACE inst_ptr.
        // Only newly-allocated MPLACE instructions (not in commit_zone_mswaps) are
        // recorded here; hits and passthroughs are the original pointer and need
        // no map entry.
        std::unordered_map<inst_ptr, inst_ptr> replacement_map;
        {
            std::unordered_set<inst_ptr> commit_set{commit_zone_mswaps.begin(),
                                                     commit_zone_mswaps.end()};
            for (inst_ptr r : replacements)
            {
                if (!commit_set.count(r))
                {
                    // r is a new MPLACE — match to original MSWAP by ld qubit.
                    // ld qubits are unique across the commit zone (DAG invariant).
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

        // Retire exactly one layer (the current front layer).
        {
            auto front_layer = dag->get_front_layer();

            for (inst_ptr inst : front_layer)
            {
                // Save ucount before any potential delete of inst.
                const int64_t ucount = static_cast<int64_t>(inst->unrolled_inst_count());

                if (is_memory_access(inst->type))
                {
                    auto it = replacement_map.find(inst);
                    if (it != replacement_map.end())
                        outgoing_buffer.push_back(it->second);  // emit MPLACE
                    else
                        outgoing_buffer.push_back(inst);         // emit original MSWAP
                }
                else
                {
                    outgoing_buffer.push_back(inst);
                }

                // Retire from DAG first (uses inst pointer for lookup internally).
                // Only then free the original if it was replaced by MPLACE.
                dag->remove_instruction_from_front_layer(inst);
                if (is_memory_access(inst->type) && replacement_map.count(inst))
                    delete inst;

                inst_done += ucount;
            }
        }

        window_steps++;

        // Flush outgoing buffer if it gets too large.
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

    // Drain remaining buffered instructions.
    drain_buffer_into_stream(outgoing_buffer.begin(), outgoing_buffer.end(), ostrm);
    outgoing_buffer.clear();

    return placer.rri_stats();
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
