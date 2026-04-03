/*
    author: Suhas Vittal
    date:   10 November 2025
*/

#include "argparse.h"
#include "generic_io.h"
#include "instruction.h"

#include <algorithm>
#include <vector>
#include <iostream>
#include <iomanip>

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

struct ProgramStats
{
    uint32_t num_qubits{0};
    uint64_t total_instructions{0};
    uint64_t unrolled_instructions{0};
    uint64_t unrolled_t_gates{0};
    uint64_t s_gates{0};
    uint64_t h_gates{0};
    uint64_t cx_gates{0};
    uint64_t mswap_instructions{0};
    uint64_t mprefetch_instructions{0};
    uint64_t rz_instructions{0};
    uint64_t rz_non_software_uops{0};
    std::vector<size_t> rz_non_software_uops_samples{};
};

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

ProgramStats analyze_binary_file(const std::string& input_file, uint64_t instruction_limit = 0, bool verbose = false)
{
    ProgramStats stats;
    auto count_non_software_rotation_uops = [](const INSTRUCTION& inst) {
        return std::count_if(inst.urotseq.begin(), inst.urotseq.end(), [](INSTRUCTION::TYPE gate_type) {
            return !is_software_instruction(gate_type);
        });
    };
    
    generic_strm_type istrm;
    generic_strm_open(istrm, input_file, "rb");
    
    // Read number of qubits (first 4 bytes)
    generic_strm_read(istrm, &stats.num_qubits, sizeof(stats.num_qubits));
    
    std::cout << "[ QS_REPORT ] Reading binary file: " << input_file << std::endl;
    std::cout << "[ QS_REPORT ] Number of qubits: " << stats.num_qubits << std::endl;
    if (instruction_limit > 0) {
        std::cout << "[ QS_REPORT ] Instruction limit: " << instruction_limit << std::endl;
    }
    
    // Read and analyze each instruction
    while (!generic_strm_eof(istrm))
    {
        // Check instruction limit
        if (instruction_limit > 0 && stats.total_instructions >= instruction_limit) {
            std::cout << "[ QS_REPORT ] Reached instruction limit of " << instruction_limit << std::endl;
            break;
        }
        
        // Read instruction from stream
        INSTRUCTION* inst_ptr = read_instruction_from_stream(istrm);

        // If we hit EOF during reading, break
        if (inst_ptr == nullptr || generic_strm_eof(istrm))
            break;
            
        // Get reference to instruction
        INSTRUCTION& inst = *inst_ptr;
        
        if (is_software_instruction(inst.type))
        {
            delete inst_ptr;
            continue;
        }

        if (verbose)
            std::cout << "[" << stats.total_instructions << "] " << inst << "\n";

        stats.total_instructions++;

        // Count different gate types
        switch (inst.type)
        {
            case INSTRUCTION::TYPE::T:
            case INSTRUCTION::TYPE::TDG:
            case INSTRUCTION::TYPE::TX:
            case INSTRUCTION::TYPE::TXDG:
                stats.unrolled_t_gates++;
                stats.unrolled_instructions++;
                break;

            case INSTRUCTION::TYPE::S:
            case INSTRUCTION::TYPE::SDG:
            case INSTRUCTION::TYPE::SX:
            case INSTRUCTION::TYPE::SXDG:
                stats.s_gates++;
                stats.unrolled_instructions++;
                break;

            case INSTRUCTION::TYPE::H:
                stats.h_gates++;
                stats.unrolled_instructions++;
                break;

            case INSTRUCTION::TYPE::CX:
                stats.cx_gates++;
                stats.unrolled_instructions++;
                break;

            case INSTRUCTION::TYPE::MPLACE:
            case INSTRUCTION::TYPE::MSWAP:
                stats.mswap_instructions++;
                stats.unrolled_instructions++;
                break;

            case INSTRUCTION::TYPE::MPREFETCH:
                stats.mprefetch_instructions++;
                stats.unrolled_instructions++;
                break;

            case INSTRUCTION::TYPE::RX:
            case INSTRUCTION::TYPE::RZ:
            {
                const size_t non_software_rotation_uops = count_non_software_rotation_uops(inst);
                if (inst.type == INSTRUCTION::TYPE::RZ)
                {
                    stats.rz_instructions++;
                    stats.rz_non_software_uops += non_software_rotation_uops;
                    stats.rz_non_software_uops_samples.push_back(non_software_rotation_uops);
                }

                // For rotation gates, count only non-software gates in the unrolled sequence
                for (auto gate_type : inst.urotseq)
                {
                    if (is_software_instruction(gate_type))
                        continue;

                    if (gate_type == INSTRUCTION::TYPE::T ||
                        gate_type == INSTRUCTION::TYPE::TDG ||
                        gate_type == INSTRUCTION::TYPE::TX ||
                        gate_type == INSTRUCTION::TYPE::TXDG)
                    {
                        stats.unrolled_t_gates++;
                    }
                    else if (gate_type == INSTRUCTION::TYPE::S ||
                             gate_type == INSTRUCTION::TYPE::SDG ||
                             gate_type == INSTRUCTION::TYPE::SX ||
                             gate_type == INSTRUCTION::TYPE::SXDG)
                    {
                        stats.s_gates++;
                    }
                    else if (gate_type == INSTRUCTION::TYPE::H)
                    {
                        stats.h_gates++;
                    }
                    stats.unrolled_instructions++;
                }
                break;
            }

            case INSTRUCTION::TYPE::CCX:
            case INSTRUCTION::TYPE::CCZ:
                stats.unrolled_instructions += 13;  // 15 uops for CCX, 13 for CCZ
                stats.cx_gates += 6;
                stats.unrolled_t_gates += 7;
                break;

            default:
                break;
        }
        
        // Print progress every 1M instructions
        if (stats.total_instructions % 1000000 == 0)
            std::cout << "[ QS_REPORT ] Processed " << stats.total_instructions << " instructions..." << std::endl;
        
        delete inst_ptr;
    }
    
    generic_strm_close(istrm);
    
    std::cout << "[ QS_REPORT ] Analysis complete. Total instructions processed: " << stats.total_instructions << std::endl;
    
    return stats;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    std::string input_file;
    int64_t instruction_limit = 0;
    bool verbose = false;
    
    ARGPARSE()
        .required("input-file", "compressed binary program file (.bin, .gz, .xz)", input_file)
        .optional("-i", "--instruction-limit", "Maximum number of instructions to read (0 = unlimited)", instruction_limit, (int64_t)0)
        .optional("-v", "--verbose", "Print each instruction as it is read", verbose, false)
        .parse(argc, argv);
    
    try
    {
        ProgramStats stats = analyze_binary_file(input_file, (uint64_t)instruction_limit, verbose);
        
        // Print the report
        std::cout << "\n";
        std::cout << "PROGRAM REPORT" << std::endl;
        std::cout << "==============" << std::endl;
        
        print_stat_line(std::cout, "PROGRAM_QUBITS", stats.num_qubits);
        print_stat_line(std::cout, "TOTAL_INSTRUCTIONS", stats.total_instructions);
        print_stat_line(std::cout, "UNROLLED_INSTRUCTIONS", stats.unrolled_instructions);
        print_stat_line(std::cout, "UNROLLED_T_GATES", stats.unrolled_t_gates);
        print_stat_line(std::cout, "S_GATES", stats.s_gates);
        print_stat_line(std::cout, "H_GATES", stats.h_gates);
        print_stat_line(std::cout, "CX_GATES", stats.cx_gates);
        print_stat_line(std::cout, "MSWAP_INSTRUCTIONS", stats.mswap_instructions);
        print_stat_line(std::cout, "MPREFETCH_INSTRUCTIONS", stats.mprefetch_instructions);
        print_stat_line(std::cout, "AVG_RZ_NON_SOFTWARE_UOPS",
                        stats.rz_instructions > 0
                            ? static_cast<double>(stats.rz_non_software_uops)
                                  / static_cast<double>(stats.rz_instructions)
                            : 0.0);
        if (!stats.rz_non_software_uops_samples.empty())
        {
            std::vector<size_t> sorted_rz_non_software_uops = stats.rz_non_software_uops_samples;
            std::sort(sorted_rz_non_software_uops.begin(), sorted_rz_non_software_uops.end());
            const size_t mid = sorted_rz_non_software_uops.size() / 2;
            const double median_rz_non_software_uops =
                (sorted_rz_non_software_uops.size() % 2 == 1)
                    ? static_cast<double>(sorted_rz_non_software_uops[mid])
                    : 0.5 * (static_cast<double>(sorted_rz_non_software_uops[mid - 1])
                             + static_cast<double>(sorted_rz_non_software_uops[mid]));
            print_stat_line(std::cout, "MEDIAN_RZ_NON_SOFTWARE_UOPS",
                            median_rz_non_software_uops);
        }
        else
        {
            print_stat_line(std::cout, "MEDIAN_RZ_NON_SOFTWARE_UOPS", 0.0);
        }

        // Calculate and print percentages
        if (stats.unrolled_instructions > 0)
        {
            double t_gate_percentage = (static_cast<double>(stats.unrolled_t_gates) / static_cast<double>(stats.unrolled_instructions)) * 100.0;
            double s_gate_percentage = (static_cast<double>(stats.s_gates) / static_cast<double>(stats.unrolled_instructions)) * 100.0;
            double h_gate_percentage = (static_cast<double>(stats.h_gates) / static_cast<double>(stats.unrolled_instructions)) * 100.0;
            double cx_gate_percentage = (static_cast<double>(stats.cx_gates) / static_cast<double>(stats.unrolled_instructions)) * 100.0;
            double memory_instruction_percentage = (static_cast<double>(stats.mswap_instructions + stats.mprefetch_instructions) / static_cast<double>(stats.unrolled_instructions)) * 100.0;

            std::cout << "\n";
            print_stat_line(std::cout, "T_GATE_PERCENTAGE", t_gate_percentage);
            print_stat_line(std::cout, "S_GATE_PERCENTAGE", s_gate_percentage);
            print_stat_line(std::cout, "H_GATE_PERCENTAGE", h_gate_percentage);
            print_stat_line(std::cout, "CX_GATE_PERCENTAGE", cx_gate_percentage);
            print_stat_line(std::cout, "MEMORY_INSTRUCTION_PERCENTAGE", memory_instruction_percentage);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
