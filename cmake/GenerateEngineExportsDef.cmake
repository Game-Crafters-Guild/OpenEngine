# GenerateEngineExportsDef.cmake — PRE_LINK build step that produces the
# `exports.def` consumed by Engine.dll's linker.
#
# Why we don't rely on CMake's built-in WINDOWS_EXPORT_ALL_SYMBOLS:
#   That property's auto-def generation walks only the SHARED target's own
#   directly-compiled .obj files. OBJECT libraries reached via target_sources
#   `$<TARGET_OBJECTS:...>` or target_link_libraries() are NOT walked on the
#   Visual Studio generator (known CMake gap, tracked upstream for years).
#   So module-public symbols like ?InvokeInDomain@CoreCLRHost@..., FontAtlas's
#   methods, GenerationalManager::Create, etc. — all the things Engine.dll
#   needs to export so Editor.exe/GameEngine.Native/Player can resolve them
#   across the DLL boundary — silently get dropped from the export table.
#
# What this script does:
#   1. Glob the .obj files of Engine's own compile and read the exact
#      TARGET_OBJECTS-generated .obj list for every OBJECT module.
#   2. Write that combined list to an objlist file.
#   3. Invoke `cmake -E __create_def` (the same internal helper CMake uses for
#      WINDOWS_EXPORT_ALL_SYMBOLS, just with our complete object list).
#   4. Apply the same PCH-sentinel filters FilterExportsDef.cmake did.
#
# The linker is given /DEF:<OUTPUT_DEF>, and the Visual Studio link tracker
# relinks Engine.dll whenever that input is newer than the DLL. The definition
# is therefore generated beside OUTPUT_DEF and copied over it only when its
# content differs, so an unchanged export list never triggers a link. The
# generation itself, the cost of a build that compiled nothing, is skipped when
# no input changed since the last one completed: the object set, any object,
# the module object list, this script and its filter. The `.inputs` record
# beside OUTPUT_DEF holds that object set. The project file is read only to
# derive the object set, so its timestamp is not an input: a regenerated
# project that compiles the same objects leaves the definition up to date.
#
# Args (set via -D on the cmake invocation):
#   ENGINE_OBJ_DIR  - absolute path to Engine.dir/<Config>/ (Engine's own .obj;
#                     VS-generator layout — empty under Ninja, where Engine's own
#                     objects arrive via MODULE_OBJECTS_FILE instead)
#   ENGINE_PROJECT_FILE - optional Visual Studio project file for exact Engine-owned sources
#   MODULE_OBJECTS_FILE - newline-separated TARGET_OBJECTS-generated .obj list
#                     (Engine's own objects + every OBJECT module)
#   OUTPUT_DEF      - absolute path to write exports.def to

foreach(_v ENGINE_OBJ_DIR MODULE_OBJECTS_FILE OUTPUT_DEF)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "GenerateEngineExportsDef: ${_v} not set")
    endif()
endforeach()

string(REPLACE "\"" "" ENGINE_OBJ_DIR "${ENGINE_OBJ_DIR}")
if(DEFINED ENGINE_PROJECT_FILE)
    string(REPLACE "\"" "" ENGINE_PROJECT_FILE "${ENGINE_PROJECT_FILE}")
endif()
string(REPLACE "\"" "" MODULE_OBJECTS_FILE "${MODULE_OBJECTS_FILE}")
string(REPLACE "\"" "" OUTPUT_DEF "${OUTPUT_DEF}")

set(_all_objs "")

# Engine's own object files. Prefer the Visual Studio project file when it is
# available so stale .obj files left behind by source removals do not leak into
# the export table.
file(GLOB_RECURSE _engine_objs_all "${ENGINE_OBJ_DIR}/*.obj")
set(_engine_stale_objs 0)
if(DEFINED ENGINE_PROJECT_FILE AND EXISTS "${ENGINE_PROJECT_FILE}")
    file(STRINGS "${ENGINE_PROJECT_FILE}" _engine_project_compile_lines REGEX "<ClCompile Include=")
    set(_engine_current_obj_names "")
    foreach(_line IN LISTS _engine_project_compile_lines)
        if(_line MATCHES "<ClCompile Include=\"([^\"]+)\"")
            # NAME_WLE, not NAME_WE: MSVC derives the .obj name by stripping only
            # the LAST extension, so `Foo.gen.cpp` compiles to `Foo.gen.obj`.
            # NAME_WE strips the longest extension and would yield `Foo.obj` —
            # missing the live object and vouching for a stale `Foo.obj` instead.
            get_filename_component(_src_wle "${CMAKE_MATCH_1}" NAME_WLE)
            list(APPEND _engine_current_obj_names "${_src_wle}.obj")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _engine_current_obj_names)

    foreach(_obj IN LISTS _engine_objs_all)
        get_filename_component(_obj_name "${_obj}" NAME)
        if(_obj_name IN_LIST _engine_current_obj_names)
            list(APPEND _all_objs "${_obj}")
        else()
            math(EXPR _engine_stale_objs "${_engine_stale_objs} + 1")
        endif()
    endforeach()
else()
    list(APPEND _all_objs ${_engine_objs_all})
endif()
if(_engine_stale_objs GREATER 0)
    message(STATUS "GenerateEngineExportsDef: ignored ${_engine_stale_objs} stale Engine object files")
endif()

# Each OBJECT module's object files. This file is generated at configure time
# from $<TARGET_OBJECTS:...> expressions, so it includes only sources that are
# still part of the build graph. Do not glob module directories here: Visual
# Studio leaves stale .obj files behind after source moves/removals, and those
# stale objects can produce bogus Engine.dll exports.
if(NOT EXISTS "${MODULE_OBJECTS_FILE}")
    message(FATAL_ERROR "GenerateEngineExportsDef: module object list not found: ${MODULE_OBJECTS_FILE}")
endif()

file(STRINGS "${MODULE_OBJECTS_FILE}" _module_objs)
set(_missing_module_objs 0)
foreach(_obj IN LISTS _module_objs)
    if(_obj STREQUAL "")
        continue()
    endif()
    if(EXISTS "${_obj}")
        list(APPEND _all_objs "${_obj}")
    else()
        math(EXPR _missing_module_objs "${_missing_module_objs} + 1")
    endif()
endforeach()
if(_missing_module_objs GREATER 0)
    message(STATUS "GenerateEngineExportsDef: WARNING - ${_missing_module_objs} listed module object files are missing")
endif()

# The VS glob above and the TARGET_OBJECTS list overlap for Engine's own objects.
list(REMOVE_DUPLICATES _all_objs)

list(LENGTH _all_objs _obj_count)
message(STATUS "GenerateEngineExportsDef: collected ${_obj_count} object files for export")

# The objlist: one path per line, native separators.
set(_objlist_content "")
foreach(_obj IN LISTS _all_objs)
    file(TO_NATIVE_PATH "${_obj}" _native)
    string(APPEND _objlist_content "${_native}\n")
endforeach()

# The definition is up to date when the last generation that completed read the
# same object set and no input is newer than its record. IS_NEWER_THAN is also
# true for equal timestamps and for a missing file, so any doubt regenerates.
set(_inputs_record "${OUTPUT_DEF}.inputs")
set(_defined_filter_script "${CMAKE_CURRENT_LIST_DIR}/FilterDefinedExports.ps1")
set(_generation_inputs ${_all_objs} "${MODULE_OBJECTS_FILE}" "${CMAKE_CURRENT_LIST_FILE}" "${_defined_filter_script}")
if(EXISTS "${OUTPUT_DEF}" AND EXISTS "${_inputs_record}")
    file(READ "${_inputs_record}" _recorded_objlist)
    if(_recorded_objlist STREQUAL _objlist_content)
        set(_inputs_changed FALSE)
        foreach(_input IN LISTS _generation_inputs)
            if("${_input}" IS_NEWER_THAN "${_inputs_record}")
                set(_inputs_changed TRUE)
                break()
            endif()
        endforeach()
        if(NOT _inputs_changed)
            message(STATUS "GenerateEngineExportsDef: ${OUTPUT_DEF} up to date (${_obj_count} objects unchanged)")
            return()
        endif()
    endif()
endif()

set(_objlist "${OUTPUT_DEF}.objlist")
file(WRITE "${_objlist}" "${_objlist_content}")

# Locate the cmake executable that's currently running this script.
set(_cmake_exe "${CMAKE_COMMAND}")
if(NOT _cmake_exe OR NOT EXISTS "${_cmake_exe}")
    message(FATAL_ERROR "GenerateEngineExportsDef: CMAKE_COMMAND not usable: ${_cmake_exe}")
endif()

# Invoke `cmake -E __create_def` with our combined objlist. This is the same
# internal helper CMake uses for WINDOWS_EXPORT_ALL_SYMBOLS — we just hand it
# the union of Engine's own .obj files + every OBJECT lib's .obj files.
set(_staged_def "${OUTPUT_DEF}.staged")
file(REMOVE "${_staged_def}")
execute_process(
    COMMAND "${_cmake_exe}" -E __create_def "${_staged_def}" "${_objlist}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "GenerateEngineExportsDef: __create_def failed (rc=${_rc})\nstdout: ${_out}\nstderr: ${_err}")
endif()

# Filter the .def. After bundling 30+ OBJECT modules into Engine.dll's exports
# we have ~125k entries — well over MSVC's 65535 export limit. The vast majority
# are header-instantiated template symbols and lambda-scoped RTTI that consumers
# can (and do) re-instantiate locally rather than import across the DLL boundary.
#
# Drop, in order of bulk:
#   - `??$` template-function instantiations (~49k). Each consumer's TU that
#     uses the same template gets its own copy at compile time; exporting the
#     instantiation is duplicative.
#   - `<lambda_` lambda-scoped symbols (~9k). These are local to the TU.
#   - Lambda-scoped RTTI (??_R0?A.<lambda) — DATA entries for lambdas.
# Keep all non-template, non-lambda C++ symbols + the PCH-sentinel filter from
# the original FilterExportsDef.cmake.
# CRT inline helpers that surface as weak-defined symbols in OBJECT-lib .obj
# files (the UCRT headers define `wmemcmp`, `wmemcpy`, `wmemset`, `wmemchr`,
# and a few vsnprintf-style helpers as inline). The linker treats them as
# imports from `ucrtbased.dll` / `ucrtbase.dll` at the final link stage, so
# trying to re-export them from Engine.dll produces LNK2001 (no definition
# in the link inputs). Hard-coded blacklist — these names are stable.
set(_GE_CRT_INLINE_NAMES
    wmemcmp wmemcpy wmemset wmemchr wmemmove
    vsnprintf vsnprintf_s vsnprintf_l vsnprintf_s_l
    vsnwprintf vsnwprintf_s
    snprintf snprintf_s
    swprintf swprintf_s
    sprintf sprintf_s
)

# Private C dependency helpers that can surface in CMake's __create_def output
# when an OBJECT module references a DLL-backed C library. These are not Engine
# ABI and may not be present in the dependency import library.
set(_GE_PRIVATE_C_DEPENDENCY_NAMES
    next_token
    te_free_parameters
)

# Each filter is one list(FILTER) pass over what the earlier ones kept, so a
# line is counted once, by the first filter in this order that matches it. A
# per-line loop over the ~190k lines __create_def writes cost more than half a
# minute of every regeneration.
file(STRINGS "${_staged_def}" _kept)
# _ge_drop_matching(<count var> <regex>): drops the lines matching <regex> and
# adds how many went to <count var>.
function(_ge_drop_matching count_var regex)
    list(LENGTH _kept _before)
    list(FILTER _kept EXCLUDE REGEX "${regex}")
    list(LENGTH _kept _after)
    math(EXPR _dropped "${_before} - ${_after}")
    set(_kept "${_kept}" PARENT_SCOPE)
    set(${count_var} ${_dropped} PARENT_SCOPE)
endfunction()
# PCH-sentinel pathologies (from CMake's __create_def).
_ge_drop_matching(_stripped_pch "^[ \t]+__[ \t]*(DATA)?[ \t]*$")
_ge_drop_matching(_stripped_pch_sym "__@@_PchSym_@")
math(EXPR _stripped_pch "${_stripped_pch} + ${_stripped_pch_sym}")
# Auto-exported DATA symbols commonly include RTTI, function-local statics,
# and implementation globals; these are not stable Engine DLL ABI.
_ge_drop_matching(_stripped_data "[ \t]+DATA[ \t]*$")
# CRT inline helpers — see _GE_CRT_INLINE_NAMES comment above.
list(JOIN _GE_CRT_INLINE_NAMES "|" _ge_crt_names)
_ge_drop_matching(_stripped_crt "^[ \t]+(${_ge_crt_names})[ \t]*$")
# Private C dependency helpers — see _GE_PRIVATE_C_DEPENDENCY_NAMES.
list(JOIN _GE_PRIVATE_C_DEPENDENCY_NAMES "|" _ge_private_c_names)
_ge_drop_matching(_stripped_private_c "^[ \t]+(${_ge_private_c_names})[ \t]*$")
# NVTX v3's header-only implementation emits a `nvtx*_v3` trampoline per API
# entry point in whichever TU includes it (NvtxRange.cpp). They are upstream
# internals, not Engine ABI, and re-exporting them would invite a consumer
# to bind to NVTX through Engine.dll.
_ge_drop_matching(_stripped_nvtx "^[ \t]+nvtx[A-Za-z0-9_]*[ \t]*$")
# The global allocation functions (operator new, delete, new[], delete[]:
# `??2@Y`, `??3@Y`, `??_U@Y`, `??_V@Y`) that the allocation counter's hook
# defines in Engine.dll under GE_DEBUG_INSTRUMENTATION. Each image carries its
# own replacement; exporting Engine's would let an image without one bind to it.
# The same prefixes cover the placement forms (`operator new(size_t, void*)`,
# `??2@YAPEAX_KPEAX@Z`), which Engine.dll exports in every configuration, hook or
# not; they go too, harmlessly: <vcruntime_new.h> defines them inline.
_ge_drop_matching(_stripped_allocation "^[ \t]+\\?\\?(2|3|_U|_V)@Y")
# Lambda-scoped symbols. The `<lambda_` substring appears in any symbol
# whose enclosing scope is a lambda, including the lambda's call operator,
# RTTI of types declared inside, and so on.
_ge_drop_matching(_stripped_lambda "<lambda_")
# Template-function instantiations: mangled name starts with `??$`.
_ge_drop_matching(_stripped_template "^[ \t]+\\?\\?\\$")
# Member functions of template classes: pattern `<methodname>@?$<class>` —
# method scoped inside a template instantiation. Drops std::vector::push_back,
# std::unordered_map::_Insert, etc. Consumers re-instantiate these locally.
_ge_drop_matching(_stripped_tmember "@\\?\\$")
# Operators on template classes: `??<op>?$<class>@...` — operator=, ctor,
# dtor, etc., dispatched on a template instantiation. Same rationale.
_ge_drop_matching(_stripped_toperator "^[ \t]+\\?\\?[0-9A-Z_]+\\?\\$")
math(EXPR _stripped_tmember "${_stripped_tmember} + ${_stripped_toperator}")

list(JOIN _kept "\n" _kept_text)
list(LENGTH _kept _kept_count)
if(_kept_count GREATER 0)
    string(APPEND _kept_text "\n")
endif()
file(WRITE "${_staged_def}" "${_kept_text}")

find_program(_powershell_exe NAMES powershell pwsh)
if(_powershell_exe)
    execute_process(
        COMMAND "${_powershell_exe}" -NoProfile -ExecutionPolicy Bypass
                -File "${_defined_filter_script}"
                -Objlist "${_objlist}"
                -Def "${_staged_def}"
        RESULT_VARIABLE _defined_filter_rc
        OUTPUT_VARIABLE _defined_filter_out
        ERROR_VARIABLE _defined_filter_err
    )
    if(NOT _defined_filter_rc EQUAL 0)
        message(FATAL_ERROR "GenerateEngineExportsDef: defined-symbol filter failed (rc=${_defined_filter_rc})\nstdout: ${_defined_filter_out}\nstderr: ${_defined_filter_err}")
    endif()
    string(STRIP "${_defined_filter_out}" _defined_filter_out)
    if(_defined_filter_out)
        message(STATUS "GenerateEngineExportsDef: ${_defined_filter_out}")
    endif()
else()
    message(STATUS "GenerateEngineExportsDef: powershell unavailable; skipping defined-symbol filtering")
endif()

file(STRINGS "${_staged_def}" _final_lines)
list(LENGTH _final_lines _final_count)
set(_def_state "updated")
if(EXISTS "${OUTPUT_DEF}")
    file(SHA256 "${OUTPUT_DEF}" _current_hash)
    file(SHA256 "${_staged_def}" _staged_hash)
    if(_current_hash STREQUAL _staged_hash)
        set(_def_state "unchanged")
    endif()
endif()
if(_def_state STREQUAL "updated")
    file(COPY_FILE "${_staged_def}" "${OUTPUT_DEF}")
endif()
file(REMOVE "${_staged_def}")
file(WRITE "${_inputs_record}" "${_objlist_content}")
message(STATUS "GenerateEngineExportsDef: ${OUTPUT_DEF} ${_def_state} - ${_final_count} exports kept (objs=${_obj_count}, stripped: pch=${_stripped_pch}, data=${_stripped_data}, templates=${_stripped_template}, template-members=${_stripped_tmember}, lambdas=${_stripped_lambda}, crt=${_stripped_crt}, private-c=${_stripped_private_c}, nvtx=${_stripped_nvtx}, allocation-functions=${_stripped_allocation})")
