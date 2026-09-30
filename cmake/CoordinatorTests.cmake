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

add_executable(coordinator_sync_test tests/coordinator/sync.cpp)
target_include_directories(coordinator_sync_test PRIVATE src/storage src/coordinator)
target_link_libraries(coordinator_sync_test PRIVATE keyhunt_coordination)
keyhunt_configure_target(coordinator_sync_test)
add_test(NAME coordinator_sync COMMAND coordinator_sync_test)
set_tests_properties(coordinator_sync PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;recovery")

# The production server never includes these process-exit injection hooks.
add_executable(coordinator_sync_failures_test tests/coordinator/sync.cpp
    src/coordinator/certificate.cpp src/coordinator/repository.cpp
    src/storage/sqlite.cpp src/storage/free_tree.cpp src/storage/journal.cpp src/storage/checkpoint_data.cpp)
target_include_directories(coordinator_sync_failures_test PRIVATE src/storage src/coordinator "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(coordinator_sync_failures_test PRIVATE KEYHUNT_TEST_STORAGE_FAILURES=1 KEYHUNT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(coordinator_sync_failures_test PRIVATE keyhunt_core SQLite::SQLite3 nlohmann_json::nlohmann_json OpenSSL::Crypto)
keyhunt_configure_target(coordinator_sync_failures_test)
add_test(NAME coordinator_sync_failures COMMAND coordinator_sync_failures_test)
set_tests_properties(coordinator_sync_failures PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;recovery")

add_executable(coordinator_worker_test tests/coordinator/worker.cpp)
target_include_directories(coordinator_worker_test PRIVATE src/storage src/coordinator)
target_link_libraries(coordinator_worker_test PRIVATE keyhunt_coordination)
keyhunt_configure_target(coordinator_worker_test)
add_test(NAME coordinator_worker COMMAND coordinator_worker_test)
set_tests_properties(coordinator_worker PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;recovery")

add_executable(coordinator_worker_failures_test tests/coordinator/worker_failures.cpp
    src/coordinator/certificate.cpp src/coordinator/repository.cpp src/coordinator/worker.cpp
    src/storage/sqlite.cpp src/storage/free_tree.cpp src/storage/journal.cpp src/storage/checkpoint_data.cpp src/storage/checkpoint.cpp)
target_include_directories(coordinator_worker_failures_test PRIVATE src/storage src/coordinator "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(coordinator_worker_failures_test PRIVATE KEYHUNT_TEST_STORAGE_FAILURES=1 KEYHUNT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(coordinator_worker_failures_test PRIVATE keyhunt_core SQLite::SQLite3 nlohmann_json::nlohmann_json OpenSSL::Crypto)
keyhunt_configure_target(coordinator_worker_failures_test)
add_test(NAME coordinator_worker_failures COMMAND coordinator_worker_failures_test)
set_tests_properties(coordinator_worker_failures PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;recovery")

if(KEYHUNT_TEST_APACHE_ROOT AND TARGET keyhunt-worker)
    set(coordinator_worker_options "")
    if(KEYHUNT_ENABLE_HIP)
        list(APPEND coordinator_worker_options --hip)
    endif()
    add_test(NAME coordinator_https_worker COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/coordinator/https_worker.py
        --coordinator $<TARGET_FILE:keyhunt-coordinator> --worker $<TARGET_FILE:keyhunt-worker>
        --keyhunt $<TARGET_FILE:keyhunt> --apache-root ${KEYHUNT_TEST_APACHE_ROOT} ${coordinator_worker_options})
    set_tests_properties(coordinator_https_worker PROPERTIES TIMEOUT 300 LABELS "coordinator;security;integration")
    if(KEYHUNT_ENABLE_HIP)
        set_tests_properties(coordinator_https_worker PROPERTIES LABELS "hip;hardware;coordinator;integration")
    endif()
endif()

add_executable(coordinator_recovery_test tests/coordinator/recovery.cpp
    src/coordinator/certificate.cpp src/coordinator/repository.cpp src/coordinator/worker.cpp
    src/storage/sqlite.cpp src/storage/free_tree.cpp src/storage/journal.cpp src/storage/checkpoint_data.cpp src/storage/checkpoint.cpp)
target_include_directories(coordinator_recovery_test PRIVATE src/storage src/coordinator "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(coordinator_recovery_test PRIVATE KEYHUNT_TEST_STORAGE_FAILURES=1 KEYHUNT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(coordinator_recovery_test PRIVATE keyhunt_core SQLite::SQLite3 nlohmann_json::nlohmann_json OpenSSL::Crypto)
keyhunt_configure_target(coordinator_recovery_test)
add_test(NAME coordinator_recovery COMMAND coordinator_recovery_test)
set_tests_properties(coordinator_recovery PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;recovery")

if(KEYHUNT_TEST_APACHE_ROOT AND TARGET keyhunt-worker)
    add_test(NAME coordinator_local_launcher COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/coordinator/local_launcher.py
        --coordinator $<TARGET_FILE:keyhunt-coordinator> --worker $<TARGET_FILE:keyhunt-worker>
        --apache-root ${KEYHUNT_TEST_APACHE_ROOT})
    set_tests_properties(coordinator_local_launcher PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;integration")
endif()

add_executable(coordinator_budget_test tests/coordinator/budget.cpp)
target_include_directories(coordinator_budget_test PRIVATE src/storage src/coordinator)
target_link_libraries(coordinator_budget_test PRIVATE keyhunt_coordination)
keyhunt_configure_target(coordinator_budget_test)
add_test(NAME coordinator_budget COMMAND coordinator_budget_test)
set_tests_properties(coordinator_budget PROPERTIES TIMEOUT 120 LABELS "cpu;coordinator;budget")
