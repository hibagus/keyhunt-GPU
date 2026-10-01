add_executable(portable_glv_probe tests/gpu/glv_probe.cpp)
target_include_directories(portable_glv_probe PRIVATE kernels)
keyhunt_configure_target(portable_glv_probe)
set(glv_backends portable)
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_glv_probe tests/gpu/glv_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_glv_probe PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_glv_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_glv_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    list(APPEND glv_backends ${KEYHUNT_GPU_BACKEND})
endif()
foreach(backend IN LISTS glv_backends)
    add_test(NAME ${backend}_glv_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/glv_oracle.py" --binary $<TARGET_FILE:${backend}_glv_probe>
        --oracle $<TARGET_FILE:secp256k1_oracle> --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-glv-oracle.json")
    set_tests_properties(${backend}_glv_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;glv;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_glv_oracle PROPERTIES LABELS "${backend};hardware;glv;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()

# GLV is an execution choice: reuse the independent coordinate/match matrix for
# unit, positive-stride and reverse ranges without creating new job algorithms.
foreach(order forward reverse)
    add_test(NAME glv_${order}_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/strides_cli.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --kernel glv --order ${order} --report "${CMAKE_CURRENT_BINARY_DIR}/glv-${order}-cli.json" ${stride_cli_args})
    set_tests_properties(glv_${order}_cli PROPERTIES TIMEOUT 900 LABELS "cpu;glv;oracle")
    if(KEYHUNT_ENABLE_GPU)
        set_tests_properties(glv_${order}_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;glv;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()
if(KEYHUNT_ENABLE_GPU)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_glv_xpoint_executor COMMAND ${KEYHUNT_GPU_BACKEND}_xpoint_executor_test --glv)
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_glv_xpoint_executor PROPERTIES TIMEOUT 120 LABELS "${KEYHUNT_GPU_BACKEND};hardware;glv" RESOURCE_LOCK gpu_device)
endif()

if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_glv_benchmark tests/gpu/glv_benchmark.cpp)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_glv_benchmark PRIVATE keyhunt_gpu_options keyhunt_backend)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_glv_benchmark PROPERTIES LINKER_LANGUAGE CXX)
endif()
