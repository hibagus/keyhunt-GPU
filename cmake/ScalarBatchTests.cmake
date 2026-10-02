add_executable(scalar_batch_probe tests/unit/scalar_batch_probe.cpp)
target_link_libraries(scalar_batch_probe PRIVATE keyhunt_ranges)
keyhunt_configure_target(scalar_batch_probe)
add_test(NAME scalar_batch_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/scalar_batch_oracle.py" --binary $<TARGET_FILE:scalar_batch_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-batch-oracle.json")
set_tests_properties(scalar_batch_oracle PROPERTIES TIMEOUT 180 LABELS "cpu;scalar-batches;oracle")

foreach(mapping forward reverse)
    foreach(family strides orbits)
        add_test(NAME scalar_both_ends_${family}_${mapping}_cli COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/${family}_cli.py"
            --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
            --batch-order both-ends --order ${mapping}
            --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-both-ends-${family}-${mapping}.json" ${stride_cli_args})
        set_tests_properties(scalar_both_ends_${family}_${mapping}_cli PROPERTIES TIMEOUT 1200 LABELS "cpu;scalar-batches;oracle")
        if(KEYHUNT_ENABLE_GPU)
            set_tests_properties(scalar_both_ends_${family}_${mapping}_cli PROPERTIES
                LABELS "${KEYHUNT_GPU_BACKEND};hardware;scalar-batches;oracle" RESOURCE_LOCK gpu_device)
        endif()
    endforeach()
endforeach()

foreach(mapping forward reverse)
    foreach(family strides orbits)
        add_test(NAME scalar_dance_${family}_${mapping}_cli COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/${family}_cli.py"
            --binary $<TARGET_FILE:keyhunt> --oracle $<TARGET_FILE:secp256k1_oracle>
            --batch-order dance --order ${mapping}
            --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-dance-${family}-${mapping}.json" ${stride_cli_args})
        set_tests_properties(scalar_dance_${family}_${mapping}_cli PROPERTIES TIMEOUT 1200 LABELS "cpu;scalar-batches;oracle")
        if(KEYHUNT_ENABLE_GPU)
            set_tests_properties(scalar_dance_${family}_${mapping}_cli PROPERTIES
                LABELS "${KEYHUNT_GPU_BACKEND};hardware;scalar-batches;oracle" RESOURCE_LOCK gpu_device)
        endif()
    endforeach()
endforeach()

set(scalar_batch_example_backend cpu)
if(KEYHUNT_ENABLE_GPU)
    set(scalar_batch_example_backend ${KEYHUNT_GPU_BACKEND})
endif()
add_test(NAME scalar_both_ends_documented_example COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/scalar_both_ends_examples.py"
    --binary $<TARGET_FILE:keyhunt> --backend ${scalar_batch_example_backend}
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-both-ends-example.json")
set_tests_properties(scalar_both_ends_documented_example PROPERTIES TIMEOUT 150 LABELS "cpu;scalar-batches;examples")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(scalar_both_ends_documented_example PROPERTIES
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;scalar-batches;examples" RESOURCE_LOCK gpu_device)
endif()

add_test(NAME scalar_dance_documented_example COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/scalar_both_ends_examples.py"
    --binary $<TARGET_FILE:keyhunt> --backend ${scalar_batch_example_backend} --dance
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-dance-example.json")
set_tests_properties(scalar_dance_documented_example PROPERTIES TIMEOUT 150 LABELS "cpu;scalar-batches;examples")
if(KEYHUNT_ENABLE_GPU)
    set_tests_properties(scalar_dance_documented_example PROPERTIES
        LABELS "${KEYHUNT_GPU_BACKEND};hardware;scalar-batches;examples" RESOURCE_LOCK gpu_device)
endif()

add_test(NAME scalar_random_window_oracle COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/scalar_random_windows.py" --binary $<TARGET_FILE:scalar_batch_probe>
    --report "${CMAKE_CURRENT_BINARY_DIR}/scalar-random-window-oracle.json")
set_tests_properties(scalar_random_window_oracle PROPERTIES TIMEOUT 240 LABELS "cpu;scalar-batches;oracle")
