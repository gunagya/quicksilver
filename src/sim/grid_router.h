#ifndef SIM_GRID_ROUTER_h
#define SIM_GRID_ROUTER_h

#include "sim/configuration/allocator.h"
#include "sim/factory.h"
#include <cstddef>
#include <vector>

namespace sim {

struct Cell {
    int busy_cycles;
    T_FACTORY_BASE* factory;
};

class GRID_ROUTER: public OPERABLE
{
    std::vector<std::vector<Cell>> grid_;
    std::pair<size_t, size_t> origin_;

public: 
    GRID_ROUTER(configuration::FACTORY_ALLOCATION& alloc, double freq_khz);

    T_FACTORY_BASE* request_magic_state();

    long operate() override;
    void print_deadlock_info(std::ostream&) const override;

    void print_grid(std::ostream& out) const;
};

} // namespace sim

#endif  // SIM_GRID_ROUTER_h