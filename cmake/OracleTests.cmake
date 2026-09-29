# Test-only pinned dependency; never linked into keyhunt or installed.
function(keyhunt_add_oracle)
    set(SECP256K1_DISABLE_SHARED ON)
    set(SECP256K1_INSTALL OFF)
    set(SECP256K1_ASM OFF)
    set(SECP256K1_VALGRIND OFF)
    foreach(feature ECDH RECOVERY EXTRAKEYS SCHNORRSIG MUSIG ELLSWIFT)
        set(SECP256K1_ENABLE_MODULE_${feature} OFF)
    endforeach()
    foreach(feature BENCHMARK TESTS EXHAUSTIVE_TESTS CTIME_TESTS EXAMPLES)
        set(SECP256K1_BUILD_${feature} OFF)
    endforeach()
    add_subdirectory(third_party/secp256k1-oracle EXCLUDE_FROM_ALL)
    if(KEYHUNT_ENABLE_SANITIZERS)
        foreach(target secp256k1 secp256k1_precomputed)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        endforeach()
    endif()
endfunction()
keyhunt_add_oracle()
add_executable(secp256k1_oracle tests/oracle/secp256k1_probe.cpp)
target_link_libraries(secp256k1_oracle PRIVATE secp256k1)
keyhunt_configure_target(secp256k1_oracle)
add_test(NAME oracle_selftest COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/oracle_selftest.py"
    --binary $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/oracle-selftest-results.json")
set_tests_properties(oracle_selftest PROPERTIES TIMEOUT 120 LABELS "cpu;oracle")
add_executable(cpu_arithmetic_probe tests/oracle/cpu_arithmetic_probe.cpp)
target_link_libraries(cpu_arithmetic_probe PRIVATE keyhunt_core)
keyhunt_configure_target(cpu_arithmetic_probe)
add_test(NAME cpu_field_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/field_oracle.py"
    --binary $<TARGET_FILE:cpu_arithmetic_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/cpu-field-oracle-results.json")
set_tests_properties(cpu_field_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;oracle")
add_test(NAME cpu_point_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/point_oracle.py"
    --binary $<TARGET_FILE:cpu_arithmetic_probe>
    --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/cpu-point-oracle-results.json")
set_tests_properties(cpu_point_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;oracle")
add_executable(bounded_search_probe tests/oracle/bounded_search_probe.cpp)
target_link_libraries(bounded_search_probe PRIVATE keyhunt_core)
keyhunt_configure_target(bounded_search_probe)
add_test(NAME bounded_search_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/search_oracle.py"
    --binary $<TARGET_FILE:bounded_search_probe>
    --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/bounded-search-oracle-results.json")
set_tests_properties(bounded_search_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;oracle")
