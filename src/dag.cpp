/*
 *  author: Suhas Vittal
 *  date:   4 January 2026
 * */

#include "dag.h"
#include "instruction.h"

#include <unordered_set>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

DAG::DAG(size_t _qubit_count)
    :qubit_count(_qubit_count),
    front_layer_(),
    back_instructions_(_qubit_count, nullptr)
{
    front_layer_.reserve(qubit_count);
}

DAG::~DAG()
{
    // delete all nodes and instructions remaining in the DAG:
    // can do this efficiently via DFS:
    std::vector<node_type*> dfss;
    for (const auto& [__unused_inst, node] : front_layer_)
        dfss.push_back(node);

    while (!dfss.empty())
    {
        auto* x = dfss.back();
        dfss.pop_back();

        x->pred_count--;
        if (x->pred_count == 0)
        {
            // traverse now -- since we will delete `x`
            for (auto* y : x->dependent)
                dfss.push_back(y);
            delete x->inst;
            delete x;
        }
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
DAG::add_instruction(inst_ptr inst)
{
    node_type* x = new node_type{inst};

    std::unordered_set<node_type*> visited;
    for (auto it = inst->q_begin(); it != inst->q_end(); it++)
    {
        auto q = *it;
        if (back_instructions_[q] != nullptr)
        {
            // avoid double counting the dependency
            if (!visited.count(back_instructions_[q]))
            {
                back_instructions_[q]->dependent.push_back(x);
                x->pred_count++;
            }
            visited.insert(back_instructions_[q]);
        }
        back_instructions_[q] = x;
    }

    if (x->pred_count == 0)
        front_layer_[inst] = x;
    inst_count_++;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
DAG::remove_instruction_from_front_layer(inst_ptr inst)
{
    auto node_it = front_layer_.find(inst);
    if (node_it == front_layer_.end())
    {
        std::cerr << "DAG::remove_instruction_from_layer: inst " << *inst 
                    << " was not found in the front layer"
                    << "\nfront layer contents:";
        for (const auto& [inst, node] : front_layer_)
        {
            std::cerr << "\n\tinst = " << *inst 
                        << ", node pred count = " << node->pred_count
                        << ", node dependents =";
            for (auto* x : node->dependent)
                std::cerr << "\n\t\t" << *x->inst;
        }
        std::cerr << _die{};
    }
    node_type* head_node = node_it->second;
    front_layer_.erase(node_it);

    // update `inst` dependents:
    for (auto* dep : head_node->dependent)
    {
        dep->pred_count--;
        if (dep->pred_count == 0)
            front_layer_[dep->inst] = dep;
    }

    // finally, if `inst` is also in `back_instructions_`, then we need to clear the entry
    for (auto it = inst->q_begin(); it != inst->q_end(); it++)
        if (back_instructions_[*it] == head_node)
            back_instructions_[*it] = nullptr;

    delete head_node;
    inst_count_--;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

std::vector<DAG::inst_ptr>
DAG::get_front_layer() const
{
    return get_front_layer_if([] (const auto*) { return true; });
}

size_t
DAG::inst_count() const
{
    return inst_count_;
}

std::vector<std::pair<size_t, DAG::inst_ptr>>
DAG::get_memory_instructions_upto_layers(size_t num_layers) const
{
    std::vector<std::pair<size_t, inst_ptr>> mem_insts;
    size_t current_layer = 0;
    
    // Modified version of for_each_instruction_in_layer_order that tracks layer numbers
    iteration_generation_++;
    const size_t gen = iteration_generation_;

    std::vector<node_type*> current_layer_nodes;
    current_layer_nodes.reserve(front_layer_.size());
    for (const auto& [_, node] : front_layer_)
        current_layer_nodes.push_back(node);

    while (!current_layer_nodes.empty() && current_layer < num_layers)
    {
        std::vector<node_type*> next_layer;
        next_layer.reserve(current_layer_nodes.size());

        for (auto* x : current_layer_nodes)
        {
            if (is_memory_access(x->inst->type))
                mem_insts.push_back({current_layer, x->inst});

            for (node_type* y : x->dependent)
            {
                if (y->last_generation_ != gen)
                {
                    y->last_generation_ = gen;
                    y->tmp_pred_count_ = 0;
                }
                if ((++y->tmp_pred_count_) == y->pred_count)
                    next_layer.push_back(y);
            }
        }
        current_layer_nodes = std::move(next_layer);
        current_layer++;
    }
    return mem_insts;
}

std::vector<std::pair<size_t, DAG::inst_ptr>>
DAG::get_memory_instructions_upto_depth(size_t depth) const
{
    std::vector<std::pair<size_t, inst_ptr>> mem_insts;
    
    // Modified version of for_each_instruction_upto_circuit_depth that tracks depth
    iteration_generation_++;
    const size_t gen = iteration_generation_;

    std::vector<node_type*> current_layer;
    current_layer.reserve(front_layer_.size());
    for (const auto& [_, node] : front_layer_)
    {
        node->last_generation_ = gen;
        node->tmp_pred_count_ = 0;
        node->tmp_depth_ = 0;
        current_layer.push_back(node);
    }

    while (!current_layer.empty())
    {
        std::vector<node_type*> next_layer;
        next_layer.reserve(current_layer.size());

        for (auto* x : current_layer)
        {
            if (x->tmp_depth_ <= depth)
            {
                if (is_memory_access(x->inst->type))
                    mem_insts.push_back({x->tmp_depth_, x->inst});

                size_t next_depth = x->tmp_depth_ + instruction_depth_weight(x->inst->type);
                for (node_type* y : x->dependent)
                {
                    if (y->last_generation_ != gen)
                    {
                        y->last_generation_ = gen;
                        y->tmp_pred_count_ = 0;
                        y->tmp_depth_ = 0;
                    }
                    y->tmp_depth_ = std::max(y->tmp_depth_, next_depth);
                    if ((++y->tmp_pred_count_) == y->pred_count)
                        next_layer.push_back(y);
                }
            }
        }
        current_layer = std::move(next_layer);
    }
    return mem_insts;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
