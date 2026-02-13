/*
 *  author: Suhas Vittal
 *  date:   12 January 2026
 * */

#include "sim/memory_subsystem.h"
#include "globals.h"
#include "sim/routing_model/multi_channel_bus.h"

#include <algorithm>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

MEMORY_SUBSYSTEM::MEMORY_SUBSYSTEM(std::vector<STORAGE*>&& storages)
    :storages_(std::move(storages))
{
    using MBC = routing::MULTI_CHANNEL_BUS<STORAGE>;

    routing_ = std::make_unique<MBC>(storages_, 2);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

MEMORY_SUBSYSTEM::access_result_type
MEMORY_SUBSYSTEM::do_memory_access(QUBIT* ld, 
                                    QUBIT* st,
                                    cycle_type c_current_cycle,
                                    double c_freq_khz)
{
    auto storage_it = lookup(ld);
    if (storage_it == storages_.end())
    {
        std::cerr << "MEMORY_SUBSYSTEM::do_memory_access: qubit " << *ld << " not found";
        for (auto* s : storages_)
        {
            std::cerr << "\n\t" << s->name << " :";
            for (auto* q : s->contents())
                std::cerr << " " << *q;
        }
        std::cerr << _die{};
    }

    if (routing_->can_route_to(*storage_it, c_current_cycle))
    {
        auto result = (*storage_it)->do_memory_access(ld, st);
        if (result.success)
        {
            cycle_type latency = convert_cycles(result.latency, result.storage_freq_khz, c_freq_khz);
            routing_->lock_route_to(*storage_it, c_current_cycle+latency);
            result.latency = latency;
        }
        return result;
    }
    
    return access_result_type{};
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

QUBIT*
MEMORY_SUBSYSTEM::retrieve_qubit(client_id_type c_id, qubit_type q_id) const
{
    for (const auto* s : storages_)
    {
        const auto& contents = s->contents();
        auto q_it = std::find_if(contents.begin(), contents.end(),
                            [c_id, q_id] (const auto* q) { return q->client_id == c_id && q->qubit_id == q_id; });
        if (q_it != contents.end())
            return *q_it;
    }
    return nullptr;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

const std::vector<STORAGE*>&
MEMORY_SUBSYSTEM::storages() const
{
    return storages_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

bool
MEMORY_SUBSYSTEM::has_free_adapter_for(QUBIT* q) const
{
    return (*lookup(q))->has_free_adapter();
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

std::vector<STORAGE*>::iterator
MEMORY_SUBSYSTEM::lookup(QUBIT* q)
{
    return std::find_if(storages_.begin(), storages_.end(),
        [q] (STORAGE* s) { return s->contents().count(q) > 0; });
}

std::vector<STORAGE*>::const_iterator
MEMORY_SUBSYSTEM::lookup(QUBIT* q) const
{
    return std::find_if(storages_.begin(), storages_.end(),
        [q] (STORAGE* s) { return s->contents().count(q) > 0; });
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

} // namespace sim
