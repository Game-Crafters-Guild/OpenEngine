# GlslcCompiler.cmake
#
# ge_find_glslc(): sets GLSLC_EXECUTABLE, the GLSL-to-SPIR-V compiler every
# shader build step runs, and SPIRV_VAL_EXECUTABLE, the SPIR-V validator the
# steps that validate their output run (empty when absent; validation is
# optional). Called once from the top-level CMakeLists.txt before
# any engine directory is added: a find_program result is visible only to the
# directories processed after it, and the Engine directory, which compiles test
# shaders, is added before the Rendering module. Prefers the vcpkg shaderc
# tools when available, and still works with a system Vulkan SDK install, found
# through find_package(Vulkan) or, when that finds nothing (a standalone macOS
# SDK without the vcpkg toolchain), through the VULKAN_SDK environment variable.
#
# The browser build compiles no SPIR-V (the wasm runtime consumes prebuilt
# shader packages), so there both are empty.
function(ge_find_glslc)
    if(EMSCRIPTEN)
        set(GLSLC_EXECUTABLE "" PARENT_SCOPE)
        set(SPIRV_VAL_EXECUTABLE "" PARENT_SCOPE)
        return()
    endif()

    find_package(Vulkan QUIET)
    set(_ge_glslc_hint_dirs ${Vulkan_INCLUDE_DIRS}/../bin)
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND _ge_glslc_hint_dirs "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    endif()
    if(VCPKG_DIR AND VCPKG_ARCH)
        list(APPEND _ge_glslc_hint_dirs
            "${VCPKG_DIR}/installed/${VCPKG_ARCH}/tools"
            "${VCPKG_DIR}/installed/${VCPKG_ARCH}/tools/shaderc")
    endif()
    find_program(GLSLC_EXECUTABLE glslc HINTS ${_ge_glslc_hint_dirs})
    find_program(SPIRV_VAL_EXECUTABLE spirv-val HINTS ${_ge_glslc_hint_dirs})

    if(GLSLC_EXECUTABLE)
        message(STATUS "✅ Found glslc: ${GLSLC_EXECUTABLE}")
    else()
        message(WARNING "⚠️ glslc not found - shader compilation will be skipped; install Vulkan SDK or ensure vcpkg shaderc is available")
    endif()
endfunction()
