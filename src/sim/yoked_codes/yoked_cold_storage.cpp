/*
 *  author: Gunagya Singh Mamak
 *  date:   27 January 2026
 * */

#include "sim/yoked_codes/yoked_cold_storage.h"
#include "sim/configuration/resource_estimation.h"
#include "sim/memory_subsystem.h"
#include <cmath>
#include <cstddef>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace {

constexpr int threshold = 3;

size_t grid_length(size_t logical_qubit_count) {
    // Grid length must be a multiple of 4. Finds the smallest multiple of 4 that suffices.
    return std::ceil((std::sqrt(logical_qubit_count + 2) + 2) / 4) * 4; 
}

size_t calculate_physical_qubit_count(int grid_length, size_t inner_code_distance, size_t effective_code_distance) {
    return configuration::surface_code_physical_qubit_count(inner_code_distance) * grid_length * grid_length +
    2 * configuration::surface_code_physical_qubit_count(effective_code_distance, inner_code_distance) * grid_length +
    configuration::surface_code_physical_qubit_count(effective_code_distance);
}

} // anon

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

YOKED_COLD_STORAGE::YOKED_COLD_STORAGE(double freq_khz, size_t logical_qubit_count, size_t inner_code_distance, size_t effective_code_distance)
    :STORAGE(freq_khz,
             calculate_physical_qubit_count(grid_length(logical_qubit_count), inner_code_distance, effective_code_distance), // n
             logical_qubit_count,                                          // k
             effective_code_distance,                                      // d
             1,                                                            // num_adapters
             4,                                                         // load_latency
             3),                                                         // store_latency
        grid_length_(grid_length(logical_qubit_count)),
        inner_code_distance_(inner_code_distance), effective_code_distance_(effective_code_distance),
        yoke_cycle_rounds_((25 * grid_length_ + 4) * inner_code_distance) {
    cycle_available_[0] = 1;
}

std::vector<QUBIT*>
YOKED_COLD_STORAGE::drain_newly_verified_qubits()
{
    std::vector<QUBIT*> result;
    std::swap(result, newly_verified_qubits_);
    return result;
}

std::vector<QUBIT*>
YOKED_COLD_STORAGE::drain_newly_stored_qubits()
{
    std::vector<QUBIT*> result;
    std::swap(result, newly_stored_qubits_);
    return result;
}

STORAGE::access_result_type YOKED_COLD_STORAGE::do_memory_access(QUBIT* ld, QUBIT* st) {
    auto result = STORAGE::do_memory_access(ld, st);
    if (result.success) {
        // Track the qubit loaded from cold storage (st is being stored, ld is being loaded)
        unverified_loaded_qubits_.push_back(ld);
        newly_stored_qubits_.push_back(st);
    }
    return result;
}

long YOKED_COLD_STORAGE::operate() {
    r_ += effective_code_distance_; 
    switch (current_phase_) {
    case CHECK_YOKE:
        yoke_cycle_progress_ += effective_code_distance_;
        if (yoke_cycle_progress_ >= yoke_cycle_rounds_) {
            sum_rpow4_ += pow(r_, 4);
            r_ = 0;
            yoke_cycle_progress_ = 0;
            current_phase_ = MEMORY_OPS;
            
            for (QUBIT* q : unverified_loaded_qubits_) {
                newly_verified_qubits_.push_back(q);
            }
            unverified_loaded_qubits_.clear();
        } else {
            cycle_available_[0]++;
        }
        break;
    case MEMORY_OPS:
        // If hallway is left available for threshold cycles, switch back to yoke check phase.
        if (cycle_available_[0] + threshold <= current_cycle()) {
            current_phase_ = CHECK_YOKE;
            cycle_available_[0] = current_cycle() + 2;
            ro_++;
        }
        break;
    }
    return 1;
}

void YOKED_COLD_STORAGE::error_stats() {
    std::cout << "YOKED_COLD_STORAGE Error Stats:\n";
    std::cout << "Total yoke cycles completed: " << ro_ << "\n";
    std::cout << "RMQ r per yoke cycle: " << pow(sum_rpow4_ / ro_, 0.25) << "\n";
    std::cout << "Ideal yoke cycle rounds: " << yoke_cycle_rounds_ << "\n";
}

} // namespace sim