/*
 *  author: Gunagya Singh Mamak
 *  date:   4 March 2026
 * */

#include "compiler/memory_scheduler/singlepass.h"
#include "instruction.h"

#include <algorithm>
#include <iostream>

namespace compile
{
namespace memory_scheduler
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

SINGLEPASS_SCHEDULER::SINGLEPASS_SCHEDULER(scheduler_fn base, config_type conf)
    : intermediate_storage_capacity_(conf.intermediate_storage_capacity),
      prefetch_min_layer_distance_(conf.prefetch_min_layer_distance),
      verbose_(conf.verbose),
      base_scheduler_(base)
{
    /*
     * Initialize one dummy universal entry per intermediate qubit.
     * These entries have is_universal = true, so they are valid victims for
     * every future MSWAP without any dependent_qubit_layers check.
     * dependent_qubit_layers is left empty (universals skip propagation).
     * */
    const qubit_type start = static_cast<qubit_type>(conf.active_set_capacity);
    const qubit_type end   = static_cast<qubit_type>(
        conf.active_set_capacity + conf.intermediate_storage_capacity);
    for (qubit_type q = start; q < end; q++)
    {
        tracked_op_entry e;
        e.ld           = q;   // dummy: treated as if this qubit was loaded
        e.st           = q;   // the intermediate slot itself
        e.is_universal = true;
        tracked_ops_.push_back(std::move(e));
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
SINGLEPASS_SCHEDULER::observe_compute_instructions(const std::vector<inst_ptr>& insts) const
{
    /*
     * For each compute instruction I, check every non-universal tracked entry T.
     * If any qubit of I is already in T.dependent_qubit_layers, update:
     *   new_layer = max(T.dependent_qubit_layers[q] for q in I, 0 if absent)
     *               + instruction_depth_weight(I.type)
     * Then set all qubits of I to new_layer in T.dependent_qubit_layers.
     * This propagates dependence (inserts new qubits) and advances local depth.
     * Universal entries are skipped — they match everything without tracking.
     * */
    for (auto* inst : insts)
    {
        const size_t weight = instruction_depth_weight(inst->type);

        for (auto& entry : tracked_ops_)
        {
            if (entry.is_universal)
                continue;

            // Check if this instruction touches any qubit already tracked.
            bool touches = false;
            for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
            {
                if (entry.dependent_qubit_layers.count(*it))
                {
                    touches = true;
                    break;
                }
            }

            if (!touches)
                continue;

            // Compute new_layer = max(local layer of each inst qubit, 0 if absent) + weight.
            size_t max_local = 0;
            for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
            {
                auto mit = entry.dependent_qubit_layers.find(*it);
                if (mit != entry.dependent_qubit_layers.end() && mit->second > max_local)
                    max_local = mit->second;
            }
            const size_t new_layer = max_local + weight;

            // Set all instruction qubits to new_layer (inserts new qubits too).
            for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
                entry.dependent_qubit_layers[*it] = new_layer;
        }
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

result_type
SINGLEPASS_SCHEDULER::operator()(const active_set_type& active_set,
                                  const dag_ptr&         dag,
                                  config_type            conf) const
{
    // Delegate to the wrapped base scheduler for the MSWAP list.
    result_type out = base_scheduler_(active_set, dag, conf);

    // -------------------------------------------------------
    // Epoch entry: per-MSWAP state for the two-pass procedure.
    // -------------------------------------------------------
    struct candidate_info
    {
        size_t idx;             // index into tracked_ops_
        size_t local_distance;  // max local layer of MSWAP qubits in this entry
    };

    struct epoch_entry
    {
        inst_ptr                     mswap;
        qubit_type                   ld, st;
        std::vector<candidate_info>  candidates; // sorted most-stale first
        bool                         is_hit{false};
        size_t                       hit_idx{SIZE_MAX};
        bool                         got_victim{false};
    };

    const size_t             num_tracked = tracked_ops_.size();
    std::vector<epoch_entry> entries;
    entries.reserve(out.memory_accesses.size());

    // -------------------------------------------------------
    // Pass 1: for each MSWAP M1=(ld, st), collect candidates.
    //   candidate(T, M1): T.is_universal ||
    //                      M1.st ∈ T.dependent_qubit_layers ||
    //                      M1.ld ∈ T.dependent_qubit_layers
    //   local_distance:   max(T.dependent_qubit_layers[ld],
    //                         T.dependent_qubit_layers[st])
    //                     SIZE_MAX for universal entries.
    //   hit(T, M1):       candidate && T.st == M1.ld
    //   Candidates are sorted by local_distance descending
    //   (most stale first).
    // -------------------------------------------------------
    for (inst_ptr m : out.memory_accesses)
    {
        if (!is_memory_access(m->type))
            continue;

        epoch_entry e;
        e.mswap = m;
        e.ld    = m->qubits[0];
        e.st    = m->qubits[1];

        for (size_t i = 0; i < num_tracked; i++)
        {
            const auto& T = tracked_ops_[i];

            size_t local_dist = 0;
            if (T.is_universal)
            {
                local_dist = SIZE_MAX;
            }
            else
            {
                auto it_st = T.dependent_qubit_layers.find(e.st);
                auto it_ld = T.dependent_qubit_layers.find(e.ld);
                const bool has_st = (it_st != T.dependent_qubit_layers.end());
                const bool has_ld = (it_ld != T.dependent_qubit_layers.end());

                if (!has_st && !has_ld)
                    continue;  // not a candidate

                if (has_st) local_dist = it_st->second;
                if (has_ld && it_ld->second > local_dist)
                    local_dist = it_ld->second;
            }

            e.candidates.push_back({i, local_dist});

            // Record first hit encountered (T.st == M1.ld: ld is already staged).
            if (!e.is_hit && T.st == e.ld)
            {
                e.is_hit  = true;
                e.hit_idx = i;
            }
        }

        // Sort candidates by local_distance descending (most stale first).
        std::stable_sort(e.candidates.begin(), e.candidates.end(),
                         [](const candidate_info& a, const candidate_info& b) {
                             return a.local_distance > b.local_distance;
                         });

        entries.push_back(std::move(e));
    }

    // -------------------------------------------------------
    // Pass 2: assign victims.  Hits first, then prefetches.
    // consumed[i] tracks which tracked_ops_ indices are taken.
    // -------------------------------------------------------
    std::vector<bool>     consumed(num_tracked, false);
    std::vector<inst_ptr> prefetch_accesses;

    // --- Hits: no prefetch emitted, just retire the matched entry ---
    for (auto& e : entries)
    {
        if (!e.is_hit)
            continue;
        assert(!consumed[e.hit_idx]);
        consumed[e.hit_idx] = true;
        e.got_victim         = true;
        s_prefetch_hits_++;

        if (verbose_)
            std::cout << "[SINGLEPASS] HIT for MSWAP(ld=" << e.ld
                      << ", st=" << e.st << ") -> ld=" << e.ld
                      << " was already staged in intermediate\n";
    }

    // --- Prefetch assignments: pick most-stale unconsumed candidate ---
    for (auto& e : entries)
    {
        if (e.is_hit)
            continue;  // already handled above

        // Find most-stale unconsumed candidate (candidates sorted descending).
        size_t victim_idx      = SIZE_MAX;
        size_t victim_distance = 0;
        for (const auto& c : e.candidates)
        {
            if (!consumed[c.idx])
            {
                victim_idx      = c.idx;
                victim_distance = c.local_distance;
                break;
            }
        }

        if (victim_idx == SIZE_MAX)
        {
            // Cold miss — no evictable slot available for this MSWAP.
            s_prefetch_misses_++;
            if (verbose_)
                std::cout << "[SINGLEPASS] cold miss for MSWAP(ld=" << e.ld
                          << ", st=" << e.st << "): no candidate available\n";
            continue;
        }

        // Distance threshold check using per-entry local distance.
        if (prefetch_min_layer_distance_ > 0 && victim_distance != SIZE_MAX)
        {
            if (static_cast<int64_t>(victim_distance) < prefetch_min_layer_distance_)
            {
                s_prefetch_suppressed_++;
                s_prefetch_misses_++;
                if (verbose_)
                    std::cout << "[SINGLEPASS] suppressed prefetch for MSWAP(ld=" << e.ld
                              << ", st=" << e.st << "): local distance " << victim_distance
                              << " < threshold " << prefetch_min_layer_distance_ << "\n";
                continue;
            }
        }

        consumed[victim_idx] = true;
        e.got_victim          = true;
        const qubit_type victim_st = tracked_ops_[victim_idx].st;

        // Emit MPREFETCH(M1.ld, T.st)
        inst_ptr pf = new INSTRUCTION{INSTRUCTION::TYPE::MPREFETCH, {e.ld, victim_st}};
        prefetch_accesses.push_back(pf);
        s_prefetches_emitted_++;

        if (verbose_)
            std::cout << "[SINGLEPASS] MPREFETCH(ld=" << e.ld
                      << ", victim=" << victim_st
                      << ") for MSWAP(ld=" << e.ld << ", st=" << e.st << ")\n";
    }

    // -------------------------------------------------------
    // Rebuild tracked_ops_: remove consumed entries, append a
    // fresh entry for each MSWAP that received a hit or prefetch.
    // Insertion order suffices — victim ranking is done per-
    // candidate by local_distance, not by deque position.
    // -------------------------------------------------------
    {
        std::deque<tracked_op_entry> rebuilt;
        for (size_t i = 0; i < num_tracked; i++)
        {
            if (!consumed[i])
                rebuilt.push_back(std::move(tracked_ops_[i]));
        }
        tracked_ops_ = std::move(rebuilt);
    }

    for (const auto& e : entries)
    {
        if (!e.got_victim)
            continue;

        // Fresh entry: ld enters compute, st enters intermediate.
        // dependent_qubit_layers starts as {ld → 0, st → 0}.
        // The local layer closure will grow as subsequent compute
        // instructions are observed.
        tracked_op_entry new_entry;
        new_entry.ld           = e.ld;
        new_entry.st           = e.st;
        new_entry.dependent_qubit_layers[e.ld] = 0;
        new_entry.dependent_qubit_layers[e.st] = 0;
        new_entry.is_universal = false;
        tracked_ops_.push_back(std::move(new_entry));
    }

    assert (static_cast<int64_t>(tracked_ops_.size()) == intermediate_storage_capacity_);

    out.prefetch_accesses = std::move(prefetch_accesses);
    return out;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
SINGLEPASS_SCHEDULER::collect_stats(stats_type& stats) const
{
    stats.prefetches_emitted   = s_prefetches_emitted_;
    stats.prefetch_hits        = s_prefetch_hits_;
    stats.prefetch_misses      = s_prefetch_misses_;
    stats.prefetch_suppressed  = s_prefetch_suppressed_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace memory_scheduler
}  // namespace compile
