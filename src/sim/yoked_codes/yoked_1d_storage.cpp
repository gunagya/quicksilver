/*
 *  author: Gunagya Singh Mamak
 *  date:   17 February 2026
 * */

#include "sim/yoked_codes/yoked_1d_storage.h"

#include "globals.h"
#include "sim/client.h"
#include "sim/configuration/resource_estimation.h"
#include "sim/memory_subsystem.h"

namespace sim {

namespace {

size_t row_length(size_t rows, size_t logical_qubit_count) {
    // Row length must be a multiple of 2. Finds the smallest multiple of 2 that suffices.
    size_t row_length = (logical_qubit_count + rows - 1) / rows;
    if (row_length % 2 != 0) {
        row_length++;
    }
    return row_length;
}

size_t calculate_physical_qubit_count(size_t rows, size_t row_length, size_t inner_code_distance, size_t effective_code_distance) {
    return configuration::surface_code_physical_qubit_count(inner_code_distance) * rows * row_length 
    + configuration::surface_code_physical_qubit_count(effective_code_distance, inner_code_distance) * (rows + row_length)
    + configuration::surface_code_physical_qubit_count(effective_code_distance);
}

} // anon

YOKED_1D_STORAGE::YOKED_1D_STORAGE(double freq_khz, 
    size_t rows, 
    size_t logical_qubit_count, 
    size_t inner_code_distance, 
    size_t effective_code_distance)
    : STORAGE(freq_khz,
              calculate_physical_qubit_count(rows, 
                row_length(rows, logical_qubit_count), 
                inner_code_distance, 
                effective_code_distance),
              logical_qubit_count,
              effective_code_distance,
              1,
              7,
              2),
      rows_(rows),
      row_length_(row_length(rows, logical_qubit_count)),
      yoke_cycle_rounds_((8*rows+2)*inner_code_distance),
      max_mem_rounds_(4*yoke_cycle_rounds_),
      inner_code_distance_(inner_code_distance), 
      effective_code_distance_(effective_code_distance) {
    cycle_available_[0] = 1;
}

void YOKED_1D_STORAGE::set_memory_subsystem(MEMORY_SUBSYSTEM* mem_subsystem) {
    memory_subsystem_ = mem_subsystem;
}

long YOKED_1D_STORAGE::operate() {
    r_ += effective_code_distance_;
    
    // Track percentage of needed qubits in 1D storage every cycle
    if (!contents().empty()) {
        size_t needed_count = 0;
        for (auto* q : contents()) {
            if (need_qubits_.count(q) > 0 && need_qubits_[q]) {
                needed_count++;
            }
        }
        double needed_percentage = 100.0 * needed_count / contents().size();
        s_total_needed_percentage += needed_percentage;
        s_cycle_samples++;
    }
    
    switch (current_phase_) {
        case CHECK_YOKE:
            phase_progress_ += effective_code_distance_;
            if (phase_progress_ >= yoke_cycle_rounds_) {
                // Yoke check complete, reset progress and move to memory ops
                phase_progress_ = 0;
                current_phase_ = MEMORY_OPS;
            } else {
                cycle_available_[0]++;
            }
            break;
        case MEMORY_OPS:
            phase_progress_ += effective_code_distance_;
            if (has_free_adapter() && phase_progress_ >= max_mem_rounds_) {
                // Memory ops complete, reset progress and move back to yoke check
                sum_rpow2_ += pow(r_, 2);
                r_ = 0;
                phase_progress_ = 0;
                current_phase_ = CHECK_YOKE;
                cycle_available_[0] = current_cycle() + 2;
                ro_++;
            } else {
                schedule_memory_operations();
            }
            break;
    }
    return 1;
}

YOKED_1D_STORAGE::access_result_type 
YOKED_1D_STORAGE::do_memory_access(QUBIT* ld, QUBIT* st) {
    // Call base class implementation
    auto result = STORAGE::do_memory_access(ld, st);
    
    if (result.success) {
        // Track residence time for qubit going to compute region
        if (qubit_entry_cycle_.count(ld) > 0) {
            s_residence_time_to_compute += current_cycle() - qubit_entry_cycle_[ld];
            s_qubits_to_compute++;
            qubit_entry_cycle_.erase(ld);
        }
        
        // Track entry time for qubit being stored in 1D
        qubit_entry_cycle_[st] = current_cycle();
    }
    
    return result;
}

void YOKED_1D_STORAGE::schedule_memory_operations() {
    if (!has_free_adapter() || remove_qubits_.empty() || need_qubits_.empty())
        return;

    // 1. Find the first qubit in need_qubits that is in the memory system and not already in 1d storage.
    QUBIT* ld = nullptr;
    auto it = need_qubits_.begin();
    while (it != need_qubits_.end()) {
        QUBIT* candidate = it->first;
        if (contains(candidate) || memory_subsystem_->retrieve_qubit(candidate->client_id, candidate->qubit_id) == nullptr) {
            // Already in 1d storage or not in memory system -- skip.
            it = need_qubits_.erase(it);
        } else {
            ld = candidate;
            break;
        }
    }
    if (ld == nullptr)
        return;

    // 2. Pick the back of remove_qubits as the qubit to evict.
    QUBIT* st = remove_qubits_.back();

    // 3. Try to schedule the memory operation.
    STORAGE::access_result_type result = memory_subsystem_->do_memory_access(ld, st, current_cycle(), freq_khz);
    if (!result.success)
        return;

    // 4. Swap in 1d storage and clean up.
    result = STORAGE::do_memory_access(st, ld);
    assert(result.success);

    // Track residence time statistics
    // ld is entering 1D storage from 2D
    qubit_entry_cycle_[ld] = current_cycle();
    
    // st is exiting 1D storage to 2D
    if (qubit_entry_cycle_.count(st) > 0) {
        s_residence_time_to_2d += current_cycle() - qubit_entry_cycle_[st];
        s_qubits_to_2d++;
        qubit_entry_cycle_.erase(st);
    }

    remove_qubits_.pop_back();
    need_qubits_.erase(ld);
}

void YOKED_1D_STORAGE::feed_memory_instructions(std::vector<INSTRUCTION*> mem_insts, 
    const std::vector<QUBIT*>& client_qubits) {
    need_qubits_.clear();
    remove_qubits_.clear();
    for (auto* inst : mem_insts) {
        need_qubits_[client_qubits[inst->q_begin()[0]]] = true;
    }
    for (auto qubit: contents()) {
        if (!need_qubits_[qubit]) {
            remove_qubits_.push_back(qubit);
        }
    }
}

void YOKED_1D_STORAGE::error_stats() {
    std::cout << "YOKED_1D_STORAGE Error Stats:\n";
    std::cout << "Total yoke cycles completed: " << ro_ << "\n";
    std::cout << "RMS r per yoke cycle: " << pow(sum_rpow2_ / ro_, 0.5) << "\n";
    std::cout << "Ideal yoke cycle rounds: " << yoke_cycle_rounds_ << "\n";
    
    std::cout << "\nResidence Time Statistics:\n";
    if (s_qubits_to_compute > 0) {
        double avg_residence_to_compute = static_cast<double>(s_residence_time_to_compute) / s_qubits_to_compute;
        print_stat_line(std::cout, "Avg residence time (1D -> Compute) [cycles]", avg_residence_to_compute);
        print_stat_line(std::cout, "Total qubits: 1D -> Compute", s_qubits_to_compute);
    } else {
        print_stat_line(std::cout, "Avg residence time (1D -> Compute) [cycles]", 0.0);
    }
    
    if (s_qubits_to_2d > 0) {
        double avg_residence_to_2d = static_cast<double>(s_residence_time_to_2d) / s_qubits_to_2d;
        print_stat_line(std::cout, "Avg residence time (1D -> 2D) [cycles]", avg_residence_to_2d);
        print_stat_line(std::cout, "Total qubits: 1D -> 2D", s_qubits_to_2d);
    } else {
        print_stat_line(std::cout, "Avg residence time (1D -> 2D) [cycles]", 0.0);
    }
    
    std::cout << "\n1D Storage Utilization:\n";
    if (s_cycle_samples > 0) {
        double avg_needed_percentage = s_total_needed_percentage / s_cycle_samples;
        print_stat_line(std::cout, "Avg % of needed qubits in 1D storage", avg_needed_percentage);
        print_stat_line(std::cout, "Cycles sampled", s_cycle_samples);
    } else {
        print_stat_line(std::cout, "Avg % of needed qubits in 1D storage", 0.0);
    }
}

} // namespace sim