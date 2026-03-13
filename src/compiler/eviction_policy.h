/*
 *  author: Gunagya Singh Mamak
 *  date:   12 March 2026
 * */

#ifndef COMPILER_EVICTION_POLICY_h
#define COMPILER_EVICTION_POLICY_h

#include "globals.h"

#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compile
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `LayerType` controls how circuit layer numbers are computed during
 * both the usage-data pre-pass (build_usage_data) and the run loops that
 * need per-MSWAP layer numbers.
 *
 *   UNWEIGHTED : layer = number of DAG front-layer retirements so far.
 *                All instructions retired in the same BFS batch share
 *                the same monotone counter value (+1 per batch).
 *   WEIGHTED   : layer = max(layer of predecessor qubits)
 *                        + instruction_depth_weight(type).
 *                Each instruction gets its own circuit-depth value.
 *
 * Note: for RRI_PLACER, `layer_counter_` (incremented once per operator()
 * call regardless of LayerType) is passed as the current_layer to
 * select_eviction_candidate. UNWEIGHTED mode produces front-layer batch
 * counters that match this convention. WEIGHTED produces depth-based layer
 * numbers, which are consistent with the singlepass_prefetch run loop but
 * diverge from layer_counter_; changes to RRI_PLACER layer tracking are
 * deferred.
 * */
enum class LayerType { UNWEIGHTED, WEIGHTED };

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `EvictionPolicy` selects the victim-choice strategy.
 *
 *   RRI : evict the qubit whose next use is furthest in the future
 *         (argmax next_use_after). Bélády-optimal.
 *   LRU : evict the qubit whose last use is furthest in the past
 *         (argmin last_use_at_or_before).
 * */
enum class EvictionPolicy { RRI, LRU };

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `UsageData` stores, for each qubit, a sorted vector of circuit layer
 * numbers at which that qubit appears in an MSWAP instruction, either as
 * qubits[0] (ld) or qubits[1] (st).  Both operands count as a "use".
 *
 * Binary-search helpers:
 *   next_use_after(q, layer)        : smallest layer L > `layer` for q;
 *                                     INF if no future use exists.
 *   last_use_at_or_before(q, layer) : largest layer L <= `layer` for q;
 *                                     0 if no past use exists.
 * */
struct UsageData
{
    static constexpr size_t INF = std::numeric_limits<size_t>::max();

    std::unordered_map<qubit_type, std::vector<size_t>> layers;

    /* Record a use of qubit q at the given layer (must be called before finalise()). */
    void record(qubit_type q, size_t layer);

    /* Sort every per-qubit vector; call once after all records are done. */
    void finalise();

    /* Returns the smallest layer strictly greater than `layer` at which q is
     * used, or INF if no future use exists. */
    size_t next_use_after(qubit_type q, size_t layer) const;

    /* Returns the largest layer less than or equal to `layer` at which q is
     * used, or 0 if no past use exists. */
    size_t last_use_at_or_before(qubit_type q, size_t layer) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `build_usage_data` performs a lightweight streaming pre-pass over the
 * binary at `file_path`.
 *
 * Opens a second independent stream (supports plain, .gz, and .xz formats),
 * reads instructions incrementally (fill DAG to `dag_inst_capacity` →
 * retire front layer → record MSWAP qubit layers), and returns a finalised
 * UsageData.
 *
 * Layer numbers are assigned per `layer_type`:
 *   UNWEIGHTED : front-layer batch counter (+1 per retired batch).
 *   WEIGHTED   : per-qubit accumulated depth
 *                (max predecessor + instruction_depth_weight).
 *
 * Software instructions are elided (not added to the DAG).
 * Only MSWAP qubits[0] (ld) and qubits[1] (st) are recorded.
 * */
UsageData build_usage_data(const std::string& file_path,
                           LayerType          layer_type,
                           size_t             dag_inst_capacity);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `EvictionResult` is returned by `EvictionPolicyInstance::select_eviction_candidate`.
 *
 *   qubit  : the selected victim, or qubit_type{-1} if no valid candidate found.
 *   metric : RRI -> next_use_after value for the victim;
 *            LRU -> last_use_at_or_before value for the victim.
 *   found  : true iff a valid victim was identified.
 * */
struct EvictionResult
{
    qubit_type qubit{-1};
    size_t     metric{0};
    bool       found{false};
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `EvictionPolicyInstance` wraps an `EvictionPolicy` and a `UsageData`.
 *
 * Callers pass a vector of candidate qubits and the current circuit layer;
 * the instance selects the best victim without the caller needing separate
 * RRI tables or LRU timestamps.
 *
 *   RRI : selects argmax next_use_after(q, current_layer) among candidates.
 *   LRU : selects argmin last_use_at_or_before(q, current_layer) among candidates.
 * */
struct EvictionPolicyInstance
{
    EvictionPolicy policy{EvictionPolicy::RRI};
    UsageData      usage_data;

    /*
     * Select the best victim from `candidates`, skipping any qubit in
     * `exclusions` (e.g. freshly-placed qubits that create a same-layer hazard).
     *
     * Returns EvictionResult::found = false if no valid candidate remains.
     * */
    EvictionResult select_eviction_candidate(
        const std::vector<qubit_type>&        candidates,
        size_t                                current_layer,
        const std::unordered_set<qubit_type>& exclusions) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace compile

#endif  // COMPILER_EVICTION_POLICY_h
