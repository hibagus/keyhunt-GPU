add_executable(xpoint_search_test tests/unit/xpoint_search.cpp)
target_link_libraries(xpoint_search_test PRIVATE keyhunt_core)
keyhunt_configure_target(xpoint_search_test)
add_test(NAME xpoint_search_contract COMMAND xpoint_search_test)
set_tests_properties(xpoint_search_contract PROPERTIES TIMEOUT 30 LABELS "cpu;core;xpoint")
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_xpoint_executor_test tests/gpu/xpoint_executor.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_xpoint_executor_test PRIVATE keyhunt_gpu_options)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_xpoint_executor_test PRIVATE keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_xpoint_executor_test PROPERTIES LINKER_LANGUAGE CXX)
    add_executable(${KEYHUNT_GPU_BACKEND}_xpoint_failure_test tests/gpu/xpoint_failures.hip src/backend/gpu/xpoint.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_xpoint_failure_test PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_xpoint_failure_test PRIVATE kernels kernels/search src/backend/gpu)
    target_compile_definitions(${KEYHUNT_GPU_BACKEND}_xpoint_failure_test PRIVATE KEYHUNT_TEST_GPU_FAILURES=1)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_xpoint_failure_test PRIVATE keyhunt_core)
    # The preserved CPU archives contain GCC LTO objects. Link with the host C++
    # driver, as the application does, while retaining HIP compilation for kernels.
    set_target_properties(${KEYHUNT_GPU_BACKEND}_xpoint_failure_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_xpoint_failures COMMAND ${KEYHUNT_GPU_BACKEND}_xpoint_failure_test)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_xpoint_executor COMMAND ${KEYHUNT_GPU_BACKEND}_xpoint_executor_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_xpoint_executor ${KEYHUNT_GPU_BACKEND}_xpoint_failures PROPERTIES TIMEOUT 120 LABELS "${KEYHUNT_GPU_BACKEND};hardware;xpoint" RESOURCE_LOCK gpu_device)
endif()
set(xpoint_cli_args)
if(KEYHUNT_ENABLE_GPU)
    list(APPEND xpoint_cli_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME xpoint_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/xpoint_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/xpoint-cli-results.json" ${xpoint_cli_args})
set_tests_properties(xpoint_cli PROPERTIES TIMEOUT 300 LABELS "cpu;backend;xpoint")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(xpoint_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;xpoint;oracle" RESOURCE_LOCK gpu_device)
endif()

if(KEYHUNT_ENABLE_GPU)
    add_test(NAME xpoint_cli_direct COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/xpoint_cli.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --report "${CMAKE_CURRENT_BINARY_DIR}/xpoint-cli-direct-results.json" --hardware --backend ${KEYHUNT_GPU_BACKEND} --kernel direct)
    set_tests_properties(xpoint_cli_direct PROPERTIES TIMEOUT 300
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;xpoint;oracle" RESOURCE_LOCK gpu_device)
    add_executable(${KEYHUNT_GPU_BACKEND}_xpoint_benchmark tests/gpu/xpoint_benchmark.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_xpoint_benchmark PRIVATE keyhunt_gpu_options)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_xpoint_benchmark PRIVATE keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_xpoint_benchmark PROPERTIES LINKER_LANGUAGE CXX)
endif()
