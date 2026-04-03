/*
 *  author: Gunagya Singh Mamak
 *  date:   12 March 2026
 * */

#include "compiler/eviction_policy.h"
#include "dag.h"
#include "instruction.h"

#include <algorithm>
#include <iostream>
#include <memory>

namespace compile
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
UsageData::record(qubit_type q, size_t layer)
{
    layers[q].push_back(layer);
}

void
UsageData::finalise()
{
    for (auto& [q, vec] : layers)
        std::sort(vec.begin(), vec.end());
}

size_t
UsageData::next_use_after(qubit_type q, size_t layer) const
{
    auto it = layers.find(q);
    if (it == layers.end())
        return INF;
    const auto& vec = it->second;
    auto pos = std::upper_bound(vec.begin(), vec.end(), layer);
    return (pos == vec.end()) ? INF : *pos;
}

size_t
UsageData::last_use_at_or_before(qubit_type q, size_t layer) const
{
    auto it = layers.find(q);
    if (it == layers.end())
        return 0;
    const auto& vec = it->second;
    auto pos = std::upper_bound(vec.begin(), vec.end(), layer);
    if (pos == vec.begin())
        return 0;
    --pos;
    return *pos;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

UsageData
build_usage_data(const std::string& file_path,
                 LayerType          layer_type,
                 size_t             dag_inst_capacity,
                 int64_t            inst_compile_limit)
{
    UsageData ud;

    // Open a second independent stream for the pre-pass (works for plain,
    // .gz and .xz files since generic_strm_open dispatches on extension).
    generic_strm_type strm;
    generic_strm_open(strm, file_path, "rb");

    uint32_t num_qubits;
    generic_strm_read(strm, &num_qubits, sizeof(num_qubits));

    std::unique_ptr<DAG> dag{new DAG{num_qubits}};

    // Per-qubit layer tracker for WEIGHTED depth propagation.
    std::unordered_map<qubit_type, size_t> qubit_layer;
    // Front-layer batch counter for UNWEIGHTED mode.
    size_t front_layer_counter = 0;
    int64_t inst_done = 0;

    while (inst_done < inst_compile_limit)
    {
        bool saw_invalid_record = false;
        const char* invalid_reason = nullptr;
        int64_t bad_q0 = -1, bad_q1 = -1, bad_q2 = -1;
        uint64_t bad_type = 0;

        // Top-up the DAG.
        while (dag->inst_count() < dag_inst_capacity && !generic_strm_eof(strm))
        {
            INSTRUCTION* inst = read_instruction_from_stream(strm);

            if (inst == nullptr)
            {
                saw_invalid_record = true;
                invalid_reason = "null instruction record";
                break;
            }

            // Defensive check for truncated/corrupted tail records.
            // Keep this local to the RRI pre-pass to avoid broad base changes.
            bool valid_qubits = true;
            for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
            {
                if (*it < 0 || *it >= static_cast<qubit_type>(num_qubits))
                {
                    valid_qubits = false;
                    break;
                }
            }
            if (!valid_qubits)
            {
                bad_type = static_cast<uint64_t>(inst->type);
                bad_q0 = inst->qubits[0];
                bad_q1 = inst->qubits[1];
                bad_q2 = inst->qubits[2];
                delete inst;
                saw_invalid_record = true;
                invalid_reason = "qubit index out of range";
                break;
            }

            if (is_software_instruction(inst->type))
                delete inst;
            else
                dag->add_instruction(inst);
        }

        if (saw_invalid_record && dag->inst_count() == 0)
        {
            std::cerr << "[build_usage_data] invalid record while scanning "
                      << file_path
                      << " | reason=" << (invalid_reason ? invalid_reason : "unknown")
                      << " | front_layer_counter=" << front_layer_counter
                      << " | dag_inst_count=" << dag->inst_count();
            if (invalid_reason && std::string(invalid_reason) == "qubit index out of range")
                std::cerr << " | type=" << bad_type
                          << " | qubits={" << bad_q0 << "," << bad_q1 << "," << bad_q2 << "}"
                          << " | num_qubits=" << num_qubits;
            std::cerr << "\n";
            break;
        }

        const auto front_layer = dag->get_front_layer();
        if (front_layer.empty())
            break;

        // Process front layer: compute layer numbers and record/update in single pass.
        // Since each qubit appears at most once per front layer, we can safely update
        // qubit_layer as we go without affecting other instructions in this batch.
        for (INSTRUCTION* inst : front_layer)
        {
            size_t layer;
            if (layer_type == LayerType::UNWEIGHTED)
            {
                layer = front_layer_counter;
            }
            else  // WEIGHTED
            {
                size_t max_pred = 0;
                for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
                {
                    auto jt = qubit_layer.find(*it);
                    if (jt != qubit_layer.end() && jt->second > max_pred)
                        max_pred = jt->second;
                }
                layer = max_pred + instruction_depth_weight(*inst);
            }

            // Record ld (qubits[0]) and st (qubits[1]) for every MSWAP.
            if (inst->type == INSTRUCTION::TYPE::MSWAP)
            {
                ud.record(inst->qubits[0], layer);
                ud.record(inst->qubits[1], layer);
            }

            // Update qubit_layer for WEIGHTED depth propagation.
            if (layer_type == LayerType::WEIGHTED)
            {
                for (auto it = inst->q_begin(); it != inst->q_end(); ++it)
                {
                    size_t& ql = qubit_layer[*it];
                    if (layer > ql)
                        ql = layer;
                }
            }

            dag->remove_instruction_from_front_layer(inst);
            inst_done += inst->uop_count();
            delete inst;
        }

        ++front_layer_counter;

        if (saw_invalid_record)
        {
            std::cerr << "[build_usage_data] invalid record while scanning "
                      << file_path
                      << " | reason=" << (invalid_reason ? invalid_reason : "unknown")
                      << " | front_layer_counter=" << front_layer_counter
                      << " | dag_inst_count=" << dag->inst_count();
            if (invalid_reason && std::string(invalid_reason) == "qubit index out of range")
                std::cerr << " | type=" << bad_type
                          << " | qubits={" << bad_q0 << "," << bad_q1 << "," << bad_q2 << "}"
                          << " | num_qubits=" << num_qubits;
            std::cerr << "\n";
            break;
        }
    }

    generic_strm_close(strm);
    ud.finalise();
    return ud;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

EvictionResult
EvictionPolicyInstance::select_eviction_candidate(
    const std::vector<qubit_type>&        candidates,
    size_t                                current_layer,
    const std::unordered_set<qubit_type>& exclusions) const
{
    EvictionResult result;

    if (policy == EvictionPolicy::RRI)
    {
        // Evict the candidate with the LARGEST next_use_after (furthest future use).
        for (qubit_type q : candidates)
        {
            if (exclusions.count(q))
                continue;
            const size_t nxt = usage_data.next_use_after(q, current_layer);
            if (!result.found || nxt > result.metric)
            {
                result.qubit  = q;
                result.metric = nxt;
                result.found  = true;
            }
        }
    }
    else  // EvictionPolicy::LRU
    {
        // Evict the candidate with the SMALLEST last_use_at_or_before (oldest past use).
        for (qubit_type q : candidates)
        {
            if (exclusions.count(q))
                continue;
            const size_t lst = usage_data.last_use_at_or_before(q, current_layer);
            if (!result.found || lst < result.metric)
            {
                result.qubit  = q;
                result.metric = lst;
                result.found  = true;
            }
        }
    }

    return result;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace compile
