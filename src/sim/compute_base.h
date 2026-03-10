/*
 *  author: Suhas Vittal
 *  date:   13 January 2026
 * */

#ifndef SIM_COMPUTE_BASE_h
#define SIM_COMPUTE_BASE_h

#include "sim/client.h"
#include "sim/factory.h"
#include "sim/storage.h"

#include <array>
#include <memory>
#include <vector>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class MEMORY_SUBSYSTEM;

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class COMPUTE_BASE : public OPERABLE
{
public:
    using inst_ptr = CLIENT::inst_ptr;

    /*
     * Results of `execute_instruction()`
     * */
    struct execute_result_type
    {
        long       progress{0};
        cycle_type latency;
    };

    uint64_t s_t_gates{0};
    uint64_t s_t_gate_teleports{0};
    uint64_t s_t_gate_teleport_episodes{0};

    const size_t local_memory_capacity;
protected:
    std::unique_ptr<STORAGE>     local_memory_;
    std::vector<T_FACTORY_BASE*> top_level_t_factories_;
    MEMORY_SUBSYSTEM*            memory_hierarchy_;
public:
    COMPUTE_BASE(std::string_view             name, 
                 double                       freq_khz,
                 size_t                       local_memory_capacity,
                 std::vector<T_FACTORY_BASE*> top_level_t_factories,
                 MEMORY_SUBSYSTEM*            memory_hierarchy);

    const std::unique_ptr<STORAGE>&     local_memory() const;
    const std::vector<T_FACTORY_BASE*>& top_level_t_factories() const;
    MEMORY_SUBSYSTEM*                   memory_hierarchy() const;
protected:
    virtual execute_result_type execute_instruction(inst_ptr, std::array<QUBIT*, 3>&& args);

    virtual execute_result_type do_h_or_s_gate(inst_ptr, QUBIT*);
    virtual execute_result_type do_cx_like_gate(inst_ptr, QUBIT* ctrl, QUBIT* target);
    virtual execute_result_type do_t_like_gate(inst_ptr, QUBIT*);
    virtual execute_result_type do_memory_access(inst_ptr, QUBIT* ld, QUBIT* st);
    virtual execute_result_type do_placement_access(inst_ptr, QUBIT* ld, QUBIT* st, QUBIT* evict_1d);

    /*
     * Executes the uops for a rotation gate. Upon a success, additional gates
     * are teleported onto the gate (max is `GL_T_TELEPORTATION_MAX`)
     * */
    virtual execute_result_type do_rotation_gate_with_teleportation(inst_ptr, QUBIT*, size_t max_teleports);

    /*
     * This is an extension of `do_rotation_gate_with_teleportation` that stops
     * applying T gates if the given predicate becomes false.
     *
     * The predicate should return a boolean (obviously), and takes the rotation instruction (`inst_ptr`)
     * and the current uop.
     * */
    template <class PRED> 
    execute_result_type do_rotation_gate_with_teleportation_while_predicate_holds(inst_ptr, 
                                                                                    QUBIT*, 
                                                                                    size_t max_teleports, 
                                                                                    const PRED&);

    size_t count_available_magic_states() const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Implementation of `do_rotation_gate_with_teleportation_while_predicate_holds`
 * */

template <class PRED> COMPUTE_BASE::execute_result_type
COMPUTE_BASE::do_rotation_gate_with_teleportation_while_predicate_holds(inst_ptr inst, 
                                                                            QUBIT* q, 
                                                                            size_t tp_remaining, 
                                                                            const PRED& pred)
{
    if (!pred(inst, inst->current_uop()))
        return execute_result_type{};
    execute_result_type out = execute_instruction(inst->current_uop(), {q});
    if (!out.progress || inst->retire_current_uop())
        return out;

    bool any_teleports{false};
    while (tp_remaining > 0 && pred(inst, inst->current_uop()))
    {
        // need to keep resetting available so instructions keep getting executed:
        q->cycle_available = current_cycle();

        auto result = execute_instruction(inst->current_uop(), {q});
        if (result.progress)
        {
            if (is_t_like_instruction(inst->current_uop()->type))
            {
                tp_remaining--;
                s_t_gate_teleports++;
                if (!GL_T_GATE_DO_AUTOCORRECT)
                    out.latency += (GL_RNG() & 3) ? 2 : 0; // any possible correction incurs a 2 cycle latency
                any_teleports = true;
            }
            out.progress += result.progress;
            if (inst->retire_current_uop())
                break;
        }
        else
        {
            break;
        }
    }

    if (any_teleports)
    {
        s_t_gate_teleport_episodes++;
        if (GL_T_GATE_DO_AUTOCORRECT)
            out.latency += 2;
    }

    if (GL_ZERO_LATENCY_T_GATES)
        out.latency = 0;

    q->cycle_available = current_cycle()+out.latency;

    return out;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace sim

#endif  // SIM_COMPUTE_BASE_h
