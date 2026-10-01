add_executable(scalar_orbit_test tests/unit/scalar_orbit.cpp)
target_link_libraries(scalar_orbit_test PRIVATE keyhunt_core)
keyhunt_configure_target(scalar_orbit_test)
add_test(NAME scalar_orbit_contract COMMAND scalar_orbit_test)
set_tests_properties(scalar_orbit_contract PROPERTIES TIMEOUT 120 LABELS "cpu;orbit;core")
add_executable(scalar_orbit_probe tests/unit/orbit_probe.cpp)
target_link_libraries(scalar_orbit_probe PRIVATE keyhunt_ranges)
keyhunt_configure_target(scalar_orbit_probe)
add_test(NAME scalar_orbit_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/orbit_oracle.py" --binary $<TARGET_FILE:scalar_orbit_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-orbit-oracle.json")
set_tests_properties(scalar_orbit_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;orbit;oracle")
