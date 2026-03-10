/*
 *  author: Gunagya Singh Mamak
 *  date:   9 March 2026
 * */

#include "compiler/prefetcher/rri_placer.h"
#include "compiler/prefetcher/rri_placer.tpp"

#include <iostream>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

RRI_PLACER::RRI_PLACER(const rri_placer_config_type& conf, size_t num_qubits)
{
    // qubits [0, active_set_capacity) start in compute
    for (qubit_type q = 0; q < static_cast<qubit_type>(conf.active_set_capacity); q++)
        compute_qubits_.insert(q);

    // qubits [active_set_capacity, active_set_capacity + intermediate_buffer_capacity)
    // start in 1D intermediate storage with RRI = INF (no known reuse yet)
    const qubit_type intermediate_end = static_cast<qubit_type>(
        conf.active_set_capacity + conf.intermediate_buffer_capacity);
    for (qubit_type q = static_cast<qubit_type>(conf.active_set_capacity);
         q < intermediate_end && q < static_cast<qubit_type>(num_qubits);
         q++)
    {
        intermediate_qubits_.emplace(q, INF_RRI);
    }

    // remaining qubits are in cold storage (not tracked explicitly)
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
RRI_PLACER::collect_stats(stats_type& stats) const
{
    // map onto the generic stats fields used by the prefetcher run() loop
    stats.cold_memory_accesses = s_stats_.mswap_cold_misses;
    stats.prefetch_operations  = s_stats_.mplace_emitted;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

const rri_placer_stats_type&
RRI_PLACER::rri_stats() const
{
    return s_stats_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
rri_placer_stats_type::print(std::ostream& os) const
{
    const uint64_t total_mem = mswap_1d_hits + mswap_cold_misses;
    const double   hit_rate  = total_mem > 0
        ? static_cast<double>(mswap_1d_hits) / total_mem
        : 0.0;
    const double   placement_rate = mswap_cold_misses > 0
        ? static_cast<double>(mplace_emitted) / mswap_cold_misses
        : 0.0;

    print_stat_line(os, "RRI_MSWAP_1D_HITS",      mswap_1d_hits);
    print_stat_line(os, "RRI_MSWAP_COLD_MISSES",   mswap_cold_misses);
    print_stat_line(os, "RRI_MPLACE_EMITTED",       mplace_emitted);
    print_stat_line(os, "RRI_MSWAP_PASSTHROUGH",    mswap_passthrough);
    print_stat_line(os, "RRI_1D_HIT_RATE",          hit_rate);
    print_stat_line(os, "RRI_PLACEMENT_RATE",        placement_rate);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
