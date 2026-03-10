/*
 *  author: Gunagya Singh Mamak
 *  date:   17 February 2026
 * */

#include "sim/yoked_codes/yoked_1d_storage.h"

#include "globals.h"
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
                execute_prefetch();
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

void YOKED_1D_STORAGE::execute_prefetch() {
    if (pending_prefetches_.empty() || !has_free_adapter())
        return;
    // Drain stale entries from the front of the queue.
    // A prefetch (ld, st) is stale if:
    //   - st is no longer in 1D storage (already evicted), OR
    //   - ld is already in 1D storage (already prefetched), OR
    //   - ld is not in the memory hierarchy at all (it reached compute).
    while (!pending_prefetches_.empty()) {
        auto& entry = pending_prefetches_.front();
        QUBIT* ld = entry.ld;
        QUBIT* st = entry.st;
        const bool st_in_1d   = contains(st);
        const bool ld_in_1d   = contains(ld);
        const bool ld_in_cold = !ld_in_1d
            && memory_subsystem_->retrieve_qubit(ld->client_id, ld->qubit_id) != nullptr;
        if (!st_in_1d || !ld_in_cold) {
            completed_prefetches_.push(entry.inst);
            pending_prefetches_.pop();
            s_prefetches_elided++;
            // ld not in 1D and not found in cold storage -> already reached compute
            if (!ld_in_1d && !ld_in_cold)
                s_prefetches_elided_ld_in_compute++;
            continue;
        }
        break;  // valid prefetch at front
    }

    if (pending_prefetches_.empty())
        return;

    auto& entry = pending_prefetches_.front();
    QUBIT* ld = entry.ld;
    QUBIT* st = entry.st;

    // Attempt the cold-storage side of the swap.
    STORAGE::access_result_type result =
        memory_subsystem_->do_memory_access(ld, st, current_cycle(), freq_khz);
    if (!result.success)
        return;  // cold adapter busy; leave entry in queue, retry next tick

    INSTRUCTION* inst = entry.inst;
    pending_prefetches_.pop();

    // Update 1D storage contents: st leaves 1D, ld enters 1D.
    result = STORAGE::do_memory_access(st, ld);
    assert(result.success);

    // Track residence time: st exits 1D to cold.
    if (qubit_entry_cycle_.count(st) > 0) {
        s_residence_time_to_2d += current_cycle() - qubit_entry_cycle_[st];
        s_qubits_to_2d++;
        qubit_entry_cycle_.erase(st);
    }
    // Track entry time: ld enters 1D from cold.
    qubit_entry_cycle_[ld] = current_cycle();

    s_prefetches_executed++;
    completed_prefetches_.push(inst);
}

size_t YOKED_1D_STORAGE::feed_prefetch_instructions(
    std::vector<std::tuple<QUBIT*, QUBIT*, INSTRUCTION*>> prefetches)
{
    size_t newly_added = 0;
    for (auto& [ld, st, inst] : prefetches) {
        if (inflight_prefetches_.count(inst))
            continue;  // already queued, skip silently
        inflight_prefetches_.insert(inst);
        pending_prefetches_.push({ld, st, inst});
        s_prefetches_received++;
        newly_added++;
    }
    return newly_added;
}

std::vector<INSTRUCTION*> YOKED_1D_STORAGE::drain_completed_prefetches() {
    std::vector<INSTRUCTION*> out;
    while (!completed_prefetches_.empty()) {
        INSTRUCTION* inst = completed_prefetches_.front();
        completed_prefetches_.pop();
        inflight_prefetches_.erase(inst);
        out.push_back(inst);
    }
    return out;
}

YOKED_1D_STORAGE::access_result_type
YOKED_1D_STORAGE::do_placement_eviction(QUBIT* evict_1d, QUBIT* st_compute)
{
    // Perform the base storage swap: evict_1d leaves 1D, st_compute enters 1D.
    auto result = STORAGE::do_memory_access(evict_1d, st_compute);
    if (!result.success)
        return result;

    // Residence time: evict_1d exits 1D toward cold (not compute).
    if (qubit_entry_cycle_.count(evict_1d) > 0) {
        s_residence_time_to_2d += current_cycle() - qubit_entry_cycle_[evict_1d];
        s_qubits_to_2d++;
        qubit_entry_cycle_.erase(evict_1d);
    }
    // Track entry time for the compute qubit entering 1D storage.
    qubit_entry_cycle_[st_compute] = current_cycle();

    return result;
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
    
    std::cout << "\nPrefetch Stats:\n";
    print_stat_line(std::cout, "Prefetches received",  s_prefetches_received);
    print_stat_line(std::cout, "Prefetches executed",  s_prefetches_executed);
    print_stat_line(std::cout, "Prefetches elided (stale)", s_prefetches_elided);
    print_stat_line(std::cout, "  of which: ld already in compute", s_prefetches_elided_ld_in_compute);
    if (s_prefetches_received > 0) {
        double hit_rate = 100.0 * s_prefetches_executed / s_prefetches_received;
        print_stat_line(std::cout, "Prefetch success rate (%)", hit_rate);
    } else {
        print_stat_line(std::cout, "Prefetch success rate (%)", 0.0);
    }
}

} // namespace sim