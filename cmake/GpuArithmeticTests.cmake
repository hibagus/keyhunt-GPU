# Host-compilation of the same portable headers is a sanitizer/debug aid; it is
# never the sole arithmetic oracle. Independent expected values come from Python
# and the pinned libsecp256k1 probe.
add_executable(portable_arithmetic_probe tests/gpu/arithmetic_probe.cpp)
target_include_directories(portable_arithmetic_probe PRIVATE kernels)
target_compile_options(portable_arithmetic_probe PRIVATE -Wall -Wextra)
if(KEYHUNT_ENABLE_SANITIZERS)
    target_compile_options(portable_arithmetic_probe PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(portable_arithmetic_probe PRIVATE -fsanitize=address,undefined)
endif()
set(keyhunt_arithmetic_backends portable ${KEYHUNT_GPU_BACKEND})
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_arithmetic_probe tests/gpu/arithmetic_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_arithmetic_probe PRIVATE keyhunt_gpu_options)
    add_executable(${KEYHUNT_GPU_BACKEND}_arithmetic_benchmark tests/gpu/arithmetic_benchmark.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_arithmetic_benchmark PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_arithmetic_benchmark PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_arithmetic_benchmark PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_arithmetic_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_arithmetic_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    if(KEYHUNT_GFX942_CARRY)
        # Run the same independent corpus on real HIP with the optimization
        # disabled. Host-only tests cannot certify the device fallback path.
        add_executable(hip_portable_arithmetic_probe tests/gpu/arithmetic_probe.hip)
        target_link_libraries(hip_portable_arithmetic_probe PRIVATE keyhunt_gpu_options)
        target_include_directories(hip_portable_arithmetic_probe PRIVATE kernels src/backend/gpu)
        target_compile_options(hip_portable_arithmetic_probe PRIVATE -UKEYHUNT_GFX942_CARRY)
        set_target_properties(hip_portable_arithmetic_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
        list(APPEND keyhunt_arithmetic_backends hip_portable)
    endif()
endif()
foreach(backend IN LISTS keyhunt_arithmetic_backends)
    if(NOT backend STREQUAL "portable" AND NOT KEYHUNT_ENABLE_GPU)
        continue()
    endif()
    add_test(NAME ${backend}_field_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/field_oracle.py"
        --binary $<TARGET_FILE:${backend}_arithmetic_probe>
        --cpu $<TARGET_FILE:cpu_arithmetic_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-field-oracle-results.json")
    set_tests_properties(${backend}_field_oracle PROPERTIES TIMEOUT 180 LABELS "arithmetic;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_field_oracle PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;arithmetic;oracle" RESOURCE_LOCK gpu_device)
    else()
        set_tests_properties(${backend}_field_oracle PROPERTIES LABELS "cpu;arithmetic;oracle")
    endif()
endforeach()

foreach(backend IN LISTS keyhunt_arithmetic_backends)
    if(NOT backend STREQUAL "portable" AND NOT KEYHUNT_ENABLE_GPU)
        continue()
    endif()
    add_test(NAME ${backend}_point_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/point_oracle.py"
        --binary $<TARGET_FILE:${backend}_arithmetic_probe>
        --cpu $<TARGET_FILE:cpu_arithmetic_probe> --oracle $<TARGET_FILE:secp256k1_oracle>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-point-oracle-results.json")
    set_tests_properties(${backend}_point_oracle PROPERTIES TIMEOUT 240 LABELS "cpu;arithmetic;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_point_oracle PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;arithmetic;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()

if(KEYHUNT_ENABLE_GPU)
    add_test(NAME ${KEYHUNT_GPU_BACKEND}_arithmetic_devices COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/device_arithmetic.py"
        --binary $<TARGET_FILE:keyhunt> --probe $<TARGET_FILE:${KEYHUNT_GPU_BACKEND}_arithmetic_probe> --backend ${KEYHUNT_GPU_BACKEND}
        --oracle $<TARGET_FILE:secp256k1_oracle>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${KEYHUNT_GPU_BACKEND}-arithmetic-devices.json")
    set_tests_properties(${KEYHUNT_GPU_BACKEND}_arithmetic_devices PROPERTIES TIMEOUT 300
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;arithmetic;oracle" RESOURCE_LOCK gpu_device)
endif()
