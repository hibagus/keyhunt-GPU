add_executable(hash160_search_test tests/unit/hash160_search.cpp)
target_link_libraries(hash160_search_test PRIVATE keyhunt_core)
keyhunt_configure_target(hash160_search_test)
add_test(NAME hash160_search_contract COMMAND hash160_search_test)
set_tests_properties(hash160_search_contract PROPERTIES TIMEOUT 30 LABELS "cpu;core;hash160")

add_executable(portable_hash160_probe tests/gpu/hash160_probe.cpp)
target_include_directories(portable_hash160_probe PRIVATE kernels)
keyhunt_configure_target(portable_hash160_probe)
set(hash160_backends portable)
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_hash160_probe tests/gpu/hash160_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_hash160_probe PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_hash160_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_hash160_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    list(APPEND hash160_backends ${KEYHUNT_GPU_BACKEND})
endif()
foreach(backend IN LISTS hash160_backends)
    add_test(NAME ${backend}_hash160_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/hash160_oracle.py"
        --binary $<TARGET_FILE:${backend}_hash160_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-hash160-oracle.json")
    set_tests_properties(${backend}_hash160_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;hash160;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_hash160_oracle PROPERTIES LABELS "${backend};hardware;hash160;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()

if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_hash160_executor_test tests/gpu/hash160_executor.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_hash160_executor_test PRIVATE keyhunt_gpu_options keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_hash160_executor_test PROPERTIES LINKER_LANGUAGE CXX)
    add_executable(${KEYHUNT_GPU_BACKEND}_hash160_failure_test tests/gpu/hash160_failures.hip src/backend/gpu/hash160.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_hash160_failure_test PRIVATE keyhunt_gpu_options keyhunt_core)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_hash160_failure_test PRIVATE kernels kernels/search src/backend/gpu)
    target_compile_definitions(${KEYHUNT_GPU_BACKEND}_hash160_failure_test PRIVATE KEYHUNT_TEST_GPU_FAILURES=1)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_hash160_failure_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_hash160_executor COMMAND ${KEYHUNT_GPU_BACKEND}_hash160_executor_test)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_hash160_failures COMMAND ${KEYHUNT_GPU_BACKEND}_hash160_failure_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_hash160_executor ${KEYHUNT_GPU_BACKEND}_hash160_failures
        PROPERTIES TIMEOUT 180 LABELS "${KEYHUNT_GPU_BACKEND};hardware;hash160" RESOURCE_LOCK gpu_device)
endif()

set(hash160_cli_args)
if(KEYHUNT_ENABLE_GPU)
    list(APPEND hash160_cli_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME hash160_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/hash160_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/hash160-cli-results.json" ${hash160_cli_args})
set_tests_properties(hash160_cli PROPERTIES TIMEOUT 300 LABELS "cpu;hash160")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(hash160_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;hash160;oracle" RESOURCE_LOCK gpu_device)
    add_test(NAME hash160_cli_direct COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/hash160_cli.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --report "${CMAKE_CURRENT_BINARY_DIR}/hash160-cli-direct-results.json" ${hash160_cli_args} --kernel direct)
    set_tests_properties(hash160_cli_direct PROPERTIES TIMEOUT 300 LABELS "${KEYHUNT_GPU_BACKEND};hardware;hash160;oracle" RESOURCE_LOCK gpu_device)
endif()
