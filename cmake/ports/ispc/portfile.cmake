# Prebuilt ISPC release archives (the official GitHub release, pinned). A host
# tool only: it compiles cmake/ports/bc7e and nothing of it ships. ISPC
# publishes no Windows arm64 build; Windows on arm64 runs the x64 one.
set(GE_ISPC_VERSION "1.32.0")

if(VCPKG_TARGET_IS_WINDOWS)
    set(GE_ISPC_PLATFORM "windows")
    set(GE_ISPC_EXTENSION "zip")
    set(GE_ISPC_SHA512 732fb5702cc8f2d77975a87b21ef697384d9b7d49f75086b3a89fdf39f155584340719012e3721e42d8cb5945b8b85bfea6b182a7afbc701a1af6e7e9b85d980)
elseif(VCPKG_TARGET_IS_OSX)
    set(GE_ISPC_PLATFORM "macOS.universal")
    set(GE_ISPC_EXTENSION "tar.gz")
    set(GE_ISPC_SHA512 609682539404b0cba15d6e507548ec1175a90e27f186933993e3458b33814ee2cd19bc9ac0c11ba33e23888bb0a25d0957050b327042002e19fb0e969f65b86f)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(GE_ISPC_PLATFORM "linux.aarch64")
    set(GE_ISPC_EXTENSION "tar.gz")
    set(GE_ISPC_SHA512 85892e88677e89399ef6104bf0d7395a97ee918f42d42f793ba834d78f07a49167cc79135513f7745b0c766698040f5a4c27ff6a12cefe2e7276d369603ae070)
else()
    set(GE_ISPC_PLATFORM "linux")
    set(GE_ISPC_EXTENSION "tar.gz")
    set(GE_ISPC_SHA512 b99db7514971795673b707b4647813230f9b90f12b6f3f2f4cd9339c450a6d76433ef436a70487094d7deb77e8e5f6fa3be8b759ed5d2da38441a421b4f4d3fa)
endif()

vcpkg_download_distfile(GE_ISPC_ARCHIVE
    URLS "https://github.com/ispc/ispc/releases/download/v${GE_ISPC_VERSION}/ispc-v${GE_ISPC_VERSION}-${GE_ISPC_PLATFORM}.${GE_ISPC_EXTENSION}"
    FILENAME "ispc-v${GE_ISPC_VERSION}-${GE_ISPC_PLATFORM}.${GE_ISPC_EXTENSION}"
    SHA512 ${GE_ISPC_SHA512}
)

vcpkg_extract_source_archive(GE_ISPC_SOURCE ARCHIVE "${GE_ISPC_ARCHIVE}")

# The compiler executable and its shared libraries (ispc.dll on Windows, libispc
# under lib/ elsewhere). The macOS compiler links neither, so lib/libispc* is
# installed there but not loaded. The runtime and examples are not used.
file(INSTALL "${GE_ISPC_SOURCE}/bin/" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin" USE_SOURCE_PERMISSIONS)
if(NOT VCPKG_TARGET_IS_WINDOWS)
    file(INSTALL "${GE_ISPC_SOURCE}/lib/" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/lib"
         USE_SOURCE_PERMISSIONS FILES_MATCHING PATTERN "libispc.*")
endif()

# Intel's macOS binaries are signed with the hardened runtime. vcpkg's Mach-O
# rpath fixup, which runs on every package on macOS by default, adds an
# LC_RPATH to bin/ispc and rewrites the dylib install names; that voids the
# signature and the kernel kills ispc at exec. The binaries need no rpath.
set(VCPKG_FIXUP_MACHO_RPATH OFF)

set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled)
set(VCPKG_POLICY_SKIP_ALL_POST_BUILD_CHECKS enabled)

file(INSTALL "${GE_ISPC_SOURCE}/LICENSE.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
