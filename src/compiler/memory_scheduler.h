/*
 *  author: Suhas Vittal
 *  date:   4 January 2026
 * */

#ifndef COMPILER_MEMORY_SCHEDULER_h
#define COMPILER_MEMORY_SCHEDULER_h

#include "dag.h"
#include "generic_io.h"

#include <memory>
#include <unordered_set>

namespace compile
{
namespace memory_scheduler
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Type aliases for common types used throughout memory scheduler
 * */

using inst_ptr = DAG::inst_ptr;
using dag_ptr = std::unique_ptr<DAG>;
using active_set_type = std::unordered_set<qubit_type>;

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `config_type` allows the user to control
 * execution knobs, such as verbosity or active set
 * size.
 * */

struct config_type
{
    int64_t active_set_capacity{12};
    int64_t inst_compile_limit{15'000'000};
    int64_t print_progress_frequency{1'000'000};
    int64_t dag_inst_capacity{8192};
    bool    verbose{false};

    /* Policy specific parameters */
    int64_t hint_lookahead_depth{16};
    bool    hint_use_complex_selection{true};

    /* Singlepass scheduler parameters */
    int64_t intermediate_storage_capacity{8};
    int64_t prefetch_min_layer_distance{0};  // suppress prefetch if mswap_layer - candidate_layer < this; 0 = disabled
};

/*
 * `stats_type` contains relevant compilation
 * statistics. Feel free to add your own.
 * */

struct stats_type
{
    uint64_t unrolled_inst_done{0};
    uint64_t memory_accesses{0};
    uint64_t scheduler_epochs{0};
    uint64_t total_unused_bandwidth{0};

    /* Singlepass prefetch stats (zero for EIF/HINT) */
    uint64_t prefetches_emitted{0};
    uint64_t prefetch_hits{0};
    uint64_t prefetch_misses{0};
    uint64_t prefetch_suppressed{0};  // suppressed by min-layer-distance threshold
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * For `run` (see below), the user must pass in
 * a function (or lambda) that corresponds to their
 * memory scheduler implementation (essentially, what happens
 * when no more compute instructions can be scheduled
 * on some set of active qubits).
 *
 * This function must take in:
 *  (1) the current active set of qubits (i.e., `const active_set_type&`)
 *  (2) a reference to the DAG (i.e., `const dag_ptr&`)
 *  (3) the configuration (`config_type`)
 *
 * And return `result_type`, which is defined here.
 *
 * For constructing `result_type`, it is recommended to create
 * a "target active set" (essentially, what you want to have
 * in the active set) and call `transform_active_set` which
 * will handle memory instruction generation for you.
 * */

struct result_type
{
    /*
     * This is the list of MSWAP instructions generated during
     * this scheduling epoch.
     * */
    std::vector<inst_ptr> memory_accesses;

    /*
     * This is the updated `active_set`.
     * */
    active_set_type active_set;

    /*
     * Number of qubits left untouched by the memory accesses.
     * A nonzero number indicates that other memory accesses
     * could've been done (they may not be useful, however).
     * */
    size_t unused_bandwidth;

    /*
     * MPREFETCH instructions to emit before `memory_accesses` this epoch.
     * Populated only by singlepass schedulers; empty for EIF/HINT.
     * */
    std::vector<inst_ptr> prefetch_accesses;
};

/*
 * Transforms the current active set to match the target active set
 * by generating MSWAP instructions.
 *
 * Typically, a memory scheduler will identify *what* it wants
 * in the active set. This function converts between the
 * current active set and desired active set.
 * */
result_type transform_active_set(const active_set_type& current,
                                 const active_set_type& target);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * This is the main scheduler function.
 *
 * `schedule` schedules memory accesses for
 * the instructions contained in the file pointed to by
 * `istrm` and writes the new program to `ostrm`.
 *
 * The user must provide a scheduler implementation that
 * has an `emit_memory_instructions` method with signature:
 *   result_type emit_memory_instructions(const active_set_type&,
 *                                        const dag_ptr&,
 *                                        config_type)
 * */
template <class SCHEDULER_IMPL>
stats_type run(generic_strm_type& ostrm, generic_strm_type& istrm, const SCHEDULER_IMPL&, config_type);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Helper functions for memory scheduling
 * */

/*
 * Reads instructions into the DAG until `DAG::inst_count() >= until_capacity`
 * */
void read_instructions_into_dag(dag_ptr& dag, generic_strm_type& istrm, size_t until_capacity);

/*
 * Returns true if all of the instruction's args are in `active_set`
 * */
bool instruction_is_ready(inst_ptr inst, const active_set_type& active_set);

/*
 * Drains all instructions from `begin` to `end` and writes them to `ostrm`.
 * Instructions are also freed after doing so.
 * */
template <class ITER>
void drain_buffer_into_stream(ITER begin, ITER end, generic_strm_type& ostrm);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace memory_scheduler
}  // namespace compile

#include "compiler/memory_scheduler.tpp"

#endif  // COMPILER_MEMORY_SCHEDULER_h
