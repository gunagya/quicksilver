/*
 *  author: Claude
 *  date:   15 January 2026
 * */

#include "argparse.h"
#include "globals.h"
#include "sim.h"
#include "sim/configuration/allocator.h"
#include "sim/factory.h"
#include "sim/grid_router.h"
#include "sim/operable.h"

#include <algorithm>
#include <iostream>
#include <vector>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace
{

/*
 * Simulation state:
 * */
cycle_type SIM_CURRENT_CYCLE{0};
uint64_t   SIM_MAGIC_STATES_CONSUMED{0};

/*
 * Simulation functions:
 * */
void sim_init(sim::configuration::FACTORY_ALLOCATION&, bool with_router = false);
void sim_tick(sim::configuration::FACTORY_ALLOCATION&);
void sim_cleanup(sim::configuration::FACTORY_ALLOCATION&);

}  // anon

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

int
main(int argc, char* argv[])
{
    int64_t physical_qubit_budget;
    int64_t sim_cycles;
    bool    enable_router;

    ARGPARSE()
        .optional("-q", "--budget", "Physical qubit budget", physical_qubit_budget, 12'000)
        .optional("-c", "--cycles", "Number of simulation cycles", sim_cycles, 1'000'000)
        .optional("-r", "--enable-router", "Enable grid router in the simulation", enable_router, false)
        .parse(argc, argv);

    /*
     * Setup factory specifications:
     *
     * L1: d = 3 color code cultivation
     * L2: 15:1 (dx,dz,dm) = (25,11,11) distillation
     * */
    sim::configuration::FACTORY_SPECIFICATION l1_spec;
    l1_spec.is_cultivation = true;
    l1_spec.syndrome_extraction_round_time_ns = 1200;
    l1_spec.buffer_capacity = 1;
    l1_spec.output_error_rate = 2e-6;
    l1_spec.escape_distance = 11;
    l1_spec.round_length = 22;
    l1_spec.probability_of_success = 0.35;

    sim::configuration::FACTORY_SPECIFICATION l2_spec;
    l2_spec.is_cultivation = false;
    l2_spec.syndrome_extraction_round_time_ns = 1200;
    l2_spec.buffer_capacity = 4;
    l2_spec.output_error_rate = 1e-12;
    l2_spec.dx = 23;
    l2_spec.dz = 11;
    l2_spec.dm = 11;
    l2_spec.input_count = 4;
    l2_spec.output_count = 1;
    l2_spec.rotations = 11;

    /*
     * Allocate factories:
     * */
    sim::configuration::FACTORY_ALLOCATION alloc =
        sim::configuration::throughput_aware_factory_allocation(physical_qubit_budget, l1_spec, l2_spec);

    const double estimated_throughput =
        sim::configuration::estimate_throughput_of_allocation(alloc, l1_spec.is_cultivation);

    /*
     * Run simulation:
     * */
    sim_init(alloc, enable_router);
    sim::GL_SIM_WALL_START = std::chrono::steady_clock::now();

    while (SIM_CURRENT_CYCLE < sim_cycles)
    {
        sim_tick(alloc);
        SIM_CURRENT_CYCLE++;
    }

    /*
     * Compute true throughput:
     * */

    const double simulated_time_s = alloc.second_level[0]->current_cycle() / (alloc.second_level[0]->freq_khz * 1e3);
    const double true_throughput = static_cast<double>(SIM_MAGIC_STATES_CONSUMED) / simulated_time_s;

    /*
     * Print statistics:
     * */

    print_stat_line(std::cout, "PHYSICAL_QUBIT_BUDGET", physical_qubit_budget);
    print_stat_line(std::cout, "L1_FACTORY_FREQ_KHZ", alloc.first_level[0]->freq_khz);
    print_stat_line(std::cout, "L1_FACTORY_COUNT", alloc.first_level.size());
    print_stat_line(std::cout, "L2_FACTORY_FREQ_KHZ", alloc.second_level[0]->freq_khz);
    print_stat_line(std::cout, "L2_FACTORY_COUNT", alloc.second_level.size());
    print_stat_line(std::cout, "SIMULATION_CYCLES", SIM_CURRENT_CYCLE);
    print_stat_line(std::cout, "MAGIC_STATES_CONSUMED", SIM_MAGIC_STATES_CONSUMED);
    print_stat_line(std::cout, "ESTIMATED_THROUGHPUT_PER_SECOND", estimated_throughput);
    print_stat_line(std::cout, "TRUE_THROUGHPUT_PER_SECOND", true_throughput);

    sim_cleanup(alloc);
    return 0;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

namespace
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
sim_init(sim::configuration::FACTORY_ALLOCATION& alloc, bool with_router)
{
    std::vector<sim::OPERABLE*> operables;
    operables.reserve(alloc.first_level.size() + alloc.second_level.size()+1);
    if (with_router) {
        alloc.router_ = new sim::GRID_ROUTER(alloc, alloc.second_level[0]->freq_khz);
        static_cast<sim::T_DISTILLATION*>(alloc.second_level[0])->add_grid_router(alloc.router_);
        operables.push_back(alloc.router_);
    }
    std::copy(alloc.first_level.begin(), alloc.first_level.end(), std::back_inserter(operables));
    std::copy(alloc.second_level.begin(), alloc.second_level.end(), std::back_inserter(operables));
    sim::coordinate_clock_scale(operables);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
sim_tick(sim::configuration::FACTORY_ALLOCATION& alloc)
{
    for (auto* f : alloc.first_level)
        f->tick();
    if (alloc.router_ != nullptr) {
        // alloc.router_->print_grid(std::cout);
        alloc.router_->tick();
    }
    for (auto* f : alloc.second_level)
        f->tick();

    // consume magic states from second level factories
    for (auto* f : alloc.second_level)
    {
        size_t avail = f->buffer_occupancy();
        f->consume(avail);
        SIM_MAGIC_STATES_CONSUMED += avail;
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void
sim_cleanup(sim::configuration::FACTORY_ALLOCATION& alloc)
{
    for (auto* f : alloc.first_level)
        delete f;
    for (auto* f : alloc.second_level)
        delete f;
    delete alloc.router_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // anon
