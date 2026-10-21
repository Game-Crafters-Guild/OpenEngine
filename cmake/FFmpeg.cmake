# FFmpeg.cmake
#
# ge_find_ffmpeg(): finds the FFmpeg libraries the desktop video backends link
# (avformat, avcodec, swscale, avutil) in the vcpkg install tree, and sets in
# the caller's scope:
#   GE_FFMPEG_FOUND        TRUE when the headers and all four libraries exist
#   GE_FFMPEG_INCLUDE_DIR  the include root holding libavcodec/, libavformat/, ...
#   GE_FFMPEG_LIBRARIES    the four libraries, in link order
#
# vcpkg's FindFFMPEG is not used. Its body runs only while FFMPEG_FOUND is not
# cached, and its vcpkg-cmake-wrapper appends system libraries (secur32,
# ncrypt, crypt32) only when that body did not run, so a tree's first configure
# and every later one would link different library lists, and the first
# reconfigure of every tree would rewrite Engine's project and relink
# Engine.dll. This find gives every configure the list the first one resolves. Every engine
# triplet links FFmpeg dynamically, so the libraries' own dependencies stay inside their DLLs
# and shared objects.

include(SelectLibraryConfigurations)

function(ge_find_ffmpeg)
    set(GE_FFMPEG_FOUND FALSE PARENT_SCOPE)
    if(DEFINED VCPKG_INSTALLED_DIR)
        set(_ge_installed_dir "${VCPKG_INSTALLED_DIR}")
    elseif(DEFINED _VCPKG_INSTALLED_DIR)
        set(_ge_installed_dir "${_VCPKG_INSTALLED_DIR}")
    else()
        return() # FFmpeg comes from the vcpkg manifest only
    endif()
    set(_ge_root "${_ge_installed_dir}/${VCPKG_TARGET_TRIPLET}")

    find_path(GE_FFMPEG_INCLUDE_DIR NAMES libavcodec/avcodec.h
        PATHS "${_ge_root}/include" NO_DEFAULT_PATH)
    if(NOT GE_FFMPEG_INCLUDE_DIR)
        return()
    endif()

    set(_ge_libraries "")
    foreach(_ge_name IN ITEMS avformat avcodec swscale avutil)
        find_library(GE_FFMPEG_${_ge_name}_LIBRARY_RELEASE NAMES ${_ge_name}
            PATHS "${_ge_root}/lib" NO_DEFAULT_PATH)
        find_library(GE_FFMPEG_${_ge_name}_LIBRARY_DEBUG NAMES ${_ge_name}d ${_ge_name}
            PATHS "${_ge_root}/debug/lib" NO_DEFAULT_PATH)
        select_library_configurations(GE_FFMPEG_${_ge_name})
        if(NOT GE_FFMPEG_${_ge_name}_LIBRARY)
            return()
        endif()
        list(APPEND _ge_libraries ${GE_FFMPEG_${_ge_name}_LIBRARY})
    endforeach()

    set(GE_FFMPEG_FOUND TRUE PARENT_SCOPE)
    set(GE_FFMPEG_LIBRARIES "${_ge_libraries}" PARENT_SCOPE)
endfunction()
