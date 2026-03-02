/*
 *  author: Gunagya Singh Mamak
 *  date:   17 February 2026
 * */

#ifndef SIM_YOKED_CODES_YOKED_1D_STORAGE_h
#define SIM_YOKED_CODES_YOKED_1D_STORAGE_h

#include "globals.h"
#include "instruction.h"
#include "sim/memory_subsystem.h"
#include "sim/storage.h"

#include <cstddef>
#include <map>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class YOKED_1D_STORAGE : public sim::STORAGE
{
  public:
    const size_t inner_code_distance_; 
    const size_t effective_code_distance_;

    YOKED_1D_STORAGE(double freq_khz, 
        size_t rows, 
        size_t logical_qubit_count, 
        size_t inner_code_distance,
        size_t effective_code_distance);

    void set_memory_subsystem(MEMORY_SUBSYSTEM* mem_subsystem);

    void feed_memory_instructions(std::vector<std::pair<size_t, INSTRUCTION*>> mem_insts, const std::vector<QUBIT*>& client_qubits);

    void error_stats();

  protected:
    const size_t rows_;
    const size_t row_length_;
    const size_t yoke_cycle_rounds_;
    const size_t max_mem_rounds_;

    std::map<QUBIT*, size_t> need_qubits_;  // Maps qubit to its order (layer or depth)
    std::vector<QUBIT*> remove_qubits_;

    // Statistics for tracking residence time (split by destination)
    std::map<QUBIT*, cycle_type> qubit_entry_cycle_;  // Track when qubits enter 1D storage
    uint64_t s_residence_time_to_compute{0};  // Cycles for qubits loaded to compute region
    uint64_t s_qubits_to_compute{0};  // Count of qubits loaded to compute region
    uint64_t s_residence_time_to_2d{0};  // Cycles for qubits evicted to 2D storage
    uint64_t s_qubits_to_2d{0};  // Count of qubits evicted to 2D storage
    
    // Statistics for tracking needed vs unneeded qubits in 1D storage
    double s_total_needed_percentage{0};  // Cumulative percentage of needed qubits
    uint64_t s_cycle_samples{0};  // Number of cycles sampled
    
    // Statistics for feed_memory_instructions calls
    uint64_t s_total_needed_qubits{0};  // Cumulative count of needed qubits from all feed calls
    uint64_t s_feed_call_count{0};  // Number of times feed_memory_instructions was called
  
    enum PHASE {
        CHECK_YOKE,
        MEMORY_OPS,
    } current_phase_{CHECK_YOKE};
    
    virtual access_result_type do_memory_access(QUBIT* ld, QUBIT* st) override;
    size_t phase_progress_{0};

    size_t r_{0}, ro_{0};
    double sum_rpow2_{0};

    virtual long operate() override;

  private:
    MEMORY_SUBSYSTEM* memory_subsystem_;

    void schedule_memory_operations();
};

}  // namespace sim

#endif  // SIM_YOKED_CODES_YOKED_COLD_STORAGE_h