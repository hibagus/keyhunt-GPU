add_executable(scalar_batch_probe tests/unit/scalar_batch_probe.cpp)
target_link_libraries(scalar_batch_probe PRIVATE keyhunt_ranges)
keyhunt_configure_target(scalar_batch_probe)
add_test(NAME scalar_batch_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/scalar_batch_oracle.py" --binary $<TARGET_FILE:scalar_batch_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-batch-oracle.json")
set_tests_properties(scalar_batch_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;scalar-batches;oracle")
