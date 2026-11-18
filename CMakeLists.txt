# ==============================================================================
# AEGIS Critical Systems - Master CMake Toolchain & Build Specification
# Standards: ISO C17 (MISRA C:2012) | ISO C++23 (MISRA C++:2023) | DO-178C DAL-A
# ==============================================================================
cmake_minimum_required(VERSION 3.24)

project(aegis_critical_systems
    VERSION 1.0.0
    DESCRIPTION "Deterministic Mixed-Language Architecture for Mission-Critical Telemetry & HIL"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict Mission-Critical Compiler Flags
set(AEGIS_COMMON_WARNINGS
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wformat=2
    -Wundef
    -fno-common
)

set(AEGIS_C_FLAGS
    ${AEGIS_COMMON_WARNINGS}
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wold-style-definition
)

set(AEGIS_CXX_FLAGS
    ${AEGIS_COMMON_WARNINGS}
    -Wswitch-enum
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -fno-exceptions
    -fno-rtti
)

# ------------------------------------------------------------------------------
# Subsystem 01: Hybrid Safety Gateway (C17 ABI & Core)
# ------------------------------------------------------------------------------
add_library(aegis_gateway_abi STATIC
    01-hybrid-can-gateway/src/aegis_abi_check.c
)

target_include_directories(aegis_gateway_abi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/01-hybrid-can-gateway/include
)

target_compile_options(aegis_gateway_abi PRIVATE ${AEGIS_C_FLAGS})

# ABI Contract Deterministic Test Harness
enable_testing()

add_executable(test_abi_contract
    01-hybrid-can-gateway/src/test_abi_contract.c
)

target_link_libraries(test_abi_contract PRIVATE aegis_gateway_abi)
target_compile_options(test_abi_contract PRIVATE ${AEGIS_C_FLAGS})

add_test(NAME abi_contract_verification COMMAND test_abi_contract)

# Rust no_std Static Archive FFI Integration Test
set(AEGIS_RUST_PARSER_LIB "${CMAKE_CURRENT_SOURCE_DIR}/target/release/libaegis_parser.a")
if(EXISTS "${AEGIS_RUST_PARSER_LIB}")
    add_executable(test_rust_ffi
        01-hybrid-can-gateway/src/test_rust_ffi.c
    )
    target_link_libraries(test_rust_ffi PRIVATE
        aegis_gateway_abi
        "${AEGIS_RUST_PARSER_LIB}"
    )
    target_compile_options(test_rust_ffi PRIVATE ${AEGIS_C_FLAGS} -ffunction-sections -fdata-sections)
    target_link_options(test_rust_ffi PRIVATE -Wl,--gc-sections)
    add_test(NAME rust_nostd_ffi_verification COMMAND test_rust_ffi)

    # C17 PREEMPT_RT Cyclic Executive & SocketCAN Node
    add_library(aegis_gateway_rt STATIC
        01-hybrid-can-gateway/src/aegis_rt_exec.c
    )
    target_link_libraries(aegis_gateway_rt PUBLIC
        aegis_gateway_abi
        "${AEGIS_RUST_PARSER_LIB}"
    )
    target_compile_options(aegis_gateway_rt PRIVATE ${AEGIS_C_FLAGS} -ffunction-sections -fdata-sections)

    add_executable(aegis_gateway_node
        01-hybrid-can-gateway/src/main_gateway.c
    )
    target_link_libraries(aegis_gateway_node PRIVATE aegis_gateway_rt)
    target_compile_options(aegis_gateway_node PRIVATE ${AEGIS_C_FLAGS})
    target_link_options(aegis_gateway_node PRIVATE -Wl,--gc-sections)

    add_executable(test_gateway_exec
        01-hybrid-can-gateway/src/test_gateway_exec.c
    )
    target_link_libraries(test_gateway_exec PRIVATE aegis_gateway_rt)
    target_compile_options(test_gateway_exec PRIVATE ${AEGIS_C_FLAGS})
    target_link_options(test_gateway_exec PRIVATE -Wl,--gc-sections)
    add_test(NAME gateway_cyclic_exec_verification COMMAND test_gateway_exec)

    # --------------------------------------------------------------------------
    # Subsystem 02: HIL HV-BMS & Propulsion Simulator (C++23)
    # --------------------------------------------------------------------------
    add_library(aegis_hil_plant STATIC
        02-hil-hvbms-sim/src/hvbms_plant.cpp
    )
    target_include_directories(aegis_hil_plant PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/02-hil-hvbms-sim/include
    )
    target_link_libraries(aegis_hil_plant PUBLIC aegis_gateway_rt)
    target_compile_options(aegis_hil_plant PRIVATE ${AEGIS_CXX_FLAGS} -ffunction-sections -fdata-sections)

    add_executable(aegis_hil_sim
        02-hil-hvbms-sim/src/main_hil_sim.cpp
    )
    target_link_libraries(aegis_hil_sim PRIVATE aegis_hil_plant)
    target_compile_options(aegis_hil_sim PRIVATE ${AEGIS_CXX_FLAGS})
    target_link_options(aegis_hil_sim PRIVATE -Wl,--gc-sections)

    add_executable(test_hil_fsm
        02-hil-hvbms-sim/src/test_hil_fsm.cpp
    )
    target_link_libraries(test_hil_fsm PRIVATE aegis_hil_plant)
    target_compile_options(test_hil_fsm PRIVATE ${AEGIS_CXX_FLAGS})
    target_link_options(test_hil_fsm PRIVATE -Wl,--gc-sections)
    add_test(NAME hil_fsm_closed_loop_verification COMMAND test_hil_fsm)
endif()

# ------------------------------------------------------------------------------
# Subsystem 03: Lock-Free HUMS & Edge Anomaly Engine (C++23)
# ------------------------------------------------------------------------------
find_package(Threads REQUIRED)
option(AEGIS_ENABLE_MALLOC_TRAP "Enable --wrap=malloc post-init trap" ON)

add_library(aegis_hums_core STATIC
    03-lockfree-hums/src/q15_ekf.cpp
    03-lockfree-hums/src/malloc_guard.c
)
target_include_directories(aegis_hums_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/03-lockfree-hums/include
)
target_link_libraries(aegis_hums_core PUBLIC aegis_gateway_abi)
target_compile_options(aegis_hums_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${AEGIS_CXX_FLAGS}>
    $<$<COMPILE_LANGUAGE:C>:${AEGIS_C_FLAGS}>
    -ffunction-sections -fdata-sections
)

add_executable(aegis_hums_bench
    03-lockfree-hums/src/main_hums_bench.cpp
)
target_link_libraries(aegis_hums_bench PRIVATE aegis_hums_core)
target_compile_options(aegis_hums_bench PRIVATE ${AEGIS_CXX_FLAGS})
target_link_options(aegis_hums_bench PRIVATE
    -Wl,--gc-sections
    $<$<BOOL:${AEGIS_ENABLE_MALLOC_TRAP}>:-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc>
)

add_executable(test_lockfree_hums
    03-lockfree-hums/src/test_lockfree_hums.cpp
)
target_link_libraries(test_lockfree_hums PRIVATE aegis_hums_core Threads::Threads)
target_compile_options(test_lockfree_hums PRIVATE ${AEGIS_CXX_FLAGS})
target_link_options(test_lockfree_hums PRIVATE
    -Wl,--gc-sections
    $<$<BOOL:${AEGIS_ENABLE_MALLOC_TRAP}>:-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc>
)
add_test(NAME hums_lockfree_verification COMMAND test_lockfree_hums)

if(AEGIS_ENABLE_MALLOC_TRAP)
    add_executable(test_malloc_trap
        03-lockfree-hums/src/test_malloc_trap.cpp
    )
    target_link_libraries(test_malloc_trap PRIVATE aegis_hums_core)
    target_compile_options(test_malloc_trap PRIVATE
        ${AEGIS_CXX_FLAGS}
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc
    )
    target_link_options(test_malloc_trap PRIVATE
        -Wl,--gc-sections
        -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc
    )
    add_test(NAME hums_malloc_trap_verification COMMAND test_malloc_trap)
endif()

if(EXISTS "${AEGIS_RUST_PARSER_LIB}")
    add_executable(aegis_e2e_hil_gw_driver
        02-hil-hvbms-sim/src/e2e_closed_loop_driver.cpp
    )
    target_link_libraries(aegis_e2e_hil_gw_driver PRIVATE aegis_hil_plant aegis_hums_core)
    target_compile_options(aegis_e2e_hil_gw_driver PRIVATE ${AEGIS_CXX_FLAGS})
    target_link_options(aegis_e2e_hil_gw_driver PRIVATE -Wl,--gc-sections)
endif()

# ------------------------------------------------------------------------------
# Subsystem 04: Tactical GCS - Embedded C17 TM/TC Daemon
# ------------------------------------------------------------------------------
add_library(aegis_tmtc_core STATIC
    04-tactical-gcs/tmtc-daemon/src/aegis_crc32c.c
    04-tactical-gcs/tmtc-daemon/src/aegis_tmtc_epoll.c
)
target_include_directories(aegis_tmtc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/04-tactical-gcs/tmtc-daemon/include
)
target_link_libraries(aegis_tmtc_core PUBLIC aegis_gateway_abi)
target_compile_options(aegis_tmtc_core PRIVATE ${AEGIS_C_FLAGS} -ffunction-sections -fdata-sections)

add_executable(aegis_tmtc_daemon
    04-tactical-gcs/tmtc-daemon/src/main_tmtc_daemon.c
)
target_link_libraries(aegis_tmtc_daemon PRIVATE aegis_tmtc_core)
target_compile_options(aegis_tmtc_daemon PRIVATE ${AEGIS_C_FLAGS})
target_link_options(aegis_tmtc_daemon PRIVATE -Wl,--gc-sections)

add_executable(test_tmtc_daemon
    04-tactical-gcs/tmtc-daemon/src/test_tmtc_daemon.c
)
target_link_libraries(test_tmtc_daemon PRIVATE aegis_tmtc_core)
target_compile_options(test_tmtc_daemon PRIVATE ${AEGIS_C_FLAGS})
target_link_options(test_tmtc_daemon PRIVATE -Wl,--gc-sections)
add_test(NAME tmtc_epoll_crc32c_verification COMMAND test_tmtc_daemon)
