/*
 *  author: Gunagya Singh Mamak
 *  date:   12 March 2026
 * */

#include "compiler/prefetcher/singlepass_prefetch.h"
#include "compiler/prefetcher/singlepass_prefetch.tpp"
#include "instruction.h"

#include <algorithm>
#include <cassert>
#include <iostream>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

SINGLEPASS_PREFETCH::SINGLEPASS_PREFETCH(const singlepass_prefetch_config_type& conf)
    : intermediate_storage_capacity_(conf.intermediate_buffer_capacity),
      prefetch_min_layer_distance_(conf.prefetch_min_layer_distance),
      verbose_(conf.verbose)
{
    // Initialize one dummy universal entry per intermediate qubit slot.
    const qubit_type start = static_cast<qubit_type>(conf.active_set_capacity);
    const qubit_type end   = static_cast<qubit_type>(
        conf.active_set_capacity + conf.intermediate_buffer_capacity);
    for (qubit_type q = start; q < end; q++)
    {
        sp_tracked_op_entry e;
        e.ld           = q;
        e.st           = q;
        e.is_universal = true;
        tracked_ops_.push_back(std::move(e));
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
SINGLEPASS_PREFETCH::observe_compute_instructions(const std::vector<inst_ptr>& insts) const
{
    for (auto* inst : insts)
    {
        const size_t weight = instruction_depth_weight(inst->type);

        for (auto& entry : tracked_ops_)
        {
            if (entry.is_universal)
                continue;

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

            size_t max_local = 0;
            for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
            {
                auto mit = entry.dependent_qubit_layers.find(*it);
                if (mit != entry.dependent_qubit_layers.end() && mit->second > max_local)
                    max_local = mit->second;
            }
            const size_t new_layer = max_local + weight;

            for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
                entry.dependent_qubit_layers[*it] = new_layer;
        }
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

std::vector<inst_ptr>
SINGLEPASS_PREFETCH::operator()(const std::vector<inst_ptr>& mswaps,
                                 const std::vector<size_t>&   mswap_layers) const
{
    assert(mswap_layers.size() == mswaps.size());

    struct candidate_info
    {
        size_t idx;
        size_t local_distance;
    };

    struct epoch_entry
    {
        inst_ptr                    mswap;
        qubit_type                  ld, st;
        size_t                      layer{0};
        std::vector<candidate_info> candidates;
        bool                        is_hit{false};
        size_t                      hit_idx{SIZE_MAX};
        bool                        got_victim{false};
    };

    const size_t num_tracked = tracked_ops_.size();
    std::vector<epoch_entry> entries;
    entries.reserve(mswaps.size());

    // -------------------------------------------------------
    // Pass 1: collect candidates per MSWAP.
    // -------------------------------------------------------
    for (size_t mi = 0; mi < mswaps.size(); ++mi)
    {
        inst_ptr m = mswaps[mi];
        if (!is_memory_access(m->type))
            continue;

        epoch_entry e;
        e.mswap = m;
        e.ld    = m->qubits[0];
        e.st    = m->qubits[1];
        e.layer = mswap_layers[mi];

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

            if (!e.is_hit && T.st == e.ld)
            {
                e.is_hit  = true;
                e.hit_idx = i;
            }
        }

        entries.push_back(std::move(e));
    }

    // -------------------------------------------------------
    // Pass 2: assign victims using the eviction policy.
    // -------------------------------------------------------
    std::vector<bool>     consumed(num_tracked, false);
    std::vector<inst_ptr> prefetch_accesses;

    // --- Hits first ---
    for (auto& e : entries)
    {
        if (!e.is_hit)
            continue;
        assert(!consumed[e.hit_idx]);
        consumed[e.hit_idx] = true;
        e.got_victim         = true;
        s_prefetch_hits_++;

        if (verbose_)
            std::cout << "[SP_PREFETCH] HIT for MSWAP(ld=" << e.ld
                      << ", st=" << e.st << "): ld was staged\n";
    }

    // --- Prefetch assignments ---
    for (auto& e : entries)
    {
        if (e.is_hit)
            continue;

        // Collect unconsumed candidate qubit_types for the eviction policy.
        // Also build an ordered list (most stale first) for the LRU fallback
        // based on local_distance.
        std::vector<qubit_type>    cand_qubits;
        std::vector<candidate_info> sorted_cands = e.candidates;
        std::stable_sort(sorted_cands.begin(), sorted_cands.end(),
            [](const candidate_info& a, const candidate_info& b) {
                return a.local_distance > b.local_distance;
            });

        for (const auto& c : sorted_cands)
        {
            if (consumed[c.idx])
                continue;
            if (prefetch_min_layer_distance_ > 0
                && c.local_distance != SIZE_MAX
                && static_cast<int64_t>(c.local_distance) < prefetch_min_layer_distance_)
            {
                continue;
            }
            cand_qubits.push_back(tracked_ops_[c.idx].st);
        }

        if (cand_qubits.empty())
        {
            s_prefetch_misses_++;
            if (verbose_)
                std::cout << "[SP_PREFETCH] cold miss for MSWAP(ld=" << e.ld
                          << ", st=" << e.st << "): no eligible candidates\n";
            continue;
        }

        // Use the eviction policy to pick the best victim.
        // For RRI: argmax next_use_after(q, e.layer).
        // For LRU: argmin last_use_at_or_before(q, e.layer).
        auto evict_result = eviction_policy_.select_eviction_candidate(
            cand_qubits, e.layer, {});

        size_t victim_idx = SIZE_MAX;
        if (evict_result.found)
        {
            // Locate the tracked_ops_ index for the selected qubit.
            for (const auto& c : sorted_cands)
            {
                if (!consumed[c.idx]
                    && tracked_ops_[c.idx].st == evict_result.qubit)
                {
                    victim_idx = c.idx;
                    break;
                }
            }
        }

        if (victim_idx == SIZE_MAX)
        {
            s_prefetch_misses_++;
            continue;
        }

        consumed[victim_idx] = true;
        e.got_victim          = true;
        const qubit_type victim_st = tracked_ops_[victim_idx].st;

        inst_ptr pf = new INSTRUCTION{INSTRUCTION::TYPE::MPREFETCH, {e.ld, victim_st}};
        prefetch_accesses.push_back(pf);
        s_prefetches_emitted_++;

        if (verbose_)
            std::cout << "[SP_PREFETCH] MPREFETCH(ld=" << e.ld
                      << ", victim=" << victim_st << ")\n";
    }

    // -------------------------------------------------------
    // Rebuild tracked_ops_: remove consumed, append fresh entries.
    // -------------------------------------------------------
    {
        std::deque<sp_tracked_op_entry> rebuilt;
        for (size_t i = 0; i < num_tracked; i++)
            if (!consumed[i])
                rebuilt.push_back(std::move(tracked_ops_[i]));
        tracked_ops_ = std::move(rebuilt);
    }

    for (const auto& e : entries)
    {
        if (!e.got_victim)
            continue;

        sp_tracked_op_entry ne;
        ne.ld                           = e.ld;
        ne.st                           = e.st;
        ne.dependent_qubit_layers[e.ld] = 0;
        ne.dependent_qubit_layers[e.st] = 0;
        ne.is_universal                 = false;
        tracked_ops_.push_back(std::move(ne));
    }

    assert(static_cast<int64_t>(tracked_ops_.size()) == intermediate_storage_capacity_);

    return prefetch_accesses;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
SINGLEPASS_PREFETCH::collect_stats(stats_type& stats) const
{
    stats.prefetch_operations = s_prefetches_emitted_;
    stats.cold_memory_accesses = s_prefetch_misses_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
