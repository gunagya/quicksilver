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
#include <algorithm>
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
    client_(client_trace_file, 0),
    simulation_instructions_(simulation_instructions)
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
            for (QUBIT* q : yoked_cold_storage->drain_newly_verified_qubits()) {
                // Only mark ready if the qubit is not in cold storage.
                if (local_memory_->contains(q) || (yoked_1d_storage_!=nullptr && yoked_1d_storage_->contains(q))) {
                    can_operate_non_clifford_[q] = true;
                    if (qubit_2d_load_cycle_.count(q) > 0) {
                        s_total_2d_to_ready_delay += current_cycle() - qubit_2d_load_cycle_[q];
                        s_2d_loads_delayed++;
                    }
                    // Track 1D delay only if the qubit has already been loaded into compute.
                    if (local_memory_->contains(q) && qubit_1d_load_cycle_.count(q) > 0) {
                        s_total_1d_to_ready_delay += current_cycle() - qubit_1d_load_cycle_[q];
                        s_1d_loads_delayed++;
                    }
                }
                qubit_1d_load_cycle_.erase(q);
                qubit_2d_load_cycle_.erase(q);
            }
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
    if (yoked_1d_storage_ != nullptr) {
        // Retire MPREFETCH instructions whose 1D<->cold operations have
        // completed (or been elided) since the last tick.  Retiring them
        // here — before get_ready_instructions — allows any downstream MSWAP
        // that was blocked on a MPREFETCH to become ready in the same tick.
        for (INSTRUCTION* done_inst : yoked_1d_storage_->drain_completed_prefetches())
            c->retire_instruction(done_inst);

        // Feed newly visible MPREFETCHes to 1D storage.  YOKED_1D_STORAGE
        // deduplicates by instruction pointer so re-feeding already-inflight
        // ones is safe.  Loop because retiring elided prefetches may unblock
        // further MPREFETCH nodes at the DAG front.
        while (true) {
            auto prefetch_insts = c->dag()->get_front_layer_if(
                [] (const auto* inst) { return is_prefetch_instruction(inst->type); });
            if (prefetch_insts.empty())
                break;
            std::vector<std::tuple<QUBIT*, QUBIT*, INSTRUCTION*>> prefetches;
            prefetches.reserve(prefetch_insts.size());
            for (auto* inst : prefetch_insts) {
                QUBIT* ld = c->qubits()[inst->q_begin()[0]];
                QUBIT* st = c->qubits()[inst->q_begin()[1]];
                prefetches.emplace_back(ld, st, inst);
            }
            size_t newly_fed = yoked_1d_storage_->feed_prefetch_instructions(std::move(prefetches));
            // Drain any immediately elided entries so downstream MSWAPs unblock.
            for (INSTRUCTION* done_inst : yoked_1d_storage_->drain_completed_prefetches())
                c->retire_instruction(done_inst);
            // If nothing new was enqueued all front-layer MPREFETCHes are
            // already in-flight; no further progress possible this tick.
            if (newly_fed == 0)
                break;
        }
    }

    auto front_layer = c->get_ready_instructions(
                            [&c, cc=current_cycle(), this] (const auto* inst)
                            {
                                // MPREFETCH instructions are handled separately; never execute them here.
                                if (is_prefetch_instruction(inst->type))
                                    return false;

                                const inst_ptr inst_ptr_mut = const_cast<sim::CLIENT::inst_ptr>(inst);
                                bool ready_without_non_clifford = true;
                                bool requires_non_clifford_ready = false;
                                bool non_clifford_ready = true;
                                // Check if all qubits are available.
                                ready_without_non_clifford &= std::all_of(inst->q_begin(), inst->q_end(),
                                            [&c, cc] (auto q_id) { return c->qubits()[q_id]->cycle_available <= cc; });
                                // Check if memory request can be served.
                                if (is_memory_access(inst->type)) {
                                    if (inst->type == INSTRUCTION::TYPE::MPLACE) {
                                        // MPLACE needs both cold adapter (for ld) AND
                                        // 1D adapter (for evict_1d) simultaneously.
                                        QUBIT* ld_qubit = memory_hierarchy_->retrieve_qubit(0, inst->q_begin()[0]);
                                        ready_without_non_clifford &= (ld_qubit != nullptr)
                                            && (*memory_hierarchy_->lookup(ld_qubit))->has_free_adapter();
                                        ready_without_non_clifford &= (yoked_1d_storage_ != nullptr)
                                            && yoked_1d_storage_->has_free_adapter();
                                    } else {
                                        QUBIT* fetched_qubit = memory_hierarchy_->retrieve_qubit(0, inst->q_begin()[0]);
                                        ready_without_non_clifford &= (*memory_hierarchy_->lookup(fetched_qubit))->has_free_adapter();
                                    }
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
                                    ready_without_non_clifford &= count_available_magic_states() > 0;
                                    requires_non_clifford_ready = true;
                                    non_clifford_ready &= can_operate_non_clifford_.at(c->qubits()[inst->q_begin()[0]]);
                                }

                                if (is_cx_like_instruction(inst->type) 
                                    || is_toffoli_like_instruction(inst->type) && is_cx_like_instruction(inst->current_uop()->type)) {
                                    // For CX-like gates, ensure both control and target are ready for non-Clifford operations
                                    requires_non_clifford_ready = true;
                                    auto curr_inst = inst;
                                    if (is_toffoli_like_instruction(inst->type))
                                        curr_inst = inst->current_uop();
                                    non_clifford_ready &= can_operate_non_clifford_.at(c->qubits()[curr_inst->q_begin()[0]]); // control
                                    non_clifford_ready &= can_operate_non_clifford_.at(c->qubits()[curr_inst->q_begin()[1]]); // target
                                }

                                const bool blocked_only_by_non_clifford =
                                    ready_without_non_clifford
                                    && requires_non_clifford_ready
                                    && !non_clifford_ready;

                                auto active_it = non_clifford_only_block_start_cycle_.find(inst_ptr_mut);
                                if (blocked_only_by_non_clifford) {
                                    if (active_it == non_clifford_only_block_start_cycle_.end())
                                        non_clifford_only_block_start_cycle_[inst_ptr_mut] = current_cycle();
                                } else if (active_it != non_clifford_only_block_start_cycle_.end()) {
                                    non_clifford_only_instruction_delay_[inst_ptr_mut] += current_cycle() - active_it->second;
                                    non_clifford_only_block_start_cycle_.erase(active_it);
                                }

                                return ready_without_non_clifford && non_clifford_ready;
                            });

    // Record the first cycle a top-level non-memory instruction becomes visible
    // at the DAG front layer, regardless of whether it is ready to issue yet.
    for (auto* inst : c->dag()->get_front_layer()) {
        if (is_prefetch_instruction(inst->type) || is_memory_access(inst->type))
            continue;
        front_layer_entry_cycle_.try_emplace(inst, current_cycle());
    }

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

YOKED_ARCHITECTURE::execute_result_type
YOKED_ARCHITECTURE::do_memory_access(inst_ptr inst, QUBIT* ld, QUBIT* st)
{
    // Determine which storage contains the qubit being loaded
    auto storage_it = memory_hierarchy_->lookup(ld);
    bool is_1d_load = false;
    bool is_2d_load = false;
    
    if (storage_it != memory_hierarchy_->storages().end()) {
        if (dynamic_cast<YOKED_1D_STORAGE*>(*storage_it)) {
            is_1d_load = true;
        } else if (dynamic_cast<YOKED_COLD_STORAGE*>(*storage_it)) {
            is_2d_load = true;
        }
    }
    
    // Call base class implementation
    auto result = COMPUTE_BASE::do_memory_access(inst, ld, st);
    
    if (result.progress) {
        record_memory_op_bucket(ld);

        // Track statistics
        if (is_1d_load) {
            s_1d_loads++;
            if (can_operate_non_clifford_[ld]) {
                // Qubit was already verified before the MSWAP fired — prefetch succeeded.
                s_1d_loads_already_ready++;
            } else {
                // Qubit not yet verified — record load cycle so we can measure the wait.
                qubit_1d_load_cycle_[ld] = current_cycle();
            }
        } else if (is_2d_load) {
            s_2d_loads++;
            qubit_2d_load_cycle_[ld] = current_cycle();
        }
    }
    
    return result;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

YOKED_ARCHITECTURE::execute_result_type
YOKED_ARCHITECTURE::do_placement_access(inst_ptr inst, QUBIT* ld, QUBIT* st, QUBIT* evict_1d)
{
    if (yoked_1d_storage_ == nullptr) {
        std::cerr << "YOKED_ARCHITECTURE::do_placement_access: no 1D storage present" << _die{};
    }

    // Multiple MPLACEs can appear in the same DAG front layer (the RRI placer
    // emits up to k per commit zone).  All pass the readiness check while the
    // 1D adapter is free, but execution is serial: the first consumes it.
    // Return progress=0 for subsequent ones so they retry next cycle.
    if (!yoked_1d_storage_->has_free_adapter())
        return execute_result_type{};

    // Step 1: Cold storage swap (parallel path A):
    //   ld leaves cold; evict_1d enters cold from 1D.
    auto cold_result = memory_hierarchy_->do_memory_access(ld, evict_1d, current_cycle(), freq_khz);
    if (!cold_result.success)
        return execute_result_type{};

    // Step 2: 1D storage swap (parallel path B, parallel with step 1):
    //   evict_1d leaves 1D; st enters 1D from compute.
    auto one_d_result = yoked_1d_storage_->do_placement_eviction(evict_1d, st);
    if (!one_d_result.success) {
        std::cerr << "YOKED_ARCHITECTURE::do_placement_access: 1D swap failed (internal error)\n" << _die{};
    }

    // Step 3: Local memory swap:
    //   st leaves compute → 1D; ld enters compute from cold.
    auto local_result = local_memory_->do_memory_access(st, ld);
    if (!local_result.success) {
        std::cerr << "YOKED_ARCHITECTURE::do_placement_access: local memory swap failed\n" << _die{};
    }

    // Parallel latency: cold swap and 1D swap run concurrently, then +2 for local.
    cycle_type one_d_latency = convert_cycles(
        one_d_result.latency, one_d_result.storage_freq_khz, freq_khz);
    cycle_type total_latency = std::max(cold_result.latency, one_d_latency) + 2;

    // Update cycle_available for all three qubits.
    ld->cycle_available       = std::max(ld->cycle_available,       current_cycle() + total_latency);
    st->cycle_available       = std::max(st->cycle_available,       current_cycle() + total_latency);
    evict_1d->cycle_available = std::max(evict_1d->cycle_available, current_cycle() + total_latency);

    // Stats:
    record_memory_op_bucket(ld);
    // ld arrives from cold — counted separately from plain cold MSWAP.
    s_mplace_loads++;
    qubit_2d_load_cycle_[ld] = current_cycle();
    // can_operate_non_clifford_ for ld remains false (was false in cold);
    // evict_1d going to cold is handled by drain_newly_stored_qubits() in operate().

    return execute_result_type{.progress=1, .latency=total_latency};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
YOKED_ARCHITECTURE::record_memory_op_bucket(QUBIT* ld)
{
    const size_t bucket = current_cycle() / 200;
    if (bucket >= s_unique_loaded_qubits_per_250_cycles_.size())
        s_unique_loaded_qubits_per_250_cycles_.resize(bucket + 1);
    s_unique_loaded_qubits_per_250_cycles_[bucket].insert(ld);
}

void
YOKED_ARCHITECTURE::retire_instruction(CLIENT* c, inst_ptr inst, cycle_type inst_latency)
{
    auto active_it = non_clifford_only_block_start_cycle_.find(inst);
    if (active_it != non_clifford_only_block_start_cycle_.end()) {
        non_clifford_only_instruction_delay_[inst] += current_cycle() - active_it->second;
        non_clifford_only_block_start_cycle_.erase(active_it);
    }

    s_total_non_clifford_only_instruction_delay += non_clifford_only_instruction_delay_[inst];
    s_instructions_considered_for_non_clifford_delay++;

    if (!is_memory_access(inst->type)) {
        auto front_layer_it = front_layer_entry_cycle_.find(inst);
        if (front_layer_it != front_layer_entry_cycle_.end()) {
            s_total_front_layer_to_retire_delay +=
                current_cycle() + inst_latency - front_layer_it->second;
            front_layer_entry_cycle_.erase(front_layer_it);
        }
    }

    non_clifford_only_instruction_delay_.erase(inst);

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

void
YOKED_ARCHITECTURE::print_yoked_storage_stats()
{
    std::cout << "\nYoked Storage Statistics:\n";
    std::cout << "=========================\n";
    
    uint64_t total_loads = s_1d_loads + s_2d_loads + s_mplace_loads;
    if (total_loads > 0) {
        double miss_rate = 100.0 * (s_2d_loads + s_mplace_loads) / total_loads;
        print_stat_line(std::cout, "Total memory loads", total_loads);
        print_stat_line(std::cout, "Loads from 1D storage (MSWAP)", s_1d_loads);
        print_stat_line(std::cout, "Loads from 2D storage (cold MSWAP)", s_2d_loads);
        print_stat_line(std::cout, "Loads from 2D storage via MPLACE", s_mplace_loads);
        print_stat_line(std::cout, "Miss rate (%) [cold+mplace / total]", miss_rate);
    } else {
        print_stat_line(std::cout, "Total memory loads", 0);
    }
    
    if (s_1d_loads_delayed > 0) {
        double avg_delay = static_cast<double>(s_total_1d_to_ready_delay) / s_1d_loads_delayed;
        print_stat_line(std::cout, "1D loads: already verified at load time",  s_1d_loads_already_ready);
        print_stat_line(std::cout, "1D loads: needed to wait for verification", s_1d_loads_delayed);
        print_stat_line(std::cout, "Avg delay: 1D load to non-Clifford ready (cycles)", avg_delay);
    } else {
        print_stat_line(std::cout, "1D loads: already verified at load time",  s_1d_loads_already_ready);
        print_stat_line(std::cout, "1D loads: needed to wait for verification", 0UL);
        print_stat_line(std::cout, "Avg delay: 1D load to non-Clifford ready (cycles)", 0.0);
    }

    if (s_2d_loads_delayed > 0) {
        double avg_delay = static_cast<double>(s_total_2d_to_ready_delay) / s_2d_loads_delayed;
        print_stat_line(std::cout, "2D/cold loads: reached non-Clifford ready", s_2d_loads_delayed);
        print_stat_line(std::cout, "Avg delay: 2D/cold load to non-Clifford ready (cycles)", avg_delay);
    } else {
        print_stat_line(std::cout, "2D/cold loads: reached non-Clifford ready", 0UL);
        print_stat_line(std::cout, "Avg delay: 2D/cold load to non-Clifford ready (cycles)", 0.0);
    }

    const uint64_t unrolled_instructions_done = client_.s_unrolled_inst_done;

    const double avg_non_clifford_only_instruction_delay =
        unrolled_instructions_done > 0
            ? static_cast<double>(s_total_non_clifford_only_instruction_delay)
                / unrolled_instructions_done
            : 0.0;
    print_stat_line(std::cout,
                    "Avg instruction delay due only to non-Clifford readiness (cycles)",
                    avg_non_clifford_only_instruction_delay);

    const double avg_front_layer_to_retire_delay =
        unrolled_instructions_done > 0
            ? static_cast<double>(s_total_front_layer_to_retire_delay)
                / unrolled_instructions_done
            : 0.0;
    print_stat_line(std::cout,
                    "Avg instruction delay from front layer to retire (cycles)",
                    avg_front_layer_to_retire_delay);

    const double readiness_share_of_front_layer_delay =
        s_total_front_layer_to_retire_delay > 0
            ? 100.0 * static_cast<double>(s_total_non_clifford_only_instruction_delay)
                / s_total_front_layer_to_retire_delay
            : 0.0;
    print_stat_line(std::cout,
                    "Instruction front-layer delay due only to non-Clifford readiness (%)",
                    readiness_share_of_front_layer_delay);

    if (!s_unique_loaded_qubits_per_250_cycles_.empty()) {
        std::vector<uint64_t> bucket_counts;
        bucket_counts.reserve(s_unique_loaded_qubits_per_250_cycles_.size());
        for (const auto& bucket : s_unique_loaded_qubits_per_250_cycles_)
            bucket_counts.push_back(bucket.size());

        const double mean_unique_loaded_qubits_per_250_cycles =
            static_cast<double>(std::accumulate(bucket_counts.begin(),
                                                bucket_counts.end(),
                                                uint64_t{0}))
            / bucket_counts.size();

        auto sorted_buckets = bucket_counts;
        std::sort(sorted_buckets.begin(), sorted_buckets.end());
        double median_unique_loaded_qubits_per_250_cycles;
        const size_t mid = sorted_buckets.size() / 2;
        if (sorted_buckets.size() % 2 == 0) {
            median_unique_loaded_qubits_per_250_cycles =
                (static_cast<double>(sorted_buckets[mid - 1]) + sorted_buckets[mid]) / 2.0;
        } else {
            median_unique_loaded_qubits_per_250_cycles = sorted_buckets[mid];
        }

        print_stat_line(std::cout,
                        "Mean unique qubits loaded per 200 cycles",
                        mean_unique_loaded_qubits_per_250_cycles);
        print_stat_line(std::cout,
                        "Median unique qubits loaded per 200 cycles",
                        median_unique_loaded_qubits_per_250_cycles);

        std::vector<uint64_t> nonzero_buckets;
        std::copy_if(bucket_counts.begin(),
                     bucket_counts.end(),
                     std::back_inserter(nonzero_buckets),
                     [] (uint64_t count) { return count > 0; });

        if (!nonzero_buckets.empty()) {
            const double mean_nonzero_unique_loaded_qubits_per_250_cycles =
                static_cast<double>(std::accumulate(nonzero_buckets.begin(),
                                                    nonzero_buckets.end(),
                                                    uint64_t{0}))
                / nonzero_buckets.size();

            std::sort(nonzero_buckets.begin(), nonzero_buckets.end());
            double median_nonzero_unique_loaded_qubits_per_250_cycles;
            const size_t mid_nonzero = nonzero_buckets.size() / 2;
            if (nonzero_buckets.size() % 2 == 0) {
                median_nonzero_unique_loaded_qubits_per_250_cycles =
                    (static_cast<double>(nonzero_buckets[mid_nonzero - 1]) + nonzero_buckets[mid_nonzero]) / 2.0;
            } else {
                median_nonzero_unique_loaded_qubits_per_250_cycles = nonzero_buckets[mid_nonzero];
            }

            print_stat_line(std::cout,
                            "Mean unique qubits loaded per 200 cycles (non-zero windows)",
                            mean_nonzero_unique_loaded_qubits_per_250_cycles);
            print_stat_line(std::cout,
                            "Median unique qubits loaded per 200 cycles (non-zero windows)",
                            median_nonzero_unique_loaded_qubits_per_250_cycles);
        } else {
            print_stat_line(std::cout,
                            "Mean unique qubits loaded per 200 cycles (non-zero windows)",
                            0.0);
            print_stat_line(std::cout,
                            "Median unique qubits loaded per 200 cycles (non-zero windows)",
                            0.0);
        }
    } else {
        print_stat_line(std::cout, "Mean unique qubits loaded per 200 cycles", 0.0);
        print_stat_line(std::cout, "Median unique qubits loaded per 200 cycles", 0.0);
        print_stat_line(std::cout, "Mean unique qubits loaded per 200 cycles (non-zero windows)", 0.0);
        print_stat_line(std::cout, "Median unique qubits loaded per 200 cycles (non-zero windows)", 0.0);
    }
    
    // Print error stats for all yoked storages
    std::cout << "\n";
    for (auto* storage : memory_hierarchy_->storages()) {
        if (auto* yoked_cold_storage = dynamic_cast<YOKED_COLD_STORAGE*>(storage))
            yoked_cold_storage->error_stats();
        else if (auto* yoked_1d_storage = dynamic_cast<YOKED_1D_STORAGE*>(storage))
            yoked_1d_storage->error_stats();
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace yoked_codes
}  // namespace sim
