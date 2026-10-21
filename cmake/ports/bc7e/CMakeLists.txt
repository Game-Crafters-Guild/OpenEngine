cmake_minimum_required(VERSION 3.25)
project(bc7e LANGUAGES C ISPC)

# arm64 builds one neon object and no dispatcher. Without --cpu, ISPC tunes
# neon code for the machine it runs on (an Apple-silicon host gets an apple-a*
# CPU matching its features, an x86-64 host cortex-a35), and the tuning changes
# the emitted code and the instructions it may use. The pin gives every build
# host the same object: apple-a14 on macOS, the M1's core and so the oldest
# arm64 Mac, and cortex-a35, the plain ARMv8-A baseline, elsewhere.
# BC7E_VERSION names the CPU so the texture cook's cache key does too.
# x64 builds one object per instruction set plus ISPC's dispatcher, which picks
# the best set the CPU supports on the first call; bc7e_isa reports that choice.
# ISPC derives each x64 set's CPU from the set (corei7, core-avx2, skx), not
# from the host, and refuses --cpu with more than one target.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(BC7E_ISPC_TARGETS "neon-i32x4")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(BC7E_ISPC_CPU "apple-a14")
    else()
        set(BC7E_ISPC_CPU "cortex-a35")
    endif()
    set(BC7E_ISPC_FLAGS --arch=aarch64 --cpu=${BC7E_ISPC_CPU})
    string(APPEND BC7E_VERSION "-${BC7E_ISPC_CPU}")
else()
    set(BC7E_ISPC_TARGETS "sse4-i32x4;avx2-i32x8;avx512skx-i32x16")
    set(BC7E_ISPC_FLAGS --arch=x86-64)
endif()

add_library(bc7e STATIC bc7e.ispc bc7e_isa.ispc)
set_target_properties(bc7e PROPERTIES
    ISPC_INSTRUCTION_SETS "${BC7E_ISPC_TARGETS}"
    ISPC_HEADER_SUFFIX "_ispc.h"
    ISPC_HEADER_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include"
    POSITION_INDEPENDENT_CODE ON
    LINKER_LANGUAGE C)
# The same flags in every configuration, so Debug and Release emit the same
# bytes. No --opt=fast-math: it changes the bytes too.
target_compile_options(bc7e PRIVATE ${BC7E_ISPC_FLAGS} -O2 --opt=disable-assertions --woff)
target_include_directories(bc7e INTERFACE $<INSTALL_INTERFACE:include/bc7e>)
target_compile_definitions(bc7e INTERFACE "BC7E_VERSION=\"${BC7E_VERSION}\"")

install(TARGETS bc7e EXPORT unofficial-bc7e-targets ARCHIVE DESTINATION lib)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/bc7e_ispc.h" "${CMAKE_CURRENT_BINARY_DIR}/include/bc7e_isa_ispc.h"
        DESTINATION include/bc7e)
install(EXPORT unofficial-bc7e-targets NAMESPACE unofficial::bc7e:: DESTINATION share/unofficial-bc7e)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7e-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/unofficial-bc7e-targets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7e-config.cmake" DESTINATION share/unofficial-bc7e)
