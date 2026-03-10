/*
 *  author: Gunagya Singh Mamak
 *  date:   9 March 2026
 * */

#ifndef COMPILER_PREFETCHER_RRI_PLACER_h
#define COMPILER_PREFETCHER_RRI_PLACER_h

#include "compiler/prefetcher/prefetcher.h"
#include "instruction.h"

#include <limits>
#include <unordered_map>

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
 *   window_size      : total number of DAG layers to scan for RRI computation.
 *                      The full window is used to build the qubit → next_ld_layer lookup.
 *   commit_zone_size : number of layers at the front of the window from which
 *                      cold-fetch MSWAPs are candidates for MPLACE replacement.
 *                      Must be <= window_size.
 * */
struct rri_placer_config_type : config_type
{
    int64_t window_size{16};
    int64_t commit_zone_size{8};
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `rri_placer_stats_type` records compilation statistics for the RRI placer pass.
 *
 *   mswap_1d_hits       : MSWAPs where ld was already in intermediate (1D) storage.
 *   mswap_cold_misses   : MSWAPs where ld was fetched from cold (2D) storage.
 *   mplace_emitted      : cold-fetch MSWAPs replaced with MPLACE instructions.
 *   mswap_passthrough   : cold-fetch MSWAPs left unchanged (RRI too high or guard triggered).
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
 *   (b) RRI(st) is finite (st reappears as an ld within the window).
 *   (c) RRI(st) < RRI(evict_1d) — never evict a resident with a shorter RRI.
 *
 * Per window step, the top-k cold-fetch MSWAPs in the commit zone with the
 * smallest finite RRIs are selected as MPLACE candidates, where k is bounded
 * by intermediate_buffer_capacity.
 *
 * Qubit location model (compile-time):
 *   compute_qubits_      : qubits currently in the compute region.
 *   intermediate_qubits_ : map from qubit → RRI for qubits in 1D intermediate storage.
 *   cold (implicit)      : all qubits absent from both sets.
 *
 * RRI = ∞ is represented as std::numeric_limits<size_t>::max().
 * */
struct RRI_PLACER
{
    static constexpr size_t INF_RRI = std::numeric_limits<size_t>::max();

    /*
     * Qubit location tracking (mutable — updated as layers are processed)
     * */
    mutable std::unordered_set<qubit_type>        compute_qubits_;
    mutable std::unordered_map<qubit_type, size_t> intermediate_qubits_;  // qubit → current RRI

    /*
     * Statistics
     * */
    mutable rri_placer_stats_type s_stats_;

    /*
     * Constructor — initializes qubit location sets from config:
     *   qubits [0, active_set_capacity)                                    -> compute
     *   qubits [active_set_capacity, active_set_capacity +
     *            intermediate_buffer_capacity)                             -> intermediate (RRI = INF)
     *   remaining qubits                                                   -> cold (not tracked)
     * */
    explicit RRI_PLACER(const rri_placer_config_type& conf, size_t num_qubits);

    /*
     * Called once per DAG front layer by `run()`.
     *
     * For each MSWAP in the front layer:
     *   - If ld ∈ intermediate_qubits_ (1D hit):
     *       emit the MSWAP unchanged; slot-swap st into 1D with its RRI from the window lookup
     *       (INF_RRI if st has no visible reuse in the window).
     *   - If ld is cold:
     *       defer to the per-window placement logic (see rri_placer.tpp).
     *       May emit MPLACE or the original MSWAP depending on RRI ranking and the guard.
     *
     * Returns a vector of replacement instructions (MPLACE instances) to be emitted
     * in place of selected MSWAPs. The caller (run()) is responsible for routing
     * unmodified MSWAPs to output as well.
     *
     * NOTE: Unlike LOOKAHEAD_PREFETCHER, this operator is called ONCE per window step
     * (covering commit_zone_size layers), not once per layer. The run() loop in
     * rri_placer.tpp drives the window advance.
     * */
    std::vector<inst_ptr> operator()(const std::vector<inst_ptr>& commit_zone_mswaps,
                                     const dag_ptr& dag,
                                     const rri_placer_config_type& conf) const;

    /*
     * Copies per-run statistics into the base stats_type.
     * Cold misses are reported as cold_memory_accesses; placements as prefetch_operations.
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
 * Advances the DAG in steps of `conf.commit_zone_size` layers, scanning
 * `conf.window_size` layers ahead for RRI computation. For each step,
 * calls `RRI_PLACER::operator()` with the commit-zone MSWAPs and writes
 * the resulting instruction stream to `ostrm`.
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
