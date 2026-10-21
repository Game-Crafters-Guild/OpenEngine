cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SRC_DIR OR NOT DEFINED DST_DIR)
  message(FATAL_ERROR "StageShaderPkgs.cmake requires -DSRC_DIR=... -DDST_DIR=...")
endif()

file(MAKE_DIRECTORY "${DST_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/GeStagedCopy.cmake")
ge_staged_copy_cleanup("${DST_DIR}")

# Copies run in-process and only when the content differs: this runs on every
# build. A failed copy retries after renaming the destination aside; an
# unrecoverable failure stops the build instead of leaving stale shaders.
# Copy .shaderpkg files (packaged shader bundles)
file(GLOB _PKGS RELATIVE "${SRC_DIR}" "${SRC_DIR}/*.shaderpkg")
foreach(_F IN LISTS _PKGS)
  ge_staged_copy("${SRC_DIR}/${_F}" "${DST_DIR}" "StageShaderPkgs")
endforeach()

# Copy individual .spv files (compiled SPIR-V shaders used by UI, etc.)
file(GLOB _SPVS RELATIVE "${SRC_DIR}" "${SRC_DIR}/*.spv")
foreach(_F IN LISTS _SPVS)
  ge_staged_copy("${SRC_DIR}/${_F}" "${DST_DIR}" "StageShaderPkgs")
endforeach()

