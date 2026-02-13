/*
 *  author: Gunagya Singh Mamak
 *  date:   27 January 2026
 * */

#ifndef SIM_YOKED_CODES_YOKED_COLD_STORAGE_h
#define SIM_YOKED_CODES_YOKED_COLD_STORAGE_h

#include "sim/storage.h"
#include <cstddef>
#include <functional>
#include <unordered_set>

namespace sim
{

// Callback type: invoked with set of qubits that are ready for non-Clifford ops
using yoke_complete_callback_t = std::function<void(const std::unordered_set<QUBIT*>&)>;

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class YOKED_COLD_STORAGE : public sim::STORAGE
{
  public:
    const size_t inner_code_distance_; 
    const size_t effective_code_distance_;

    YOKED_COLD_STORAGE(double freq_khz, size_t logical_qubit_count, size_t inner_code_distance, size_t effective_code_distance);

    // Register callback to be invoked when yoke cycle completes
    void set_yoke_complete_callback(yoke_complete_callback_t callback);

    // Override to track loaded qubits
    access_result_type do_memory_access(QUBIT* ld, QUBIT* st) override;

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

    // Callback invoked when yoke cycle completes
    yoke_complete_callback_t yoke_complete_callback_{nullptr};

    // Track qubits loaded during current yoke cycle
    std::unordered_set<QUBIT*> qubits_loaded_this_cycle_;

    virtual long operate() override;
};

}  // namespace sim

#endif  // SIM_YOKED_CODES_YOKED_COLD_STORAGE_h