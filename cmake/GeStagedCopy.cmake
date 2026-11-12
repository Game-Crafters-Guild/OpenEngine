# GeStagedCopy.cmake
#
# Shared copy helper for the runtime-staging scripts, robust to a destination
# locked by a running process. On Windows a loaded PE image cannot be
# overwritten or deleted, but it CAN be renamed. `file(COPY_FILE ... ONLY_IF_DIFFERENT)`
# short-circuits to success when the destination is already identical, so a
# copy failure means the staged file is BOTH stale AND locked — exactly the
# case where letting the build exit 0 ships a stale runtime: the relaunched
# process silently runs old code while the developer debugs a phantom.
#
# Strategy: rename the locked stale file aside (the running process keeps its
# old image mapped) and copy the fresh one into place — the build stays green
# AND the staged output is current for the next launch. If even the rename
# fails, FAIL the build loudly; a silently stale staged runtime is never OK.

# _ge_staged_set_aside(<dest-file> <label> <reason>)
#
# Renames a locked staged file to '<name>.stale-<tag>' so that a fresh file can
# take its name; the running process keeps the old file open. If the rename
# fails, the build fails.
function(_ge_staged_set_aside _dest _label _reason)
  get_filename_component(_name "${_dest}" NAME)
  get_filename_component(_dest_dir "${_dest}" DIRECTORY)
  string(RANDOM LENGTH 8 _tag)
  file(RENAME "${_dest}" "${_dest}.stale-${_tag}" RESULT _rename_err)
  if(_rename_err)
    message(FATAL_ERROR
        "${_label}: staged '${_name}' in '${_dest_dir}' is STALE and cannot be "
        "replaced (${_reason}; rename-aside failed: ${_rename_err}). A "
        "running process (Editor/Player?) is holding it — close it and rebuild. "
        "Failing the build instead of leaving a stale runtime staged.")
  endif()
  message(STATUS
      "${_label}: '${_name}' was locked by a running process — renamed it aside "
      "to '${_name}.stale-${_tag}' (a later build cleans it up).")
endfunction()

# ge_staged_copy(<src-file> <dest-dir> <label>)
#
# The copy runs in-process: these scripts stage dozens of files on every build
# (the Visual Studio generator runs POST_BUILD steps even when nothing relinked),
# and a cmake process per file costs more than the comparisons themselves.
function(ge_staged_copy _src _dest_dir _label)
  get_filename_component(_name "${_src}" NAME)
  set(_dest "${_dest_dir}/${_name}")
  file(COPY_FILE "${_src}" "${_dest}" ONLY_IF_DIFFERENT RESULT _rc)
  if(_rc EQUAL 0)
    return()
  endif()

  _ge_staged_set_aside("${_dest}" "${_label}" "copy rc=${_rc}")
  file(COPY_FILE "${_src}" "${_dest}" ONLY_IF_DIFFERENT RESULT _rc2)
  if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR
        "${_label}: failed staging '${_src}' -> '${_dest_dir}' (rc=${_rc2}) even "
        "after renaming the locked destination aside. The staged runtime is "
        "STALE — failing the build instead of shipping it.")
  endif()
endfunction()

# Best-effort cleanup of rename-aside leftovers. Files still mapped by a live
# process survive REMOVE silently and get another chance next build.
function(ge_staged_copy_cleanup _dest_dir)
  file(GLOB _ge_stale_leftovers "${_dest_dir}/*.stale-*")
  if(_ge_stale_leftovers)
    file(REMOVE ${_ge_stale_leftovers})
  endif()
endfunction()

# ge_staged_link_debug_symbols(<module-file> <dest-dir> <label>)
#
# Stages the PDB beside a Windows module into <dest-dir> as a hard link, so a
# copy of the staged folder carries the PDB of each module it loads. A hard link
# has the timestamp of its file; a link to a PDB that the linker replaced is older.
function(ge_staged_link_debug_symbols _module _dest_dir _label)
  get_filename_component(_module_dir "${_module}" DIRECTORY)
  get_filename_component(_stem "${_module}" NAME_WLE)
  set(_src "${_module_dir}/${_stem}.pdb")
  set(_dest "${_dest_dir}/${_stem}.pdb")
  if(NOT EXISTS "${_src}")
    return()
  endif()
  if(EXISTS "${_dest}" AND "${_dest}" IS_NEWER_THAN "${_src}")
    return()
  endif()

  file(CREATE_LINK "${_src}" "${_dest}" RESULT _rc COPY_ON_ERROR)
  if(_rc EQUAL 0)
    return()
  endif()

  _ge_staged_set_aside("${_dest}" "${_label}" "link: ${_rc}")
  file(CREATE_LINK "${_src}" "${_dest}" RESULT _rc2 COPY_ON_ERROR)
  if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "${_label}: failed staging '${_src}' -> '${_dest_dir}' (${_rc2}).")
  endif()
endfunction()
