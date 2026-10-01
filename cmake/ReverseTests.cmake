add_executable(scalar_reverse_test tests/unit/scalar_reverse.cpp)
target_link_libraries(scalar_reverse_test PRIVATE keyhunt_core)
keyhunt_configure_target(scalar_reverse_test)
add_test(NAME scalar_reverse_contract COMMAND scalar_reverse_test)
set_tests_properties(scalar_reverse_contract PROPERTIES TIMEOUT 120 LABELS "cpu;core;reverse")
add_test(NAME scalar_reverse_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/reverse_oracle.py" --binary $<TARGET_FILE:scalar_stride_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-reverse-oracle.json")
set_tests_properties(scalar_reverse_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;reverse;oracle")
