/*
 * author:  Suhas Vittal
 * date:    12 January 2026
 * */

#include "sim/storage.h"

#include <algorithm>
#include <cassert>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace
{
    
/*
 * Returns a generated name for the storage given its code parameters.
 * */
std::string _storage_name(size_t n, size_t k, size_t d);

/*
 * Fills up the storage by allocating qubits for each client in a round robin.
 * Only considers clients from `0 <= i < idx_upper_bound`
 * */
template <class ITER>
void _fill_up_storage_round_robin(ITER st_begin,
                                    ITER st_end, 
                                    std::vector<size_t>& qubits_allocated,
                                    const std::vector<std::vector<QUBIT*>>& qubits, 
                                    size_t idx_upper_bound);

} // anon

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

STORAGE::STORAGE(double freq_khz, size_t n, size_t k, size_t d, 
                size_t num_adapters, cycle_type _load_latency, cycle_type _store_latency)
    :OPERABLE(_storage_name(n,k,d), freq_khz),
    physical_qubit_count(n),
    logical_qubit_count(k),
    code_distance(d),
    load_latency(_load_latency),
    store_latency(_store_latency),
    contents_{},
    cycle_available_(num_adapters,0)
{
    contents_.reserve(k);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

bool
STORAGE::contains(QUBIT* q) const
{
    return contents_.count(q) > 0;
}

void
STORAGE::insert(QUBIT* q)
{
    assert(contents_.size() < logical_qubit_count);
    contents_.insert(q);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

STORAGE::access_result_type
STORAGE::do_memory_access(QUBIT* ld, QUBIT* st)
{
    assert(contents_.count(ld) > 0);
    assert(contents_.count(st) == 0);

    // find ready adapter to serve memory access
    auto adapter_it = std::find_if(cycle_available_.begin(), cycle_available_.end(),
                                [cc=current_cycle()] (cycle_type c) { return c <= cc; });
    if (adapter_it == cycle_available_.end())
        return access_result_type{};

    // complete memory access since adapter is available
    *adapter_it = current_cycle() + load_latency + store_latency;
    contents_.erase(ld);
    contents_.insert(st);
    return access_result_type{.success=true, .latency=load_latency+store_latency, .storage_freq_khz=freq_khz};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

bool
STORAGE::has_free_adapter() const
{
    return std::any_of(cycle_available_.begin(), cycle_available_.end(),
                    [cc=current_cycle()] (cycle_type c) { return c <= cc; });
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
STORAGE::print_adapter_debug_info(std::ostream& out) const
{
    out << name << " adapters (free cycle delta):";
    for (cycle_type c : cycle_available_)
    {
        int64_t delta = static_cast<int64_t>(c) - static_cast<int64_t>(current_cycle());
        out << " " << delta;
    }
    out << "\n";
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

const STORAGE::backing_buffer_type&
STORAGE::contents() const
{
    return contents_;
}

long
STORAGE::operate()
{
    return 1;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
storage_striped_initialization(const std::vector<STORAGE*>& storage_array,
                                const std::vector<std::vector<QUBIT*>>& qubits,
                                size_t num_active_clients)
{
    std::vector<size_t> qubits_allocated(qubits.size(), 0);

    auto st_begin = storage_array.begin(),
         st_end = storage_array.end();

    // first handle compute subsystem's local memory:
    _fill_up_storage_round_robin(st_begin, st_begin+1, qubits_allocated, qubits, num_active_clients);
    _fill_up_storage_round_robin(st_begin+1, st_begin+2, qubits_allocated, qubits, num_active_clients);
    if (storage_array.size() > 2)
        _fill_up_storage_round_robin(st_begin+2, st_end, qubits_allocated, qubits, qubits.size());

    // verify that all clients have been fully allocated
    bool any_clients_incomplete = false;
    for (size_t i = 0; i < qubits.size(); i++)
        any_clients_incomplete |= qubits_allocated[i] < qubits[i].size();
    if (any_clients_incomplete)
    {
        std::cerr << "storage_striped_initialization: storage_array was insufficient to allocate memory for"
                      " all clients:";
        for (size_t i = 0; i < qubits.size(); i++)
        {
            std::cerr << "\n\tclient " << i << " : allocated "
                        << qubits_allocated[i] << " of " << qubits[i].size();
        }
        std::cerr << _die{};
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace
{

std::string
_storage_name(size_t n, size_t k, size_t d)
{
    return "[[" + std::to_string(n) + ", " + std::to_string(k) + ", " + std::to_string(d) + "]]";
}

template <class ITER> void
_fill_up_storage_round_robin(ITER st_begin,
                              ITER st_end,
                              std::vector<size_t>& qubits_allocated,
                              const std::vector<std::vector<QUBIT*>>& qubits,
                              size_t idx_upper_bound)
{
    /* 
     * Do the following:
     *      For each storage from `st_begin` to `st_end`, add one qubit
     *      from each client (up-to `idx_upper_bound`)
     * */

    bool done;
    do
    {
        bool any_qubit_inserted{false};
        for (auto it = st_begin; it != st_end; it++)
        {
            STORAGE* s = *it;
            for (size_t i = 0; i < idx_upper_bound; i++)
            {
                if (s->contents().size() == s->logical_qubit_count)
                    break;
                if (qubits_allocated[i] >= qubits[i].size())
                    continue;
                // get next qubit for client and place it into `s`
                QUBIT* q = qubits[i][qubits_allocated[i]];
                qubits_allocated[i]++;
                s->insert(q);

                any_qubit_inserted = true;
            }
        }

        done = !any_qubit_inserted;
    }
    while (!done);
}

}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace sim
