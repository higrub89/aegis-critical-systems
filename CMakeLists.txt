# ==============================================================================
# AEGIS Critical Systems - Master CMake Toolchain & Build Specification
# Standards: ISO C17 (MISRA C:2012) | ISO C++23 (MISRA C++:2023) | DO-178C DAL-A
# ==============================================================================
cmake_minimum_required(VERSION 3.24)

project(aegis_critical_systems
    VERSION 0.1.0
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
endif()
