add_executable(coordinator_registry_test tests/coordinator/registry.cpp)
target_include_directories(coordinator_registry_test PRIVATE src/storage src/coordinator)
target_link_libraries(coordinator_registry_test PRIVATE keyhunt_coordination)
keyhunt_configure_target(coordinator_registry_test)
add_test(NAME coordinator_registry COMMAND coordinator_registry_test)
set_tests_properties(coordinator_registry PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;security")

# An explicit root supports isolated package extraction without system install.
set(KEYHUNT_TEST_APACHE_ROOT "" CACHE PATH "Apache package root for live coordinator TLS tests")
if(KEYHUNT_TEST_APACHE_ROOT)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME coordinator_tls COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/coordinator/tls.py
        --coordinator $<TARGET_FILE:keyhunt-coordinator>
        --apache-root ${KEYHUNT_TEST_APACHE_ROOT})
    set_tests_properties(coordinator_tls PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;security;integration")
endif()
