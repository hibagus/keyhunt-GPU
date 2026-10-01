add_executable(scalar_stride_test tests/unit/scalar_stride.cpp)
target_link_libraries(scalar_stride_test PRIVATE keyhunt_core)
keyhunt_configure_target(scalar_stride_test)
add_test(NAME scalar_stride_contract COMMAND scalar_stride_test)
set_tests_properties(scalar_stride_contract PROPERTIES TIMEOUT 120 LABELS "cpu;core;stride")
add_executable(scalar_stride_probe tests/unit/stride_probe.cpp)
target_link_libraries(scalar_stride_probe PRIVATE keyhunt_ranges)
keyhunt_configure_target(scalar_stride_probe)
add_test(NAME scalar_stride_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/stride_oracle.py" --binary $<TARGET_FILE:scalar_stride_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-stride-oracle.json")
set_tests_properties(scalar_stride_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;stride;oracle")

add_executable(portable_stride_probe tests/gpu/stride_probe.cpp)
target_include_directories(portable_stride_probe PRIVATE kernels)
keyhunt_configure_target(portable_stride_probe)
set(stride_backends portable)
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_stride_probe tests/gpu/stride_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_stride_probe PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_stride_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_stride_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    list(APPEND stride_backends ${KEYHUNT_GPU_BACKEND})
endif()
foreach(backend IN LISTS stride_backends)
    add_test(NAME ${backend}_stride_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/stride_oracle.py" --binary $<TARGET_FILE:${backend}_stride_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-stride-oracle.json")
    set_tests_properties(${backend}_stride_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;stride;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_stride_oracle PROPERTIES LABELS "${backend};hardware;stride;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()

set(stride_cli_args)
if(KEYHUNT_ENABLE_GPU)
    list(APPEND stride_cli_args --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME scalar_strides_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/strides_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-strides-cli-results.json" ${stride_cli_args})
set_tests_properties(scalar_strides_cli PROPERTIES TIMEOUT 900 LABELS "cpu;stride;oracle")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(scalar_strides_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;stride;oracle" RESOURCE_LOCK gpu_device)
endif()

if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_stride_executor_test tests/gpu/stride_executor.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_stride_executor_test PRIVATE keyhunt_gpu_options keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_stride_executor_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_stride_executor COMMAND ${KEYHUNT_GPU_BACKEND}_stride_executor_test)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_stride_executor PROPERTIES TIMEOUT 120
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;stride" RESOURCE_LOCK gpu_device)
endif()
