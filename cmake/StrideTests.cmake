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
