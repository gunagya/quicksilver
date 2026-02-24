/*
 *  author: Suhas Vittal
 *  date:   5 January 2026
 * */

#include "instruction.h"
#include <unordered_set>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

template <class PRED> std::vector<DAG::inst_ptr>
DAG::get_front_layer_if(const PRED& pred) const
{
    std::vector<inst_ptr> front_layer_insts;
    front_layer_insts.reserve(front_layer_.size());
    for (const auto& [inst, __unused_node] : front_layer_)
        if (pred(inst))
            front_layer_insts.push_back(inst);
    return front_layer_insts;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

template <class CALLBACK> void
DAG::for_each_instruction_in_layer_order(const CALLBACK& callback, size_t min_layer, size_t max_layer) const
{
    // update iteration generation so we know when to reset predecessor
    iteration_generation_++;
    const size_t gen = iteration_generation_;

    std::vector<node_type*> current_layer;
    current_layer.reserve(front_layer_.size());
    for (const auto& [_, node] : front_layer_)
        current_layer.push_back(node);

    size_t layer_count{0};
    while (!current_layer.empty() && layer_count < max_layer)
    {
        std::vector<node_type*> next_layer;
        next_layer.reserve(current_layer.size());

        for (auto* x : current_layer)
        {
            if (layer_count >= min_layer)
                callback(x->inst);

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
        current_layer = std::move(next_layer);
        layer_count++;
    }
}

inline size_t
get_inst_depth_score(INSTRUCTION::TYPE t)
{
    if (is_rotation_instruction(t))
        return 20;
    else if (is_toffoli_like_instruction(t))
        return 10;
    else if (is_cx_like_instruction(t))
        return 2;
    else if (is_software_instruction(t))
        return 0;
    else if (is_memory_access(t))
        return 5;
    else
        return 1;
}

template <class CALLBACK> void
DAG::for_each_instruction_upto_circuit_depth(const CALLBACK& callback, size_t max_depth) const
{
    iteration_generation_++;
    const size_t gen = iteration_generation_;

    // Seed the traversal with the front layer (depth = 0).
    // We use a vector as a queue sorted by depth via BFS-like processing.
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
            if (x->tmp_depth_ > max_depth)
                continue;

            callback(x->inst);

            size_t parent_finish = x->tmp_depth_ + get_inst_depth_score(x->inst->type);
            for (node_type* y : x->dependent)
            {
                if (y->last_generation_ != gen)
                {
                    y->last_generation_ = gen;
                    y->tmp_pred_count_ = 0;
                    y->tmp_depth_ = 0;
                }
                // depth of y = max over all predecessors of (pred_depth + pred_score)
                if (parent_finish > y->tmp_depth_)
                    y->tmp_depth_ = parent_finish;

                if ((++y->tmp_pred_count_) == y->pred_count)
                    next_layer.push_back(y);
            }
        }
        current_layer = std::move(next_layer);
    }
}

template <class PRED> std::pair<typename DAG::inst_ptr, size_t>
DAG::find_earliest_dependent_instruction_such_that(const PRED& pred, 
                                                    inst_ptr source, 
                                                    size_t min_layer,
                                                    size_t max_layer) const
{
    node_type* source_node = front_layer_.at(source);
    std::vector<node_type*> curr_layer(source_node->dependent);

    // use an `std::unordered_set` to avoid duplicates when setting `curr_layer`
    std::unordered_set<node_type*> next_layer_set;
    next_layer_set.reserve(curr_layer.size());

    size_t layer_count{0};
    while (layer_count < max_layer)
    {
        for (auto* x : curr_layer)
        {
            next_layer_set.insert(x->dependent.begin(), x->dependent.end());
            if (layer_count >= min_layer && pred(x->inst))
                return std::make_pair(x->inst, layer_count);
        }
        curr_layer.assign(next_layer_set.begin(), next_layer_set.end());
        next_layer_set.clear();
        layer_count++;
    }
    
    return std::make_pair(nullptr, 0);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
