/*
 *  author: Suhas Vittal
 *  date:   15 January 2026
 * */

namespace sim
{
namespace configuration
{

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

constexpr size_t
surface_code_physical_qubit_count(size_t d)
{
    return surface_code_physical_qubit_count(d,d);
}

constexpr size_t
surface_code_physical_qubit_count(size_t dx, size_t dz)
{
    return 2*dx*(dz+1);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

constexpr size_t
bivariate_bicycle_code_physical_qubit_count(size_t d)
{
    return 12*d;
}

constexpr size_t
bivariate_bicycle_code_logical_qubit_count(size_t d)
{
    return 12;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

constexpr size_t
magic_state_cultivation_physical_qubit_count(size_t d /* escape distance */)
{
    return surface_code_physical_qubit_count(d);
}

constexpr size_t
magic_state_distillation_physical_qubit_count(size_t input_count, size_t output_count, size_t dx, size_t dz)
{
    const size_t total_logical_qubits = input_count+output_count;
    const size_t assumed_routing_overhead = total_logical_qubits/2;
    return (input_count+assumed_routing_overhead) * surface_code_physical_qubit_count(dx, dz)
         + output_count * surface_code_physical_qubit_count(dx);
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

}  // namespace configuration
}  // namespace sim

