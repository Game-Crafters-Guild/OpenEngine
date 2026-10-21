# EngineModuleDuplicationCheck.cmake
#
# Engine is one shared library that contains the engine's OBJECT modules (the
# GE_ENGINE_MODULES splice in Engine/CMakeLists.txt). A target that has Engine in its
# process and also links one of those modules, or splices its objects, compiles a second
# copy of the module into its own image: a second allocation counter and stats registry
# (Memory), a second UIManager, a second texture registry. Nothing reports the split; each
# copy only sees half the process.
#
# This pass fails the configure for every executable, shared library and module library
# that does so, and names the fix. Engine re-exports every module's include directories and
# compile definitions, so linking Engine alone loses nothing. Objects reach a target three
# ways, and each is checked: a directly linked OBJECT module, $<TARGET_OBJECTS:module> in
# its own sources, and the same splice in the INTERFACE_SOURCES of a target it links (an
# INTERFACE library such as FileSystemObjects), followed through plain link items.
#
# Out of scope: a link item wrapped in a generator expression (for example
# $<BUILD_INTERFACE:...>) is not followed, and the pass judges each target alone, so a shared
# library that carries a module privately without linking Engine, loaded into a process beside
# Engine, is not refused. A STATIC library that links an engine module carries the module's objects
# in its archive, and the linker pulls the members a target references from there; this
# pass does not walk archives.
#
# It runs once, deferred to the end of the top-level directory, so it sees every target.
# It does nothing where Engine is a static library (the WebAssembly build), whose archive
# members are not linked twice.

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/BuildsystemTargets.cmake)

# The target a link item names, with its alias resolved; empty for a plain library name,
# a linker flag or a generator expression other than $<LINK_ONLY:...>.
function(_ge_link_item_target item out_var)
    set(${out_var} "" PARENT_SCOPE)
    set(_name "${item}")
    if(_name MATCHES "^\\$<LINK_ONLY:([^$<>]+)>$")
        set(_name "${CMAKE_MATCH_1}")
    endif()
    if(_name MATCHES "[$<>]" OR _name MATCHES "^::@" OR NOT TARGET "${_name}")
        return()
    endif()
    get_target_property(_aliased "${_name}" ALIASED_TARGET)
    if(_aliased)
        set(_name "${_aliased}")
    endif()
    set(${out_var} "${_name}" PARENT_SCOPE)
endfunction()

# The targets `target` links directly (its LINK_LIBRARIES), aliases resolved.
function(_ge_direct_link_targets target out_var)
    set(_result "")
    get_target_property(_items "${target}" LINK_LIBRARIES)
    if(_items)
        foreach(_item IN LISTS _items)
            _ge_link_item_target("${_item}" _linked)
            if(_linked)
                list(APPEND _result "${_linked}")
            endif()
        endforeach()
    endif()
    set(${out_var} ${_result} PARENT_SCOPE)
endfunction()

# The engine modules that `sources` splice with $<TARGET_OBJECTS:module>, appended to the
# list named by `out_var`. The splice may sit inside another generator expression
# ($<BUILD_INTERFACE:...>, a configuration condition), so the match is not anchored.
function(_ge_append_spliced_modules sources engine_modules out_var)
    set(_result ${${out_var}})
    foreach(_source IN LISTS sources)
        if(_source MATCHES "\\$<TARGET_OBJECTS:([^$<>]+)>")
            set(_spliced "${CMAKE_MATCH_1}")
            if(_spliced IN_LIST engine_modules)
                list(APPEND _result "${_spliced}")
            endif()
        endif()
    endforeach()
    set(${out_var} ${_result} PARENT_SCOPE)
endfunction()

# The engine modules spliced into `target` through the INTERFACE_SOURCES of the targets it
# links and of their usage requirements (INTERFACE_LINK_LIBRARIES, transitively). A
# $<LINK_ONLY:...> item carries no usage requirements, so it is not followed.
function(_ge_interface_spliced_modules target engine_modules out_var)
    set(_result "")
    get_target_property(_worklist "${target}" LINK_LIBRARIES)
    if(NOT _worklist)
        set(_worklist "")
    endif()
    set(_seen "")
    while(_worklist)
        list(POP_FRONT _worklist _item)
        if(_item MATCHES "^\\$<LINK_ONLY:")
            continue()
        endif()
        _ge_link_item_target("${_item}" _linked)
        if(NOT _linked OR _linked IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_linked}")
        get_target_property(_interface_sources "${_linked}" INTERFACE_SOURCES)
        if(_interface_sources)
            _ge_append_spliced_modules("${_interface_sources}" "${engine_modules}" _result)
        endif()
        get_target_property(_interface_links "${_linked}" INTERFACE_LINK_LIBRARIES)
        if(_interface_links)
            list(APPEND _worklist ${_interface_links})
        endif()
    endwhile()
    set(${out_var} ${_result} PARENT_SCOPE)
endfunction()

# TRUE when `wanted` is reachable from `target` through any link, direct or transitive.
# Private links count: a shared library that links Engine privately still loads it into
# every process that loads the library.
function(_ge_reaches_target target wanted out_var)
    set(_worklist "${target}")
    set(_seen "")
    while(_worklist)
        list(POP_FRONT _worklist _current)
        if(_current IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_current}")
        set(_items "")
        get_target_property(_imported "${_current}" IMPORTED)
        if(NOT _imported)
            get_target_property(_private "${_current}" LINK_LIBRARIES)
            if(_private)
                list(APPEND _items ${_private})
            endif()
        endif()
        get_target_property(_interface "${_current}" INTERFACE_LINK_LIBRARIES)
        if(_interface)
            list(APPEND _items ${_interface})
        endif()
        foreach(_item IN LISTS _items)
            _ge_link_item_target("${_item}" _linked)
            if(_linked STREQUAL wanted)
                set(${out_var} TRUE PARENT_SCOPE)
                return()
            endif()
            if(_linked)
                list(APPEND _worklist "${_linked}")
            endif()
        endforeach()
    endwhile()
    set(${out_var} FALSE PARENT_SCOPE)
endfunction()

function(ge_check_engine_module_duplication)
    if(NOT TARGET Engine)
        return()
    endif()
    get_target_property(_engine_type Engine TYPE)
    if(NOT _engine_type STREQUAL "SHARED_LIBRARY")
        return()
    endif()

    _ge_direct_link_targets(Engine _engine_links)
    set(_engine_modules "")
    foreach(_linked IN LISTS _engine_links)
        get_target_property(_type "${_linked}" TYPE)
        if(_type STREQUAL "OBJECT_LIBRARY")
            list(APPEND _engine_modules "${_linked}")
        endif()
    endforeach()

    ge_collect_buildsystem_targets("${CMAKE_SOURCE_DIR}" _targets)
    set(_violations "")
    foreach(_target IN LISTS _targets)
        if(_target STREQUAL "Engine")
            continue()
        endif()
        get_target_property(_type "${_target}" TYPE)
        if(NOT _type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            continue()
        endif()

        # A directly linked OBJECT library's objects go into the target; a transitive one's
        # do not. $<TARGET_OBJECTS:...> in the sources, or in a linked target's
        # INTERFACE_SOURCES, splices them as well.
        set(_duplicated "")
        _ge_direct_link_targets("${_target}" _links)
        foreach(_linked IN LISTS _links)
            if(_linked IN_LIST _engine_modules)
                list(APPEND _duplicated "${_linked}")
            endif()
        endforeach()
        get_target_property(_sources "${_target}" SOURCES)
        if(_sources)
            _ge_append_spliced_modules("${_sources}" "${_engine_modules}" _duplicated)
        endif()
        _ge_interface_spliced_modules("${_target}" "${_engine_modules}" _interface_spliced)
        list(APPEND _duplicated ${_interface_spliced})
        if(NOT _duplicated)
            continue()
        endif()

        _ge_reaches_target("${_target}" Engine _has_engine)
        if(_has_engine)
            list(REMOVE_DUPLICATES _duplicated)
            list(JOIN _duplicated ", " _duplicated_text)
            string(APPEND _violations "\n  ${_target} links Engine and ${_duplicated_text}")
        endif()
    endforeach()

    if(_violations)
        message(FATAL_ERROR
            "Engine already contains these OBJECT modules, so each target below would carry a "
            "second copy of their state (two allocation counters and stats registries, two of "
            "every module singleton):${_violations}\n"
            "Link Engine alone: it re-exports every module's include directories and compile "
            "definitions. A module Engine does not contain (Apps/Editor splices MeshPicking) "
            "may still be linked or spliced beside it.")
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL ge_check_engine_module_duplication)
