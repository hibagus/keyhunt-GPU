add_executable(vanity_search_test tests/unit/vanity_search.cpp)
target_link_libraries(vanity_search_test PRIVATE keyhunt_core)
keyhunt_configure_target(vanity_search_test)
add_test(NAME vanity_search_contract COMMAND vanity_search_test)
set_tests_properties(vanity_search_contract PROPERTIES TIMEOUT 30 LABELS "cpu;core;vanity")

add_executable(portable_base58_probe tests/gpu/base58_probe.cpp)
target_include_directories(portable_base58_probe PRIVATE kernels)
keyhunt_configure_target(portable_base58_probe)
set(base58_backends portable)
if(KEYHUNT_ENABLE_GPU)
    add_executable(${KEYHUNT_GPU_BACKEND}_base58_probe tests/gpu/base58_probe.hip)
    target_link_libraries(${KEYHUNT_GPU_BACKEND}_base58_probe PRIVATE keyhunt_gpu_options)
    target_include_directories(${KEYHUNT_GPU_BACKEND}_base58_probe PRIVATE kernels src/backend/gpu)
    set_target_properties(${KEYHUNT_GPU_BACKEND}_base58_probe PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF LINKER_LANGUAGE CXX)
    list(APPEND base58_backends ${KEYHUNT_GPU_BACKEND})
endif()
foreach(backend IN LISTS base58_backends)
    add_test(NAME ${backend}_base58_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/base58_oracle.py"
        --binary $<TARGET_FILE:${backend}_base58_probe>
        --report "${CMAKE_CURRENT_BINARY_DIR}/${backend}-base58-oracle.json")
    set_tests_properties(${backend}_base58_oracle PROPERTIES TIMEOUT 120 LABELS "cpu;base58;oracle")
    if(NOT backend STREQUAL "portable")
        set_tests_properties(${backend}_base58_oracle PROPERTIES LABELS "${backend};hardware;base58;oracle" RESOURCE_LOCK gpu_device)
    endif()
endforeach()


