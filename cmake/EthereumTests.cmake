add_executable(ethereum_search_test tests/unit/ethereum_search.cpp)
target_link_libraries(ethereum_search_test PRIVATE keyhunt_core)
keyhunt_configure_target(ethereum_search_test)
add_test(NAME ethereum_search_contract COMMAND ethereum_search_test)
set_tests_properties(ethereum_search_contract PROPERTIES TIMEOUT 30 LABELS "cpu;core;ethereum")

add_executable(portable_keccak_probe tests/gpu/keccak_probe.cpp)
target_include_directories(portable_keccak_probe PRIVATE kernels)
keyhunt_configure_target(portable_keccak_probe)
set(keccak_backends portable)
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_keccak_probe tests/gpu/keccak_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_keccak_probe PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_keccak_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_keccak_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    list(APPEND keccak_backends ${KEYHUNT_GPU_BACKEND})
endif()
foreach(backend IN LISTS keccak_backends)
    add_test(NAME ${backend}_keccak_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/keccak_oracle.py"
        --binary $<TARGET_FILE:${backend}_keccak_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-keccak-oracle.json")
    set_tests_properties(${backend}_keccak_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;keccak;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_keccak_oracle PROPERTIES LABELS "${backend};hardware;keccak;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()

if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_ethereum_executor_test tests/gpu/ethereum_executor.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_ethereum_executor_test PRIVATE keyhunt_gpu_options keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_ethereum_executor_test PROPERTIES LINKER_LANGUAGE CXX)
    add_executable(${KEYHUNT_GPU_BACKEND}_ethereum_failure_test tests/gpu/ethereum_failures.hip src/backend/gpu/ethereum.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_ethereum_failure_test PRIVATE keyhunt_gpu_options keyhunt_core)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_ethereum_failure_test PRIVATE kernels kernels/search src/backend/gpu)
    target_compile_definitions(${KEYHUNT_GPU_BACKEND}_ethereum_failure_test PRIVATE KEYHUNT_TEST_GPU_FAILURES=1)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_ethereum_failure_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_ethereum_executor COMMAND ${KEYHUNT_GPU_BACKEND}_ethereum_executor_test)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_ethereum_failures COMMAND ${KEYHUNT_GPU_BACKEND}_ethereum_failure_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_ethereum_executor ${KEYHUNT_GPU_BACKEND}_ethereum_failures
        PROPERTIES TIMEOUT 180 LABELS "${KEYHUNT_GPU_BACKEND};hardware;ethereum" RESOURCE_LOCK gpu_device)
endif()

set(ethereum_cli_args)
if(KEYHUNT_ENABLE_GPU)
    list(APPEND ethereum_cli_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME ethereum_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/ethereum_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/ethereum-cli-results.json" ${ethereum_cli_args})
set_tests_properties(ethereum_cli PROPERTIES TIMEOUT 300 LABELS "cpu;ethereum")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(ethereum_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;ethereum;oracle" RESOURCE_LOCK gpu_device)
    add_test(NAME ethereum_cli_direct COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/ethereum_cli.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --report "${CMAKE_CURRENT_BINARY_DIR}/ethereum-cli-direct-results.json" ${ethereum_cli_args} --kernel direct)
    set_tests_properties(ethereum_cli_direct PROPERTIES TIMEOUT 300 LABELS "${KEYHUNT_GPU_BACKEND};hardware;ethereum;oracle" RESOURCE_LOCK gpu_device)
endif()
