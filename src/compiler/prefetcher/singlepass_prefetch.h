/*
 *  author: Gunagya Singh Mamak
 *  date:   12 March 2026
 * */

#ifndef COMPILER_PREFETCHER_SINGLEPASS_PREFETCH_h
#define COMPILER_PREFETCHER_SINGLEPASS_PREFETCH_h

#include "compiler/prefetcher/prefetcher.h"
#include "compiler/eviction_policy.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `singlepass_prefetch_config_type` extends the base prefetcher config with
 * parameters for the Stage-B MPREFETCH-insertion pass.
 *
 *   eviction_policy         : RRI (Bélády) or LRU for victim selection.
 *   layer_type              : UNWEIGHTED or WEIGHTED depth for UsageData pre-pass.
 *   input_file_path         : path to the MSWAP-only binary to pre-scan.
 *   prefetch_min_layer_distance : require a candidate victim to have local
 *                             distance >= this value before it is eligible
 *                             for eviction. 0 = disabled.
 * */
struct singlepass_prefetch_config_type : config_type
{
    EvictionPolicy eviction_policy{EvictionPolicy::RRI};
    LayerType      layer_type{LayerType::UNWEIGHTED};
    std::string    input_file_path;
    int64_t        prefetch_min_layer_distance{0};
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `sp_tracked_op_entry` tracks a single past MSWAP whose stored qubit `st`
 * is sitting in intermediate storage and is a candidate for eviction.
 *
 * `dependent_qubit_layers` maps qubit → local weighted compute-depth since
 * this entry was created.  It starts as {ld → 0, st → 0} and grows via
 * observe_compute_instructions() as downstream compute instructions are retired.
 * Universal (initial dummy) entries have is_universal = true and skip tracking.
 * */
struct sp_tracked_op_entry
{
    qubit_type                             ld{0};
    qubit_type                             st{0};
    std::unordered_map<qubit_type, size_t> dependent_qubit_layers;
    bool                                   is_universal{false};
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `SINGLEPASS_PREFETCH` is a second-pass (Stage B) scheduler that reads a
 * MSWAP-annotated binary and inserts MPREFETCH instructions.
 *
 * It uses a two-pass greedy victim-selection flow backed by an
 * EvictionPolicyInstance (backed by a
 * full-circuit UsageData pre-pass) for victim selection, instead of the
 * local-distance heuristic.
 *
 * For LRU eviction: the "current layer" passed to select_eviction_candidate
 * is the absolute circuit layer of the incoming MSWAP.  Candidates with
 * last_use_at_or_before(q, mswap_layer) furthest in the past are evicted first.
 *
 * State invariant: tracked_ops_.size() == intermediate_storage_capacity at
 * all times after construction.
 * */
struct SINGLEPASS_PREFETCH
{
    mutable std::deque<sp_tracked_op_entry> tracked_ops_;
    int64_t                                  intermediate_storage_capacity_;
    int64_t                                  prefetch_min_layer_distance_;
    bool                                     verbose_;

    /*
     * Eviction policy instance (populated by run_singlepass_prefetch
     * via build_usage_data before the main loop).
     * */
    EvictionPolicyInstance eviction_policy_;

    mutable uint64_t s_prefetches_emitted_{0};
    mutable uint64_t s_prefetch_hits_{0};
    mutable uint64_t s_prefetch_misses_{0};

    explicit SINGLEPASS_PREFETCH(const singlepass_prefetch_config_type& conf);

    /*
     * Propagate qubit dependency and local depth through non-universal entries.
     * Call after each batch of compute instructions is retired.
     * */
    void observe_compute_instructions(const std::vector<inst_ptr>& insts) const;

    /*
     * Called once per front-layer MSWAP batch.
     *
     * Returns a vector of MPREFETCH instructions to emit before the MSWAPs.
     * `mswap_layers[i]` is the absolute circuit layer of `mswaps[i]`, used as
     * the current_layer argument for eviction queries.
     * */
    std::vector<inst_ptr> operator()(const std::vector<inst_ptr>& mswaps,
                                     const std::vector<size_t>&   mswap_layers) const;

    void collect_stats(stats_type& stats) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `run_singlepass_prefetch`
 *
 * Stage-B run loop.  Reads a MSWAP-only binary (the output of Stage A /
 * EIF/HINT) from `istrm` and inserts MPREFETCH instructions, writing the
 * result to `ostrm`.
 *
 * Before the main loop:
 *   1. Calls build_usage_data(conf.input_file_path, ...) for the pre-pass.
 *   2. Tracks a per-qubit `qubit_layer` map to assign absolute layer numbers
 *      to each MSWAP batch as it is retired.
 *
 * Output order per epoch: [MPREFETCH*] [MSWAP*] [compute*]
 * */
stats_type run_singlepass_prefetch(generic_strm_type&                     ostrm,
                                   generic_strm_type&                     istrm,
                                   SINGLEPASS_PREFETCH&                   scheduler,
                                   const singlepass_prefetch_config_type& conf);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile

#endif  // COMPILER_PREFETCHER_SINGLEPASS_PREFETCH_h
