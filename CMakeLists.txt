cmake_minimum_required(VERSION 3.22)
project(keyhunt LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/cpu")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "The preserved CPU backend currently requires Linux x86-64 with SSSE3; other hosts are not yet ported.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$" OR NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "The existing CPU assembly requires GCC or Clang.")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # Original sources use GNU extensions.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KEYHUNT_NATIVE_CPU "Optimize for the build host's CPU (disable for portable x86-64 binaries)" ON)
option(KEYHUNT_ENABLE_LTO "Preserve the original per-source LTO selection in optimized builds" ON)
option(KEYHUNT_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(KEYHUNT_BUILD_LEGACY "Build the separate GMP/OpenSSL legacy executable" OFF)
option(KEYHUNT_BUILD_BSGSD "Build the original local BSGS daemon" OFF)
option(KEYHUNT_ENABLE_HIP "Request the future HIP backend" OFF)
option(KEYHUNT_ENABLE_CUDA "Request the future CUDA backend" OFF)
if(KEYHUNT_ENABLE_HIP OR KEYHUNT_ENABLE_CUDA)
    message(FATAL_ERROR "GPU backends are not implemented yet: HIP is milestone C07 and CUDA is C18. Configure a CPU build with both options OFF.")
endif()

include(cmake/CompilerOptions.cmake)
keyhunt_configure_original_lto()
find_package(Threads REQUIRED)

# Keep the SHA-3 sources in C++ mode, matching the original Makefile/linkage.
set_source_files_properties(third_party/sha3/sha3.c third_party/sha3/keccak.c PROPERTIES LANGUAGE CXX)
add_library(keyhunt_common STATIC
    src/core/util.cpp
    third_party/base58/base58.c
    third_party/bloom/bloom.cpp
    third_party/oldbloom/bloom.cpp
    third_party/rmd160/rmd160.c
    third_party/sha3/sha3.c
    third_party/sha3/keccak.c
    third_party/xxhash/xxhash.c)
target_include_directories(keyhunt_common PUBLIC include third_party)
target_link_libraries(keyhunt_common PUBLIC Threads::Threads m)
keyhunt_configure_target(keyhunt_common)
set_source_files_properties(third_party/base58/base58.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")

add_library(keyhunt_cpu_crypto STATIC
    src/crypto/secp256k1/Int.cpp
    src/crypto/secp256k1/IntMod.cpp
    src/crypto/secp256k1/IntGroup.cpp
    src/crypto/secp256k1/Point.cpp
    src/crypto/secp256k1/Random.cpp
    src/crypto/secp256k1/SECP256K1.cpp
    src/crypto/hash/sha256.cpp
    src/crypto/hash/sha256_sse.cpp
    src/crypto/hash/ripemd160.cpp
    src/crypto/hash/ripemd160_sse.cpp)
target_link_libraries(keyhunt_cpu_crypto PUBLIC keyhunt_common)
keyhunt_configure_target(keyhunt_cpu_crypto)

add_executable(keyhunt src/app/keyhunt.cpp)
target_link_libraries(keyhunt PRIVATE keyhunt_cpu_crypto)
keyhunt_configure_target(keyhunt)

if(KEYHUNT_BUILD_BSGSD)
    add_executable(bsgsd legacy/bsgsd.cpp)
    target_link_libraries(bsgsd PRIVATE keyhunt_cpu_crypto)
    keyhunt_configure_target(bsgsd)
endif()

if(KEYHUNT_BUILD_LEGACY)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(GMP REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    add_library(keyhunt_legacy_crypto STATIC
        legacy/hashing.cpp
        legacy/gmp256k1/Int.cpp
        legacy/gmp256k1/IntMod.cpp
        legacy/gmp256k1/IntGroup.cpp
        legacy/gmp256k1/Point.cpp
        legacy/gmp256k1/Random.cpp
        legacy/gmp256k1/GMP256K1.cpp)
    target_link_libraries(keyhunt_legacy_crypto PUBLIC keyhunt_common GMP::GMP OpenSSL::Crypto)
    keyhunt_configure_target(keyhunt_legacy_crypto)
    add_executable(keyhunt_legacy legacy/keyhunt_legacy.cpp)
    set_target_properties(keyhunt_legacy PROPERTIES OUTPUT_NAME keyhunt-legacy)
    target_link_libraries(keyhunt_legacy PRIVATE keyhunt_legacy_crypto)
    keyhunt_configure_target(keyhunt_legacy)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_test(NAME cpu_baseline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/baseline/run_cpu_baseline.py"
        --binary $<TARGET_FILE:keyhunt>
        --report "${CMAKE_CURRENT_BINARY_DIR}/cpu-baseline-results.json")
    set_tests_properties(cpu_baseline PROPERTIES TIMEOUT 180 LABELS "cpu;baseline")
    if(KEYHUNT_BUILD_LEGACY)
        add_test(NAME legacy_xpoint_baseline COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/baseline/run_cpu_baseline.py"
            --binary $<TARGET_FILE:keyhunt_legacy> --case xpoint
            --report "${CMAKE_CURRENT_BINARY_DIR}/legacy-xpoint-results.json")
        set_tests_properties(legacy_xpoint_baseline PROPERTIES TIMEOUT 90 LABELS "cpu;legacy")
    endif()
    if(KEYHUNT_BUILD_BSGSD)
        add_test(NAME bsgsd_loopback COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgsd_smoke.py"
            --binary $<TARGET_FILE:bsgsd>
            --report "${CMAKE_CURRENT_BINARY_DIR}/bsgsd-results.json")
        set_tests_properties(bsgsd_loopback PROPERTIES TIMEOUT 90 LABELS "cpu;bsgsd"
            RESOURCE_LOCK bsgsd_loopback_port)
    endif()
endif()

install(TARGETS keyhunt RUNTIME DESTINATION bin)
if(KEYHUNT_BUILD_LEGACY)
    install(TARGETS keyhunt_legacy RUNTIME DESTINATION bin)
endif()
if(KEYHUNT_BUILD_BSGSD)
    install(TARGETS bsgsd RUNTIME DESTINATION bin)
endif()
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION share/doc/keyhunt)
install(DIRECTORY third_party/ DESTINATION share/doc/keyhunt/third_party
    FILES_MATCHING PATTERN "LICENSE*" PATTERN "COPYING*")
install(DIRECTORY third_party/licenses/ DESTINATION share/doc/keyhunt/licenses)
