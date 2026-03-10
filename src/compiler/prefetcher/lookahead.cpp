/*
 *  author: Gunagya Singh Mamak
 *  date:   2 March 2026
 * */

#include "compiler/prefetcher/lookahead.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <random>
#include <vector>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

LOOKAHEAD_PREFETCHER::LOOKAHEAD_PREFETCHER(config_type conf, size_t num_qubits)
{
    // qubits [0, active_set_capacity) start in compute
    for (qubit_type q = 0; q < static_cast<qubit_type>(conf.active_set_capacity); q++)
        compute_qubits_.insert(q);

    // qubits [active_set_capacity, active_set_capacity + intermediate_buffer_capacity) start in intermediate
    const qubit_type intermediate_end = static_cast<qubit_type>(
        conf.active_set_capacity + conf.intermediate_buffer_capacity);
    for (qubit_type q = static_cast<qubit_type>(conf.active_set_capacity);
         q < intermediate_end && q < static_cast<qubit_type>(num_qubits);
         q++)
    {
        evictable_qubits_.insert(q);
    }

    // remaining qubits are in cold storage (not tracked explicitly)
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

std::vector<inst_ptr>
LOOKAHEAD_PREFETCHER::operator()(const std::vector<inst_ptr>& front_layer,
                                  const dag_ptr& dag,
                                  config_type conf) const
{
    const uint64_t layer_idx = s_layers_processed_++;

    if (conf.verbose)
        std::cout << "[PREFETCH] --- layer " << layer_idx
                  << " (size=" << front_layer.size() << ") ---\n";

    // --------------------------------------------------------
    // Phase 1: process front-layer MSWAPs; update qubit locations.
    // --------------------------------------------------------
    for (inst_ptr inst : front_layer)
    {
        if (!is_memory_access(inst->type))
            continue;

        const qubit_type ld = inst->qubits[0];  // qubit entering compute
        const qubit_type st = inst->qubits[1];  // qubit leaving compute

        const bool ld_in_intermediate =
            evictable_qubits_.count(ld) || prefetched_qubits_.count(ld);

        if (ld_in_intermediate)
        {
            // Hit: ld was staged in intermediate — move it to compute.
            // st leaves compute and enters intermediate as evictable
            // (unless it is itself already prefetched).
            const bool was_prefetched = prefetched_qubits_.count(ld) > 0;
            evictable_qubits_.erase(ld);
            prefetched_qubits_.erase(ld);
            compute_qubits_.insert(ld);
            compute_qubits_.erase(st);
            if (!prefetched_qubits_.count(st))
                evictable_qubits_.insert(st);

            if (conf.verbose)
                std::cout << "[PREFETCH]   MSWAP(ld=" << ld << ", st=" << st << ")"
                          << (was_prefetched ? " [PREFETCH HIT]" : " [INTERMEDIATE HIT]")
                          << " -> ld enters compute, st enters intermediate"
                          << " (evictable=" << evictable_qubits_.size() << ")\n";
        }
        else
        {
            // Cold miss: ld was in cold storage, st goes to cold storage.
            compute_qubits_.insert(ld);
            compute_qubits_.erase(st);
            s_cold_memory_accesses_++;

            if (conf.verbose)
                std::cout << "[PREFETCH]   MSWAP(ld=" << ld << ", st=" << st << ")"
                          << " [COLD MISS] -> ld from cold, st goes to cold"
                          << " (cold_total=" << s_cold_memory_accesses_ << ")\n";
        }
    }

    // --------------------------------------------------------
    // Phase 2a: query upcoming memory instructions.
    // --------------------------------------------------------
    const auto upcoming = dag->get_memory_instructions_upto_depth(
        static_cast<size_t>(conf.prefetch_lookahead_depth));

    // Collect the set of all ld qubits needed in the lookahead window.
    std::unordered_set<qubit_type> upcoming_ld_set;
    for (const auto& [layer, inst] : upcoming)
        upcoming_ld_set.insert(inst->qubits[0]);

    // --------------------------------------------------------
    // Phase 2b: repartition intermediate storage.
    //   prefetched = needed as an ld in the lookahead window.
    //   evictable  = not needed → safe to evict.
    // --------------------------------------------------------
    {
        std::unordered_set<qubit_type> all_intermediate;
        all_intermediate.insert(evictable_qubits_.begin(), evictable_qubits_.end());
        all_intermediate.insert(prefetched_qubits_.begin(), prefetched_qubits_.end());

        evictable_qubits_.clear();
        prefetched_qubits_.clear();

        for (qubit_type q : all_intermediate)
        {
            if (upcoming_ld_set.count(q))
                prefetched_qubits_.insert(q);
            else
                evictable_qubits_.insert(q);
        }

        if (conf.verbose)
            std::cout << "[PREFETCH]   repartition: prefetched=" << prefetched_qubits_.size()
                      << ", evictable=" << evictable_qubits_.size() << "\n";
    }

    // --------------------------------------------------------
    // Phase 2c: collect cold ld qubits in layer order (deduplicated).
    // --------------------------------------------------------
    std::vector<std::pair<size_t, qubit_type>> cold_needs;
    {
        std::unordered_set<qubit_type> seen;
        for (const auto& [layer, inst] : upcoming)
        {
            const qubit_type ld = inst->qubits[0];
            if (seen.count(ld))
                continue;
            seen.insert(ld);
            if (!compute_qubits_.count(ld)
                && !evictable_qubits_.count(ld)
                && !prefetched_qubits_.count(ld))
            {
                cold_needs.emplace_back(layer, ld);
            }
        }
    }

    // --------------------------------------------------------
    // Phase 2d: prefetch — evict a random evictable qubit for each
    //           cold need, in layer order, until capacity is exhausted.
    // --------------------------------------------------------
    std::vector<inst_ptr> pending_prefetches;
    static std::mt19937 rng{std::random_device{}()};

    for (const auto& [layer, ld] : cold_needs)
    {
        if (evictable_qubits_.empty())
            break;

        std::uniform_int_distribution<size_t> dist(0, evictable_qubits_.size() - 1);
        auto victim_it = evictable_qubits_.begin();
        std::advance(victim_it, dist(rng));
        const qubit_type victim = *victim_it;

        inst_ptr prefetch = new INSTRUCTION{INSTRUCTION::TYPE::MPREFETCH, {ld, victim}};
        pending_prefetches.push_back(prefetch);

        evictable_qubits_.erase(victim_it);
        prefetched_qubits_.insert(ld);
        s_prefetch_operations_++;

        if (conf.verbose)
            std::cout << "[PREFETCH]   MPREFETCH(ld=" << ld << ", victim=" << victim << ")"
                      << " for upcoming layer+" << layer
                      << " (evictable=" << evictable_qubits_.size() << ")\n";
    }

    if (conf.verbose && cold_needs.size() > pending_prefetches.size())
        std::cout << "[PREFETCH]   " << (cold_needs.size() - pending_prefetches.size())
                  << " cold need(s) unmet (no evictable capacity)\n";

    // --------------------------------------------------------
    // Phase 3: return the MPREFETCH instructions to the caller (run()),
    //          which will append them after the current front-layer
    //          instructions in the output buffer.
    // --------------------------------------------------------
    if (conf.verbose && !pending_prefetches.empty())
        std::cout << "[PREFETCH]   returning " << pending_prefetches.size()
                  << " MPREFETCH(s) to be written after current layer\n";

    return pending_prefetches;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
LOOKAHEAD_PREFETCHER::collect_stats(stats_type& stats) const
{
    stats.prefetch_operations  = s_prefetch_operations_;
    stats.cold_memory_accesses = s_cold_memory_accesses_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
