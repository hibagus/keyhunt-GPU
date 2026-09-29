include(CheckIPOSupported)
set(KEYHUNT_IPO_SUPPORTED OFF)
if(KEYHUNT_ENABLE_LTO AND NOT KEYHUNT_ENABLE_SANITIZERS)
    check_ipo_supported(RESULT KEYHUNT_IPO_SUPPORTED OUTPUT KEYHUNT_IPO_ERROR LANGUAGES C CXX)
    if(NOT KEYHUNT_IPO_SUPPORTED)
        message(WARNING "IPO/LTO unavailable; continuing without it: ${KEYHUNT_IPO_ERROR}")
    endif()
endif()

function(keyhunt_configure_target target)
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    target_compile_options(${target} PRIVATE -m64 -mssse3 -Wall -Wextra
        "$<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-copy>")
    if(KEYHUNT_NATIVE_CPU)
        target_compile_options(${target} PRIVATE -march=native -mtune=native)
    endif()
    if(KEYHUNT_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    else()
        target_compile_options(${target} PRIVATE
            "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-Ofast;-ftree-vectorize>")
    endif()
endfunction()

# Whole-target IPO stalls startup on the C01 host (root cause unresolved). Keep
# exactly the original Makefile's LTO translation units, and do not enable IPO
# on the executable or on the other integer/point implementations.
function(keyhunt_configure_original_lto)
    if(KEYHUNT_IPO_SUPPORTED)
        set_property(SOURCE
            third_party/bloom/bloom.cpp third_party/oldbloom/bloom.cpp
            src/crypto/secp256k1/Random.cpp src/crypto/secp256k1/IntGroup.cpp
            src/crypto/hash/sha256.cpp src/crypto/hash/sha256_sse.cpp
            src/crypto/hash/ripemd160.cpp src/crypto/hash/ripemd160_sse.cpp
            legacy/gmp256k1/Random.cpp legacy/gmp256k1/IntGroup.cpp
            APPEND PROPERTY COMPILE_OPTIONS
            "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto>")
    endif()
endfunction()
