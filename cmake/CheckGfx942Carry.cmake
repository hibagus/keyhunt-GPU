# Keep try_compile settings local so this probe cannot change later checks.
function(keyhunt_check_gfx942_carry)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(KEYHUNT_HAVE_GFX942_CARRY
        "${CMAKE_BINARY_DIR}/CMakeFiles/gfx942-carry-check"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_gfx942_carry.hip"
        CMAKE_FLAGS "-DCMAKE_HIP_ARCHITECTURES=${CMAKE_HIP_ARCHITECTURES}"
                    "-DCMAKE_HIP_STANDARD=17"
        COMPILE_DEFINITIONS -DKEYHUNT_GFX942_CARRY=1
        OUTPUT_VARIABLE carry_compile_output)
    if(NOT KEYHUNT_HAVE_GFX942_CARRY)
        message(FATAL_ERROR "The HIP compiler cannot build gfx942 carry chains:\n${carry_compile_output}")
    endif()
endfunction()
