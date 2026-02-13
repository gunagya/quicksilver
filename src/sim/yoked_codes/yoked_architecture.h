/*
 *  author: Gunagya Singh Mamak
 *  date:   9 February 2026
 * */

#ifndef SIM_YOKED_CODES_YOKED_ARCHITECTURE_h
#define SIM_YOKED_CODES_YOKED_ARCHITECTURE_h

#include "sim/client.h"
#include "sim/compute_base.h"
#include "sim/operable.h"
#include "sim/factory.h"
#include "sim/memory_subsystem.h"
#include "sim/storage.h"
#include "sim/yoked_codes/yoked_cold_storage.h"
#include "globals.h"

#include <map>
#include <vector>
#include <unordered_set>

namespace sim
{
namespace yoked_codes
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class YOKED_ARCHITECTURE: public COMPUTE_BASE
{
  public:
    const uint64_t simulation_instructions_;

    CLIENT client_;

    YOKED_ARCHITECTURE(double freq_khz,
                        size_t local_memory_capacity,
                        uint64_t simulation_instructions,
                        MEMORY_SUBSYSTEM* memory,
                        std::vector<T_FACTORY_BASE*> factories,
                        std::string client_trace_file);

    bool done();

    void print_deadlock_info(std::ostream& ostrm, std::vector<CLIENT::inst_ptr> front_layer) const;

  protected:
    long operate() override;

    long fetch_and_execute_instructions_from_client(CLIENT*);

    // Override to track qubits loaded from yoked cold storage
    execute_result_type do_memory_access(inst_ptr, QUBIT* ld, QUBIT* st) override;

  private:
    // Track which qubits can be used for non-Clifford operations
    // - Qubits in local memory at start: true (already verified)
    // - Qubits loaded from cold storage: false (need yoke cycle)
    // - After yoke cycle completes: true (verified and ready)
    std::map<QUBIT*, bool> can_operate_non_clifford_; 

    // Handler for when yoke cycle completes in cold storage
    void handle_yoke_complete(const std::unordered_set<QUBIT*>& ready_qubits);
    
    // Set up callbacks for all yoked cold storage devices in memory hierarchy
    void setup_yoke_callbacks();

    void retire_instruction(CLIENT* c, inst_ptr inst, cycle_type inst_latency);
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace yoked_codes
}  // namespace sim

#endif  // SIM_YOKED_CODES_YOKED_ARCHITECTURE_h
