/*
 *  author: Suhas Vittal
 *  date:   12 January 2026
 * */

#ifndef SIM_STORAGE_h
#define SIM_STORAGE_h

#include "globals.h"
#include "sim/operable.h"

#include <unordered_set>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class STORAGE : public OPERABLE
{
public:
    using backing_buffer_type = std::unordered_set<QUBIT*>;

    struct access_result_type
    {
        bool success{false};

        /*
         * Latency is given in terms of cycles for this `STORAGE`
         * */
        cycle_type latency;

        /*
         * We provide the frequency of the storage to help convert
         * to the desired cycles.
         * */
        double storage_freq_khz;
    };

    /*
     * These are just characteristics of the storage medium.
     * They have no simulator relevance (other than `logical_qubit_count`).
     * */
    const size_t physical_qubit_count;
    const size_t logical_qubit_count;
    const size_t code_distance;

    /*
     * Latency variables: relevant to simulation
     * */
    const cycle_type load_latency;
    const cycle_type store_latency;
protected:
    backing_buffer_type contents_;

    /*
     * `cycle_available` is a vector that corresponds
     * to the cycle (of the `STORAGE`) when an adapter
     * becomes free. If the STORAGE supports random
     * access for all qubits, then there is one adapter
     * per qubit.
     * */
    std::vector<cycle_type> cycle_available_;
public:
    STORAGE(double freq_khz, 
            size_t n, size_t k, size_t d, 
            size_t num_adapters, 
            cycle_type load_latency, 
            cycle_type store_latency);
    
    bool contains(QUBIT*) const;
    
    /*
     * Adds the given qubit to `contents_`. Should be used
     * only when initializing the storage.
     * */
    void insert(QUBIT*);

    /*
     * Attempts a memory access to the given storage.
     * Fails if the storage cannot serve the memory access.
     * Otherwise, succeeds and swaps the two qubits.
     *
     * This function is marked virtual in case a new `STORAGE`
     * descendant is defined that modifies how latency
     * or other functionality is implemented.
     * */
    virtual access_result_type do_memory_access(QUBIT* ld, QUBIT* st);

    /*
     * Returns true if any adapter is free this cycle.
     * */
    bool has_free_adapter() const;

    void print_adapter_debug_info(std::ostream&) const;

    const backing_buffer_type& contents() const;
protected:
    long operate() override;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Initializes the qubits for all clients by striping them
 * to maximize paralellism.
 *
 * Some pre-conditions:
 *  (1) It is assumed that the first storage in the vector is
 *      the compute subsystem's local memory. This memory is
 *      treated special and is split between the active clients.
 *  (2) The active clients correspond are the first in the
 *      `qubit_counts_by_client` array.
 * */

void storage_striped_initialization(const std::vector<STORAGE*>&,
                                    const std::vector<std::vector<QUBIT*>>& qubits_by_client,
                                    size_t num_active_clients);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace sim

#endif  // SIM_STORAGE_h
