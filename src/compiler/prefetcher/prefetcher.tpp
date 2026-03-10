/*
 *  author: Gunagya Singh Mamak
 *  date:   2 March 2026
 * */

#include <cstdint>
#include <deque>

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

template <class PREFETCHER_IMPL> stats_type
run(generic_strm_type& ostrm, generic_strm_type& istrm, const PREFETCHER_IMPL& prefetcher, config_type conf)
{
    constexpr size_t OUTGOING_CAPACITY{16384};

    stats_type stats;

    // read number of qubits from `istrm`
    uint32_t num_qubits;
    generic_strm_read(istrm, &num_qubits, sizeof(num_qubits));
    generic_strm_write(ostrm, &num_qubits, sizeof(num_qubits));

    dag_ptr               dag{new DAG{num_qubits}};
    std::deque<inst_ptr>  outgoing_buffer;
    int64_t               inst_done{0};

    while (inst_done < conf.inst_compile_limit)
    {
        const uint64_t inst_done_before{inst_done};

        // try to fill up the DAG during every iteration
        if (!generic_strm_eof(istrm))
            read_instructions_into_dag(dag, istrm, conf.dag_inst_capacity);

        // get the front layer
        auto front_layer = dag->get_front_layer();

        if (front_layer.empty())
        {
            // no more instructions to process
            break;
        }

        // process the layer with the prefetcher implementation
        auto prefetches = prefetcher(front_layer, dag, conf);

        // move instructions to outgoing buffer and retire from DAG
        for (auto* inst : front_layer)
        {
            outgoing_buffer.push_back(inst);
            dag->remove_instruction_from_front_layer(inst);
            inst_done += inst->uop_count();
        }

        // append MPREFETCH instructions after the current front-layer instructions
        for (inst_ptr pf : prefetches)
            outgoing_buffer.push_back(pf);

        stats.layers_processed++;

        // flush outgoing buffer if it gets too large
        if (outgoing_buffer.size() >= OUTGOING_CAPACITY)
        {
            auto begin = outgoing_buffer.begin();
            auto end = begin + (OUTGOING_CAPACITY/2);
            drain_buffer_into_stream(begin, end, ostrm);
            outgoing_buffer.erase(begin, end);
        }

        if (conf.print_progress_frequency > 0
            && (inst_done % conf.print_progress_frequency) < (inst_done_before % conf.print_progress_frequency))
        {
            std::cout << "\nPrefetcher =================================================="
                        << "\ninstructions done = " << inst_done
                        << "\nlayers processed  = " << stats.layers_processed
                        << "\nDAG inst count = " << dag->inst_count() 
                            << " of " << conf.dag_inst_capacity
                        << "\n";
        }
    }

    // drain remaining instructions
    drain_buffer_into_stream(outgoing_buffer.begin(), outgoing_buffer.end(), ostrm);
    outgoing_buffer.clear();

    stats.unrolled_inst_done = inst_done;
    prefetcher.collect_stats(stats);
    return stats;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

template <class ITER> void
drain_buffer_into_stream(ITER begin, ITER end, generic_strm_type& ostrm)
{
    std::for_each(begin, end, 
            [&ostrm] (inst_ptr inst) 
            {
                write_instruction_to_stream(ostrm, inst);
                delete inst;
            });
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
