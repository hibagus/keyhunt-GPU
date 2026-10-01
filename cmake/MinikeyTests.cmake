add_executable(minikey_search_test tests/unit/minikey_search.cpp)
target_link_libraries(minikey_search_test PRIVATE keyhunt_core)
keyhunt_configure_target(minikey_search_test)
add_test(NAME minikey_search_contract COMMAND minikey_search_test)
set_tests_properties(minikey_search_contract PROPERTIES TIMEOUT 60 LABELS "cpu;core;minikeys")

add_executable(portable_minikey_probe tests/gpu/minikey_probe.cpp)
target_include_directories(portable_minikey_probe PRIVATE kernels)
keyhunt_configure_target(portable_minikey_probe)
set(minikey_backends portable)
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_minikey_probe tests/gpu/minikey_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_minikey_probe PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_minikey_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_minikey_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    list(APPEND minikey_backends ${KEYHUNT_GPU_BACKEND})
endif()
foreach(backend IN LISTS minikey_backends)
    add_test(NAME ${backend}_minikey_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/minikey_oracle.py"
        --binary $<TARGET_FILE:${backend}_minikey_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-minikey-oracle.json")
    set_tests_properties(${backend}_minikey_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;minikey;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_minikey_oracle PROPERTIES LABELS "${backend};hardware;minikey;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()

if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_minikeys_executor_test tests/gpu/minikeys_executor.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_minikeys_executor_test PRIVATE keyhunt_gpu_options keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_minikeys_executor_test PROPERTIES LINKER_LANGUAGE CXX)
    add_executable(${KEYHUNT_GPU_BACKEND}_minikeys_failure_test tests/gpu/minikeys_failures.hip src/backend/gpu/minikeys.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_minikeys_failure_test PRIVATE keyhunt_gpu_options keyhunt_core)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_minikeys_failure_test PRIVATE kernels kernels/search src/backend/gpu)
    target_compile_definitions(${KEYHUNT_GPU_BACKEND}_minikeys_failure_test PRIVATE KEYHUNT_TEST_GPU_FAILURES=1)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_minikeys_failure_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_minikeys_executor COMMAND ${KEYHUNT_GPU_BACKEND}_minikeys_executor_test)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_minikeys_failures COMMAND ${KEYHUNT_GPU_BACKEND}_minikeys_failure_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_minikeys_executor ${KEYHUNT_GPU_BACKEND}_minikeys_failures
        PROPERTIES TIMEOUT 180 LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys" RESOURCE_LOCK gpu_device)
endif()

set(minikeys_cli_args)
if(KEYHUNT_ENABLE_GPU)
    list(APPEND minikeys_cli_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME minikeys_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikeys_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/minikeys-cli-results.json" ${minikeys_cli_args})
set_tests_properties(minikeys_cli PROPERTIES TIMEOUT 600 LABELS "cpu;minikeys")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(minikeys_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys;oracle" RESOURCE_LOCK gpu_device)
endif()

add_test(NAME minikeys_reverse_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikeys_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --reverse --report "${CMAKE_CURRENT_BINARY_DIR}/minikeys-reverse-cli-results.json" ${minikeys_cli_args})
set_tests_properties(minikeys_reverse_cli PROPERTIES TIMEOUT 600 LABELS "cpu;minikeys")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(minikeys_reverse_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys;oracle" RESOURCE_LOCK gpu_device)
endif()

add_executable(minikey_order_probe tests/unit/minikey_order_probe.cpp)
target_link_libraries(minikey_order_probe PRIVATE keyhunt_core)
keyhunt_configure_target(minikey_order_probe)
add_test(NAME minikey_order_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/minikey_order.py" --binary $<TARGET_FILE:minikey_order_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/minikey-order-oracle.json")
set_tests_properties(minikey_order_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;minikeys;reverse;oracle")

set(minikey_example_backend cpu)
if(KEYHUNT_ENABLE_GPU)
    set(minikey_example_backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME minikey_reverse_documented_example COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikey_reverse_examples.py"
    --binary $<TARGET_FILE:keyhunt> --backend ${minikey_example_backend}
    --report "${CMAKE_CURRENT_BINARY_DIR}/minikey-reverse-documented-example.json")
set_tests_properties(minikey_reverse_documented_example PROPERTIES TIMEOUT 150 LABELS "cpu;minikeys;reverse;examples")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(minikey_reverse_documented_example PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys;reverse;examples" RESOURCE_LOCK gpu_device)
endif()

add_test(NAME minikeys_both_ends_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikeys_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --both-ends --report "${CMAKE_CURRENT_BINARY_DIR}/minikeys-both-ends-cli-results.json" ${minikeys_cli_args})
set_tests_properties(minikeys_both_ends_cli PROPERTIES TIMEOUT 600 LABELS "cpu;minikeys")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(minikeys_both_ends_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys;oracle" RESOURCE_LOCK gpu_device)
endif()


add_test(NAME minikey_both_ends_documented_example COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikey_both_ends_examples.py"
    --binary $<TARGET_FILE:keyhunt> --backend ${minikey_example_backend}
    --report "${CMAKE_CURRENT_BINARY_DIR}/minikey-both-ends-documented-example.json")
set_tests_properties(minikey_both_ends_documented_example PROPERTIES TIMEOUT 150 LABELS "cpu;minikeys;both-ends;examples")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(minikey_both_ends_documented_example PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys;both-ends;examples" RESOURCE_LOCK gpu_device)
endif()

add_test(NAME minikeys_dance_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikeys_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --dance --report "${CMAKE_CURRENT_BINARY_DIR}/minikeys-dance-cli-results.json" ${minikeys_cli_args})
set_tests_properties(minikeys_dance_cli PROPERTIES TIMEOUT 600 LABELS "cpu;minikeys")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(minikeys_dance_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys;oracle" RESOURCE_LOCK gpu_device)
endif()


add_test(NAME minikey_dance_documented_example COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikey_dance_examples.py"
    --binary $<TARGET_FILE:keyhunt> --backend ${minikey_example_backend}
    --report "${CMAKE_CURRENT_BINARY_DIR}/minikey-dance-documented-example.json")
set_tests_properties(minikey_dance_documented_example PROPERTIES TIMEOUT 150 LABELS "cpu;minikeys;dance;examples")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(minikey_dance_documented_example PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;minikeys;dance;examples" RESOURCE_LOCK gpu_device)
endif()
