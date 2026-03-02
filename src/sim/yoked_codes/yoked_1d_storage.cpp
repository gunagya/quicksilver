/*
 *  author: Gunagya Singh Mamak
 *  date:   17 February 2026
 * */

#include "sim/yoked_codes/yoked_1d_storage.h"

#include "globals.h"
#include "sim/client.h"
#include "sim/configuration/resource_estimation.h"
#include "sim/memory_subsystem.h"
#include <cmath>

namespace sim {

namespace {

size_t row_length(size_t rows, size_t logical_qubit_count) {
    // Row length must be a multiple of 2. Finds the smallest multiple of 2 that suffices.
    size_t row_length = (logical_qubit_count + rows - 1) / rows;
    if (row_length % 2 != 0) {
        row_length++;
    }
    return row_length + 2;
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
            if (need_qubits_.count(q) > 0) {
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

    // 1. Find the qubit with lowest order (earliest layer/depth) that is in memory system and not in 1d storage.
    QUBIT* ld = nullptr;
    size_t min_order = std::numeric_limits<size_t>::max();
    
    auto it = need_qubits_.begin();
    while (it != need_qubits_.end()) {
        QUBIT* candidate = it->first;
        size_t order = it->second;
        
        if (contains(candidate) || memory_subsystem_->retrieve_qubit(candidate->client_id, candidate->qubit_id) == nullptr) {
            // Already in 1d storage or not in memory system -- skip.
            it = need_qubits_.erase(it);
        } else {
            if (order < min_order) {
                min_order = order;
                ld = candidate;
            }
            ++it;
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

void YOKED_1D_STORAGE::feed_memory_instructions(std::vector<std::pair<size_t, INSTRUCTION*>> mem_insts, 
    const std::vector<QUBIT*>& client_qubits) {
    need_qubits_.clear();
    remove_qubits_.clear();
    for (const auto& [order, inst] : mem_insts) {
        QUBIT* q = client_qubits[inst->q_begin()[0]];
        // Keep the minimum order if qubit appears multiple times
        if (need_qubits_.count(q) == 0) {
            need_qubits_[q] = order;
        } else {
            need_qubits_[q] = std::min(need_qubits_[q], order);
        }
    }
    for (auto qubit: contents()) {
        if (need_qubits_.count(qubit) == 0) {
            remove_qubits_.push_back(qubit);
        }
    }
    
    // Track statistics
    s_total_needed_qubits += need_qubits_.size();
    s_feed_call_count++;
}

void YOKED_1D_STORAGE::error_stats() {
    std::cout << "YOKED_1D_STORAGE Error Stats:\n";
    std::cout << "RMS r per yoke cycle: " << std::pow(sum_rpow2_ / ro_, 0.5) << " vs an ideal " << yoke_cycle_rounds_ << "\n";
    std::cout << "Per logical-qubit round error rate: " << std::scientific << (sum_rpow2_ * pow(row_length_, 2) 
    * std::pow(15.0, -static_cast<double>(inner_code_distance_)) / 100.0) 
    / (current_cycle() * effective_code_distance_ * (row_length_-2))<<'\n';
    
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
    
    if (s_feed_call_count > 0) {
        double avg_needed_qubits = static_cast<double>(s_total_needed_qubits) / s_feed_call_count;
        print_stat_line(std::cout, "Avg needed qubits per feed call", avg_needed_qubits);
        print_stat_line(std::cout, "Total feed_memory_instructions calls", s_feed_call_count);
    } else {
        print_stat_line(std::cout, "Avg needed qubits per feed call", 0.0);
    }
}

} // namespace sim