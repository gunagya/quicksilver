/*
 *  author: Suhas Vittal
 *  date:   12 January 2026
 * */

#ifndef SIM_MEMORY_SUBSYSTEM_h
#define SIM_MEMORY_SUBSYSTEM_h

#include "sim/storage.h"
#include "sim/routing_model.h"

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class MEMORY_SUBSYSTEM
{
public:
    using access_result_type = STORAGE::access_result_type;
    using routing_base_type = ROUTING_MODEL<STORAGE>;
private:
    std::vector<STORAGE*> storages_;

    std::unique_ptr<routing_base_type> routing_;
public:
    MEMORY_SUBSYSTEM(std::vector<STORAGE*>&&);

    /*
     * Finds the requisite `STORAGE*` location to serve
     * the access and executes `STORAGE::do_memory_access`
     *
     * Note that the latencies in `access_result_type` are
     * converted for the caller within `do_memory_access`
     * */
    access_result_type do_memory_access(QUBIT* ld, 
                                        QUBIT* st,
                                        cycle_type caller_current_cycle,
                                        double caller_freq_khz);

    /*
     * Searches for the `QUBIT*` that matches the given client id
     * and qubit id
     * */
    QUBIT* retrieve_qubit(client_id_type, qubit_type) const;

    const std::vector<STORAGE*>& storages() const;

    /*
     * Returns an iterator to the `STORAGE*` containing
     * the given qubit.
     * */
    std::vector<STORAGE*>::iterator       lookup(QUBIT*);
    std::vector<STORAGE*>::const_iterator lookup(QUBIT*) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace sim

#endif // SIM_MEMORY_SUBSYSTEM_h
