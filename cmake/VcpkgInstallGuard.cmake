# vcpkg install-root guard.
#
# A build directory's object files are only valid against the exact set of
# vcpkg packages they were compiled against. Nothing in vcpkg or CMake ties the
# two together: the vcpkg toolchain registers vcpkg.json and
# vcpkg-configuration.json as configure dependencies and nothing else, so the
# installed tree can be replaced under a build directory without that build
# directory noticing. The next build recompiles the sources that changed and
# the objects that read a header newer than themselves (MSBuild and Ninja both
# record every header a compile read, the installed ones included, and compare
# timestamps), relinks everything against the new import libraries, and stages
# the new runtime DLLs. A reinstalled header is not necessarily newer: a package
# built from source keeps its source archive's timestamps, which can predate
# every object here. When nothing recompiles against a library that changed,
# the result links and stages without one diagnostic and can fault at startup
# inside an unrelated subsystem, because two objects in the same DLL disagree
# about a type's layout.
#
# Three checks, at the points where that state is producible.
#
# ge_check_vcpkg_install_root_not_shared() runs BEFORE any installer, because
# it is the write into a shared root that damages every other build directory
# on the machine. Both spellings of sharing are refused — a link, and a plain
# path outside the build directory — because the installer writes through
# either one the same way. Each build tree restores its own install from the
# vcpkg binary cache in seconds, so sharing one buys nothing and costs that
# entire failure class.
#
# ge_check_vcpkg_install_root_written_here() runs BEFORE any installer too, and
# covers the root a build directory owns outright but did not produce. vcpkg
# records the manifest that produced an install root, and a root produced by
# another tree was resolved against that tree's vcpkg checkout and toolset, so
# its packages can carry an ABI this tree never computes. Neither other check
# sees it: the root check above is satisfied by a real directory in the right
# place, and the set check below adopts whatever a build directory that has
# never recorded a set first shows it.
#
# ge_check_vcpkg_install_set_unchanged() runs AFTER every installer, because it
# compares against the post-install state. It records the installed package
# set in the build directory and compares each package this directory already
# compiled against:
#   - a new version (or port-version) is adopted in place: the installer has
#     already replaced the package, and the guard touches the package's
#     installed headers (the include/ entries of its vcpkg/info file list), so
#     the next build recompiles exactly the objects that include them, whatever
#     timestamps the package was installed with. A package whose version stayed
#     but which was rebuilt against a dependency that moved version is adopted
#     the same way;
#   - a new ABI at the same version, with no moved dependency, is a refusal.
#     That is a toolset update (the ABI hash covers the compiler), a moved vcpkg
#     checkout or a port rebuilt with other flags: the headers are unchanged, so
#     nothing would recompile against the library that changed under them.
#
# Scope: only packages present in BOTH the recorded and the current set, and
# differing, are compared. An added package cannot invalidate an object that
# never included its headers (turning a manifest feature on must not force a
# rebuild), and a removed one produces a loud missing-library link error. The
# record is the union of what it held and what is installed, so a package that
# leaves the set keeps its recorded identity: coming back at a different ABI is
# a comparison, not a fresh record, and a short read of the status database
# cannot shrink the record into "nothing left to compare".

# Reads ${statusFile} and returns, in ${outVar}, one "name|version#port-version|abi"
# line per installed package of ${triplet}, sorted by name. An empty triplet
# accepts every architecture. The status database is append-only: a package's
# last paragraph is its current state, and a package whose last paragraph is
# not "install ok installed" is not installed.
function(z_ge_vcpkg_installed_set statusFile triplet outVar)
    file(READ "${statusFile}" statusText)
    # Normalize so every paragraph is "\nPackage: ...\n(line\n)*" regardless of
    # position in the file, and CRLF cannot break line-anchored matching.
    string(REPLACE "\r\n" "\n" statusText "${statusText}")
    set(statusText "\n${statusText}\n")
    # A ";" in the data (a Description carries prose) would otherwise split the
    # matched paragraph into two list elements, one without its Package line and
    # one without its Status line, dropping the package from the set unseen.
    string(REPLACE ";" "\;" statusText "${statusText}")

    set(names "")
    string(REGEX MATCHALL "\nPackage: [^\n]+\n([^\n]+\n)*" paragraphs "${statusText}")
    foreach(paragraph IN LISTS paragraphs)
        # Feature paragraphs repeat the package name and carry no version.
        if(paragraph MATCHES "\nFeature: ")
            continue()
        endif()
        if(NOT paragraph MATCHES "\nPackage: ([^\n]+)\n")
            continue()
        endif()
        set(name "${CMAKE_MATCH_1}")
        if(NOT triplet STREQUAL "" AND NOT paragraph MATCHES "\nArchitecture: ${triplet}\n")
            continue()
        endif()

        if(NOT name IN_LIST names)
            list(APPEND names "${name}")
        endif()

        if(NOT paragraph MATCHES "\nStatus: install ok installed\n")
            set("z_ge_installed_${name}" "")
            continue()
        endif()

        set(version "")
        if(paragraph MATCHES "\nVersion: ([^\n]*)\n")
            set(version "${CMAKE_MATCH_1}")
        endif()
        set(portVersion 0)
        if(paragraph MATCHES "\nPort-Version: ([0-9]+)\n")
            set(portVersion "${CMAKE_MATCH_1}")
        endif()
        set(abi "")
        if(paragraph MATCHES "\nAbi: ([^\n]*)\n")
            set(abi "${CMAKE_MATCH_1}")
        endif()
        set("z_ge_installed_${name}" "${name}|${version}#${portVersion}|${abi}")
    endforeach()

    list(SORT names)
    set(entries "")
    foreach(name IN LISTS names)
        if(NOT "${z_ge_installed_${name}}" STREQUAL "")
            list(APPEND entries "${z_ge_installed_${name}}")
        endif()
    endforeach()
    list(JOIN entries "\n" text)
    set(${outVar} "${text}" PARENT_SCOPE)
endfunction()

# Reads ${statusFile} and returns, in ${outVar}, one "name|dependency,..." line
# per package of ${triplet} that has dependencies: the union of the Depends
# fields of its installed paragraphs (the package and its features), each
# dependency reduced to its port name.
function(z_ge_vcpkg_installed_depends statusFile triplet outVar)
    file(READ "${statusFile}" statusText)
    string(REPLACE "\r\n" "\n" statusText "${statusText}")
    set(statusText "\n${statusText}\n")
    string(REPLACE ";" "\;" statusText "${statusText}")

    set(names "")
    string(REGEX MATCHALL "\nPackage: [^\n]+\n([^\n]+\n)*" paragraphs "${statusText}")
    foreach(paragraph IN LISTS paragraphs)
        if(NOT paragraph MATCHES "\nPackage: ([^\n]+)\n")
            continue()
        endif()
        set(name "${CMAKE_MATCH_1}")
        if(NOT triplet STREQUAL "" AND NOT paragraph MATCHES "\nArchitecture: ${triplet}\n")
            continue()
        endif()
        if(NOT paragraph MATCHES "\nStatus: install ok installed\n"
           OR NOT paragraph MATCHES "\nDepends: ([^\n]*)\n")
            continue()
        endif()
        string(REPLACE "," ";" dependencies "${CMAKE_MATCH_1}")
        foreach(dependency IN LISTS dependencies)
            string(STRIP "${dependency}" dependency)
            string(REGEX REPLACE "[:[].*$" "" dependency "${dependency}")
            if(NOT dependency STREQUAL "" AND NOT dependency IN_LIST "z_ge_depends_${name}")
                list(APPEND "z_ge_depends_${name}" "${dependency}")
            endif()
        endforeach()
        if(NOT name IN_LIST names)
            list(APPEND names "${name}")
        endif()
    endforeach()

    set(entries "")
    foreach(name IN LISTS names)
        list(JOIN "z_ge_depends_${name}" "," joined)
        list(APPEND entries "${name}|${joined}")
    endforeach()
    list(JOIN entries "\n" text)
    set(${outVar} "${text}" PARENT_SCOPE)
endfunction()

# Compares the packages that appear in both sets with a different version or
# ABI, and sorts them for the guard, formatted for a human:
#   ${movedVar}   adopted in place: a new version ("curl 8.21.0#1 -> 8.22.0#0"),
#                 or the same version rebuilt against a dependency that moved,
#                 directly or through another such package
#                 ("libpng 1.6.50#0 (rebuilt against zlib)")
#   ${movedNamesVar} the names of the packages in ${movedVar}
#   ${abiOnlyVar} refused: a new ABI at the same version with no moved
#                 dependency ("shaderc 2026.3#0 (ABI only)")
# ${dependsText} is z_ge_vcpkg_installed_depends()'s output for the current set.
function(z_ge_vcpkg_changed_packages recordedText currentText dependsText movedVar movedNamesVar abiOnlyVar)
    string(REPLACE "\n" ";" recordedLines "${recordedText}")
    foreach(line IN LISTS recordedLines)
        if(line MATCHES "^([^|]+)\\|([^|]*)\\|(.*)$")
            set("z_ge_was_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}|${CMAKE_MATCH_3}")
        endif()
    endforeach()
    string(REPLACE "\n" ";" dependsLines "${dependsText}")
    foreach(line IN LISTS dependsLines)
        if(line MATCHES "^([^|]+)\\|(.*)$")
            string(REPLACE "," ";" "z_ge_depends_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
        endif()
    endforeach()

    set(moved "")
    set(movedNames "")
    set(abiOnlyNames "")
    string(REPLACE "\n" ";" currentLines "${currentText}")
    foreach(line IN LISTS currentLines)
        if(NOT line MATCHES "^([^|]+)\\|([^|]*)\\|(.*)$")
            continue()
        endif()
        set(name "${CMAKE_MATCH_1}")
        set(nowVersion "${CMAKE_MATCH_2}")
        set(nowAbi "${CMAKE_MATCH_3}")
        if(NOT DEFINED "z_ge_was_${name}")
            continue() # added since the record: cannot invalidate existing objects
        endif()
        set(was "${z_ge_was_${name}}")
        if(was STREQUAL "${nowVersion}|${nowAbi}")
            continue()
        endif()
        string(REGEX REPLACE "\\|.*$" "" wasVersion "${was}")
        if(wasVersion STREQUAL nowVersion)
            list(APPEND abiOnlyNames "${name}")
            set("z_ge_version_${name}" "${nowVersion}")
        else()
            list(APPEND moved "${name} ${wasVersion} -> ${nowVersion}")
            list(APPEND movedNames "${name}")
        endif()
    endforeach()

    # A rebuild against a moved dependency can itself be the dependency another
    # package was rebuilt against, so repeat until a pass adopts nothing.
    set(adopted TRUE)
    while(adopted)
        set(adopted FALSE)
        foreach(name IN LISTS abiOnlyNames)
            foreach(dependency IN LISTS "z_ge_depends_${name}")
                if(dependency IN_LIST movedNames)
                    list(APPEND moved "${name} ${z_ge_version_${name}} (rebuilt against ${dependency})")
                    list(APPEND movedNames "${name}")
                    list(REMOVE_ITEM abiOnlyNames "${name}")
                    set(adopted TRUE)
                    break()
                endif()
            endforeach()
        endforeach()
    endwhile()

    set(abiOnly "")
    foreach(name IN LISTS abiOnlyNames)
        list(APPEND abiOnly "${name} ${z_ge_version_${name}} (ABI only)")
    endforeach()
    set(${movedVar} "${moved}" PARENT_SCOPE)
    set(${movedNamesVar} "${movedNames}" PARENT_SCOPE)
    set(${abiOnlyVar} "${abiOnly}" PARENT_SCOPE)
endfunction()

# Union of ${recordedText} and ${currentText}, one "name|version#port-version|abi"
# line per package sorted by name, the current entry winning. A package absent
# from the current set keeps its recorded identity: a purge does not end what
# this build directory already compiled against.
function(z_ge_vcpkg_merged_set recordedText currentText outVar)
    set(names "")
    foreach(text IN ITEMS "${recordedText}" "${currentText}")
        string(REPLACE "\n" ";" lines "${text}")
        foreach(line IN LISTS lines)
            if(NOT line MATCHES "^([^|]+)\\|")
                continue()
            endif()
            set(name "${CMAKE_MATCH_1}")
            if(NOT name IN_LIST names)
                list(APPEND names "${name}")
            endif()
            set("z_ge_merged_${name}" "${line}")
        endforeach()
    endforeach()

    list(SORT names)
    set(entries "")
    foreach(name IN LISTS names)
        list(APPEND entries "${z_ge_merged_${name}}")
    endforeach()
    list(JOIN entries "\n" text)
    set(${outVar} "${text}" PARENT_SCOPE)
endfunction()

# Both refusals below state their own one-line fix and then share this
# explanation, because the damage is the shared write, not the spelling of the
# sharing.
function(z_ge_refuse_shared_install_root headline installedDir buildDir)
    message(FATAL_ERROR
        "${headline}\n"
        "  Install root: ${installedDir}\n"
        "  Build directory: ${buildDir}\n"
        "  A shared install root is rewritten in place by whichever tree next installs a new\n"
        "  dependency version, under every other build directory pointing at it. Their objects\n"
        "  stay compiled against the libraries that were there before, relink against the ones\n"
        "  that are there now, and produce binaries that crash at startup in whatever subsystem\n"
        "  happens to own a type whose layout moved.\n"
        "  Restoring this manifest into a fresh root from the vcpkg binary cache takes seconds,\n"
        "  and the cache is keyed by package ABI, so it is shared safely between every tree.")
endfunction()

# Refuses an install root this build directory does not own: a junction or a
# symbolic link, or a path resolving outside ${buildDir}. Call before anything
# can install into it.
function(ge_check_vcpkg_install_root_not_shared installedDir buildDir)
    if(installedDir STREQUAL "")
        return()
    endif()
    if(buildDir STREQUAL "")
        message(FATAL_ERROR
            "ge_check_vcpkg_install_root_not_shared: the owning build directory is required")
    endif()

    set(linkTarget "")
    if(IS_SYMLINK "${installedDir}")
        # Checked before EXISTS: a link whose target was deleted still
        # redirects the install that is about to run.
        file(READ_SYMLINK "${installedDir}" linkTarget)
        file(TO_CMAKE_PATH "${linkTarget}" linkTarget)
    elseif(EXISTS "${installedDir}")
        # IS_SYMLINK covers Windows junctions on the CMake this repo builds
        # with; resolving the leaf against its already-resolved parent catches
        # the same redirection without depending on that, and does not fire
        # when the whole build tree merely sits below a link.
        get_filename_component(parentDir "${installedDir}" DIRECTORY)
        get_filename_component(leafName "${installedDir}" NAME)
        get_filename_component(resolvedParent "${parentDir}" REALPATH)
        get_filename_component(resolvedDir "${installedDir}" REALPATH)
        set(expectedDir "${resolvedParent}/${leafName}")
        if(CMAKE_HOST_WIN32)
            string(TOLOWER "${expectedDir}" expectedDir)
            string(TOLOWER "${resolvedDir}" resolvedDir)
        endif()
        if(NOT resolvedDir STREQUAL expectedDir)
            get_filename_component(linkTarget "${installedDir}" REALPATH)
        endif()
    endif()

    if(NOT linkTarget STREQUAL "")
        set(linkNoun "symbolic link")
        if(CMAKE_HOST_WIN32)
            set(linkNoun "junction or symbolic link")
        endif()
        z_ge_refuse_shared_install_root(
            "vcpkg_installed is a ${linkNoun} to ${linkTarget}; delete it and reconfigure so this \
tree restores its own install from the binary cache."
            "${installedDir}" "${buildDir}")
    endif()

    # A plain path into another tree's install root shares it exactly as a link
    # does: the installer writes there, and that tree's build directory is the
    # one damaged. Compared lexically, so a build tree that merely sits below a
    # link is not refused.
    set(ownerDir "${buildDir}")
    set(rootDir "${installedDir}")
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${ownerDir}" ownerDir)
        string(TOLOWER "${rootDir}" rootDir)
    endif()
    cmake_path(IS_PREFIX ownerDir "${rootDir}" NORMALIZE insideBuildDir)
    if(NOT insideBuildDir)
        z_ge_refuse_shared_install_root(
            "vcpkg_installed is outside this build directory; unset VCPKG_INSTALLED_DIR (or \
_VCPKG_INSTALLED_DIR) and reconfigure so this tree installs its own root from the binary cache."
            "${installedDir}" "${buildDir}")
    endif()
endfunction()

# Refuses an install root produced by a tree other than the one configuring.
# ${manifestDir} is the directory whose vcpkg.json this configure installs from
# (VCPKG_MANIFEST_DIR). Call before anything can install into the root, so the
# refusal lands before any later check reports the tree consistent.
#
# vcpkg writes the manifest it installed from to vcpkg/manifest-info.json, so
# the producing tree is on disk to be read. Absence is not a refusal: a root
# nothing has installed into yet has no record, and neither has a classic-mode
# install. Only a record naming another tree is.
function(ge_check_vcpkg_install_root_written_here installedDir manifestDir buildDir)
    if(installedDir STREQUAL "" OR manifestDir STREQUAL "")
        return() # not a vcpkg manifest configure — nothing produced this root
    endif()

    set(infoFile "${installedDir}/vcpkg/manifest-info.json")
    if(NOT EXISTS "${infoFile}")
        # A populated root that carries no record cannot be compared, and
        # "not checked" must never be mistakable for "checked and ours".
        if(EXISTS "${installedDir}/vcpkg/status")
            message(STATUS
                "vcpkg install root: ${installedDir} records no manifest-info.json, so the tree "
                "that produced it is unknown — NOT checked")
        endif()
        return()
    endif()

    file(READ "${infoFile}" infoText)
    string(JSON recordedManifest ERROR_VARIABLE jsonError GET "${infoText}" "manifest-path")
    if(jsonError OR recordedManifest STREQUAL "")
        message(STATUS
            "vcpkg install root: ${infoFile} carries no readable manifest-path, so the tree that "
            "produced ${installedDir} is unknown — NOT checked")
        return()
    endif()

    # REALPATH resolves a tree reached through a link to the same spelling on
    # both sides, and leaves a producer that has since been deleted as it is.
    file(TO_CMAKE_PATH "${recordedManifest}" recordedManifest)
    get_filename_component(recordedDir "${recordedManifest}" DIRECTORY)
    get_filename_component(recordedDir "${recordedDir}" REALPATH)
    get_filename_component(ownerDir "${manifestDir}" REALPATH)
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${recordedDir}" recordedDir)
        string(TOLOWER "${ownerDir}" ownerDir)
    endif()
    if(recordedDir STREQUAL ownerDir)
        return()
    endif()

    message(FATAL_ERROR
        "This vcpkg install root was written by another tree; delete ${buildDir} and reconfigure.\n"
        "  Install root: ${installedDir}\n"
        "  Written from: ${recordedManifest}\n"
        "  This tree installs from: ${manifestDir}/vcpkg.json\n"
        "  The packages in that root were resolved against the other tree's vcpkg checkout and\n"
        "  toolset, so they can carry an ABI this tree never computes, and the object files\n"
        "  already in this build directory were compiled against them. Reconfiguring here\n"
        "  reinstalls the set this tree's own checkout resolves without recompiling those\n"
        "  objects — none of their sources changed — and they relink into binaries that link\n"
        "  and stage without a diagnostic and then crash at startup.\n"
        "  Deleting the build directory is the whole fix; the dependencies restore from the\n"
        "  vcpkg binary cache in seconds. Deleting only the install root is worse than doing\n"
        "  nothing: it pairs those same objects with a fresh record.")
endfunction()

# Touches the installed headers of each package in ${names}: the include/ entries
# of the package's file list (vcpkg/info/<name>_<version>_<triplet>.list, the
# version without its port-version). A package built from source keeps its
# archive's timestamps, which can predate the objects that include it, so
# without this its new headers would not read as changed. ${currentText} is the
# current installed set, which holds each package's version.
function(z_ge_vcpkg_touch_installed_headers installedDir triplet currentText names)
    string(REPLACE "\n" ";" currentLines "${currentText}")
    foreach(line IN LISTS currentLines)
        if(NOT line MATCHES "^([^|]+)\\|([^#|]*)#")
            continue()
        endif()
        set(name "${CMAKE_MATCH_1}")
        if(NOT name IN_LIST names)
            continue()
        endif()
        set(listFile "${installedDir}/vcpkg/info/${name}_${CMAKE_MATCH_2}_${triplet}.list")
        if(NOT EXISTS "${listFile}")
            message(WARNING
                "vcpkg install set: adopted ${name} has no file list at ${listFile}, so its "
                "headers were not touched and objects that include them may not recompile. "
                "Delete the build directory and reconfigure if the build misbehaves.")
            continue()
        endif()
        file(STRINGS "${listFile}" entries REGEX "^${triplet}/include/.*[^/]$")
        list(TRANSFORM entries PREPEND "${installedDir}/")
        if(entries)
            file(TOUCH_NOCREATE ${entries})
        endif()
    endforeach()
endfunction()

# Records the installed package set in ${buildDir} and refuses to continue once
# a package this build directory already compiled against changed underneath
# it. Call after every configure-time installer has run.
function(ge_check_vcpkg_install_set_unchanged installedDir triplet buildDir)
    if(installedDir STREQUAL "")
        return() # not a vcpkg configure — nothing installed for this build
    endif()

    set(statusFile "${installedDir}/vcpkg/status")
    if(NOT EXISTS "${statusFile}")
        # "not checked" must never be mistakable for "checked and current".
        message(STATUS
            "vcpkg install set: no status database in ${installedDir} "
            "— NOT checked (install root not populated yet)")
        return()
    endif()

    # Nothing else ties a build directory to its install root, so a rewrite of
    # the installed tree must itself force the reconfigure that runs this check.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${statusFile}")
    endif()

    z_ge_vcpkg_installed_set("${statusFile}" "${triplet}" currentText)
    set(stampFile "${buildDir}/ge-vcpkg-install.stamp")
    if(NOT EXISTS "${stampFile}")
        file(WRITE "${stampFile}" "${currentText}\n")
        string(REPLACE "\n" ";" currentLines "${currentText}")
        list(LENGTH currentLines packageCount)
        # First record in this directory: there is nothing to compare it
        # against, so it is adopted. Objects already here are covered only from
        # this point on.
        message(STATUS
            "vcpkg install set: recorded ${packageCount} installed package(s) as this build "
            "directory's baseline — anything already built here is assumed to match it")
        return()
    endif()

    file(READ "${stampFile}" recordedText)
    string(STRIP "${recordedText}" recordedText)
    if(recordedText STREQUAL currentText)
        string(REPLACE "\n" ";" currentLines "${currentText}")
        list(LENGTH currentLines packageCount)
        message(STATUS
            "vcpkg install set: ${packageCount} installed package(s) unchanged since this "
            "build directory was configured")
        return()
    endif()

    z_ge_vcpkg_installed_depends("${statusFile}" "${triplet}" dependsText)
    z_ge_vcpkg_changed_packages("${recordedText}" "${currentText}" "${dependsText}"
        moved movedNames abiOnly)
    if(abiOnly STREQUAL "")
        # Nothing this directory compiled against changed under unchanged
        # headers. Merging instead of replacing is what keeps a package that left
        # the set comparable when it returns, and what stops a truncated status
        # database from quietly shrinking the record.
        z_ge_vcpkg_merged_set("${recordedText}" "${currentText}" mergedText)
        if(NOT mergedText STREQUAL recordedText)
            file(WRITE "${stampFile}" "${mergedText}\n")
        endif()
        string(REPLACE "\n" ";" mergedLines "${mergedText}")
        list(LENGTH mergedLines packageCount)
        if(moved STREQUAL "")
            message(STATUS
                "vcpkg install set: the installed set moved since this build directory was "
                "configured but no package it already compiled against changed — the record "
                "now covers ${packageCount} package(s)")
        else()
            z_ge_vcpkg_touch_installed_headers("${installedDir}" "${triplet}"
                "${currentText}" "${movedNames}")
            list(LENGTH moved movedCount)
            list(JOIN moved "\n      " movedText)
            message(STATUS
                "vcpkg install set: ${movedCount} package(s) this build directory compiled "
                "against moved and were adopted in place; their installed headers were "
                "touched, so the next build recompiles the objects that include them:"
                "\n      ${movedText}")
        endif()
        return()
    endif()

    list(LENGTH abiOnly changedCount)
    set(shown "${abiOnly}")
    list(SUBLIST shown 0 5 shown)
    list(JOIN shown "\n      " shownText)
    set(moreText "")
    if(changedCount GREATER 5)
        math(EXPR remaining "${changedCount} - 5")
        set(moreText "      ...and ${remaining} more\n")
    endif()

    # The record is deliberately left stale: a second configure must fail the
    # same way, not silently adopt the new set and hand back the bad objects.
    message(FATAL_ERROR
        "${changedCount} installed package(s) this build directory compiled against were "
        "rebuilt at the same version with a different ABI; delete ${buildDir} and "
        "reconfigure.\n"
        "      ${shownText}\n"
        "${moreText}"
        "  Install root: ${installedDir}\n"
        "  A new ABI at an unchanged version is a Visual Studio toolset update, a moved\n"
        "  vcpkg checkout (dependencies/vcpkg) or a port rebuilt with other flags. The\n"
        "  package's headers did not change, so no object file in this directory recompiles,\n"
        "  but the library under them did: they would relink into binaries that can crash at\n"
        "  startup. Deleting the build directory is the whole fix; the dependencies restore\n"
        "  from the vcpkg binary cache in seconds. A version move, by contrast, is adopted in\n"
        "  place, because its headers change with it.")
endfunction()

# Entry point for the contract test (cmake/Tests/VcpkgInstallGuardTest.cmake),
# which must observe each refusal as a process exit rather than abort itself.
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT DEFINED CHECK OR NOT DEFINED INSTALLED_DIR OR NOT DEFINED BUILD_DIR)
        message(FATAL_ERROR
            "VcpkgInstallGuard.cmake: CHECK, INSTALLED_DIR and BUILD_DIR are required in script mode")
    endif()
    if(CHECK STREQUAL "root")
        ge_check_vcpkg_install_root_not_shared("${INSTALLED_DIR}" "${BUILD_DIR}")
    elseif(CHECK STREQUAL "producer")
        ge_check_vcpkg_install_root_written_here("${INSTALLED_DIR}" "${MANIFEST_DIR}" "${BUILD_DIR}")
    elseif(CHECK STREQUAL "set")
        ge_check_vcpkg_install_set_unchanged("${INSTALLED_DIR}" "${TRIPLET}" "${BUILD_DIR}")
    else()
        message(FATAL_ERROR "VcpkgInstallGuard.cmake: unknown CHECK '${CHECK}'")
    endif()
endif()
