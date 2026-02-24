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

    void feed_memory_instructions(std::vector<INSTRUCTION*> mem_insts, const std::vector<QUBIT*>& client_qubits);

    void error_stats();

  protected:
    const size_t rows_;
    const size_t row_length_;
    const size_t yoke_cycle_rounds_;
    const size_t max_mem_rounds_;

    std::map<QUBIT*, bool> need_qubits_; 
    std::vector<QUBIT*> remove_qubits_;
  
    enum PHASE {
        CHECK_YOKE,
        MEMORY_OPS,
    } current_phase_{CHECK_YOKE};
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