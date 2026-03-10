/*
 *  author: Gunagya Singh Mamak
 *  date:   27 January 2026
 * */

#ifndef SIM_YOKED_CODES_YOKED_COLD_STORAGE_h
#define SIM_YOKED_CODES_YOKED_COLD_STORAGE_h

#include "globals.h"
#include "sim/storage.h"

#include <cstddef>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class YOKED_COLD_STORAGE : public sim::STORAGE
{
  public:
    const size_t inner_code_distance_; 
    const size_t effective_code_distance_;

    YOKED_COLD_STORAGE(double freq_khz, size_t logical_qubit_count, size_t inner_code_distance, size_t effective_code_distance);

    // Override to track loads and stores.
    access_result_type do_memory_access(QUBIT* ld, QUBIT* st) override;

    // Loaded out qubits for which the yoke cycle afterward has just completed.
    std::vector<QUBIT*> drain_newly_verified_qubits();
    std::vector<QUBIT*> drain_newly_stored_qubits();

    void error_stats();

  protected:
    const size_t grid_length_;
    const size_t yoke_cycle_rounds_;
  
    enum PHASE {
        CHECK_YOKE,
        MEMORY_OPS,
    } current_phase_{CHECK_YOKE};
  
    size_t yoke_cycle_progress_{0};
    size_t r_{0}, ro_{0};
    double sum_rpow4_{0};

    // Track qubits loaded out during current yoke cycle
    std::vector<QUBIT*> unverified_loaded_qubits_;
    // Qubits newly verified this yoke cycle
    std::vector<QUBIT*> newly_verified_qubits_;
    std::vector<QUBIT*> newly_stored_qubits_;

    // Stats: memory ops served per MEMORY_OPS phase.
    uint64_t s_mem_ops_this_phase_{0};   // ops in the current MEMORY_OPS phase
    uint64_t s_total_mem_ops_active_{0}; // sum across phases that served ≥1 op
    uint64_t s_active_mem_phases_{0};    // count of phases that served ≥1 op

    virtual long operate() override;
};

}  // namespace sim

#endif  // SIM_YOKED_CODES_YOKED_COLD_STORAGE_h