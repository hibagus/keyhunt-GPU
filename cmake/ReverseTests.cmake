add_executable(scalar_reverse_test tests/unit/scalar_reverse.cpp)
target_link_libraries(scalar_reverse_test PRIVATE keyhunt_core)
keyhunt_configure_target(scalar_reverse_test)
add_test(NAME scalar_reverse_contract COMMAND scalar_reverse_test)
set_tests_properties(scalar_reverse_contract PROPERTIES TIMEOUT 120 LABELS "cpu;core;reverse")
add_test(NAME scalar_reverse_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/reverse_oracle.py" --binary $<TARGET_FILE:scalar_stride_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-reverse-oracle.json")
set_tests_properties(scalar_reverse_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;reverse;oracle")

# Reuse the native arithmetic probe, with operation 1 selecting subtraction.
foreach(backend IN LISTS stride_backends)
    add_test(NAME ${backend}_reverse_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/reverse_oracle.py" --binary $<TARGET_FILE:${backend}_stride_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-reverse-oracle.json")
    set_tests_properties(${backend}_reverse_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;reverse;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_reverse_oracle PROPERTIES LABELS "${backend};hardware;reverse;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()
add_test(NAME scalar_reverse_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/strides_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle> --order reverse
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-reverse-cli-results.json" ${stride_cli_args})
set_tests_properties(scalar_reverse_cli PROPERTIES TIMEOUT 900 LABELS "cpu;reverse;oracle")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(scalar_reverse_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;reverse;oracle" RESOURCE_LOCK gpu_device)
    add_executable(${KEYHUNT_GPU_BACKEND}_reverse_executor_test tests/gpu/reverse_executor.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_reverse_executor_test PRIVATE keyhunt_gpu_options keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_reverse_executor_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_reverse_executor COMMAND ${KEYHUNT_GPU_BACKEND}_reverse_executor_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_reverse_executor PROPERTIES TIMEOUT 120
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;reverse" RESOURCE_LOCK gpu_device)
endif()
