# Contract test for cmake/DotnetLinuxHostRids.cmake, run by ctest as
#   cmake -DSCRIPT=<rid script> -DWORK=<scratch dir> -P <this file>
# Covered: the portable RID is searched first, a distro pack for the same
# architecture follows, musl and other-architecture packs are skipped, and a
# root holding only a distro pack still yields it.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "DotnetLinuxHostRidsTest.cmake: SCRIPT and WORK are required")
endif()
include("${SCRIPT}")

function(make_root root)
    file(REMOVE_RECURSE "${root}")
    foreach(_rid IN LISTS ARGN)
        file(MAKE_DIRECTORY "${root}/packs/Microsoft.NETCore.App.Host.${_rid}/10.0.12")
    endforeach()
    file(MAKE_DIRECTORY "${root}/packs/Microsoft.NETCore.App.Ref")
endfunction()

function(expect_rids root arch)
    ge_dotnet_linux_host_rids("${root}" "${arch}" _rids)
    if(NOT _rids STREQUAL ARGN)
        message(FATAL_ERROR "DotnetLinuxHostRidsTest: ${root} ${arch}: expected '${ARGN}', got '${_rids}'")
    endif()
endfunction()

make_root("${WORK}/mixed" linux-x64 linux-musl-x64 fedora.44-x64 ubuntu.24.04-arm64)
expect_rids("${WORK}/mixed" x64 linux-x64 fedora.44-x64)
expect_rids("${WORK}/mixed" arm64 linux-arm64 ubuntu.24.04-arm64)

make_root("${WORK}/fedora" fedora.44-x64)
expect_rids("${WORK}/fedora" x64 linux-x64 fedora.44-x64)

make_root("${WORK}/empty")
expect_rids("${WORK}/empty" x64 linux-x64)
message(STATUS "DotnetLinuxHostRidsTest: passed")
