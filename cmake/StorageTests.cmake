add_executable(storage_database_test tests/storage/database.cpp)
target_include_directories(storage_database_test PRIVATE src/storage)
target_link_libraries(storage_database_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_database_test)
add_test(NAME storage_database COMMAND storage_database_test)
set_tests_properties(storage_database PROPERTIES TIMEOUT 60 LABELS "cpu;storage")
add_executable(storage_journal_test tests/storage/journal.cpp)
target_include_directories(storage_journal_test PRIVATE src/storage)
target_link_libraries(storage_journal_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_journal_test)
add_test(NAME storage_journal COMMAND storage_journal_test)
set_tests_properties(storage_journal PROPERTIES TIMEOUT 180 LABELS "cpu;storage;scheduler")
# A private build contains commit-boundary termination hooks; production code
# cannot enable them with an environment variable or command-line flag.
add_executable(storage_concurrency_test tests/storage/concurrency.cpp
    src/storage/sqlite.cpp src/storage/free_tree.cpp src/storage/journal.cpp src/storage/checkpoint_data.cpp)
target_include_directories(storage_concurrency_test PRIVATE src/storage "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(storage_concurrency_test PRIVATE KEYHUNT_TEST_STORAGE_FAILURES=1 KEYHUNT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(storage_concurrency_test PRIVATE keyhunt_core SQLite::SQLite3)
keyhunt_configure_target(storage_concurrency_test)
add_test(NAME storage_concurrency COMMAND storage_concurrency_test)
set_tests_properties(storage_concurrency PROPERTIES TIMEOUT 180 LABELS "cpu;storage;scheduler;recovery")

add_test(NAME state_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/state_cli.py"
    --binary $<TARGET_FILE:keyhunt> --report "${CMAKE_CURRENT_BINARY_DIR}/state-cli-results.json")
set_tests_properties(state_cli PROPERTIES TIMEOUT 180 LABELS "cpu;storage;cli")

add_executable(storage_checkpoint_test tests/storage/checkpoint.cpp)
target_include_directories(storage_checkpoint_test PRIVATE src/storage "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(storage_checkpoint_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_checkpoint_test)
add_test(NAME storage_checkpoint COMMAND storage_checkpoint_test)
set_tests_properties(storage_checkpoint PROPERTIES TIMEOUT 120 LABELS "cpu;storage;recovery")

# Production contains no environment-controlled checkpoint fault hooks.
add_executable(storage_checkpoint_failures_test tests/storage/checkpoint_failures.cpp
    src/storage/sqlite.cpp src/storage/free_tree.cpp src/storage/journal.cpp
    src/storage/checkpoint_data.cpp src/storage/checkpoint.cpp)
target_include_directories(storage_checkpoint_failures_test PRIVATE src/storage "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(storage_checkpoint_failures_test PRIVATE KEYHUNT_TEST_STORAGE_FAILURES=1 KEYHUNT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(storage_checkpoint_failures_test PRIVATE keyhunt_core SQLite::SQLite3)
keyhunt_configure_target(storage_checkpoint_failures_test)
add_test(NAME storage_checkpoint_failures COMMAND storage_checkpoint_failures_test)
set_tests_properties(storage_checkpoint_failures PROPERTIES TIMEOUT 120 LABELS "cpu;storage;recovery")

