cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SCRIPT OR NOT DEFINED VCPKG_DIR)
    message(FATAL_ERROR "VcpkgMakeOptionsTest.cmake: SCRIPT and VCPKG_DIR are required")
endif()

# Exercise the argument semantics of the oldest supported CMake even when
# ctest runs on a host that provides CMP0174's newer empty-argument behavior.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 OLD)
endif()

include("${VCPKG_DIR}/scripts/cmake/z_vcpkg_function_arguments.cmake")
include("${VCPKG_DIR}/scripts/cmake/vcpkg_list.cmake")

# The port runner supplies this argument validator; the path helper only
# needs the validator and vcpkg's list operations, with no compiler or install.
macro(z_vcpkg_unparsed_args)
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
endmacro()

include("${SCRIPT}")
set(CURRENT_INSTALLED_DIR "/fixture/installed")
set(CURRENT_BUILDTREES_DIR "/fixture/buildtrees")
set(PORT pthread-stubs)
set(VCPKG_LIBRARY_LINKAGE static)

function(expect_option options expected)
    if(NOT expected IN_LIST options)
        message(FATAL_ERROR "Missing configure option '${expected}' in '${options}'")
    endif()
endfunction()

foreach(config IN ITEMS RELEASE DEBUG)
    set(suffix "")
    if(config STREQUAL "DEBUG")
        set(suffix "/debug")
    endif()

    z_vcpkg_make_default_path_and_configure_options(options CONFIG "${config}")
    expect_option("${options}" "--prefix=/fixture/installed${suffix}")
    expect_option("${options}" "--enable-static")

    z_vcpkg_make_default_path_and_configure_options(options CONFIG "${config}" EXCLUDE_FILTER "")
    expect_option("${options}" "--prefix=/fixture/installed${suffix}")
    expect_option("${options}" "--enable-static")

    z_vcpkg_make_default_path_and_configure_options(options CONFIG "${config}" EXCLUDE_FILTER "^--enable-static$")
    expect_option("${options}" "--prefix=/fixture/installed${suffix}")
    if("--enable-static" IN_LIST options)
        message(FATAL_ERROR "The explicit exclusion did not remove --enable-static")
    endif()
endforeach()
message(STATUS "Release and debug configure paths survive omitted and empty exclusion filters")
