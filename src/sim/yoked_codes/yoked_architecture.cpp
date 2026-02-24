/*
 *  author: Gunagya Singh Mamak
 *  date:   9 February 2026
 * */

#include "sim/yoked_codes/yoked_architecture.h"
#include "globals.h"
#include "instruction.h"
#include "sim/client.h"
#include "sim/memory_subsystem.h"
#include "sim/storage.h"
#include "sim/yoked_codes/yoked_1d_storage.h"
#include "sim/yoked_codes/yoked_cold_storage.h"
#include <string>
#include <utility>
#include <vector>

namespace sim
{
namespace yoked_codes
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

YOKED_ARCHITECTURE::YOKED_ARCHITECTURE(double freq_khz,
                                      size_t local_memory_capacity,
                                      uint64_t simulation_instructions,
                                      MEMORY_SUBSYSTEM* memory,
                                      std::vector<T_FACTORY_BASE*> factories,
                                      std::string client_trace_file)
    : COMPUTE_BASE("yoked_compute_region", 
                    freq_khz,
                    local_memory_capacity,
                    factories,
                    memory),
    client_(client_trace_file, 0), simulation_instructions_(simulation_instructions)
{
    // initialize all the memory:
    std::vector<std::vector<QUBIT*>> qubits_by_client({client_.qubits()});
    std::vector<STORAGE*> all_storage{local_memory_.get()};
    std::copy(memory_hierarchy_->storages().begin(), memory_hierarchy_->storages().end(), std::back_inserter(all_storage));
    storage_striped_initialization(all_storage, qubits_by_client, 1);

    for (QUBIT* q : client_.qubits())
        can_operate_non_clifford_[q] = true;
    // Mark all qubits in cold storage as not ready for non-Clifford operations initially
    for (auto* storage : memory_hierarchy_->storages()) {
        if (auto* yoked_cold_storage = dynamic_cast<YOKED_COLD_STORAGE*>(storage))
            for (QUBIT* q : yoked_cold_storage->contents())
                can_operate_non_clifford_[q] = false;
        else if (auto* yoked_1d_storage = dynamic_cast<YOKED_1D_STORAGE*>(storage)) {
            yoked_1d_storage_ = yoked_1d_storage;
            yoked_1d_storage_->set_memory_subsystem(memory);
        }
    }
}

long
YOKED_ARCHITECTURE::operate()
{
    long progress{0};

    // Mark newly verified qubits from cold storage as ready for non-Clifford operations.
    for (auto* storage : memory_hierarchy_->storages())
        if (auto* yoked_cold_storage = dynamic_cast<YOKED_COLD_STORAGE*>(storage)) {
            for (QUBIT* q : yoked_cold_storage->drain_newly_verified_qubits())
                can_operate_non_clifford_[q] = true;
            for (QUBIT* q : yoked_cold_storage->drain_newly_stored_qubits())
                can_operate_non_clifford_[q] = false;
        }

    /* Handle pending instructions */

    progress += fetch_and_execute_instructions_from_client(&client_);

    /* Update stats (post-execution) */

    return progress;
}

long
YOKED_ARCHITECTURE::fetch_and_execute_instructions_from_client(CLIENT* c)
{
    if (yoked_1d_storage_!=nullptr && current_cycle()%dag_sample_rate == 0) {
        yoked_1d_storage_->feed_memory_instructions(c->dag()->get_memory_instructions_upto_layers(dag_lookahead), c->qubits());
    }

    auto front_layer = c->get_ready_instructions(
                            [&c, cc=current_cycle(), this] (const auto* inst)
                            {
                                bool ready = true;
                                // Check if all qubits are available.
                                ready &= std::all_of(inst->q_begin(), inst->q_end(),
                                            [&c, cc] (auto q_id) { return c->qubits()[q_id]->cycle_available <= cc; });
                                // Check if memory request can be served.
                                if (is_memory_access(inst->type)) {
                                    QUBIT* fetched_qubit = memory_hierarchy_->retrieve_qubit(0, inst->q_begin()[0]);
                                    ready &= (*memory_hierarchy_->lookup(fetched_qubit))->has_free_adapter();
                                } else {
                                    // Check if all qubits are in local memory for non-memory instructions.
                                    if(!std::all_of(inst->q_begin(), inst->q_end(),
                                                [this] (auto q_id) {
                                                    QUBIT* q = client_.qubits()[q_id];
                                                    return local_memory_->contains(q);
                                                }))
                                        throw std::runtime_error("YOKED_ARCHITECTURE::fetch_and_execute_instructions_from_client: instruction with qubits not in local memory reached execution stage");
                                }
                                // If this is a T or rotation gate, check for magic state availability and non-Clifford readiness.
                                if (is_t_like_instruction(inst->type) 
                                    || is_rotation_instruction(inst->type) && is_t_like_instruction(inst->current_uop()->type)
                                    || is_toffoli_like_instruction(inst->type) && is_t_like_instruction(inst->current_uop()->type)) {
                                    ready &= count_available_magic_states() > 0;
                                    ready &= can_operate_non_clifford_.at(c->qubits()[inst->q_begin()[0]]);
                                }

                                if (is_cx_like_instruction(inst->type) 
                                    || is_toffoli_like_instruction(inst->type) && is_cx_like_instruction(inst->current_uop()->type)) {
                                    // For CX-like gates, ensure both control and target are ready for non-Clifford operations
                                    ready &= can_operate_non_clifford_.at(c->qubits()[inst->q_begin()[0]]); // control
                                    ready &= can_operate_non_clifford_.at(c->qubits()[inst->q_begin()[1]]); // target
                                }

                                return ready;
                            });

    // print_deadlock_info(std::cout, front_layer);

    long success_count{0};
    for (auto* inst : front_layer)
    {
        if (GL_ELIDE_CLIFFORDS && !(is_rotation_instruction(inst->type) || is_t_like_instruction(inst->type) || is_memory_access(inst->type)))
            std::cerr << "COMPUTE_SUBSYSTEM::fetch_and_execute_instruction: unexpected clifford: " << *inst << _die{};
    
        inst->first_ready_cycle = std::min(current_cycle(), inst->first_ready_cycle);

        auto* executed_inst = (inst->uop_count() == 0) ? inst : inst->current_uop();

        std::array<QUBIT*, 3> operands;
        std::transform(executed_inst->q_begin(), executed_inst->q_end(), operands.begin(),
                [&c] (auto q_id) { return c->qubits()[q_id]; });
        
        auto result = execute_instruction(executed_inst, std::move(operands));
        success_count += result.progress;
        if (result.progress)
            if (inst->uop_count() == 0 || inst->retire_current_uop())
                retire_instruction(c, inst, result.latency);

    }

    // recursively call `fetch_and_execute_instruction_from_client` if progress was made
    if (success_count)
        success_count += fetch_and_execute_instructions_from_client(c);
    return success_count;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
YOKED_ARCHITECTURE::retire_instruction(CLIENT* c, inst_ptr inst, cycle_type inst_latency)
{
    inst->cycle_done = current_cycle() + inst_latency;
    c->retire_instruction(inst);
}

bool
YOKED_ARCHITECTURE::done()
{
    if (client_.s_unrolled_inst_done >= simulation_instructions_) {
        client_.s_cycle_complete = std::min(current_cycle(), client_.s_cycle_complete);
        return true;
    }
    return false;
}

void
YOKED_ARCHITECTURE::print_deadlock_info(std::ostream& ostrm, std::vector<CLIENT::inst_ptr> front_layer) const
{
    for (auto* f : top_level_t_factories_)
        f->print_deadlock_info(ostrm);

    std::cout << "local memory contents:";
    for (auto* q : local_memory_->contents())
        std::cout << " " << *q;
    std::cout << "\n";

    auto* c = &client_;
    {
        std::cout << "Client " << static_cast<int>(c->id) << " front layer:\n";
        for (const auto* inst : front_layer)
        {
            std::cout << "\t" << *inst;
            if (inst->uop_count() > 0)
            {
                std::cout << "\tcurrent uop = " << *inst->current_uop() << ", " << inst->uops_retired()
                            << " of " << inst->uop_count();
            }

            std::cout << "\tcycle ready (current cycle = " << current_cycle() << "):";
            std::for_each(inst->q_begin(), inst->q_end(),
                        [this, c] (auto q_id)
                        {
                            QUBIT* q = c->qubits()[q_id];
                            std::cout << " " << q->cycle_available;
                        });
            std::cout << "\tin memory: ";
            std::for_each(inst->q_begin(), inst->q_end(),
                        [this, c] (auto q_id)
                        {
                            QUBIT* q = c->qubits()[q_id];
                            std::cout << static_cast<int>(local_memory_->contains(q));
                        });
            std::cout << "\n";
        }
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace yoked_codes
}  // namespace sim
