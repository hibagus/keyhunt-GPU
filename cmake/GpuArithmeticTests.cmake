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
if(KEYHUNT_ENABLE_HIP)
    add_executable(hip_arithmetic_probe tests/gpu/arithmetic_probe.hip)
    target_include_directories(hip_arithmetic_probe PRIVATE kernels src/backend/hip)
    target_compile_options(hip_arithmetic_probe PRIVATE -Wall -Wextra)
    set_target_properties(hip_arithmetic_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endif()
foreach(backend portable hip)
    if(backend STREQUAL "hip" AND NOT KEYHUNT_ENABLE_HIP)
        continue()
    endif()
    add_test(NAME ${backend}_field_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/field_oracle.py"
        --binary $<TARGET_FILE:${backend}_arithmetic_probe>
        --cpu $<TARGET_FILE:cpu_arithmetic_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-field-oracle-results.json")
    set_tests_properties(${backend}_field_oracle PROPERTIES TIMEOUT 180 LABELS "arithmetic;oracle")
    if(backend STREQUAL "hip")
        set_tests_properties(${backend}_field_oracle PROPERTIES LABELS "hip;hardware;arithmetic;oracle" RESOURCE_LOCK hip_device)
    else()
        set_tests_properties(${backend}_field_oracle PROPERTIES LABELS "cpu;arithmetic;oracle")
    endif()
endforeach()

foreach(backend portable hip)
    if(backend STREQUAL "hip" AND NOT KEYHUNT_ENABLE_HIP)
        continue()
    endif()
    add_test(NAME ${backend}_point_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/point_oracle.py"
        --binary $<TARGET_FILE:${backend}_arithmetic_probe>
        --cpu $<TARGET_FILE:cpu_arithmetic_probe> --oracle $<TARGET_FILE:secp256k1_oracle>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-point-oracle-results.json")
    set_tests_properties(${backend}_point_oracle PROPERTIES TIMEOUT 240 LABELS "cpu;arithmetic;oracle")
    if(backend STREQUAL "hip")
        set_tests_properties(${backend}_point_oracle PROPERTIES LABELS "hip;hardware;arithmetic;oracle" RESOURCE_LOCK hip_device)
    endif()
endforeach()
