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

set(checkpoint_cli_options "")
if(KEYHUNT_ENABLE_GPU)
    list(APPEND checkpoint_cli_options --hardware --backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME checkpoint_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-cli-results.json" ${checkpoint_cli_options})
set_tests_properties(checkpoint_cli PROPERTIES TIMEOUT 240 LABELS "cpu;storage;cli")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(checkpoint_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;cli;recovery" RESOURCE_LOCK gpu_device)
endif()

add_executable(storage_checkpoint_control_test tests/storage/checkpoint_control.cpp)
target_link_libraries(storage_checkpoint_control_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_checkpoint_control_test)
add_test(NAME storage_checkpoint_control COMMAND storage_checkpoint_control_test)
set_tests_properties(storage_checkpoint_control PROPERTIES TIMEOUT 120 LABELS "cpu;storage;recovery")

# The fixture uses the production Linux control implementation with a slow CPU
# runner. It is never linked into keyhunt and needs no HIP hardware.
add_executable(checkpoint_control_driver tests/storage/control_driver.cpp src/backend/checkpoint_control.cpp)
target_include_directories(checkpoint_control_driver PRIVATE src/backend)
target_link_libraries(checkpoint_control_driver PRIVATE keyhunt_storage)
keyhunt_configure_target(checkpoint_control_driver)
add_test(NAME checkpoint_controls COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_controls.py"
    --binary $<TARGET_FILE:keyhunt> --driver $<TARGET_FILE:checkpoint_control_driver>
    --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-controls-results.json")
set_tests_properties(checkpoint_controls PROPERTIES TIMEOUT 120 LABELS "cpu;storage;cli;recovery")

if(KEYHUNT_ENABLE_GPU)
    add_test(NAME checkpoint_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --backend ${KEYHUNT_GPU_BACKEND} --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-pause-${KEYHUNT_GPU_BACKEND}-results.json")
    set_tests_properties(checkpoint_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 180 LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;cli;recovery" RESOURCE_LOCK gpu_device)
endif()

add_executable(storage_checkpoint_concurrent_test tests/storage/checkpoint_concurrent.cpp)
target_link_libraries(storage_checkpoint_concurrent_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_checkpoint_concurrent_test)
add_test(NAME storage_checkpoint_concurrent COMMAND storage_checkpoint_concurrent_test)
set_tests_properties(storage_checkpoint_concurrent PROPERTIES TIMEOUT 120 LABELS "cpu;storage;recovery")

add_executable(storage_hash160_checkpoint_test tests/storage/hash160_checkpoint.cpp)
target_include_directories(storage_hash160_checkpoint_test PRIVATE src/storage)
target_link_libraries(storage_hash160_checkpoint_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_hash160_checkpoint_test)
add_test(NAME storage_hash160_checkpoint COMMAND storage_hash160_checkpoint_test)
set_tests_properties(storage_hash160_checkpoint PROPERTIES TIMEOUT 120 LABELS "cpu;storage;hash160;recovery")

if(KEYHUNT_ENABLE_GPU)
    add_test(NAME checkpoint_hash160_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --backend ${KEYHUNT_GPU_BACKEND} --mode hash160
        --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-hash160-pause-${KEYHUNT_GPU_BACKEND}.json")
    set_tests_properties(checkpoint_hash160_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 180
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;hash160;recovery" RESOURCE_LOCK gpu_device)
endif()

add_executable(storage_ethereum_checkpoint_test tests/storage/ethereum_checkpoint.cpp)
target_include_directories(storage_ethereum_checkpoint_test PRIVATE src/storage)
target_link_libraries(storage_ethereum_checkpoint_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_ethereum_checkpoint_test)
add_test(NAME storage_ethereum_checkpoint COMMAND storage_ethereum_checkpoint_test)
set_tests_properties(storage_ethereum_checkpoint PROPERTIES TIMEOUT 120 LABELS "cpu;storage;ethereum;recovery")

if(KEYHUNT_ENABLE_GPU)
    add_test(NAME checkpoint_ethereum_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --backend ${KEYHUNT_GPU_BACKEND} --mode ethereum
        --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-ethereum-pause-${KEYHUNT_GPU_BACKEND}.json")
    set_tests_properties(checkpoint_ethereum_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 180
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;ethereum;recovery" RESOURCE_LOCK gpu_device)
endif()

add_executable(storage_vanity_checkpoint_test tests/storage/vanity_checkpoint.cpp)
target_include_directories(storage_vanity_checkpoint_test PRIVATE src/storage)
target_link_libraries(storage_vanity_checkpoint_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_vanity_checkpoint_test)
add_test(NAME storage_vanity_checkpoint COMMAND storage_vanity_checkpoint_test)
set_tests_properties(storage_vanity_checkpoint PROPERTIES TIMEOUT 120 LABELS "cpu;storage;vanity;recovery")

if(KEYHUNT_ENABLE_GPU)
    add_test(NAME checkpoint_vanity_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --backend ${KEYHUNT_GPU_BACKEND} --mode vanity
        --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-vanity-pause-${KEYHUNT_GPU_BACKEND}.json")
    set_tests_properties(checkpoint_vanity_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 180
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;vanity;recovery" RESOURCE_LOCK gpu_device)
endif()

add_test(NAME checkpoint_vanity_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/vanity_checkpoint_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-vanity-cli.json" ${checkpoint_cli_options})
set_tests_properties(checkpoint_vanity_cli PROPERTIES TIMEOUT 180 LABELS "cpu;storage;vanity;recovery")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(checkpoint_vanity_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;vanity;recovery" RESOURCE_LOCK gpu_device)
endif()

add_executable(storage_minikey_checkpoint_test tests/storage/minikey_checkpoint.cpp)
target_include_directories(storage_minikey_checkpoint_test PRIVATE src/storage)
target_link_libraries(storage_minikey_checkpoint_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_minikey_checkpoint_test)
add_test(NAME storage_minikey_checkpoint COMMAND storage_minikey_checkpoint_test)
set_tests_properties(storage_minikey_checkpoint PROPERTIES TIMEOUT 120 LABELS "cpu;storage;minikeys;recovery")

add_test(NAME checkpoint_minikeys_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/minikey_checkpoint_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-minikeys-cli.json" ${checkpoint_cli_options})
set_tests_properties(checkpoint_minikeys_cli PROPERTIES TIMEOUT 300 LABELS "cpu;storage;minikeys;recovery")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(checkpoint_minikeys_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;minikeys;recovery" RESOURCE_LOCK gpu_device)
    add_test(NAME checkpoint_minikeys_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --backend ${KEYHUNT_GPU_BACKEND} --mode minikeys22 --mode minikeys30
        --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-minikeys-pause-${KEYHUNT_GPU_BACKEND}.json")
    set_tests_properties(checkpoint_minikeys_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 180
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;minikeys;recovery" RESOURCE_LOCK gpu_device)
endif()

add_executable(storage_stride_checkpoint_test tests/storage/stride_checkpoint.cpp)
target_include_directories(storage_stride_checkpoint_test PRIVATE src/storage)
target_link_libraries(storage_stride_checkpoint_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_stride_checkpoint_test)
add_test(NAME storage_stride_checkpoint COMMAND storage_stride_checkpoint_test)
set_tests_properties(storage_stride_checkpoint PROPERTIES TIMEOUT 180 LABELS "cpu;storage;stride;recovery")

add_test(NAME checkpoint_strides_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/stride_checkpoint_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-strides-cli.json" ${checkpoint_cli_options})
set_tests_properties(checkpoint_strides_cli PROPERTIES TIMEOUT 300 LABELS "cpu;storage;stride;recovery")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(checkpoint_strides_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;stride;recovery" RESOURCE_LOCK gpu_device)
    add_test(NAME checkpoint_strides_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --backend ${KEYHUNT_GPU_BACKEND} --stride 11 --mode xpoint --mode hash160 --mode ethereum --mode vanity
        --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-strides-pause-${KEYHUNT_GPU_BACKEND}.json")
    set_tests_properties(checkpoint_strides_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 240
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;stride;recovery" RESOURCE_LOCK gpu_device)
endif()

add_executable(storage_reverse_checkpoint_test tests/storage/reverse_checkpoint.cpp)
target_include_directories(storage_reverse_checkpoint_test PRIVATE src/storage)
target_link_libraries(storage_reverse_checkpoint_test PRIVATE keyhunt_storage)
keyhunt_configure_target(storage_reverse_checkpoint_test)
add_test(NAME storage_reverse_checkpoint COMMAND storage_reverse_checkpoint_test)
set_tests_properties(storage_reverse_checkpoint PROPERTIES TIMEOUT 180 LABELS "cpu;storage;reverse;recovery")

add_test(NAME checkpoint_reverse_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/stride_checkpoint_cli.py"
    --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
    --order reverse --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-reverse-cli.json" ${checkpoint_cli_options})
set_tests_properties(checkpoint_reverse_cli PROPERTIES TIMEOUT 480 LABELS "cpu;storage;reverse;recovery")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(checkpoint_reverse_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;reverse;recovery" RESOURCE_LOCK gpu_device)
    add_test(NAME checkpoint_reverse_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --backend ${KEYHUNT_GPU_BACKEND} --order reverse --stride 11 --mode xpoint --mode hash160 --mode ethereum --mode vanity
        --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-reverse-pause-${KEYHUNT_GPU_BACKEND}.json")
    set_tests_properties(checkpoint_reverse_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 240
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;reverse;recovery" RESOURCE_LOCK gpu_device)
endif()

foreach(order forward reverse)
    add_test(NAME checkpoint_glv_${order}_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/stride_checkpoint_cli.py"
        --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
        --kernel glv --order ${order} --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-glv-${order}-cli.json" ${checkpoint_cli_options})
    set_tests_properties(checkpoint_glv_${order}_cli PROPERTIES TIMEOUT 480 LABELS "cpu;storage;glv;recovery")
    if(KEYHUNT_ENABLE_GPU)
        set_tests_properties(checkpoint_glv_${order}_cli PROPERTIES LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;glv;recovery" RESOURCE_LOCK gpu_device)
        # Forward uses legacy scalar coordinates; reverse uses indexed stride 17.
        set(glv_pause_stride 1)
        if(order STREQUAL "reverse")
            set(glv_pause_stride 11)
        endif()
        add_test(NAME checkpoint_glv_${order}_pause_${KEYHUNT_GPU_BACKEND} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checkpoint_pause_hip.py"
            --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
            --backend ${KEYHUNT_GPU_BACKEND} --kernel glv --order ${order} --stride ${glv_pause_stride}
            --mode xpoint --mode hash160 --mode ethereum --mode vanity
            --report "${CMAKE_CURRENT_BINARY_DIR}/checkpoint-glv-${order}-pause-${KEYHUNT_GPU_BACKEND}.json")
        set_tests_properties(checkpoint_glv_${order}_pause_${KEYHUNT_GPU_BACKEND} PROPERTIES TIMEOUT 300
            LABELS "${KEYHUNT_GPU_BACKEND};hardware;storage;glv;recovery" RESOURCE_LOCK gpu_device)
    endif()
endforeach()
