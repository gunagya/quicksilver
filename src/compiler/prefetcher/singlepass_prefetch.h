/*
 *  author: Gunagya Singh Mamak
 *  date:   12 March 2026
 * */

#ifndef COMPILER_PREFETCHER_SINGLEPASS_PREFETCH_h
#define COMPILER_PREFETCHER_SINGLEPASS_PREFETCH_h

#include "compiler/eviction_policy.h"
#include "compiler/prefetcher/prefetcher.h"

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
 * Victim-choice mode for singlepass prefetch:
 *   LOCAL_DISTANCE : choose most-stale candidate by local_distance (current behavior).
 *   LRU            : choose victim via EvictionPolicyInstance::LRU among candidates.
 *   RRI            : choose victim via EvictionPolicyInstance::RRI among candidates.
 * */
enum class singlepass_prefetch_eviction_mode { LOCAL_DISTANCE, LRU, RRI };

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `singlepass_prefetch_config_type` extends the base prefetcher config with
 * parameters for the Stage-B MPREFETCH-insertion pass.
 *
 *   prefetch_min_layer_distance : require the selected victim candidate to
 *                                 have local distance >= this value before
 *                                 prefetch is emitted. 0 = disabled.
 *   eviction_mode               : LOCAL_DISTANCE (default), LRU, or RRI.
 *   layer_type                  : UNWEIGHTED or WEIGHTED traversal for pre-pass
 *                                 and online layer tracking when eviction_mode
 *                                 is LRU/RRI.
 *   input_file_path             : binary path used for usage-data pre-pass.
 *                                 If empty, caller should populate it with the
 *                                 same file being transformed.
 * */
struct singlepass_prefetch_config_type : config_type
{
    int64_t                            prefetch_min_layer_distance{0};
    singlepass_prefetch_eviction_mode  eviction_mode{
        singlepass_prefetch_eviction_mode::LOCAL_DISTANCE};
    LayerType                          layer_type{LayerType::UNWEIGHTED};
    std::string                        input_file_path;
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
 * It uses the same two-pass greedy victim-selection logic as the in-pass
 * singlepass memory scheduler:
 *   - hits first (T.st == ld): consume entry, emit no prefetch
 *   - otherwise select a victim among unconsumed candidates using either:
 *       (a) local-distance ordering (LOCAL_DISTANCE mode), or
 *       (b) EvictionPolicyInstance (LRU/RRI mode)
 *     then emit MPREFETCH(ld, victim_st)
 *
 * State invariant: tracked_ops_.size() == intermediate_storage_capacity at
 * all times after construction.
 * */
struct SINGLEPASS_PREFETCH
{
    mutable std::deque<sp_tracked_op_entry> tracked_ops_;
    int64_t                                  intermediate_storage_capacity_;
    int64_t                                  prefetch_min_layer_distance_;
    singlepass_prefetch_eviction_mode        eviction_mode_;
    bool                                     verbose_;
    EvictionPolicyInstance                   eviction_policy_;

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
     * `mswap_layers[i]` is the absolute layer for `mswaps[i]` and is used by
     * LRU/RRI policy modes as `current_layer`.
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
 * If `eviction_mode` is LRU/RRI, a pre-pass is executed via build_usage_data
 * using `input_file_path` and `layer_type`.
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
