/*
 *  author: Gunagya Singh Mamak
 *  date:   2 March 2026
 * */

#include "compiler/prefetcher/prefetcher.h"

namespace compile
{
namespace prefetcher
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
read_instructions_into_dag(dag_ptr& dag, generic_strm_type& istrm, size_t until_capacity)
{
    while (dag->inst_count() < until_capacity && !generic_strm_eof(istrm))
    {
        inst_ptr inst = read_instruction_from_stream(istrm);
        // immediately elide software instructions
        if (is_software_instruction(inst->type))
            delete inst;
        else
            dag->add_instruction(inst);
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace prefetcher
}  // namespace compile
