add_executable(vanity_search_test tests/unit/vanity_search.cpp)
target_link_libraries(vanity_search_test PRIVATE keyhunt_core)
keyhunt_configure_target(vanity_search_test)
add_test(NAME vanity_search_contract COMMAND vanity_search_test)
set_tests_properties(vanity_search_contract PROPERTIES TIMEOUT 30 LABELS "cpu;core;vanity")

add_executable(portable_base58_probe tests/gpu/base58_probe.cpp)
target_include_directories(portable_base58_probe PRIVATE kernels)
keyhunt_configure_target(portable_base58_probe)
set(base58_backends portable)
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_base58_probe tests/gpu/base58_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_base58_probe PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_base58_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_base58_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    list(APPEND base58_backends ${KEYHUNT_GPU_BACKEND})
endif()
foreach(backend IN LISTS base58_backends)
    add_test(NAME ${backend}_base58_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/base58_oracle.py"
        --binary $<TARGET_FILE:${backend}_base58_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-base58-oracle.json")
    set_tests_properties(${backend}_base58_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;base58;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_base58_oracle PROPERTIES LABELS "${backend};hardware;base58;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()


if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_vanity_executor_test tests/gpu/vanity_executor.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_vanity_executor_test PRIVATE keyhunt_gpu_options keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_vanity_executor_test PROPERTIES LINKER_LANGUAGE CXX)
    add_executable(${KEYHUNT_GPU_BACKEND}_vanity_failure_test tests/gpu/vanity_failures.hip src/backend/gpu/vanity.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_vanity_failure_test PRIVATE keyhunt_gpu_options keyhunt_core)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_vanity_failure_test PRIVATE kernels kernels/search src/backend/gpu)
    target_compile_definitions(${KEYHUNT_GPU_BACKEND}_vanity_failure_test PRIVATE KEYHUNT_TEST_GPU_FAILURES=1)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_vanity_failure_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_vanity_executor COMMAND ${KEYHUNT_GPU_BACKEND}_vanity_executor_test)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_vanity_failures COMMAND ${KEYHUNT_GPU_BACKEND}_vanity_failure_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_vanity_executor ${KEYHUNT_GPU_BACKEND}_vanity_failures
        PROPERTIES TIMEOUT 180 LABELS "${KEYHUNT_GPU_BACKEND};hardware;vanity" RESOURCE_LOCK gpu_device)
endif()

set(vanity_cli_args)
if(KEYHUNT_ENABLE_GPU)
    list(APPEND vanity_cli_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME vanity_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/vanity_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/vanity-cli-results.json" ${vanity_cli_args})
set_tests_properties(vanity_cli PROPERTIES TIMEOUT 300 LABELS "cpu;vanity")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(vanity_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;vanity;oracle" RESOURCE_LOCK gpu_device)
    add_test(NAME vanity_cli_direct COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/vanity_cli.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --report "${CMAKE_CURRENT_BINARY_DIR}/vanity-cli-direct-results.json" ${vanity_cli_args} --kernel direct)
    set_tests_properties(vanity_cli_direct PROPERTIES TIMEOUT 300 LABELS "${KEYHUNT_GPU_BACKEND};hardware;vanity;oracle" RESOURCE_LOCK gpu_device)
endif()
