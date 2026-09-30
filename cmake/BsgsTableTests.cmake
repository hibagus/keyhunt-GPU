add_executable(bsgs_table_test tests/unit/bsgs_table.cpp)
target_link_libraries(bsgs_table_test PRIVATE keyhunt_core)
keyhunt_configure_target(bsgs_table_test)
add_test(NAME bsgs_table_contract COMMAND bsgs_table_test)
set_tests_properties(bsgs_table_contract PROPERTIES TIMEOUT 120 LABELS "cpu;bsgs;tables")
add_test(NAME bsgs_table_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/bsgs_table.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/bsgs-table-results.json")
set_tests_properties(bsgs_table_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;bsgs;tables;oracle")
