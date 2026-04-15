/*
 *  author: Gunagya Singh Mamak
 *  date:   27 January 2026
 * */

#include "sim/yoked_codes/yoked_cold_storage.h"
#include "sim/configuration/resource_estimation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ios>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace {

// Limit how long a cold block can remain in MEMORY_OPS before it must run a yoke check.
constexpr cycle_type max_mem_ops_phase_cycles = 100;
constexpr double error_rate_warning_threshold = 1e-15;

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
             8,                                                         // load_latency
             4),                                                         // store_latency
        grid_length_(grid_length(logical_qubit_count)),
        inner_code_distance_(inner_code_distance), effective_code_distance_(effective_code_distance),
        yoke_cycle_rounds_((25 * grid_length_ + 4) * inner_code_distance),
        check_yokes_if_idle_for_cycles_(14) {
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
    if (current_phase_ != MEMORY_OPS || yoke_check_requested_) {
        return STORAGE::access_result_type{};
    }

    auto result = STORAGE::do_memory_access(ld, st);
    if (result.success) {
        // Track the qubit loaded from cold storage (st is being stored, ld is being loaded)
        unverified_loaded_qubits_.push_back(ld);
        newly_stored_qubits_.push_back(st);
        s_mem_ops_this_phase_++;
    }
    return result;
}

bool YOKED_COLD_STORAGE::has_memory_access_in_flight() const {
    return std::any_of(
        cycle_available_.begin(),
        cycle_available_.end(),
        [cc = current_cycle()](cycle_type cycle_available) {
            return cycle_available > cc;
        });
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
            memory_ops_phase_start_cycle_ = current_cycle();
            yoke_check_requested_ = false;
            
            for (QUBIT* q : unverified_loaded_qubits_) {
                newly_verified_qubits_.push_back(q);
            }
            unverified_loaded_qubits_.clear();
        } else {
            cycle_available_[0]++;
        }
        break;
    case MEMORY_OPS:
        if (!yoke_check_requested_
            && (current_cycle() - memory_ops_phase_start_cycle_ >= max_mem_ops_phase_cycles
                || cycle_available_[0] + check_yokes_if_idle_for_cycles_ <= current_cycle())) {
            yoke_check_requested_ = true;
        }

        if (yoke_check_requested_ && !has_memory_access_in_flight()) {
            // Flush this phase's op count before switching back to CHECK_YOKE.
            if (s_mem_ops_this_phase_ > 0) {
                s_total_mem_ops_active_ += s_mem_ops_this_phase_;
                s_active_mem_phases_++;
            }
            s_mem_ops_this_phase_ = 0;
            yoke_check_requested_ = false;
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
    std::cout << "RMQ r per yoke cycle: " << std::pow(sum_rpow4_ / ro_, 0.25) << " vs an ideal " << yoke_cycle_rounds_ << "\n";
    const double logical_round_error_rate =
        (sum_rpow4_ * pow(grid_length_, 4)
        * std::pow(150.0, -static_cast<double>(inner_code_distance_)) / 50000.0)
        / (current_cycle() * effective_code_distance_ * ((grid_length_-2)*(grid_length_-2)-2));
    std::cout << "Per logical-qubit round error rate:" << std::scientific
              << logical_round_error_rate << '\n';
    if (logical_round_error_rate > error_rate_warning_threshold) {
        std::cerr << "[YOKED_COLD_STORAGE] logical-qubit round error rate exceeded threshold: "
                  << logical_round_error_rate << " > " << error_rate_warning_threshold << "\n";
    }
    if (s_active_mem_phases_ > 0) {
        const double avg = static_cast<double>(s_total_mem_ops_active_) / s_active_mem_phases_;
        std::cout << "Avg mem ops per active MEMORY_OPS phase: " << avg
                  << " (over " << s_active_mem_phases_ << " non-empty phases)\n";
    } else {
        std::cout << "Avg mem ops per active MEMORY_OPS phase: N/A (no non-empty phases)\n";
    }
}

} // namespace sim
