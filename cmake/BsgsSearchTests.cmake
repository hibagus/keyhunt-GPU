add_executable(bsgs_search_test tests/unit/bsgs_search.cpp)
target_link_libraries(bsgs_search_test PRIVATE keyhunt_core)
keyhunt_configure_target(bsgs_search_test)
add_test(NAME bsgs_search_contract COMMAND bsgs_search_test)
set_tests_properties(bsgs_search_contract PROPERTIES TIMEOUT 60 LABELS "cpu;bsgs;core")

if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_bsgs_search_test tests/gpu/bsgs_search.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_bsgs_search_test PRIVATE keyhunt_gpu_options)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_bsgs_search_test PRIVATE keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_bsgs_search_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_bsgs_search COMMAND ${KEYHUNT_GPU_BACKEND}_bsgs_search_test)
    add_executable(${KEYHUNT_GPU_BACKEND}_bsgs_search_failure_test tests/gpu/bsgs_search_failures.hip src/backend/gpu/bsgs_search.cpp src/backend/gpu/bsgs_table.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_bsgs_search_failure_test PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_bsgs_search_failure_test PRIVATE kernels kernels/search src/backend/gpu)
    target_compile_definitions(${KEYHUNT_GPU_BACKEND}_bsgs_search_failure_test PRIVATE KEYHUNT_TEST_GPU_FAILURES=1)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_bsgs_search_failure_test PRIVATE keyhunt_core)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_bsgs_search_failure_test PROPERTIES LINKER_LANGUAGE CXX INTERPROCEDURAL_OPTIMIZATION OFF)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_bsgs_search_failures COMMAND ${KEYHUNT_GPU_BACKEND}_bsgs_search_failure_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_bsgs_search ${KEYHUNT_GPU_BACKEND}_bsgs_search_failures PROPERTIES TIMEOUT 120 LABELS "${KEYHUNT_GPU_BACKEND};hardware;bsgs" RESOURCE_LOCK gpu_device)
endif()
set(bsgs_search_args)
set(bsgs_cli_timeout 300)
if(KEYHUNT_ENABLE_CUDA)
    # This corpus launches about 100 fresh processes. On the validated CUDA
    # host, preparation/teardown costs about 3 seconds per process, independently
    # of kernel time. Keep each child's 90-second watchdog and all search cases.
    set(bsgs_cli_timeout 600)
endif()
if(KEYHUNT_ENABLE_GPU)
    list(APPEND bsgs_search_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME bsgs_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgs_cli.py" --binary $<TARGET_FILE:keyhunt>
    --oracle $<TARGET_FILE:secp256k1_oracle> --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-cli-results.json" ${bsgs_search_args})
set_tests_properties(bsgs_cli PROPERTIES TIMEOUT ${bsgs_cli_timeout} LABELS "cpu;backend;bsgs")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(bsgs_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;bsgs;oracle" RESOURCE_LOCK gpu_device)
    add_test(NAME bsgs_cli_single COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgs_cli.py" --binary $<TARGET_FILE:keyhunt>
        --oracle $<TARGET_FILE:secp256k1_oracle> --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-cli-single-results.json" --hardware --backend ${KEYHUNT_GPU_BACKEND} --group 1)
    set_tests_properties(bsgs_cli_single PROPERTIES TIMEOUT ${bsgs_cli_timeout} LABELS "${KEYHUNT_GPU_BACKEND};hardware;bsgs;oracle" RESOURCE_LOCK gpu_device)
endif()
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_bsgs_search_benchmark tests/gpu/bsgs_search_benchmark.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_bsgs_search_benchmark PRIVATE keyhunt_gpu_options)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_bsgs_search_benchmark PRIVATE keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_bsgs_search_benchmark PROPERTIES LINKER_LANGUAGE CXX)
endif()

if(KEYHUNT_ENABLE_GPU)
    add_test(NAME bsgs_cli_grouped COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgs_cli.py" --binary $<TARGET_FILE:keyhunt>
        --oracle $<TARGET_FILE:secp256k1_oracle> --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-cli-grouped-results.json" --hardware --backend ${KEYHUNT_GPU_BACKEND} --group 8)
    set_tests_properties(bsgs_cli_grouped PROPERTIES TIMEOUT ${bsgs_cli_timeout} LABELS "${KEYHUNT_GPU_BACKEND};hardware;bsgs;oracle" RESOURCE_LOCK gpu_device)
endif()

add_executable(bsgs_tile_probe tests/unit/bsgs_tile_probe.cpp)
target_link_libraries(bsgs_tile_probe PRIVATE keyhunt_core)
keyhunt_configure_target(bsgs_tile_probe)
add_test(NAME bsgs_reverse_tile_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/bsgs_tiles.py" --binary $<TARGET_FILE:bsgs_tile_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-reverse-tile-oracle.json")
set_tests_properties(bsgs_reverse_tile_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;bsgs;reverse;oracle")

add_test(NAME bsgs_reverse_search_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgs_reverse_cli.py" --binary $<TARGET_FILE:keyhunt>
    --oracle $<TARGET_FILE:secp256k1_oracle> --suite search
    --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-reverse-search-cli.json" ${bsgs_search_args})
set_tests_properties(bsgs_reverse_search_cli PROPERTIES TIMEOUT 600 LABELS "cpu;bsgs;reverse;oracle")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(bsgs_reverse_search_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;bsgs;reverse;oracle" RESOURCE_LOCK gpu_device)
endif()

set(bsgs_reverse_example_backend cpu)
if(KEYHUNT_ENABLE_GPU)
    set(bsgs_reverse_example_backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME bsgs_reverse_documented_example COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgs_reverse_examples.py"
    --binary $<TARGET_FILE:keyhunt> --backend ${bsgs_reverse_example_backend}
    --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-reverse-documented-example.json")
set_tests_properties(bsgs_reverse_documented_example PROPERTIES TIMEOUT 150 LABELS "cpu;bsgs;reverse;examples")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(bsgs_reverse_documented_example PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;bsgs;reverse;examples" RESOURCE_LOCK gpu_device)
endif()

add_executable(bsgs_plan_probe tests/unit/bsgs_plan_probe.cpp)
target_link_libraries(bsgs_plan_probe PRIVATE keyhunt_core)
keyhunt_configure_target(bsgs_plan_probe)
add_test(NAME bsgs_both_ends_plan_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/bsgs_plans.py" --binary $<TARGET_FILE:bsgs_plan_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-both-ends-plan-oracle.json")
set_tests_properties(bsgs_both_ends_plan_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;bsgs;both-ends;oracle")

add_test(NAME bsgs_both_ends_search_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgs_reverse_cli.py" --binary $<TARGET_FILE:keyhunt>
    --oracle $<TARGET_FILE:secp256k1_oracle> --suite search --both-ends
    --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-both-ends-search-cli.json" ${bsgs_search_args})
set_tests_properties(bsgs_both_ends_search_cli PROPERTIES TIMEOUT 600 LABELS "cpu;bsgs;both-ends;oracle")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(bsgs_both_ends_search_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;bsgs;both-ends;oracle" RESOURCE_LOCK gpu_device)
endif()
