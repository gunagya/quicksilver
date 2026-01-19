/*
 *  author: Suhas Vittal
 *  date:   6 January 2026
 * */

#include "sim/factory.h"
#include "sim/grid_router.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <random>
#include <sstream>

namespace sim
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

extern std::mt19937_64 GL_RNG;

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/*
 * Helper functions (implemented at bottom of the file)
 * */

namespace
{

/*
 * Needed for determining failures:
 * */
static std::uniform_real_distribution FPR(0.0, 1.0);

/*
 * Assumed injection error rate:
 * */
const double INJECTION_ERROR_PROBABILITY{1e-3};

/*
 * Generate name for T_DISTILLATION factory: "D_<initial_input_count+num_rotation_steps>_<output_count>"
 * */
std::string _distillation_name(size_t initial_input_count, size_t output_count, size_t num_rotation_steps);

/*
 * Generate name for T_CULTIVATION factory: "C_p=<probability in scientific notation>"
 * */
std::string _cultivation_name(double probability_of_success);

}  // anonymous namespace

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

T_FACTORY_BASE::T_FACTORY_BASE(std::string_view name,
                                double freq_khz,
                                double _output_error_probability,
                                size_t _buffer_capacity)
    :OPERABLE(name, freq_khz),
    output_error_probability(_output_error_probability),
    buffer_capacity(_buffer_capacity)
{}

void
T_FACTORY_BASE::consume(size_t count)
{
    assert(count <= buffer_occupancy_);
    buffer_occupancy_ -= count;
}

void
T_FACTORY_BASE::print_deadlock_info(std::ostream& out) const
{
    out << name << ": buffer occupancy = " << buffer_occupancy_ << " of " << buffer_capacity << "\n";
}

long
T_FACTORY_BASE::operate()
{
    if (buffer_occupancy_ >= buffer_capacity || production_step())
        return 1;
    else 
        return 0;
}

size_t
T_FACTORY_BASE::buffer_occupancy() const
{
    return buffer_occupancy_;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

T_DISTILLATION::T_DISTILLATION(double freq_khz,
                                double output_error_probability,
                                size_t buffer_capacity,
                                size_t _initial_input_count,
                                size_t _output_count,
                                size_t _num_rotation_steps)
    :T_FACTORY_BASE(_distillation_name(_initial_input_count, _output_count, _num_rotation_steps),
                        freq_khz,
                        output_error_probability,
                        buffer_capacity),
    initial_input_count(_initial_input_count),
    output_count(_output_count),
    num_rotation_steps(_num_rotation_steps)
{}

void
T_DISTILLATION::print_deadlock_info(std::ostream& out) const
{
    out << name << ": buffer occupancy = " << buffer_occupancy_ << " of " << buffer_capacity
                << ", step: " << step_ << " of " << (1+num_rotation_steps)
                << "\n";
}

bool
T_DISTILLATION::production_step()
{
    const bool is_lowest_level = previous_level.empty();

    size_t magic_states_needed = (step_ == 0) ? std::min(initial_input_count,1ul) : 1; 
    const double p_sampled = FPR(GL_RNG);

    // get the magic states that we need and compute the error probability of these magic states.
    double p_error{0.0};
    if (is_lowest_level)
    {
        // can get all magic states via injection -- check if an error occurs.
        // If so, then restart production
        p_error = INJECTION_ERROR_PROBABILITY * magic_states_needed;
    } else if (router_!= nullptr) {
        // request magic states via the router
        size_t magic_states_avail = 0;
        while (magic_states_avail < magic_states_needed) {
            auto factory = router_->request_magic_state();
            if (factory == nullptr) {
                // unable to get magic states this cycle
                return false;
            }
            magic_states_avail++;
            factory->consume(1);
            p_error += factory->output_error_probability;
        }
    } else {
        size_t magic_states_avail = std::transform_reduce(previous_level.begin(), previous_level.end(),   
                                                            size_t{0},
                                                            std::plus<size_t>{},
                                                            [] (const auto* f) { return f->buffer_occupancy(); });
        if (magic_states_avail < magic_states_needed)
            return false;
        
        // get the magic states we need -- simultaneoulsy compute the probability of error
        for (auto* f : previous_level)
        {
            if (f->buffer_occupancy() == 0)
                continue;
            size_t count = std::min(f->buffer_occupancy(), magic_states_needed);
            f->consume(count);
            magic_states_needed -= count;
            p_error += f->output_error_probability * count;
            if (magic_states_needed == 0)
                break;
        }
    }        

    step_ = (p_sampled < p_error) ? 0 : step_+1;
    if (step_ == num_rotation_steps+1)
    {
        buffer_occupancy_++;
        step_ = 0;
    }
    return true;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

T_CULTIVATION::T_CULTIVATION(double freq_khz,
                                double output_error_probability,
                                size_t buffer_capacity,
                                double _probability_of_success)
    :T_FACTORY_BASE(_cultivation_name(_probability_of_success),
                     freq_khz,
                     output_error_probability,
                     buffer_capacity),
    probability_of_success(_probability_of_success)
{}

bool
T_CULTIVATION::production_step()
{
    if (!enabled_)
        return false;
    if (FPR(GL_RNG) <= probability_of_success)
        buffer_occupancy_++;
    return true;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

/* HELPER FUNCTIONS START HERE */

namespace
{

std::string
_distillation_name(size_t initial_input_count, size_t output_count, size_t num_rotation_steps)
{
    return "D_" + std::to_string(initial_input_count + num_rotation_steps) + "_" + std::to_string(output_count);
}

std::string
_cultivation_name(double probability_of_success)
{
    std::ostringstream oss;
    oss << "C_p=" << static_cast<int>(100*probability_of_success) << "%";
    return oss.str();
}

}  // anonymous namespace

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

} // namespace sim
