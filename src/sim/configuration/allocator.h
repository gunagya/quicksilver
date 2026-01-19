/*
 *  author: Suhas Vittal
 *  date:   14 January 2026
 * */

#ifndef SIM_CONFIGURATION_ALLOCATOR_h
#define SIM_CONFIGURATION_ALLOCATOR_h

#include "sim/factory.h"

namespace sim
{
namespace configuration
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

struct FACTORY_ALLOCATION
{
    std::vector<T_FACTORY_BASE*> first_level;
    std::vector<T_FACTORY_BASE*> second_level;
    GRID_ROUTER* router_{nullptr};

    FACTORY_ALLOCATION() =default;
    FACTORY_ALLOCATION(const FACTORY_ALLOCATION&) =default;
};

struct FACTORY_SPECIFICATION
{
    bool is_cultivation{false};

    uint64_t syndrome_extraction_round_time_ns{1200};
    size_t   buffer_capacity;
    double   output_error_rate;

    /*
     * Cultivation variables (defaults are for d = 3 color code cultivation)
     *  -- `escape_distance` is the final distance of the cultivated state
     *  -- `round_length` is the number of rounds required to cultivate the state
     *  -- `probability_of_success` is the probability of the cultivated state not being discarded
     * */
    size_t escape_distance{13};
    size_t round_length{25};
    double probability_of_success{0.2};

    /*
     * Distillation variables (defaults are for 15:1 (25,11,11) distillation)
     * */
    size_t dx{25};
    size_t dz{11};
    size_t dm{11};
    size_t input_count{4};
    size_t output_count{1};
    size_t rotations{11};
};

/*
 * Allocates a factory within a given physical qubit budget.
 *
 * Since we target an error rate of 1e-12 per magic state,
 * we allocate:
 *  (1) d = 3 color cultivation as the first level factory (pfail = 0.8%, perror = 1e-6)
 *  (2) 15:1 with (dx,dz,dm) = (25,11,11) as the second level factory (perror = 1e-12)
 * */

FACTORY_ALLOCATION throughput_aware_factory_allocation(size_t physical_qubit_budget,
                                                        FACTORY_SPECIFICATION l1_spec,
                                                        FACTORY_SPECIFICATION l2_spec);

/*
 * Estimates the magic state throughput of the given factory allocation
 * in magic states per second.
 * */
double estimate_throughput_of_allocation(const FACTORY_ALLOCATION&, bool first_level_is_cultivation);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace configuration
}  // namespace sim

#endif // SIM_CONFIGURATION_ALLOCATOR_h
