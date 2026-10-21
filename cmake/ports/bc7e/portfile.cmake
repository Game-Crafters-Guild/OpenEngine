# bc7e.ispc from bc7enc_rdo at a pinned commit (the last change to the kernel is
# c4a839c5, 2021-03-06). Only the kernel is built; the repository's other
# encoders and its bundled ispc.exe are not used.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

set(GE_BC7E_REF b9438627eef73a1157e84201b6fa6eb2ffd6d9f0)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO richgel999/bc7enc_rdo
    REF ${GE_BC7E_REF}
    SHA512 ddb91ebcc2899fc81efdb53883c117653ffb4055049e4e7c41ee2c41635f70c5aa95e5c36e2dede7d8a541f5d9cbb06dc83fa116a5f04b0cf14b3beebfcb3f70
    HEAD_REF master
)

# The repository carries the Apache-2.0 grant for bc7e.ispc in its LICENSE
# file but not the licence text itself.
vcpkg_download_distfile(GE_APACHE_LICENSE
    URLS "https://www.apache.org/licenses/LICENSE-2.0.txt"
    FILENAME "apache-license-2.0.txt"
    SHA512 98f6b79b778f7b0a15415bd750c3a8a097d650511cb4ec8115188e115c47053fe700f578895c097051c9bc3dfb6197c2b13a15de203273e1a3218884f86e90e8
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/bc7e_isa.ispc"
     DESTINATION "${SOURCE_PATH}")

# BC7E_VERSION names what decides the encoder's bytes, the kernel and the
# compiler (CMakeLists.txt appends the pinned CPU on arm64); the texture cook
# folds it into its cache key.
set(GE_ISPC "${CURRENT_HOST_INSTALLED_DIR}/tools/ispc/bin/ispc${VCPKG_HOST_EXECUTABLE_SUFFIX}")
execute_process(COMMAND "${GE_ISPC}" --version OUTPUT_VARIABLE GE_ISPC_VERSION_TEXT COMMAND_ERROR_IS_FATAL ANY)
if(NOT GE_ISPC_VERSION_TEXT MATCHES "ISPC[^,]*, ([0-9]+[.][0-9]+[.][0-9]+)")
    message(FATAL_ERROR "bc7e: cannot read the ISPC version from: ${GE_ISPC_VERSION_TEXT}")
endif()
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_ISPC_COMPILER=${GE_ISPC}"
        "-DBC7E_VERSION=${GE_BC7E_REF}-ispc${CMAKE_MATCH_1}"
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-bc7e)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

file(READ "${SOURCE_PATH}/LICENSE" GE_BC7E_REPO_LICENSE)
file(READ "${GE_APACHE_LICENSE}" GE_APACHE_TEXT)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright"
    "bc7e.ispc: Copyright (C) 2018-2021 Binomial LLC, Apache License 2.0.\n"
    "Source: https://github.com/richgel999/bc7enc_rdo at ${GE_BC7E_REF}.\n\n"
    "${GE_BC7E_REPO_LICENSE}\n\n${GE_APACHE_TEXT}")
