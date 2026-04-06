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
#include <queue>
#include <unordered_set>

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

    /*
     * Enqueues a batch of compiled prefetch operations, skipping any whose
     * INSTRUCTION* is already in-flight.  Returns the count of newly enqueued
     * (non-duplicate) entries.
     */
    size_t feed_prefetch_instructions(
        std::vector<std::tuple<QUBIT*, QUBIT*, INSTRUCTION*>> prefetches);

    /*
     * Returns the MPREFETCH instructions whose operations have finished
     * (either executed or elided as stale) since the last call.
     * The caller should retire each returned instruction from the DAG.
     */
    std::vector<INSTRUCTION*> drain_completed_prefetches();

    /*
     * Performs the 1D-side swap for an MPLACE instruction.
     * evict_1d leaves 1D storage (goes to cold); st_compute enters 1D storage (from compute).
     * Tracks residence time correctly (evict_1d departure counted as 1D→cold).
     */
    access_result_type do_placement_eviction(QUBIT* evict_1d, QUBIT* st_compute);

    void error_stats();

  protected:
    const size_t rows_;
    const size_t row_length_;
    const size_t yoke_cycle_rounds_;
    const size_t max_mem_rounds_;

    // Queue of prefetch ops fed by the compute region.
    // ld = qubit to pull from cold into 1D; st = victim to evict from 1D to cold.
    // inst = the MPREFETCH DAG instruction whose retirement is deferred.
    struct pending_prefetch_entry { QUBIT* ld; QUBIT* st; INSTRUCTION* inst; };
    std::queue<pending_prefetch_entry> pending_prefetches_;

    // Instructions that have completed (executed or elided); drained by the architecture.
    std::queue<INSTRUCTION*> completed_prefetches_;

    // Statistics for tracking residence time (split by destination)
    std::map<QUBIT*, cycle_type> qubit_entry_cycle_;  // Track when qubits enter 1D storage
    uint64_t s_residence_time_to_compute{0};  // Cycles for qubits loaded to compute region
    uint64_t s_qubits_to_compute{0};  // Count of qubits loaded to compute region
    uint64_t s_residence_time_to_2d{0};  // Cycles for qubits evicted to 2D storage
    uint64_t s_qubits_to_2d{0};  // Count of qubits evicted to 2D storage

    // Statistics for prefetch operations
    uint64_t s_prefetches_received{0};  // Total prefetch MSWAPs enqueued
    uint64_t s_prefetches_executed{0};  // Successfully executed prefetches
    uint64_t s_prefetches_elided{0};              // Prefetches dropped as stale at execution time
    uint64_t s_prefetches_elided_ld_in_compute{0}; // Elided because ld already reached compute

    // Statistics for time between consecutive 1D-side memory op starts.
    uint64_t s_memory_op_starts{0};
    uint64_t s_inter_memory_op_start_cycles{0};
    cycle_type last_memory_op_start_cycle_{0};
    bool has_last_memory_op_start_cycle_{false};
  
    enum PHASE {
        CHECK_YOKE,
        MEMORY_OPS,
    } current_phase_{CHECK_YOKE};
    
    virtual access_result_type do_memory_access(QUBIT* ld, QUBIT* st) override;
    size_t phase_progress_{0};

    size_t r_{0}, ro_{0};
    double sum_rpow2_{0};

    virtual long operate() override;

    // Instructions currently enqueued in pending_prefetches_ but not yet
    // completed.  Used to deduplicate re-feeds from the architecture.
    std::unordered_set<const INSTRUCTION*> inflight_prefetches_;

  private:
    MEMORY_SUBSYSTEM* memory_subsystem_;

    void finalize_yoke_check();
    void begin_idle_yoke_check();
    void record_memory_op_start();
    void execute_prefetch();
};

}  // namespace sim

#endif  // SIM_YOKED_CODES_YOKED_COLD_STORAGE_h
