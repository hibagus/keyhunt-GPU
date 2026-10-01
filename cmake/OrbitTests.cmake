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

# Reuse the guarded native point probe; operations 3..8 select orbit members.
foreach(backend IN LISTS glv_backends)
    add_test(NAME ${backend}_orbit_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/orbit_oracle.py" --binary $<TARGET_FILE:${backend}_glv_probe>
        --oracle $<TARGET_FILE:secp256k1_oracle> --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-orbit-oracle.json")
    set_tests_properties(${backend}_orbit_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;orbit;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_orbit_oracle PROPERTIES LABELS "${backend};hardware;orbit;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()
foreach(order forward reverse)
    add_test(NAME scalar_orbit_${order}_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/orbits_cli.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle> --order ${order}
        --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-orbit-${order}-cli.json" ${stride_cli_args})
    set_tests_properties(scalar_orbit_${order}_cli PROPERTIES TIMEOUT 1200 LABELS "cpu;orbit;oracle")
    if(KEYHUNT_ENABLE_GPU)
        set_tests_properties(scalar_orbit_${order}_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;orbit;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()
