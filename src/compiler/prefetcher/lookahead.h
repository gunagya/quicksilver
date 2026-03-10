/*
 *  author: Gunagya Singh Mamak
 *  date:   2 March 2026
 * */

#ifndef COMPILER_PREFETCHER_LOOKAHEAD_h
#define COMPILER_PREFETCHER_LOOKAHEAD_h

#include "compiler/prefetcher/prefetcher.h"
#include "instruction.h"

#include <unordered_set>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `LOOKAHEAD_PREFETCHER` is a stateful prefetcher implementation
 * that scans ahead in the DAG from each front-layer instruction
 * to find future MSWAPs whose load qubit is in cold storage,
 * and inserts prefetch MSWAPs to bring those qubits into intermediate
 * storage before they are needed.
 *
 * Qubit location model:
 *   - compute_qubits_       : qubits currently in the compute region
 *   - evictable_qubits_     : qubits in intermediate storage that were stored
 *                             from compute and have no pending prefetch MSWAP;
 *                             these are the only valid eviction candidates
 *   - prefetched_qubits_    : qubits in intermediate storage that were prefetched
 *                             from cold storage and have a pending memory MSWAP;
 *                             must NOT be evicted
 *
 * All remaining qubits are assumed to be in cold storage.
 * */

struct LOOKAHEAD_PREFETCHER
{
    /*
     * Qubit location tracking (mutable — updated as layers are processed)
     * */
    mutable std::unordered_set<qubit_type> compute_qubits_;
    mutable std::unordered_set<qubit_type> evictable_qubits_;
    mutable std::unordered_set<qubit_type> prefetched_qubits_;

    /*
     * Statistics
     * */
    mutable uint64_t s_prefetch_operations_{0};
    mutable uint64_t s_cold_memory_accesses_{0};  // MSWAPs that were NOT prefetched (ld from cold)
    mutable uint64_t s_layers_processed_{0};

    /*
     * Constructor — initializes qubit location sets from config:
     *   qubits [0, active_set_capacity)                          -> compute
     *   qubits [active_set_capacity, active_set_capacity +
     *            intermediate_buffer_capacity)                   -> evictable (intermediate)
     *   remaining qubits                                         -> cold storage (not tracked)
     * */
    explicit LOOKAHEAD_PREFETCHER(config_type conf, size_t num_qubits);

    /*
     * Called once per DAG front layer by `run()`.
     *
     * Phase 1: for each MSWAP in `front_layer`, update qubit locations:
     *   - If `ld` is in intermediate (prefetched or evictable): it enters
     *     compute and `st` enters intermediate as evictable.
     *   - If `ld` is in cold storage: cold miss — `ld` enters compute and
     *     `st` goes to cold storage; increment s_cold_memory_accesses_.
     *
     * Phase 2: call `get_memory_instructions_upto_layers` to get all upcoming
     *   MSWAPs in the lookahead window, then:
     *   a. Repartition intermediate storage: qubits that appear as an upcoming
     *      `ld` become prefetched; all others become evictable.
     *   b. For each upcoming MSWAP whose `ld` is still in cold storage
     *      (in layer order), while evictable slots remain: pick a random
     *      evictable qubit as victim, insert a prefetch MSWAP(ld, victim)
     *      into the DAG, and update tracking sets.
     * */
    std::vector<inst_ptr> operator()(const std::vector<inst_ptr>& front_layer,
                    const dag_ptr& dag,
                    config_type conf) const;

    /*
     * Copies per-run statistics into `stats`.
     * Call after `prefetcher::run()` completes.
     * */
    void collect_stats(stats_type& stats) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile

#endif  // COMPILER_PREFETCHER_LOOKAHEAD_h
