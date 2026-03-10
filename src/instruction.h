/*
    author: Suhas Vittal
    date:   19 August 2025
*/

#ifndef TYPES_h
#define TYPES_h

#include "fixed_point/angle.h"
#include "generic_io.h"
#include "globals.h"

#include <array>
#include <deque>
#include <iosfwd>
#include <vector>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

constexpr std::string_view BASIS_GATES[] = 
{
    // compute instruction:
    "h", "x", "y", "z", 
    "s", "sx", "sdg", "sxdg",
    "t", "tx", "tdg", "txdg",
    "cx", "cz", "swap",
    "rx", "rz",
    "ccx", "ccz",
    "mz", "mx",

    // memory instruction:
    "mswap",
    "mprefetch",
    "mplace",

    "nil"
};

class INSTRUCTION
{
public:
    constexpr static size_t FPA_PRECISION{64};
    constexpr static size_t MAX_QUBITS{3};
    constexpr static int64_t INVALID_NUMBER{-1};

    using qubit_array = std::array<qubit_type, MAX_QUBITS>;
    using fpa_type = FPA_TYPE<FPA_PRECISION>;

    enum class TYPE
    {
        /*
         * Quantum gates (Clifford + T + other extensions)
         * */
        H, X, Y, Z, 
        S, SX, SDG, SXDG, 
        T, TX, TDG, TXDG, 
        CX, CZ, SWAP,
        RX, RZ, 
        CCX, CCZ, 
        MZ, MX,

        /*
         * Memory instructions:
         *  MSWAP    q0, q1        loads q0 into the compute subsystem and stores q1 into the memory subsystem.
         *  MPREFETCH q0, q1       background prefetch: brings q0 from cold into intermediate storage,
         *                         evicting q1 from intermediate to cold.  Never executed by the compute region.
         *  MPLACE   q0, q1, q2   loads q0 from cold into compute, stores q1 from compute into 1D
         *                         intermediate storage, and evicts q2 from 1D to cold storage.
         *                         Only emitted when q0 comes from cold and 1D placement is beneficial.
         * */
        MSWAP,
        MPREFETCH,
        MPLACE,
        NIL
    };

    using urotseq_type = std::vector<INSTRUCTION::TYPE>;

    /*
     * `type` corresponds to some basic instruction.
     * This can be a Clifford=T gate (i.e., T, H, etc.),
     * or some other instruction (like MSWAP)
     * */
    const TYPE type;

    /*
     * `qubits` is stored in a fixed-width array (see `MAX_QUBITS`)
     * By default, an entry is `0`.
     *
     * The number of valid qubits is determined by the function
     * `get_inst_qubit_count(INSTRUCTION::TYPE)`.
     * */
    const qubit_array qubits;

    /*
     * `angle` and `urotseq` are only useful for RZ and RX gates.
     *
     * `angle` is a fixed-point representation of the angle (so we
     * have high precision for angles near a power of two).
     *
     * `urotseq` is a sequence of Clifford+T gates that approximate
     * RZ or RX of `angle`. `urotseq` is not const since it may 
     * need to be changed during compilation (to support asynchrnous 
     * synthesis).
     * */
    const fpa_type angle;
    urotseq_type   urotseq;

    /*
     * `corr_urotseq_array` contains corrective sequences for the given rotation,
     * assuming that the entire rotation was teleported at once.
     *  -- idx 0 is a correction for the angle defined by `urotseq`
     *  -- idx 1 is a correction for idx 0
     *  -- (etc.)
     *
     * By default, this is not initialized since it is a niche use case.
     * */
    std::deque<urotseq_type> corr_urotseq_array{};

    /*
     * Same value as `get_inst_qubit_count(type)`
     * */
    const size_t qubit_count;

    /*
     * These are simulator-related parameters. The simulator manages
     * them and can change them at will:
     * */
    int64_t  number{INVALID_NUMBER};
    uint64_t cycle_done{std::numeric_limits<uint64_t>::max()};

    /*
     * Other exposed variables that may be useful:
     *  `deletable` is useful for `std::remove + std::erase` patterns
     *  `first_ready_cycle` is useful for computing instruction latency
     *  `original_unrolled_inst_count` is useful for rotation instructions as `urotseq` may be modified
     * */
    bool     deletable{false};
    uint64_t first_ready_cycle{std::numeric_limits<uint64_t>::max()};
    uint64_t original_unrolled_inst_count{};

    /*
     * `rpc_*` variables correspond to variables used for
     * rotation precomputation. 
     *
     * `rpc_has_been_visited` is used to track whether this
     * is the first time a given instruction has been seen.
     * */
    bool rpc_has_been_visited{false};
private:
    /*
     * Gates like RZ and RX have micro-ops (or uops) that must be execute
     * to implement them. Here is some logic to track them.
     *
     * `current_uop` is the currently pending uop, and `uops_retired` is
     * the number of completed/retired uops. This instruction should be
     * retired once `uops_retired == uop_count()`
     * */
    INSTRUCTION* current_uop_{nullptr};
    size_t       uops_retired_{0};
public:
    /*
     * Basic constructor for initializing from a given list of qubits.
     * */
    INSTRUCTION(TYPE, std::initializer_list<qubit_type>);

    /*
     * Constructor for initializing from a container:
     * */
    template <class ITER_TYPE> 
    INSTRUCTION(TYPE, ITER_TYPE q_begin, ITER_TYPE q_end);

    /*
     * Rotation gate constructors: we require that `urotseq` is specified using
     * iterators since the sequence can be rather long.
     * */
    template <class ITER_TYPE>
    INSTRUCTION(TYPE, std::initializer_list<qubit_type>, fpa_type, ITER_TYPE urotseq_begin, ITER_TYPE urotseq_end);

    template <class Q_IT_TYPE, class U_IT_TYPE>
    INSTRUCTION(TYPE, Q_IT_TYPE q_begin, Q_IT_TYPE q_end, fpa_type, U_IT_TYPE urotseq_begin, U_IT_TYPE urotseq_end);

    INSTRUCTION(const INSTRUCTION&) =default;

    ~INSTRUCTION();

    /*
     * `retire_current_uop` deletes `current_uop` and gets the next `uop`.
     * `uops_retired` is also incremented.
     *
     * This function returns true if all uops are done.
     * */
    bool retire_current_uop();
    
    /*
     * This instruction resets the number of uops retired
     * */
    void reset_uops();

    size_t       uops_retired() const;
    INSTRUCTION* current_uop() const;

    /*
     * `uop_count` returns the number of `uops` that must be executed.
     * For rotation gates, this is just `urotseq.size()`. For CCX and CCZ
     * gates, this is the number of gates in the CX+T decomposition.
     * */
    size_t uop_count() const;

    /*
     * `unrolled_inst_count` returns `std::max(1, uop_count())`
     * This is useful for counting instructions done during simulation
     * */
    size_t unrolled_inst_count() const;

    /*
     * Iterator support for valid qubit range
     * */
    qubit_type* q_begin();
    qubit_type* q_end();
    const qubit_type* q_begin() const;
    const qubit_type* q_end() const;

    std::string to_string() const;
private:
    void get_next_uop();
};

std::ostream& operator<<(std::ostream&, const INSTRUCTION&);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * IO support for `INSTRUCTION`:
 * */
INSTRUCTION* read_instruction_from_stream(generic_strm_type&);
void         write_instruction_to_stream(generic_strm_type&, const INSTRUCTION*);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * These functions are simple boolean functions that
 * categorize a given `INSTRUCTION::TYPE`
 * */
constexpr bool is_software_instruction(INSTRUCTION::TYPE);
constexpr bool is_memory_access(INSTRUCTION::TYPE);
constexpr bool is_prefetch_instruction(INSTRUCTION::TYPE);
constexpr bool is_t_like_instruction(INSTRUCTION::TYPE);
constexpr bool is_rotation_instruction(INSTRUCTION::TYPE);
constexpr bool is_cx_like_instruction(INSTRUCTION::TYPE);
constexpr bool is_toffoli_like_instruction(INSTRUCTION::TYPE);

/*
 * Returns the DAG depth weight for an instruction type.
 * Matches the scoring used by DAG::get_memory_instructions_upto_depth:
 *   rotation  → 20,  toffoli-like → 10,  memory → 5,  cx-like → 2,
 *   software  →  0,  everything else → 1
 * */
constexpr size_t instruction_depth_weight(INSTRUCTION::TYPE);

/*
 * This function is a constexpr function that returns
 * the number of arguments for a given instruction type.
 *
 * Also useful when reading out qubits from `INSTRUCTION`
 * */
constexpr size_t get_inst_qubit_count(INSTRUCTION::TYPE t);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Other useful functions:
 * */

/*
 * `convert_qubit_container_into_qubit_array` copies the data in the range to an array and returns it.
 * An error is thrown if `std::distance(begin, end)` is not equal to the number of arguments specificed
 * by `INSTRUCTION::TYPE`.
 * */
template <class ITER>
INSTRUCTION::qubit_array convert_qubit_container_into_qubit_array(INSTRUCTION::TYPE, ITER begin, ITER end);

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

#include "instruction.tpp"

#endif
