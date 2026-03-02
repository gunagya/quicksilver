/*
 *  author: Suhas Vittal
 *  date:   4 January 2026
 * */

#ifndef DAG_h
#define DAG_h

#include "instruction.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

class DAG
{
public:
    using inst_ptr = INSTRUCTION*;

    struct node_type
    {
        inst_ptr                inst;
        std::vector<node_type*> dependent{};
        size_t                  pred_count{0};

        /*
         * These variables are for `for_each_instruction_in_layer_order` (see below)
         * */
        mutable uint8_t         tmp_pred_count_{0};
        mutable size_t          tmp_depth_{0};
        mutable size_t          last_generation_{0};
    };

    const size_t qubit_count;
private:
    /*
     * We implement `front_layer_` as a hashmap to speedup
     * deletion. So, we can lookup the corresponding node
     * for an instruction and just delete that.
     *
     * Using a std::vector or std::unordered_set would
     * require an O(n) lookup.
     * */
    std::unordered_map<inst_ptr, node_type*> front_layer_;

    /*
     * This is essentially a "bucket" of instructions.
     * Each entry corresponds to a different qubit and
     * corresponds to a node for the youngest instruction
     * to that qubit.
     * */
    std::vector<node_type*> back_instructions_;

    size_t inst_count_{0};

    /*
     * This is used during `for_each_instruction_in_layer_order`
     * */
    mutable size_t iteration_generation_{0};
public:
    DAG(size_t qubit_count);
    ~DAG();

    void add_instruction(inst_ptr);
    void remove_instruction_from_front_layer(inst_ptr);

    /*
     * This returns a list of all instructions in the front layer.
     * These are the oldest instructions in the program.
     * */
    std::vector<inst_ptr> get_front_layer() const;

    /*
     * This is a modified version of the above function that
     * returns instructions in the front layer that meet the given
     * predicate.
     * */
    template <class PRED>
    std::vector<inst_ptr> get_front_layer_if(const PRED&) const;

    /*
     * Executes the given callback for `min_layer` to `max_layer`
     * */
    template <class CALLBACK>
    void for_each_instruction_in_layer_order(const CALLBACK&, size_t min_layer, size_t max_layer) const;

    /*
     * Finds the earliest instruction dependent on the given instruction in the front layer
     * that satisfies the given predicate (not including the input instruction).
     *
     * Search is limited from `min_layer` to `max_layer`. Returns the instruction and layer it
     * was found in. If no instruction was found, then `inst_ptr == nullptr`
     * */
    template <class PRED>
    std::pair<inst_ptr, size_t> find_earliest_dependent_instruction_such_that(const PRED&, 
                                                                                inst_ptr, 
                                                                                size_t min_layer,
                                                                                size_t max_layer) const;

    size_t inst_count() const;

    /*
     * Returns pointers to memory instructions (e.g. MSWAP)
     * from the current front layer up to `num_layers` layers deep.
     * Returns pairs of (order, instruction) where order is the layer number.
     * */
    std::vector<std::pair<size_t, inst_ptr>> get_memory_instructions_upto_layers(size_t num_layers) const;
    /*
     * Returns pointers to memory instructions (e.g. MSWAP)
     * up to the given circuit depth.
     * Returns pairs of (order, instruction) where order is the depth.
     * */
    std::vector<std::pair<size_t, inst_ptr>> get_memory_instructions_upto_depth(size_t depth) const;
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

#include "dag.tpp"

#endif   // DAG_h
