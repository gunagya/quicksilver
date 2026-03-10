/*
 *  author: Suhas Vittal
 *  date:   13 January 2026
 * */

#include "sim.h"
#include "sim/compute_base.h"
#include "sim/memory_subsystem.h"

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Helper function declarations:
 * */

namespace
{

void _update_available_cycle(std::vector<QUBIT*>, cycle_type);

}  // anon

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::COMPUTE_BASE(std::string_view             name,
                           double                       freq_khz,
                           size_t                       _local_memory_capacity,
                           std::vector<T_FACTORY_BASE*> top_level_t_factories,
                           MEMORY_SUBSYSTEM*            memory_hierarchy)
    :OPERABLE(name, freq_khz),
     local_memory_capacity(_local_memory_capacity),
     top_level_t_factories_(std::move(top_level_t_factories)),
     memory_hierarchy_(memory_hierarchy)
{
    // initialize local memory:
    local_memory_ = std::make_unique<STORAGE>(freq_khz,
                                            0,                      // n (does not matter)
                                            _local_memory_capacity, // k (matters)
                                            0,                      // d (does not matter)
                                            _local_memory_capacity, // num_adapters 
                                            0,                      // load latency (0, we model with cycle available)
                                            0);                     // store latency (0, we model with cycle available)
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

const std::unique_ptr<STORAGE>&
COMPUTE_BASE::local_memory() const
{
    return local_memory_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

const std::vector<T_FACTORY_BASE*>&
COMPUTE_BASE::top_level_t_factories() const
{
    return top_level_t_factories_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

MEMORY_SUBSYSTEM*
COMPUTE_BASE::memory_hierarchy() const
{
    return memory_hierarchy_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::execute_result_type
COMPUTE_BASE::execute_instruction(inst_ptr inst, std::array<QUBIT*, 3>&& args)
{
    if (is_software_instruction(inst->type))
        return execute_result_type{.progress=1, .latency=0};

    switch (inst->type)
    {
    case INSTRUCTION::TYPE::H:
    case INSTRUCTION::TYPE::S:
    case INSTRUCTION::TYPE::SX:
    case INSTRUCTION::TYPE::SDG:
    case INSTRUCTION::TYPE::SXDG:
        return do_h_or_s_gate(inst, args[0]);

    case INSTRUCTION::TYPE::CX:
    case INSTRUCTION::TYPE::CZ:
        return do_cx_like_gate(inst, args[0], args[1]);

    case INSTRUCTION::TYPE::T:
    case INSTRUCTION::TYPE::TX:
    case INSTRUCTION::TYPE::TDG:
    case INSTRUCTION::TYPE::TXDG:
        return do_t_like_gate(inst, args[0]);

    case INSTRUCTION::TYPE::MSWAP:
        return do_memory_access(inst, args[0], args[1]);

    case INSTRUCTION::TYPE::MPLACE:
        return do_placement_access(inst, args[0], args[1], args[2]);

    default:
        std::cerr << "COMPUTE_BASE::execute_instruction: unknown instruction: " << *inst << _die{};
    }

    return execute_result_type{};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::execute_result_type
COMPUTE_BASE::do_h_or_s_gate(inst_ptr inst, QUBIT* q)
{
    _update_available_cycle({q}, current_cycle()+1);
    return execute_result_type{.progress=1, .latency=1};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::execute_result_type
COMPUTE_BASE::do_cx_like_gate(inst_ptr inst, QUBIT* q1, QUBIT* q2)
{
    _update_available_cycle({q1, q2}, current_cycle()+2);
    return execute_result_type{.progress=1, .latency=2};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::execute_result_type
COMPUTE_BASE::do_t_like_gate(inst_ptr inst, QUBIT* q)
{
    // search for an available magic state:
    auto f_it = std::find_if(top_level_t_factories_.begin(), top_level_t_factories_.end(),
                        [] (const auto* f) { return f->buffer_occupancy() > 0; });
    if (f_it == top_level_t_factories_.end())
        return execute_result_type{};

    (*f_it)->consume(1);
    cycle_type latency;
    if (GL_ZERO_LATENCY_T_GATES)
        latency = 0;
    else if (GL_T_GATE_DO_AUTOCORRECT)
        latency = 2;
    else
        latency = 1;
    _update_available_cycle({q}, current_cycle() + latency);
    s_t_gates++;
    return execute_result_type{.progress=1, .latency=latency};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::execute_result_type
COMPUTE_BASE::do_memory_access(inst_ptr inst, QUBIT* ld, QUBIT* st)
{
    auto result = memory_hierarchy_->do_memory_access(ld, st, current_cycle(), freq_khz);
    if (result.success)
    {
        // need to convert storage latency to compute cycles:
        auto local_result = local_memory_->do_memory_access(st, ld);
        if (!local_result.success)
        {
            std::cerr << "COMPUTE_BASE::do_memory_access: local memory access failed.\n";
            local_memory_->print_adapter_debug_info(std::cerr);
            std::cerr << _die{};
        }
        _update_available_cycle({ld, st}, current_cycle() + result.latency + 2);
        return execute_result_type{.progress=1, .latency=result.latency+2};
    }
    return execute_result_type{};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::execute_result_type
COMPUTE_BASE::do_placement_access(inst_ptr /*inst*/, QUBIT* /*ld*/, QUBIT* /*st*/, QUBIT* /*evict_1d*/)
{
    std::cerr << "COMPUTE_BASE::do_placement_access: MPLACE not supported by this architecture" << _die{};
    return execute_result_type{};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

COMPUTE_BASE::execute_result_type
COMPUTE_BASE::do_rotation_gate_with_teleportation(inst_ptr inst, QUBIT* q, size_t max_teleports)
{
    return do_rotation_gate_with_teleportation_while_predicate_holds(inst, q, max_teleports,
                [] (const auto*, const auto*) { return true; });
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

size_t
COMPUTE_BASE::count_available_magic_states() const
{
    return std::transform_reduce(top_level_t_factories_.begin(), top_level_t_factories_.end(),
                                size_t{0},
                                std::plus<size_t>{},
                                [] (const auto* f) { return f->buffer_occupancy(); });
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/* HELPER FUNCTION DEFINITIONS START HERE */

namespace
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
_update_available_cycle(std::vector<QUBIT*> qubits, cycle_type c)
{
    for (auto* q : qubits)
        q->cycle_available = std::max(q->cycle_available, c);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

} // anon

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace sim
