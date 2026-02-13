cmake_minimum_required(VERSION 3.20.3)
project(oq2 VERSION 1.0.0 LANGUAGES CXX)

# initialize compile options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(COMPILE_OPTIONS
            -ggdb3
            -Wall
            -Wextra
            -Wpedantic
            -Wunused
            )
else()
    set(COMPILE_OPTIONS
            -O3
            -flto=auto
            -fstrict-aliasing
            -march=native
            -Wunused
            )
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)
find_package(ZLIB REQUIRED)
find_package(LibLZMA REQUIRED)

# need `GMP_DIR` and `MPFR_DIR` to be set for GMP and MPFR
if (COMPILE_BINARY_GENERATION)
    if (APPLE)
        set(GMP_INCLUDE_DIR /opt/homebrew/opt/gmp/include)
        set(MPFR_INCLUDE_DIR /opt/homebrew/opt/mpfr/include)
        set(GMP_LIBRARIES /opt/homebrew/opt/gmp/lib/libgmp.dylib)
        set(MPFR_LIBRARIES /opt/homebrew/opt/mpfr/lib/libmpfr.dylib)
    else()
        find_package(GMP QUIET)
        find_package(MPFR QUIET)

        if(NOT TARGET GMP::gmp OR NOT TARGET GMP::gmpxx)
            find_path(GMP_INCLUDE_DIR NAMES gmp.h)
            find_library(GMP_LIBRARIES NAMES gmp)
            find_library(GMPXX_LIBRARIES NAMES gmpxx)
            if(GMP_INCLUDE_DIR AND GMP_LIBRARIES AND GMPXX_LIBRARIES)
                add_library(GMP::gmp UNKNOWN IMPORTED)
                set_target_properties(GMP::gmp PROPERTIES
                    IMPORTED_LOCATION ${GMP_LIBRARIES}
                    INTERFACE_INCLUDE_DIRECTORIES ${GMP_INCLUDE_DIR}
                )
                add_library(GMP::gmpxx UNKNOWN IMPORTED)
                set_target_properties(GMP::gmpxx PROPERTIES
                    IMPORTED_LOCATION ${GMPXX_LIBRARIES}
                    INTERFACE_INCLUDE_DIRECTORIES ${GMP_INCLUDE_DIR}
                )
            endif()
        endif()

        if(NOT TARGET MPFR::MPFR)
            find_path(MPFR_INCLUDE_DIR NAMES mpfr.h)
            find_library(MPFR_LIBRARIES NAMES mpfr)
            if(MPFR_INCLUDE_DIR AND MPFR_LIBRARIES)
                add_library(MPFR::MPFR UNKNOWN IMPORTED)
                set_target_properties(MPFR::MPFR PROPERTIES
                    IMPORTED_LOCATION ${MPFR_LIBRARIES}
                    INTERFACE_INCLUDE_DIRECTORIES ${MPFR_INCLUDE_DIR}
                )
            endif()
        endif()

        if (NOT TARGET GMP::gmp OR NOT TARGET GMP::gmpxx OR NOT TARGET MPFR::MPFR)
            message(FATAL_ERROR "GMP and MPFR are required for the compiler.")
        endif()
    endif()
endif()

# compile bison/flex files:
flex_target(OQ2Lexer 
            src/compiler/program/oq2/lexer.l 
            ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)
bison_target(OQ2Parser 
            src/compiler/program/oq2/parser.y 
            ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp 
            DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.h)

add_flex_bison_dependency(OQ2Lexer OQ2Parser)

###################################################
###################################################

message(STATUS "BISON_OQ2Parser_OUTPUTS: ${BISON_OQ2Parser_OUTPUTS}")

set(COMMON_FILES
    src/argparse.cpp
    src/dag.cpp
    src/generic_io.cpp
    src/globals.cpp
    src/instruction.cpp)

set(BINARY_GEN_FILES
    ${BISON_OQ2Parser_OUTPUTS} 
    ${FLEX_OQ2Lexer_OUTPUTS}
    src/compiler/program.cpp
    src/compiler/program/expression.cpp
    src/compiler/program/rotation_manager.cpp
    src/compiler/program/value_info.cpp)

set(SIM_FILES
    src/sim/configuration/allocator.cpp
    src/sim/client.cpp
    src/sim/compute_base.cpp
    src/sim/compute_subsystem.cpp
    src/sim/factory.cpp
    src/sim/memory_subsystem.cpp
    src/sim/operable.cpp
    src/sim/rotation_subsystem.cpp
    src/sim/storage.cpp
    src/sim/yoked_codes/yoked_cold_storage.cpp
    src/sim/yoked_codes/yoked_architecture.cpp
    src/sim.cpp)

###################################################
###################################################

# Library for common files (`qs_common`)

add_library(qs_common SHARED ${COMMON_FILES})
target_include_directories(qs_common PUBLIC "src")
target_compile_options(qs_common PUBLIC ${COMPILE_OPTIONS})
target_link_libraries(qs_common PUBLIC ZLIB::ZLIB LibLZMA::LibLZMA)

# Library for simulator files (`qs_simulator`)

add_library(qs_simulator SHARED ${SIM_FILES})
target_include_directories(qs_simulator PUBLIC "src")
target_compile_options(qs_simulator PUBLIC ${COMPILE_OPTIONS})
target_link_libraries(qs_simulator PUBLIC qs_common)

###################################################
###################################################

# Binary generation executable (`qs_gen_binary`)

if (COMPILE_BINARY_GENERATION)
    add_executable(qs_gen_binary main/qs_gen_binary.cpp ${BINARY_GEN_FILES})

    # need to add flex headers manually on macos
    if (UNIX AND APPLE)
        target_include_directories(qs_gen_binary PRIVATE "/opt/homebrew/opt/flex/include")
    endif()

    target_include_directories(qs_gen_binary PRIVATE "src" 
                                                    ${CMAKE_CURRENT_BINARY_DIR} 
                                                    "deps/nwqec/include" 
                                                    ${GMP_INCLUDE_DIR} 
                                                    ${MPFR_INCLUDE_DIR})

    target_compile_options(qs_gen_binary PRIVATE ${COMPILE_OPTIONS})
    target_compile_definitions(qs_gen_binary PRIVATE -DQELIB1_INC_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qelib1.inc")
    target_link_libraries(qs_gen_binary PRIVATE qs_common
                                                ${GMP_LIBRARIES} 
                                                ${MPFR_LIBRARIES})
endif()

###################################################
###################################################

# Memory scheduler (qs_memory_scheduler)

add_executable(qs_memory_scheduler main/qs_memory_scheduler.cpp
                                    src/compiler/memory_scheduler.cpp
                                    src/compiler/memory_scheduler/eif.cpp
                                    src/compiler/memory_scheduler/hint.cpp)
target_include_directories(qs_memory_scheduler PRIVATE "src")
target_compile_options(qs_memory_scheduler PRIVATE ${COMPILE_OPTIONS})
target_link_libraries(qs_memory_scheduler PRIVATE qs_common)

###################################################
###################################################

# Simple context switch simulator (qs_ctxsim)

add_executable(qs_ctxsim main/qs_ctxsim.cpp)
target_include_directories(qs_ctxsim PRIVATE "src")
target_compile_options(qs_ctxsim PRIVATE ${COMPILE_OPTIONS})
target_link_libraries(qs_ctxsim PRIVATE qs_simulator)

###################################################
###################################################

# Magic state throughput estimation (qs_tpest)

add_executable(qs_tpest main/qs_throughput_estimator.cpp)
target_include_directories(qs_tpest PRIVATE "src")
target_compile_options(qs_tpest PRIVATE ${COMPILE_OPTIONS})
target_link_libraries(qs_tpest PRIVATE qs_simulator)

###################################################
###################################################

# Yoked simulator (yoked_simulator)

add_executable(yoked_simulator src/sim/yoked_codes/yoked_simulator.cpp
                            src/compiler/memory_scheduler.cpp
                            src/compiler/memory_scheduler/eif.cpp
                            src/compiler/memory_scheduler/hint.cpp)
target_include_directories(yoked_simulator PRIVATE "src")
target_compile_options(yoked_simulator PRIVATE ${COMPILE_OPTIONS})
target_link_libraries(yoked_simulator PRIVATE qs_simulator)

###################################################
###################################################

# Main simulator (quicksilver)

add_executable(quicksilver main/quicksilver.cpp
                            src/compiler/memory_scheduler.cpp
                            src/compiler/memory_scheduler/eif.cpp
                            src/compiler/memory_scheduler/hint.cpp)
target_include_directories(quicksilver PRIVATE "src")
target_compile_options(quicksilver PRIVATE ${COMPILE_OPTIONS})
target_link_libraries(quicksilver PRIVATE qs_simulator)

###################################################
###################################################
