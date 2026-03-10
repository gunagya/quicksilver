/*
 *  author: Gunagya Singh Mamak
 *  date:   2 March 2026
 * */

#ifndef COMPILER_PREFETCHER_h
#define COMPILER_PREFETCHER_h

#include "dag.h"
#include "generic_io.h"

#include <memory>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Type aliases for common types used throughout prefetcher
 * */

using inst_ptr = DAG::inst_ptr;
using dag_ptr = std::unique_ptr<DAG>;

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * `config_type` allows the user to control
 * execution knobs, such as verbosity or DAG capacity.
 * */

struct config_type
{
    int64_t inst_compile_limit{15'000'000};
    int64_t print_progress_frequency{1'000'000};
    int64_t dag_inst_capacity{8192};
    bool    verbose{false};
    int64_t active_set_capacity{12};
    int64_t intermediate_buffer_capacity{8};
    int64_t prefetch_lookahead_depth{100};
};

/*
 * `stats_type` contains relevant compilation
 * statistics.
 * */

struct stats_type
{
    uint64_t unrolled_inst_done{0};
    uint64_t layers_processed{0};
    uint64_t prefetch_operations{0};
    uint64_t cold_memory_accesses{0};
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * This is the main prefetcher function.
 *
 * `run` processes instructions layer by layer
 * from the input stream and writes to output stream.
 *
 * The user must provide a prefetcher implementation callable with signature:
 *   std::vector<inst_ptr> operator()(const std::vector<inst_ptr>&, const dag_ptr&, config_type)
 * */
template <class PREFETCHER_IMPL>
stats_type run(generic_strm_type& ostrm, generic_strm_type& istrm, const PREFETCHER_IMPL&, config_type);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Helper functions for prefetching
 * */

/*
 * Reads instructions into the DAG until `DAG::inst_count() >= until_capacity`
 * */
void read_instructions_into_dag(dag_ptr& dag, generic_strm_type& istrm, size_t until_capacity);

/*
 * Drains all instructions from `begin` to `end` and writes them to `ostrm`.
 * Instructions are also freed after doing so.
 * */
template <class ITER>
void drain_buffer_into_stream(ITER begin, ITER end, generic_strm_type& ostrm);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile

#include "compiler/prefetcher/prefetcher.tpp"

#endif  // COMPILER_PREFETCHER_h
