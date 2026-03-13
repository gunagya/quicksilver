/*
 *  author: Gunagya Singh Mamak
 *  date:   9 March 2026
 * */

#ifndef COMPILER_PREFETCHER_RRI_PLACER_h
#define COMPILER_PREFETCHER_RRI_PLACER_h

#include "compiler/prefetcher/prefetcher.h"
#include "compiler/eviction_policy.h"
#include "instruction.h"

#include <string>
#include <unordered_set>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `rri_placer_config_type` extends the base prefetcher config with
 * RRI-specific parameters.
 *
 *   window_size      : DEPRECATED — retained for CLI backward compatibility,
 *                      no longer used.  RRI is now computed via a full-circuit
 *                      pre-pass (build_usage_data) controlled by `layer_type`
 *                      and `input_file_path`.
 *   commit_zone_size : currently unused (only layer-0 MSWAPs are processed
 *                      per step); reserved for future widening.
 *   layer_type       : controls how layer numbers are assigned in the pre-pass.
 *                      Currently only UNWEIGHTED is supported by rri_placer;
 *                      WEIGHTED is reserved for future implementation.
 *   input_file_path  : path to the MSWAP binary to pre-scan.  Set by the
 *                      caller before invoking run_rri_placer().
 * */
struct rri_placer_config_type : config_type
{
    int64_t        window_size{16};       // deprecated; retained for CLI compat
    int64_t        commit_zone_size{8};   // reserved
    EvictionPolicy eviction_policy{EvictionPolicy::RRI};
    LayerType      layer_type{LayerType::UNWEIGHTED};
    std::string    input_file_path;       // populated by caller before run_rri_placer()
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `rri_placer_stats_type` records compilation statistics for the RRI placer pass.
 *
 *   mswap_1d_hits       : MSWAPs where ld was already in intermediate (1D) storage.
 *   mswap_cold_misses   : MSWAPs where ld was fetched from cold (2D) storage.
 *   mplace_emitted      : cold-fetch MSWAPs replaced with MPLACE instructions.
 *   mswap_passthrough   : cold-fetch MSWAPs left unchanged (RRI guard triggered or
 *                         no eligible intermediate slot).
 * */
struct rri_placer_stats_type
{
    uint64_t mswap_1d_hits{0};
    uint64_t mswap_cold_misses{0};
    uint64_t mplace_emitted{0};
    uint64_t mswap_passthrough{0};

    void print(std::ostream& os) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `RRI_PLACER` is a stateful prefetcher-style implementation that rewrites
 * a MSWAP-only EIF-compiled binary by replacing selected cold-fetch MSWAPs
 * with MPLACE instructions.
 *
 * MPLACE(ld, st, evict_1d) encodes the following data-placement decision:
 *   - ld        : fetched from cold (2D) into the compute region.
 *   - st        : evicted from compute into 1D intermediate storage.
 *   - evict_1d  : evicted from 1D intermediate storage into cold (2D) storage.
 *
 * MPLACE is only emitted when:
 *   (a) ld is in cold storage (not 1D) at compile time.
 *   (b) next_use_after(st) is finite (st reappears within the full circuit).
 *   (c) RRI policy only: next_use_after(st) < next_use_after(evict_1d) —
 *       never evict a resident with a shorter re-reference interval.
 *
 * Per window step, the top-k cold-fetch MSWAPs with the smallest finite
 * next-use layers are selected as MPLACE candidates, where k is bounded
 * by intermediate_buffer_capacity.
 *
 * Qubit location model (compile-time):
 *   compute_qubits_      : qubits currently in the compute region.
 *   intermediate_qubits_ : set of qubits in 1D intermediate storage.
 *   cold (implicit)      : all qubits absent from both sets.
 * */
struct RRI_PLACER
{
    /*
     * Qubit location tracking (mutable — updated as layers are processed)
     * */
    mutable std::unordered_set<qubit_type> compute_qubits_;
    mutable std::unordered_set<qubit_type> intermediate_qubits_;
    mutable size_t                         layer_counter_{0};  // incremented each operator() call

    /*
     * Full-circuit usage data and eviction policy instance.
     * Populated by run_rri_placer() via the pre-pass before the main loop.
     * */
    EvictionPolicyInstance eviction_policy_;

    /*
     * Statistics
     * */
    mutable rri_placer_stats_type s_stats_;

    /*
     * Constructor — initializes qubit location sets from config:
     *   qubits [0, active_set_capacity)                                    -> compute
     *   qubits [active_set_capacity, active_set_capacity +
     *            intermediate_buffer_capacity)                             -> intermediate
     *   remaining qubits                                                   -> cold (not tracked)
     * */
    explicit RRI_PLACER(const rri_placer_config_type& conf, size_t num_qubits);

    /*
     * Called once per DAG front layer by run_rri_placer().
     *
     * For each MSWAP in the front layer:
     *   - If ld is in intermediate_qubits_ (1D hit): emit unchanged; slot-swap
     *     st into intermediate.
     *   - If ld is cold: attempt MPLACE emission using full-circuit next-use
     *     data from eviction_policy_.  Falls back to MSWAP passthrough when
     *     the guard fires or no slot is available.
     *
     * Returns a vector of replacement instructions (MPLACE or original MSWAP).
     * */
    std::vector<inst_ptr> operator()(const std::vector<inst_ptr>& commit_zone_mswaps,
                                     const dag_ptr& dag,
                                     const rri_placer_config_type& conf) const;

    /*
     * Copies per-run statistics into the base stats_type.
     * */
    void collect_stats(stats_type& stats) const;

    /*
     * Returns the full rri_placer_stats_type for detailed reporting.
     * */
    const rri_placer_stats_type& rri_stats() const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Top-level run loop for the RRI placer.
 *
 * Calls build_usage_data() on conf.input_file_path before the main loop
 * to perform a full-circuit pre-pass, then processes the DAG one front
 * layer at a time, writing the rewritten binary to `ostrm`.
 * */
rri_placer_stats_type run_rri_placer(generic_strm_type& ostrm,
                                     generic_strm_type& istrm,
                                     RRI_PLACER& placer,
                                     const rri_placer_config_type& conf);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile

#endif  // COMPILER_PREFETCHER_RRI_PLACER_h
