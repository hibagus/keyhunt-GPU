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
option(KEYHUNT_ENABLE_HIP "Build AMD HIP discovery, diagnostics and bounded xpoint search" OFF)
option(KEYHUNT_ENABLE_CUDA "Build the native NVIDIA CUDA backend" OFF)
option(KEYHUNT_GFX942_CARRY "Opt in to validated gfx942 field carry/borrow chains" OFF)
if(KEYHUNT_GFX942_CARRY AND NOT KEYHUNT_ENABLE_HIP)
    message(FATAL_ERROR "KEYHUNT_GFX942_CARRY requires the native AMD HIP backend.")
endif()
if(KEYHUNT_ENABLE_HIP AND KEYHUNT_ENABLE_CUDA)
    message(FATAL_ERROR "Select one native GPU backend per build directory: HIP or CUDA.")
endif()
set(KEYHUNT_ENABLE_GPU OFF)
set(KEYHUNT_GPU_BACKEND hip) # CPU tests exercise explicit unavailable-HIP errors.
if(KEYHUNT_ENABLE_HIP OR KEYHUNT_ENABLE_CUDA)
    if(KEYHUNT_ENABLE_SANITIZERS)
        message(FATAL_ERROR "Use a separate CPU sanitizer build; GPU sanitizer flags are not supported.")
    endif()
    set(KEYHUNT_ENABLE_GPU ON)
    if(KEYHUNT_ENABLE_CUDA)
        set(KEYHUNT_GPU_BACKEND cuda)
        set(KEYHUNT_GPU_LANGUAGE CUDA)
    else()
        set(KEYHUNT_GPU_LANGUAGE HIP)
    endif()
    include(CheckLanguage)
    check_language(${KEYHUNT_GPU_LANGUAGE})
    if(NOT CMAKE_${KEYHUNT_GPU_LANGUAGE}_COMPILER)
        message(FATAL_ERROR "${KEYHUNT_GPU_LANGUAGE} compiler not found. Set CUDACXX to nvcc or HIPCXX to ROCm clang++.")
    endif()
    enable_language(${KEYHUNT_GPU_LANGUAGE})
    set(CMAKE_${KEYHUNT_GPU_LANGUAGE}_STANDARD 17)
    set(CMAKE_${KEYHUNT_GPU_LANGUAGE}_STANDARD_REQUIRED ON)
    if(KEYHUNT_ENABLE_CUDA)
        find_package(CUDAToolkit REQUIRED)
    endif()
    # These tests deliberately compile identical kernels with each native SDK.
    file(GLOB gpu_test_sources CONFIGURE_DEPENDS tests/gpu/*.hip)
    set_source_files_properties(${gpu_test_sources} PROPERTIES LANGUAGE ${KEYHUNT_GPU_LANGUAGE})
endif()
# Keep device compilation free of x86 flags, LTO, and floating-point shortcuts.
add_library(keyhunt_gpu_options INTERFACE)
target_include_directories(keyhunt_gpu_options INTERFACE kernels kernels/search src/backend/gpu)
if(KEYHUNT_ENABLE_GPU)
    target_compile_definitions(keyhunt_gpu_options INTERFACE KEYHUNT_HAS_GPU=1 KEYHUNT_HAS_${KEYHUNT_GPU_LANGUAGE}=1)
    target_compile_options(keyhunt_gpu_options INTERFACE
        "$<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>"
        "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra>"
        "$<$<COMPILE_LANGUAGE:HIP>:-Wall;-Wextra>")
    if(KEYHUNT_ENABLE_CUDA)
        target_link_libraries(keyhunt_gpu_options INTERFACE CUDA::cudart)
    endif()
    if(KEYHUNT_GFX942_CARRY)
        if(NOT CMAKE_HIP_COMPILER_ID STREQUAL "Clang" OR
           NOT "${CMAKE_HIP_ARCHITECTURES}" MATCHES "(^|;)gfx942(:[^;]+)?(;|$)")
            message(FATAL_ERROR "KEYHUNT_GFX942_CARRY requires AMD Clang and a gfx942 code object.")
        endif()
        include(cmake/CheckGfx942Carry.cmake)
        keyhunt_check_gfx942_carry()
        # Other architectures in a multi-architecture HIP build retain portable
        # arithmetic. Host sources never receive the device specialization flag.
        target_compile_definitions(keyhunt_gpu_options INTERFACE
            "$<$<COMPILE_LANGUAGE:HIP>:KEYHUNT_GFX942_CARRY=1>")
    endif()
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

# Exact scheduling arithmetic is independent of the legacy curve engine.
add_library(keyhunt_ranges STATIC
    src/core/scalar_stride.cpp
    src/core/exact_range.cpp
    src/scheduler/block_grid.cpp
    src/scheduler/work_unit.cpp)
target_include_directories(keyhunt_ranges PUBLIC include)
keyhunt_configure_target(keyhunt_ranges)

add_library(keyhunt_core STATIC
    src/core/config.cpp
    src/core/cpu_targets.cpp
    src/core/result_verifier.cpp
    src/core/xpoint_search.cpp
    src/core/minikey_search.cpp
    src/core/hash160_search.cpp
    src/core/ethereum_search.cpp
    src/core/vanity_search.cpp
    src/core/bsgs_table.cpp
    src/core/bsgs_search.cpp)
target_include_directories(keyhunt_core PRIVATE kernels)
target_link_libraries(keyhunt_core PUBLIC keyhunt_cpu_crypto keyhunt_ranges)
keyhunt_configure_target(keyhunt_core)

# Local durable state is CPU-only and independent of either GPU SDK. Require
# the upstream WAL-reset fix for concurrent writers/checkpointers.
find_package(SQLite3 3.51.3 REQUIRED)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v1.sql" KEYHUNT_SCHEMA_V1)
configure_file(src/storage/schema_v1.h.in generated/schema_v1.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v2.sql" KEYHUNT_SCHEMA_V2)
configure_file(src/storage/schema_v2.h.in generated/schema_v2.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v3.sql" KEYHUNT_SCHEMA_V3)
configure_file(src/storage/schema_v3.h.in generated/schema_v3.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v4.sql" KEYHUNT_SCHEMA_V4)
configure_file(src/storage/schema_v4.h.in generated/schema_v4.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v5.sql" KEYHUNT_SCHEMA_V5)
configure_file(src/storage/schema_v5.h.in generated/schema_v5.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v6.sql" KEYHUNT_SCHEMA_V6)
configure_file(src/storage/schema_v6.h.in generated/schema_v6.h @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/schema_v7.sql" KEYHUNT_SCHEMA_V7)
configure_file(src/storage/schema_v7.h.in generated/schema_v7.h @ONLY)
add_library(keyhunt_storage STATIC src/storage/sqlite.cpp src/storage/free_tree.cpp src/storage/journal.cpp src/storage/checkpoint_data.cpp src/storage/checkpoint.cpp)
target_include_directories(keyhunt_storage PUBLIC include PRIVATE src/storage "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(keyhunt_storage PRIVATE KEYHUNT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(keyhunt_storage PUBLIC keyhunt_core SQLite::SQLite3)
keyhunt_configure_target(keyhunt_storage)

set(gpu_executor_sources src/backend/gpu/executor.cpp src/backend/gpu/xpoint.cpp src/backend/gpu/hash160.cpp src/backend/gpu/ethereum.cpp src/backend/gpu/vanity.cpp src/backend/gpu/minikeys.cpp
    src/backend/gpu/bsgs_table.cpp src/backend/gpu/bsgs_search.cpp)
add_library(keyhunt_backend STATIC src/backend/availability.cpp)
if(KEYHUNT_ENABLE_GPU)
    set_source_files_properties(${gpu_executor_sources} PROPERTIES LANGUAGE ${KEYHUNT_GPU_LANGUAGE})
    target_sources(keyhunt_backend PRIVATE ${gpu_executor_sources})
    if(KEYHUNT_ENABLE_CUDA)
        target_sources(keyhunt_backend PRIVATE src/backend/cuda/discovery.cu)
    else()
        target_sources(keyhunt_backend PRIVATE src/backend/hip/discovery.hip)
    endif()
    target_link_libraries(keyhunt_backend PUBLIC keyhunt_core keyhunt_gpu_options)
    set_target_properties(keyhunt_backend PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
endif()
target_include_directories(keyhunt_backend PUBLIC include)
add_library(keyhunt_backend_commands STATIC src/backend/commands.cpp src/backend/state_command.cpp src/backend/checkpoint_command.cpp src/backend/checkpoint_control.cpp src/backend/xpoint_command.cpp src/backend/hash160_command.cpp src/backend/ethereum_command.cpp src/backend/vanity_command.cpp src/backend/minikeys_command.cpp src/backend/bsgs_table_command.cpp src/backend/bsgs_command.cpp)
target_link_libraries(keyhunt_backend_commands PUBLIC keyhunt_backend keyhunt_core keyhunt_storage)
target_compile_options(keyhunt_backend_commands PRIVATE -Wall -Wextra)
if(KEYHUNT_ENABLE_SANITIZERS)
    target_compile_options(keyhunt_backend_commands PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

option(KEYHUNT_ENABLE_COORDINATOR "Build authenticated coordinator and HTTPS worker support" OFF)
option(KEYHUNT_ENABLE_HTTPS_WORKER "Build HTTPS worker executable and supervisor" ON)
if(KEYHUNT_ENABLE_COORDINATOR)
    find_package(OpenSSL 3.0 REQUIRED)
    find_package(nlohmann_json 3.10 REQUIRED)
    add_library(keyhunt_coordination STATIC src/coordinator/certificate.cpp src/coordinator/repository.cpp src/coordinator/worker.cpp src/coordinator/offline.cpp)
    target_include_directories(keyhunt_coordination PUBLIC include PRIVATE src/coordinator src/storage)
    target_link_libraries(keyhunt_coordination PUBLIC keyhunt_storage nlohmann_json::nlohmann_json OpenSSL::Crypto)
    keyhunt_configure_target(keyhunt_coordination)
    add_executable(keyhunt-coordinator src/coordinator/main.cpp src/coordinator/http.cpp)
    target_include_directories(keyhunt-coordinator PRIVATE src/coordinator src/storage)
    target_link_libraries(keyhunt-coordinator PRIVATE keyhunt_coordination)
    keyhunt_configure_target(keyhunt-coordinator)
    install(TARGETS keyhunt-coordinator RUNTIME DESTINATION bin COMPONENT coordinator)
    install(FILES deploy/apache/keyhunt.conf.in DESTINATION share/keyhunt/apache COMPONENT coordinator)
    install(FILES deploy/systemd/keyhunt-coordinator.service.in DESTINATION share/keyhunt/systemd COMPONENT coordinator)
    install(FILES docs/COORDINATOR.md docs/COORDINATOR_SERVER_PLAN.md docs/COORDINATOR_VALIDATION.md
        docs/STORAGE.md docs/BUILD.md docs/GPU_REDESIGN_PLAN.md docs/MULTI_GPU.md LICENSE THIRD_PARTY_NOTICES.md
        DESTINATION share/doc/keyhunt COMPONENT coordinator)
    install(DIRECTORY third_party/licenses/ DESTINATION share/doc/keyhunt/licenses COMPONENT coordinator)
    if(KEYHUNT_ENABLE_HTTPS_WORKER)
        find_package(CURL REQUIRED)
        add_library(keyhunt_https STATIC src/coordinator/https.cpp)
        target_include_directories(keyhunt_https PRIVATE src/coordinator src/storage)
        target_link_libraries(keyhunt_https PUBLIC keyhunt_coordination CURL::libcurl)
        keyhunt_configure_target(keyhunt_https)
        add_executable(keyhunt-worker src/coordinator/worker_main.cpp src/coordinator/self_test.cpp src/coordinator/device_worker.cpp)
        set_target_properties(keyhunt-worker PROPERTIES LINKER_LANGUAGE CXX)
        target_include_directories(keyhunt-worker PRIVATE src/coordinator src/storage src/backend)
        target_link_libraries(keyhunt-worker PRIVATE keyhunt_https keyhunt_backend_commands)
        keyhunt_configure_target(keyhunt-worker)
        install(TARGETS keyhunt-worker RUNTIME DESTINATION bin)
        install(PROGRAMS tools/coordinator_worker.py DESTINATION bin RENAME keyhunt-supervise)
        install(PROGRAMS tools/calibrate_blocks.py DESTINATION bin RENAME keyhunt-calibrate-blocks)
    endif()
endif()

add_executable(keyhunt src/app/keyhunt.cpp)
set_target_properties(keyhunt PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(keyhunt PRIVATE keyhunt_core keyhunt_backend_commands)
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
    # Benchmark acceptance logic runs in CPU CI; throughput runs stay opt-in.
    add_test(NAME gpu_benchmark_metrics COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/gpu_metrics.py")
    set_tests_properties(gpu_benchmark_metrics PROPERTIES TIMEOUT 30 LABELS "cpu;benchmark")
    add_test(NAME gpu_benchmark_pairs COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/gpu_pair.py")
    set_tests_properties(gpu_benchmark_pairs PROPERTIES TIMEOUT 30 LABELS "cpu;benchmark")
    include(cmake/OracleTests.cmake)
    include(cmake/GpuArithmeticTests.cmake)
    include(cmake/XPointTests.cmake)
    include(cmake/Hash160Tests.cmake)
    include(cmake/EthereumTests.cmake)
    include(cmake/VanityTests.cmake)
    include(cmake/MinikeyTests.cmake)
    include(cmake/StrideTests.cmake)
    include(cmake/BsgsTableTests.cmake)
    include(cmake/BsgsSearchTests.cmake)
    include(cmake/StorageTests.cmake)
    if(KEYHUNT_ENABLE_COORDINATOR)
        include(cmake/CoordinatorTests.cmake)
    endif()
    # This discovery contract test uses a private, minimal API double. It needs
    # no GPU SDK; actual HIP compilation and hardware tests remain separate.
    add_executable(hip_discovery_test tests/unit/hip_discovery.cpp)
    target_include_directories(hip_discovery_test PRIVATE tests/gpu/fakes include)
    keyhunt_configure_target(hip_discovery_test)
    add_test(NAME hip_discovery_contract COMMAND hip_discovery_test)
    set_tests_properties(hip_discovery_contract PROPERTIES TIMEOUT 30 LABELS "cpu;backend")
    if(KEYHUNT_ENABLE_GPU)
        add_executable(${KEYHUNT_GPU_BACKEND}_executor_test tests/gpu/hip_executor.cpp)
        target_link_libraries(${KEYHUNT_GPU_BACKEND}_executor_test PRIVATE keyhunt_gpu_options)
        target_link_libraries(${KEYHUNT_GPU_BACKEND}_executor_test PRIVATE keyhunt_backend)
        set_target_properties(${KEYHUNT_GPU_BACKEND}_executor_test PROPERTIES LINKER_LANGUAGE CXX)
        # A separate binary injects failures; production libraries never contain
        # the hook. Real HIP allocations/queues still exercise cleanup paths.
        add_executable(${KEYHUNT_GPU_BACKEND}_failure_test tests/gpu/hip_failures.hip src/backend/gpu/executor.cpp)
        target_link_libraries(${KEYHUNT_GPU_BACKEND}_failure_test PRIVATE keyhunt_gpu_options)
        target_include_directories(${KEYHUNT_GPU_BACKEND}_failure_test PRIVATE kernels/search src/backend/gpu)
        target_compile_definitions(${KEYHUNT_GPU_BACKEND}_failure_test PRIVATE KEYHUNT_TEST_GPU_FAILURES=1)
        target_link_libraries(${KEYHUNT_GPU_BACKEND}_failure_test PRIVATE keyhunt_ranges)
        set_target_properties(${KEYHUNT_GPU_BACKEND}_failure_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
        add_test(NAME ${KEYHUNT_GPU_BACKEND}_failures COMMAND ${KEYHUNT_GPU_BACKEND}_failure_test)
        add_test(NAME ${KEYHUNT_GPU_BACKEND}_executor COMMAND ${KEYHUNT_GPU_BACKEND}_executor_test)
        set_tests_properties(${KEYHUNT_GPU_BACKEND}_executor ${KEYHUNT_GPU_BACKEND}_failures PROPERTIES TIMEOUT 90 LABELS "${KEYHUNT_GPU_BACKEND};hardware"
            RESOURCE_LOCK gpu_device)
    endif()
    set(backend_cli_args)
    if(KEYHUNT_ENABLE_GPU)
        list(APPEND backend_cli_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
    endif()
    add_test(NAME backend_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/backend_cli.py"
        --binary $<TARGET_FILE:keyhunt> ${backend_cli_args})
    set_tests_properties(backend_cli PROPERTIES TIMEOUT 90 LABELS "cpu;backend")
    if(KEYHUNT_ENABLE_GPU)
        set_tests_properties(backend_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;backend" RESOURCE_LOCK gpu_device)
    endif()
    if(KEYHUNT_ENABLE_HIP)
        add_test(NAME hip_partitions COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/hip_partitions.py"
            --binary $<TARGET_FILE:keyhunt>
            --report "${CMAKE_CURRENT_BINARY_DIR}/hip-partition-results.json")
        set_tests_properties(hip_partitions PROPERTIES TIMEOUT 300 LABELS "${KEYHUNT_GPU_BACKEND};hardware;partitions"
            RESOURCE_LOCK gpu_device)
    endif()
    add_executable(exact_range_test tests/unit/exact_range.cpp)
    target_link_libraries(exact_range_test PRIVATE keyhunt_ranges)
    keyhunt_configure_target(exact_range_test)
    add_test(NAME exact_range COMMAND exact_range_test)
    set_tests_properties(exact_range PROPERTIES TIMEOUT 30 LABELS "cpu;core;ranges")
    add_executable(block_grid_test tests/unit/block_grid.cpp)
    target_link_libraries(block_grid_test PRIVATE keyhunt_ranges)
    keyhunt_configure_target(block_grid_test)
    add_test(NAME block_grid COMMAND block_grid_test)
    set_tests_properties(block_grid PROPERTIES TIMEOUT 30 LABELS "cpu;core;ranges")
    add_executable(work_unit_test tests/unit/work_unit.cpp)
    target_link_libraries(work_unit_test PRIVATE keyhunt_ranges)
    keyhunt_configure_target(work_unit_test)
    add_test(NAME work_unit COMMAND work_unit_test)
    set_tests_properties(work_unit PROPERTIES TIMEOUT 30 LABELS "cpu;core;ranges")
    add_executable(range_probe tests/unit/range_probe.cpp)
    target_link_libraries(range_probe PRIVATE keyhunt_ranges)
    keyhunt_configure_target(range_probe)
    add_test(NAME range_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/range_oracle.py"
        --binary $<TARGET_FILE:range_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/range-oracle-results.json")
    set_tests_properties(range_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;core;ranges")
    add_executable(result_verifier_test tests/unit/result_verifier.cpp)
    target_link_libraries(result_verifier_test PRIVATE keyhunt_core)
    keyhunt_configure_target(result_verifier_test)
    add_test(NAME result_verifier COMMAND result_verifier_test)
    set_tests_properties(result_verifier PROPERTIES TIMEOUT 30 LABELS "cpu;core")
    add_test(NAME cpu_baseline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/baseline/run_cpu_baseline.py"
        --binary $<TARGET_FILE:keyhunt>
        --report "${CMAKE_CURRENT_BINARY_DIR}/cpu-baseline-results.json")
    set_tests_properties(cpu_baseline PROPERTIES TIMEOUT 180 LABELS "cpu;baseline")
    add_test(NAME target_loading COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/target_loading.py"
        --binary $<TARGET_FILE:keyhunt>
        --report "${CMAKE_CURRENT_BINARY_DIR}/target-loading-results.json")
    set_tests_properties(target_loading PROPERTIES TIMEOUT 90 LABELS "cpu;core")
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
