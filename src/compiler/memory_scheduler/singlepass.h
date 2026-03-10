/*
 *  author: Gunagya Singh Mamak
 *  date:   4 March 2026
 * */

#ifndef COMPILER_MEMORY_SCHEDULER_SINGLEPASS_h
#define COMPILER_MEMORY_SCHEDULER_SINGLEPASS_h

#include "compiler/memory_scheduler.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace compile
{
namespace memory_scheduler
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `tracked_op_entry` tracks a single past MSWAP whose stored qubit `st`
 * is sitting in intermediate storage and is a candidate for eviction.
 *
 * Dependency & layer tracking:
 *   `dependent_qubit_layers` maps qubit → local layer number.
 *   It starts as {T.ld → 0, T.st → 0} for a real MSWAP entry.
 *   Whenever a compute instruction touches any qubit already in the map,
 *   ALL qubits of that instruction are inserted/updated.  The new local
 *   layer for all of them is max(existing layer of each inst qubit,
 *   treating absent qubits as 0) + instruction_depth_weight(type).
 *   This grows the map to contain the full transitive compute-descendant
 *   closure of T.ld, each annotated with a local depth.
 *
 * A new MSWAP M1=(ld1, st1) may use this entry as a prefetch victim iff
 *   M1.st1 ∈ T.dependent_qubit_layers  (or T.is_universal)
 *   (M1's outgoing qubit is a compute descendant of T.ld, establishing a
 *    memory dependency chain T -> M1)
 * The emitted prefetch is then MPREFETCH(M1.ld1, T.st).
 *
 * The local distance of this entry w.r.t. an MSWAP M1 is:
 *   max(T.dependent_qubit_layers[M1.ld], T.dependent_qubit_layers[M1.st])
 *   (0 if a qubit is absent from the map).
 *   Universal entries use SIZE_MAX as their local distance.
 *
 * Special case — `is_universal`:
 *   Initial dummy entries have is_universal = true.  They match every future
 *   MSWAP unconditionally (no dependent_qubit_layers check needed, no
 *   propagation).  Their local distance is SIZE_MAX (most stale).
 * */
struct tracked_op_entry
{
    qubit_type                                  ld{0};  // qubit that entered compute
    qubit_type                                  st{0};  // qubit in intermediate storage (evictable)
    std::unordered_map<qubit_type, size_t>      dependent_qubit_layers;  // qubit → local layer
    bool                                        is_universal{false};
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `SINGLEPASS_SCHEDULER` wraps an underlying memory scheduler (EIF or HINT)
 * and inserts MPREFETCH instructions during the same scheduling pass.
 *
 * State:
 *   tracked_ops_  — deque of past MSWAPs whose intermediate-storage qubits
 *                   are still evictable.  Oldest entry at front (index 0).
 *                   Capped at `intermediate_storage_capacity`.
 *
 * Protocol (called by singlepass_run):
 *   1. On every compute batch: call observe_compute_instructions().
 *   2. On every epoch:         call operator() to get MSWAPs + prefetches.
 *   3. After run:              call collect_stats() to populate stats_type.
 *
 * Epoch logic — two-pass greedy:
 *   Pass 1 — for each MSWAP M1=(ld,st), collect candidate tracked entries
 *             where (T.is_universal || st ∈ T.dependent_qubit_layers || ld ∈ T.dependent_qubit_layers).
 *             For each candidate, compute local_distance =
 *               max(T.dependent_qubit_layers[ld], T.dependent_qubit_layers[st])
 *               (SIZE_MAX for universals).  Sort candidates most-stale first.
 *             Mark hits where T.st == ld (ld already staged).
 *   Pass 2 — resolve hits first (retire matched T, no prefetch emitted);
 *             then for each remaining MSWAP, greedily pick the most-stale
 *             unconsumed candidate T, emit MPREFETCH(ld, T.st).
 *             Cold miss if no candidates remain.
 * */
struct SINGLEPASS_SCHEDULER
{
    using scheduler_fn = result_type (*)(const active_set_type&, const dag_ptr&, config_type);

    mutable std::deque<tracked_op_entry>          tracked_ops_;
    int64_t                                        intermediate_storage_capacity_;
    int64_t                                        prefetch_min_layer_distance_;
    bool                                           verbose_;
    scheduler_fn                                   base_scheduler_;

    mutable uint64_t s_prefetches_emitted_{0};
    mutable uint64_t s_prefetch_hits_{0};
    mutable uint64_t s_prefetch_misses_{0};
    mutable uint64_t s_prefetch_suppressed_{0};

    /*
     * `base`  — underlying scheduler function (eif or hint).
     * `conf`  — provides active_set_capacity and intermediate_storage_capacity
     *           for initializing dummy universal entries.
     * */
    SINGLEPASS_SCHEDULER(scheduler_fn base, config_type conf);

    /*
     * Propagate qubit dependence and local layer numbers through all
     * non-universal tracked entries.
     * For each instruction I and each non-universal entry T:
     *   if any qubit of I is in T.dependent_qubit_layers, compute
     *   new_layer = max(T.dependent_qubit_layers[q] for each q in I,
     *   treating absent qubits as 0) + instruction_depth_weight(type),
     *   then set/insert all qubits of I to new_layer in T's map.
     * Call after each batch of compute instructions is retired.
     * */
    void observe_compute_instructions(const std::vector<inst_ptr>& insts) const;

    /*
     * Scheduling epoch.
     * Delegates to base_scheduler_, then runs two-pass greedy prefetch
     * assignment.  Returns result_type with prefetch_accesses populated.
     * */
    result_type operator()(const active_set_type& active_set,
                           const dag_ptr&         dag,
                           config_type            conf) const;

    /*
     * Copy per-run statistics into stats.
     * Call after singlepass_run() completes.
     * */
    void collect_stats(stats_type& stats) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `singlepass_run` mirrors memory_scheduler::run but additionally:
 *   - Calls scheduler.observe_compute_instructions() on every compute batch.
 *   - Flushes out.prefetch_accesses before out.memory_accesses each epoch.
 *   - Calls scheduler.collect_stats() at the end.
 * */
template <class SINGLEPASS_IMPL>
stats_type singlepass_run(generic_strm_type& ostrm,
                          generic_strm_type& istrm,
                          SINGLEPASS_IMPL&   scheduler,
                          config_type        conf);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace memory_scheduler
}  // namespace compile

#include "compiler/memory_scheduler/singlepass.tpp"

#endif  // COMPILER_MEMORY_SCHEDULER_SINGLEPASS_h
