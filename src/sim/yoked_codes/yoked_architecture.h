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
#include "sim/yoked_codes/yoked_1d_storage.h"
#include "sim/yoked_codes/yoked_cold_storage.h"
#include "globals.h"

#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

    execute_result_type do_memory_access(inst_ptr, QUBIT* ld, QUBIT* st) override;
    execute_result_type do_placement_access(inst_ptr, QUBIT* ld, QUBIT* st, QUBIT* evict_1d) override;

  private:
    // Track which qubits can be used for non-Clifford operations
    // - Qubits in local memory at start: true (already verified)
    // - Qubits loaded from cold storage: false (need yoke cycle)
    // - After yoke cycle completes: true (verified and ready)
    std::map<QUBIT*, bool> can_operate_non_clifford_; 

    YOKED_1D_STORAGE* yoked_1d_storage_ = nullptr;
    std::vector<std::unordered_set<QUBIT*>> s_unique_loaded_qubits_per_250_cycles_;

    // Statistics for tracking 1D storage effectiveness
    uint64_t s_1d_loads{0};     // Loads from 1D storage
    uint64_t s_2d_loads{0};     // Loads from 2D storage (cold MSWAP)
    uint64_t s_mplace_loads{0}; // Loads from 2D storage via MPLACE (includes 1D eviction)
    std::map<QUBIT*, cycle_type> qubit_1d_load_cycle_;  // Track when qubits loaded from 1D
    uint64_t s_total_1d_to_ready_delay{0};   // Cumulative delay from 1D load to non-Clifford ready (only unverified-at-load qubits)
    uint64_t s_1d_loads_delayed{0};          // 1D loads where verification had not yet completed at load time
    uint64_t s_1d_loads_already_ready{0};    // 1D loads where qubit was already verified at load time
    uint64_t s_total_non_clifford_only_instruction_delay{0};
    uint64_t s_instructions_considered_for_non_clifford_delay{0};
    std::unordered_map<inst_ptr, cycle_type> non_clifford_only_block_start_cycle_;
    std::unordered_map<inst_ptr, cycle_type> non_clifford_only_instruction_delay_;

    void record_memory_op_bucket(QUBIT* ld);
    void retire_instruction(CLIENT* c, inst_ptr inst, cycle_type inst_latency);

  public:
    void print_yoked_storage_stats();
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace yoked_codes
}  // namespace sim

#endif  // SIM_YOKED_CODES_YOKED_ARCHITECTURE_h
