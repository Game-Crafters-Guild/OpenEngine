// Deterministic unit tests for the C9 NativeScriptManager skeleton.
//
// Covers the parts that are pure logic — extension classification, the
// Initialize/Shutdown lifecycle, and the play-mode hot-reload deferral flag. The
// async file-watch path (OS events + 300ms debounce) is intentionally NOT tested
// here; it is exercised end-to-end once the build/load pipeline lands (C10+),
// where the behavior is observable without racing the watcher thread.

#include "Core/Application.h" // PathUtils::InstallContentRootFor
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/MacBundleAssembler.h"
#include "Engine/Build/NativeScriptStaging.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/CMakeInvoker.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h"
#include "NativeScripting/NativeSourceTree.h"
#include "NativeScripting/PrebuiltModuleBinaries.h"
#include "NativeScripting/SdkManifest.h"
#include "NativeScripting/ToolchainFingerprint.h"
#include "NativeScripting/UserSystemRegistry.h"
#include "StampedBinary.h"

#include "InFlightModuleBuild.h"
#include "../TestTempDir.h"

#include "ECS/Entity.h" // full ECS::World definition (ECS.h only forward-declares it)

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace ns = GameEngine::NativeScripting;
namespace nst = GameEngine::NativeScripting::Testing;

TEST(NativeSourceTree, WatchedExtensionsAreCaseInsensitive)
{
    EXPECT_TRUE(ns::IsWatchedNativeSourceExtension("Enemy.cpp"));
    EXPECT_TRUE(ns::IsWatchedNativeSourceExtension("Enemy.h"));
    EXPECT_TRUE(ns::IsWatchedNativeSourceExtension("Enemy.hpp"));
    EXPECT_TRUE(ns::IsWatchedNativeSourceExtension("Enemy.hxx"));      // scanner reflects .hxx
    EXPECT_TRUE(ns::IsWatchedNativeSourceExtension("Enemy.hh"));       // and .hh headers
    EXPECT_TRUE(ns::IsWatchedNativeSourceExtension("Foo.HPP"));        // case-insensitive
    EXPECT_TRUE(ns::IsWatchedNativeSourceExtension("a/b/Component.CPP"));

    EXPECT_FALSE(ns::IsWatchedNativeSourceExtension("Script.cs"));     // C# is not native
    EXPECT_FALSE(ns::IsWatchedNativeSourceExtension("data.txt"));
    EXPECT_FALSE(ns::IsWatchedNativeSourceExtension("README"));        // no extension
    EXPECT_FALSE(ns::IsWatchedNativeSourceExtension("shader.hlsl"));
}

TEST(NativeScriptManager, InitializeShutdownIsIdempotent)
{
    ns::NativeScriptManager mgr;
    EXPECT_FALSE(mgr.IsInitialized());

    EXPECT_TRUE(mgr.Initialize(nullptr));
    EXPECT_TRUE(mgr.IsInitialized());
    EXPECT_TRUE(mgr.Initialize(nullptr)); // second Initialize is a no-op success

    mgr.Shutdown();
    EXPECT_FALSE(mgr.IsInitialized());
    mgr.Shutdown(); // second Shutdown is safe
    EXPECT_FALSE(mgr.IsInitialized());
}

// Where native modules cannot load, nothing is waited for: configured package modules leave no
// module pending and the first package pass counts as complete.
TEST(NativeScriptManager, NothingIsPendingWhereModulesCannotLoad)
{
    ns::NativeScriptManager mgr;
    mgr.SetModuleLoadingSupported(false);
    ASSERT_TRUE(mgr.Initialize(nullptr));
    ns::NativeBuildConfig package{};
    package.ModuleName = "SomePackage";
    mgr.SetPackageModuleConfigs({package});
    EXPECT_FALSE(mgr.AreModulesPending()) << "a module that can never load is waited for";
    EXPECT_TRUE(mgr.InitialPackageModulePassComplete());

    ns::NativeScriptManager loading;
    ASSERT_TRUE(loading.Initialize(nullptr));
    loading.SetPackageModuleConfigs({package});
    EXPECT_TRUE(loading.AreModulesPending()) << "where modules load, a configured package is waited for";
    mgr.Shutdown();
    loading.Shutdown();
}

TEST(NativeScriptManager, HotReloadDeferralFlagTogglesAndNothingPendingByDefault)
{
    ns::NativeScriptManager mgr;
    mgr.Initialize(nullptr);

    EXPECT_FALSE(mgr.IsHotReloadDeferred());
    EXPECT_FALSE(mgr.HasDeferredHotReloadPending());

    mgr.SetHotReloadDeferred(true);
    EXPECT_TRUE(mgr.IsHotReloadDeferred());

    mgr.SetHotReloadDeferred(false);
    EXPECT_FALSE(mgr.IsHotReloadDeferred());

    // No source change occurred, so there is nothing to flush.
    EXPECT_FALSE(mgr.TriggerDeferredHotReloadIfPending());

    mgr.Shutdown();
}

TEST(NativeScriptManager, InitialPackagePassSignalTracksConfigSetAndFirstKick)
{
    ns::NativeScriptManager mgr;
    mgr.Initialize(nullptr);

    // Trivially complete with no package modules configured — the editor's
    // VCS first-detect gate must open immediately for package-less projects.
    EXPECT_TRUE(mgr.InitialPackageModulePassComplete());

    // A non-empty config set arms the signal until the first KickBuild pass.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ge_nsm_initial_pass_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "src");
    {
        std::ofstream src(root / "src" / "module.cpp");
        src << "int GeInitialPassProbe() { return 1; }\n";
    }
    ns::NativeBuildConfig config;
    config.SourceDir = root / "src";
    config.BuildDir = root / "build";
    config.ModuleName = "InitialPassProbe";
    std::vector<ns::NativeBuildConfig> configs;
    configs.push_back(config);
    mgr.SetPackageModuleConfigs(configs);
    EXPECT_FALSE(mgr.InitialPackageModulePassComplete())
        << "armed until the first pass gives the module a terminal outcome";

    // First kick: the module is stale (no cache, no prebuilt) and there is no
    // JobSystem to build it — a terminal outcome for the initial pass (the
    // signal must not wedge the editor's VCS gate).
    mgr.RequestRebuild();
    mgr.Tick();
    EXPECT_TRUE(mgr.InitialPackageModulePassComplete());

    // Replacing with an empty set is trivially complete again.
    mgr.SetPackageModuleConfigs({});
    EXPECT_TRUE(mgr.InitialPackageModulePassComplete());

    mgr.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(NativeScriptManager, TickBeforeInitializeIsSafe)
{
    ns::NativeScriptManager mgr;
    mgr.Tick(); // no-op before Initialize; must not crash
    SUCCEED();
}

// ---------------------------------------------------------------------------
// B10 (near-term): project open prunes active/ shadow copies that no build
// references — the content-addressed copies otherwise accrue one file per
// distinct build forever. Files only; the cache-referenced DLL survives.
// ---------------------------------------------------------------------------
namespace
{
namespace fs = std::filesystem;

void WriteFileBytes(const fs::path& path, const char* bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// WriteFileBytes for fixtures that lay out a whole tree at once.
void WriteFileInTree(const fs::path& path, const char* bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    WriteFileBytes(path, bytes);
}

// A project-shaped cache layout: build/ + active/ with three shadow copies, the
// middle one referenced by build/last_build.txt (digest\n dllPath\n abiDigest\n).
ns::NativeBuildConfig MakePruneFixture(const fs::path& root)
{
    std::error_code ec;
    fs::remove_all(root, ec);
    ns::NativeBuildConfig config;
    config.SourceDir = root / "Assets";
    config.BuildDir = root / "build";
    config.ActiveDir = root / "active";
    fs::create_directories(config.BuildDir, ec);
    fs::create_directories(config.ActiveDir, ec);
    WriteFileBytes(config.ActiveDir / "UserScripts_aaaaaaaa.dll", "stale-a");
    WriteFileBytes(config.ActiveDir / "UserScripts_bbbbbbbb.dll", "referenced");
    WriteFileBytes(config.ActiveDir / "UserScripts_cccccccc.dll", "stale-c");
    return config;
}
} // namespace

TEST(NativeScriptManager, ProjectOpenPrunesShadowCopiesUnreferencedByBuildCache)
{
    const fs::path root = fs::temp_directory_path() / "ge_nsm_prune";
    ns::NativeBuildConfig config = MakePruneFixture(root);
    const fs::path referenced = config.ActiveDir / "UserScripts_bbbbbbbb.dll";
    {
        std::ofstream cache(config.BuildDir / "last_build.txt", std::ios::trunc);
        cache << "digest\n" << referenced.generic_string() << "\nabidigest\n";
    }
    // Directories under active/ (and their contents) are not the prune's business.
    std::error_code ec;
    fs::create_directories(config.ActiveDir / "keep_dir", ec);
    WriteFileBytes(config.ActiveDir / "keep_dir" / "nested.dll", "nested");

    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));
    mgr.SetBuildConfig(config); // the project-open prune point

    EXPECT_TRUE(fs::exists(referenced, ec)) << "cache-referenced shadow copy must survive";
    EXPECT_FALSE(fs::exists(config.ActiveDir / "UserScripts_aaaaaaaa.dll", ec));
    EXPECT_FALSE(fs::exists(config.ActiveDir / "UserScripts_cccccccc.dll", ec));
    EXPECT_TRUE(fs::exists(config.ActiveDir / "keep_dir" / "nested.dll", ec)) << "prune is files-only";

    mgr.Shutdown();
    fs::remove_all(root, ec);
}

TEST(NativeScriptManager, ProjectOpenWithoutBuildCachePrunesAllShadowCopies)
{
    const fs::path root = fs::temp_directory_path() / "ge_nsm_prune_nocache";
    ns::NativeBuildConfig config = MakePruneFixture(root); // no last_build.txt written

    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));
    mgr.SetBuildConfig(config);

    std::error_code ec;
    EXPECT_TRUE(fs::is_empty(config.ActiveDir, ec)) << "every unreferenced shadow copy is dead";

    mgr.Shutdown();
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// The source walk behind the staleness digest and the empty-project gate. A
// package whose sources sit at its root builds into a .Cache inside that root,
// so a walk that counted what the build writes would leave the module stale
// against its own output and swap a fresh generation on every editor launch.
// ---------------------------------------------------------------------------

TEST(NativeSourceTree, BuildOutputInsideTheSourceRootLeavesTheDigestAlone)
{
    const fs::path root = fs::temp_directory_path() / "ge_nst_embedded_build";
    std::error_code ec;
    fs::remove_all(root, ec);
    WriteFileInTree(root / "Source" / "Widen.cpp", "void Widen() {}\n");
    WriteFileInTree(root / "Include" / "Widen.h", "#pragma once\n");

    const std::uint64_t base = ns::HashWatchedNativeSources(root, {}, 0);

    // What a build of this very module writes inside its own root.
    const fs::path buildDir = root / ".Cache" / "NativeScripts" / "build";
    WriteFileInTree(buildDir / "Widen.gen.cpp", "// scanner-generated registrations\n");
    WriteFileInTree(buildDir / "CMakeFiles" / "4.0.0" / "CompilerIdCXX" / "CMakeCXXCompilerId.cpp",
                    "int main(int argc, char* argv[]) { return 0; }\n");
    WriteFileInTree(buildDir / "ShowIncludes" / "probe.h", "#pragma once\n");
    EXPECT_EQ(ns::HashWatchedNativeSources(root, {}, 0), base)
        << "the module's own build output is not one of its sources";

    // A real edit still invalidates.
    WriteFileInTree(root / "Source" / "More.cpp", "void More() {}\n");
    EXPECT_NE(ns::HashWatchedNativeSources(root, {}, 0), base)
        << "a source added under the module's own tree must change the digest";

    fs::remove_all(root, ec);
}

TEST(NativeSourceTree, APackagesTestsFolderLeavesItsModuleDigestAlone)
{
    // The generated project of a module rooted at the package root skips the
    // package's Tests folder, so an edit there must not invalidate the module.
    const fs::path root = fs::temp_directory_path() / "ge_nst_package_tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    WriteFileInTree(root / "Source" / "Widen.cpp", "void Widen() {}\n");

    const std::uint64_t base = ns::HashWatchedNativeSources(root, root, 0);
    WriteFileInTree(root / "Tests" / "WidenTests.cpp", "void WidenTests() {}\n");
    EXPECT_EQ(ns::HashWatchedNativeSources(root, root, 0), base);
    EXPECT_NE(ns::HashWatchedNativeSources(root, {}, 0), base) << "a project's own Tests folder is hashed";

    fs::remove_all(root, ec);
}

TEST(NativeSourceTree, BuildOutputInsideTheSourceRootDoesNotOpenTheEmptyProjectGate)
{
    const fs::path root = fs::temp_directory_path() / "ge_nst_embedded_build_gate";
    std::error_code ec;
    fs::remove_all(root, ec);
    WriteFileInTree(root / ".Cache" / "NativeScripts" / "build" / "UserScripts.gen.cpp",
                    "// scanner-generated registrations\n");
    EXPECT_FALSE(ns::HasWatchedNativeSource(root)) << "a leftover build tree is not user code";

    WriteFileInTree(root / "Source" / "Widen.cpp", "void Widen() {}\n");
    EXPECT_TRUE(ns::HasWatchedNativeSource(root));

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// C2: build-cache record + engine-ABI digest plumbing. The record format is the
// contract between the editor's build cache, the packaged-game staging step, and
// the Player's prebuilt loader; the digest is the engine identity all three
// compare. LoadPrebuiltUserModule's marker enforcement is covered here through
// its observable refusals and end-to-end (real DLL) in UserModuleBuildLoadTest.
// ---------------------------------------------------------------------------
TEST(BuildCacheRecord, RoundTripsAllFourLines)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_roundtrip";
    std::error_code ec;
    fs::remove_all(dir, ec);

    ASSERT_TRUE(ns::WriteBuildCacheRecord(
        dir, ns::BuildCacheRecord{"full", "some/dir/UserScripts.dll", "abi", "enginebuild"}));
    const auto record = ns::ReadBuildCacheRecord(dir);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->Digest, "full");
    EXPECT_EQ(record->DllPath, "some/dir/UserScripts.dll");
    EXPECT_EQ(record->AbiDigest, "abi");
    EXPECT_EQ(record->EngineBuildId, "enginebuild");

    fs::remove_all(dir, ec);
}

TEST(BuildCacheRecord, ShorterLegacyCachesReadWithTheTrailingFieldsEmpty)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_legacy";
    std::error_code ec;

    // 2-line: pre-dates the ABI digest.
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream out(dir / ns::kBuildCacheFileName, std::ios::trunc);
        out << "full\nsome/dir/UserScripts.dll\n";
    }
    auto record = ns::ReadBuildCacheRecord(dir);
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->AbiDigest.empty());
    EXPECT_TRUE(record->EngineBuildId.empty());

    // 3-line: pre-dates the engine build id. This is the shape every record
    // already sitting in the machine-global PackageCache has.
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream out(dir / ns::kBuildCacheFileName, std::ios::trunc);
        out << "full\nsome/dir/UserScripts.dll\nabi\n";
    }
    record = ns::ReadBuildCacheRecord(dir);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->AbiDigest, "abi");
    EXPECT_TRUE(record->EngineBuildId.empty());

    fs::remove_all(dir, ec);
}

TEST(BuildCacheRecord, AbsentFileOrEmptyDllPathReadsAsNoRecord)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_absent";
    std::error_code ec;
    fs::remove_all(dir, ec);
    EXPECT_FALSE(ns::ReadBuildCacheRecord(dir).has_value());

    ASSERT_TRUE(ns::WriteBuildCacheRecord(dir, ns::BuildCacheRecord{"full", "", "abi"}));
    EXPECT_FALSE(ns::ReadBuildCacheRecord(dir).has_value()) << "a record naming no DLL is no record";

    fs::remove_all(dir, ec);
}

TEST(BuildCacheRecord, EngineAbiMarkerRoundTripsAndReadsEmptyWhenAbsent)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_marker";
    std::error_code ec;
    fs::remove_all(dir, ec);

    EXPECT_TRUE(ns::ReadEngineAbiMarker(dir).empty());
    ASSERT_TRUE(ns::WriteEngineAbiMarker(dir, "0123456789abcdef"));
    EXPECT_EQ(ns::ReadEngineAbiMarker(dir), "0123456789abcdef");

    fs::remove_all(dir, ec);
}

TEST(BuildCacheRecord, EngineAbiDigestDiscriminatesConfigAndDefsAndEngineBinary)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_digest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    nst::WriteStampedBinary(dir / "EngineA.bin", 0);
    nst::WriteStampedBinary(dir / "EngineB.bin", 1);

    ns::NativeBuildConfig base;
    base.Config = "Release";
    base.EngineBinary = dir / "EngineA.bin";
    base.CompileDefinitions = {"GE_ENABLE_SCRIPTING=1"};

    ASSERT_FALSE(ns::ComputeEngineAbiDigest(base).empty());
    EXPECT_EQ(ns::ComputeEngineAbiDigest(base), ns::ComputeEngineAbiDigest(base)) << "digest must be stable";

    ns::NativeBuildConfig otherConfig = base;
    otherConfig.Config = "Debug";
    EXPECT_NE(ns::ComputeEngineAbiDigest(base), ns::ComputeEngineAbiDigest(otherConfig));

    ns::NativeBuildConfig otherDefs = base;
    otherDefs.CompileDefinitions.push_back("GE_EXTRA=1");
    EXPECT_NE(ns::ComputeEngineAbiDigest(base), ns::ComputeEngineAbiDigest(otherDefs));

    ns::NativeBuildConfig otherEngine = base;
    otherEngine.EngineBinary = dir / "EngineB.bin";
    EXPECT_NE(ns::ComputeEngineAbiDigest(base), ns::ComputeEngineAbiDigest(otherEngine));

    fs::remove_all(dir, ec);
}

// The engine_abi marker is written against the build tree's staged SDK and checked against
// whichever copy of the Editor tree runs (an extracted release zip, an isolated editor copy),
// so the digest reads the binaries' link stamps, never their paths or timestamps.
TEST(BuildCacheRecord, EngineAbiDigestIgnoresBinaryLocationAndMtime)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_digest_relocated";
    std::error_code ec;
    fs::remove_all(dir, ec);
    nst::WriteStampedBinary(dir / "build" / "Engine.bin", 0);
    nst::WriteStampedBinary(dir / "build" / "EditorSDK.bin", 2);
    fs::create_directories(dir / "copy", ec);
    for (const char* binary : {"Engine.bin", "EditorSDK.bin"})
    {
        fs::copy_file(dir / "build" / binary, dir / "copy" / binary, ec);
        fs::last_write_time(dir / "copy" / binary,
                            fs::last_write_time(dir / "build" / binary) - std::chrono::hours(48), ec);
    }

    ns::NativeBuildConfig built;
    built.Config = "Release";
    built.EngineImportLib = dir / "build" / "Engine.lib";
    built.EngineBinary = dir / "build" / "Engine.bin";
    built.EditorImportLib = dir / "build" / "EditorSDK.lib";
    built.EditorBinary = dir / "build" / "EditorSDK.bin";
    built.CompileDefinitions = {"GE_ENABLE_SCRIPTING=1"};

    ns::NativeBuildConfig relocated = built;
    relocated.EngineImportLib = dir / "copy" / "Engine.lib";
    relocated.EngineBinary = dir / "copy" / "Engine.bin";
    relocated.EditorImportLib = dir / "copy" / "EditorSDK.lib";
    relocated.EditorBinary = dir / "copy" / "EditorSDK.bin";

    ASSERT_FALSE(ns::ComputeEngineAbiDigest(built).empty());
    EXPECT_EQ(ns::ComputeEngineAbiDigest(built), ns::ComputeEngineAbiDigest(relocated));

    fs::remove_all(dir, ec);
}

// A relink that changes only struct layouts keeps every export, and MSVC then leaves the
// import library byte-identical and untouched; only the binaries' link stamps change. Two
// such engines must digest apart even though their import libraries are the same file.
TEST(BuildCacheRecord, EngineAbiDigestFollowsBinaryLinkStampNotImportLib)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_digest_stamp";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFileBytes(dir / "Engine.lib", "engine-import-library");
    WriteFileBytes(dir / "EditorSDK.lib", "editor-sdk-import-library");
    nst::WriteStampedBinary(dir / "Engine.bin", 0);
    nst::WriteStampedBinary(dir / "EngineRelinked.bin", 1);
    nst::WriteStampedBinary(dir / "EditorSDK.bin", 2);
    nst::WriteStampedBinary(dir / "EditorSDKRelinked.bin", 3);

    ns::NativeBuildConfig editor;
    editor.Config = "Release";
    editor.EngineImportLib = dir / "Engine.lib";
    editor.EngineBinary = dir / "Engine.bin";
    editor.EditorImportLib = dir / "EditorSDK.lib";
    editor.EditorBinary = dir / "EditorSDK.bin";
    editor.CompileDefinitions = {"GE_ENABLE_SCRIPTING=1"};
    const std::string original = ns::ComputeEngineAbiDigest(editor);
    ASSERT_FALSE(original.empty());

    ns::NativeBuildConfig engineRelinked = editor;
    engineRelinked.EngineBinary = dir / "EngineRelinked.bin";
    EXPECT_NE(original, ns::ComputeEngineAbiDigest(engineRelinked));

    ns::NativeBuildConfig editorRelinked = editor;
    editorRelinked.EditorBinary = dir / "EditorSDKRelinked.bin";
    EXPECT_NE(original, ns::ComputeEngineAbiDigest(editorRelinked));

    fs::remove_all(dir, ec);
}

// An engine whose binary carries no readable link stamp cannot be identified: the digest
// is empty rather than a value every such SDK would share, and nothing is matched against it.
TEST(BuildCacheRecord, EngineAbiDigestIsEmptyWithoutAReadableLinkStamp)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_digest_unreadable";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFileBytes(dir / "NotABinary.bin", "no link stamp in here");
    nst::WriteStampedBinary(dir / "Engine.bin", 0);

    ns::NativeBuildConfig missing;
    missing.Config = "Release";
    missing.EngineBinary = dir / "Absent.bin";
    EXPECT_TRUE(ns::ComputeEngineAbiDigest(missing).empty());

    ns::NativeBuildConfig unstamped = missing;
    unstamped.EngineBinary = dir / "NotABinary.bin";
    EXPECT_TRUE(ns::ComputeEngineAbiDigest(unstamped).empty());

    ns::NativeBuildConfig editorMissing = missing;
    editorMissing.EngineBinary = dir / "Engine.bin";
    editorMissing.EditorImportLib = dir / "EditorSDK.lib";
    editorMissing.EditorBinary = dir / "AbsentEditor.bin";
    EXPECT_TRUE(ns::ComputeEngineAbiDigest(editorMissing).empty());

    fs::remove_all(dir, ec);
}

// EditorSDK phase 1: an Editor-kind module's digest folds the EditorSDK
// binary's link stamp — a relinked editor surface must invalidate its cached
// DLLs — while Runtime configs (EditorImportLib empty) hash no editor input.
TEST(BuildCacheRecord, EngineAbiDigestFoldsEditorSdkBinaryOnlyWhenSet)
{
    const fs::path dir = fs::temp_directory_path() / "ge_bcr_editor_digest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    nst::WriteStampedBinary(dir / "Engine.bin", 0);
    nst::WriteStampedBinary(dir / "EditorSDK.bin", 2);
    nst::WriteStampedBinary(dir / "EditorSDKRelinked.bin", 3);

    ns::NativeBuildConfig runtime;
    runtime.Config = "Release";
    runtime.EngineBinary = dir / "Engine.bin";
    runtime.CompileDefinitions = {"GE_ENABLE_SCRIPTING=1"};

    ns::NativeBuildConfig editor = runtime;
    editor.EditorImportLib = dir / "EditorSDK.lib";
    editor.EditorBinary = dir / "EditorSDK.bin";

    const std::string runtimeDigest = ns::ComputeEngineAbiDigest(runtime);
    const std::string editorDigest = ns::ComputeEngineAbiDigest(editor);
    EXPECT_NE(runtimeDigest, editorDigest)
        << "editor modules must be invalidated by an EditorSDK relink";
    EXPECT_EQ(editorDigest, ns::ComputeEngineAbiDigest(editor)) << "digest must be stable";

    // An EditorSDK relink changes the editor digest but never the runtime one.
    ns::NativeBuildConfig relinked = editor;
    relinked.EditorBinary = dir / "EditorSDKRelinked.bin";
    EXPECT_NE(editorDigest, ns::ComputeEngineAbiDigest(relinked));
    ns::NativeBuildConfig runtimeAfterRelink = runtime;
    runtimeAfterRelink.EditorBinary = dir / "EditorSDKRelinked.bin";
    EXPECT_EQ(runtimeDigest, ns::ComputeEngineAbiDigest(runtimeAfterRelink));

    fs::remove_all(dir, ec);
}

// C2 gate parity: given the same staged SDK manifest (one that carries an
// EditorSDK interface, as the editor's always does), the digest the packaged
// build recomputes (BuildPipeline: ComputeRuntimeEngineAbiDigest) must equal the
// digest the editor wiring stamps into runtime build records (clear the editor
// interface, then ComputeEngineAbiDigest). If they diverge, every packaged
// native-script build is refused as "stale" against a perfectly fresh record.
TEST(BuildCacheRecord, PackagedRecomputeMatchesEditorStampedRuntimeDigest)
{
    const fs::path sdkRoot = fs::temp_directory_path() / "ge_bcr_sdk_parity";
    std::error_code ec;
    fs::remove_all(sdkRoot, ec);
    fs::create_directories(sdkRoot / "nativescripting", ec);
    fs::create_directories(sdkRoot / "include", ec);
    fs::create_directories(sdkRoot / "editor-include", ec);
    WriteFileBytes(sdkRoot / "Engine.lib", "engine-bytes");
    WriteFileBytes(sdkRoot / "EditorSDK.lib", "editor-sdk-bytes");
    nst::WriteStampedBinary(sdkRoot / "Engine.bin", 0);
    nst::WriteStampedBinary(sdkRoot / "EditorSDK.bin", 2);
    WriteFileBytes(sdkRoot / "nativescripting" / "manifest.txt",
                   "includedir=include\n"
                   "importlib=Engine.lib\n"
                   "enginebinary=Engine.bin\n"
                   "defs=GE_ENABLE_SCRIPTING=1\n"
                   "config=Debug\n"
                   "editorimportlib=EditorSDK.lib\n"
                   "editorbinary=EditorSDK.bin\n"
                   "editorincludes=editor-include\n");

    ns::NativeBuildConfig fromManifest;
    std::string manifestErr;
    ASSERT_TRUE(ns::LoadSdkManifest(sdkRoot, fromManifest, manifestErr)) << manifestErr;
    ASSERT_FALSE(fromManifest.EditorImportLib.empty())
        << "fixture must model the editor's staged SDK (EditorSDK interface present)";

    const std::vector<std::string> packageDefines = {"OCEAN_PACK", "EZTREE_PACK"};

    // Editor wiring path (WireNativeScriptingForProject): runtime/project module
    // configs get the editor interface cleared before the manager stamps records.
    ns::NativeBuildConfig editorStamped = fromManifest;
    editorStamped.EditorImportLib.clear();
    editorStamped.EditorBinary.clear();
    editorStamped.EditorIncludeDirs.clear();
    ns::AppendPackageDefines(editorStamped, packageDefines);
    const std::string recordDigest = ns::ComputeEngineAbiDigest(editorStamped);

    // Packaged-build path (BuildPipeline::ComputeCurrentEngineAbiDigest).
    ns::NativeBuildConfig recompute = fromManifest;
    ns::AppendPackageDefines(recompute, packageDefines);
    const std::string packageDigest = ns::ComputeRuntimeEngineAbiDigest(recompute);

    ASSERT_FALSE(recordDigest.empty());
    EXPECT_EQ(recordDigest, packageDigest)
        << "packaged recompute must hash the same runtime-only inputs the editor stamped";

    // The manifest's EditorSDK identity must be load-bearing in this fixture:
    // hashing the manifest config directly (the regression) yields a different
    // digest, which is exactly the false 'stale scripts' C2 refusal.
    EXPECT_NE(packageDigest, ns::ComputeEngineAbiDigest(recompute))
        << "fixture lost its teeth: digest no longer folds the EditorSDK binary when set";

    fs::remove_all(sdkRoot, ec);
}

// A packaged game whose record and engine_abi marker disagree must refuse the
// module BEFORE any load attempt — the DLL here is garbage that would fail
// LoadLibrary, but the refusal happens first (the record digest gate), which is
// also why the packaged probe path (<root>/NativeScripts) is exercised.
TEST(NativeScriptManager, PrebuiltLoadRefusesPackagedModuleOnEngineAbiMismatch)
{
    const fs::path root = fs::temp_directory_path() / "ge_nsm_abi_mismatch";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path pkgDir = root / "NativeScripts";
    fs::create_directories(pkgDir, ec);
    WriteFileBytes(pkgDir / "UserScripts.dll", "not-a-real-dll");
    ASSERT_TRUE(ns::WriteBuildCacheRecord(pkgDir,
        ns::BuildCacheRecord{"packaged", "NativeScripts/UserScripts.dll", "aaaa"}));
    ASSERT_TRUE(ns::WriteEngineAbiMarker(pkgDir, "bbbb"));

    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));
    EXPECT_FALSE(mgr.LoadPrebuiltUserModule(root));

    // A record with no abi digest at all (legacy build) is equally unverifiable -> refused.
    ASSERT_TRUE(ns::WriteBuildCacheRecord(pkgDir,
        ns::BuildCacheRecord{"packaged", "NativeScripts/UserScripts.dll", ""}));
    EXPECT_FALSE(mgr.LoadPrebuiltUserModule(root));

    mgr.Shutdown();
    fs::remove_all(root, ec);
}

#if defined(__APPLE__)
namespace {
// Loads the packaged module from the content root the Player passes for an executable in
// `contents`/MacOS, and counts ImageMapBegin: the manager fires it right before the load,
// after it found the record and the dylib that record names on disk.
int PackagedModuleMapBeginsFromThePlayerContentRoot(const fs::path& contents)
{
    int mapBegins = 0;
    ns::NativeScriptManager mgr;
    if (!mgr.Initialize(nullptr))
        return -1;
    ns::NativeScriptManager::ModuleImageObserver observer;
    observer.ImageMapBegin = [&mapBegins] { ++mapBegins; };
    observer.ImageMapEnd = [](std::uint64_t, std::uint64_t) {};
    mgr.SetModuleImageObserver(std::move(observer));
    // The fixture dylibs export no user-module entry points, so the load itself fails.
    EXPECT_FALSE(mgr.LoadPrebuiltUserModule(GameEngine::PathUtils::InstallContentRootFor(contents / "MacOS")));
    mgr.Shutdown();
    return mapBegins;
}
} // namespace

// A packaged macOS game: the exporter stages the record in Contents/Resources/NativeScripts
// (MacBundleAssembler::MoveNativeModulesIntoFrameworks) with the dylib in Contents/Frameworks, recorded
// relative to Contents/Resources. The Player passes InstallContentRootFor(Contents/MacOS). The
// record is written at the exporter's literal path, not one derived from the function under test,
// so a content root other than Contents/Resources finds no record. (Contents/MacOS and
// Contents/Resources both sit beside Frameworks, so the record's "../Frameworks" alone cannot tell
// them apart.)
TEST(NativeScriptManager, PackagedMacBundleRecordInResourcesReachesTheFrameworksDylib)
{
    const fs::path contents = fs::temp_directory_path() / "ge_nsm_mac_bundle" / "Game.app" / "Contents";
    std::error_code ec;
    fs::remove_all(contents.parent_path().parent_path(), ec);
    fs::create_directories(contents / "MacOS", ec);
    WriteFileInTree(contents / "Frameworks" / "UserScripts.dylib", "not-a-real-dylib");
    const fs::path recordDir = contents / "Resources" / "NativeScripts";
    fs::create_directories(recordDir, ec);
    ASSERT_TRUE(ns::WriteBuildCacheRecord(
        recordDir, ns::BuildCacheRecord{"packaged", "../Frameworks/UserScripts.dylib", "abi"}));
    ASSERT_TRUE(ns::WriteEngineAbiMarker(recordDir, "abi"));

    EXPECT_EQ(PackagedModuleMapBeginsFromThePlayerContentRoot(contents), 1)
        << "the Player's content root did not reach the record in " << recordDir << " or the dylib it names";
    fs::remove_all(contents.parent_path().parent_path(), ec);
}

// The exporter's write and the Player's read, joined: the export stages a project's native module
// into the bundle's content root (StageNativeUserScriptsForPackage, as on every target) and
// MacBundleAssembler::MoveNativeModulesIntoFrameworks moves it into Contents/Frameworks; the
// Player's load from its content root reaches it. The move runs install_name_tool on the dylib,
// so the fixture is a real (dependency-free) Mach-O dylib built beside this test.
TEST(NativeScriptManager, MacExportNativeModuleIsWhereThePlayerLoadsIt)
{
    const fs::path root = fs::temp_directory_path() / "ge_nsm_mac_export";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path projectRoot = root / "Project";
    const fs::path editorBuildDir = projectRoot / ".Cache" / "NativeScripts" / "build";
    fs::create_directories(editorBuildDir, ec);
    const fs::path editorDylib = editorBuildDir / "UserScripts.dylib";
    fs::copy_file(GE_MAC_BUNDLE_FIXTURE_DYLIB, editorDylib, ec);
    ASSERT_FALSE(ec) << "copy the fixture dylib " << GE_MAC_BUNDLE_FIXTURE_DYLIB << ": " << ec.message();
    ASSERT_TRUE(ns::WriteBuildCacheRecord(editorBuildDir,
                                          ns::BuildCacheRecord{"digest", editorDylib.string(), "abi"}));

    const fs::path bundle = root / "Game.app";
    const fs::path contentRoot = GameEngine::PathUtils::InstallContentRootFor(bundle / "Contents" / "MacOS");
    std::vector<std::string> errors;
    ASSERT_EQ(GameEngine::StageNativeUserScriptsForPackage(projectRoot, contentRoot, {"abi", {}, {}}, errors, false),
              GameEngine::NativeScriptStageOutcome::Staged);
    ASSERT_TRUE(GameEngine::MacBundleAssembler::MoveNativeModulesIntoFrameworks(bundle, errors));
    for (const std::string& error : errors)
        ADD_FAILURE() << error;
    ASSERT_TRUE(fs::exists(bundle / "Contents" / "Frameworks" / "UserScripts.dylib"));

    EXPECT_EQ(PackagedModuleMapBeginsFromThePlayerContentRoot(bundle / "Contents"), 1)
        << "the record the export wrote is not where the Player's content root looks for it";
    fs::remove_all(root, ec);
}
#endif

// ---------------------------------------------------------------------------
// Dev-cache engine-identity gate. A package module's build cache lives in the
// MACHINE-GLOBAL PackageCache, so the record a Player finds there may have been
// written by a different engine build entirely (another worktree). Loading such a
// DLL runs its static initializers against the wrong Engine.dll — a heap fault
// inside DLL_PROCESS_ATTACH, before any exported handshake can refuse it. So the
// gate has to run BEFORE the map, and that is what these tests assert.
//
// The return value alone cannot prove it: the DLL on disk is garbage, so a load
// ATTEMPT also returns false. The instrument is ImageMapBegin, which the manager
// fires immediately before LoadLibrary and nowhere else — zero begins means the
// image was never mapped. LoadModule has no earlier bail-out for a bad file, so a
// reached map always registers (the matching-identity case below is that positive
// control; without it a permanently-broken gate would look like a pass).
// ---------------------------------------------------------------------------
namespace
{
struct DevCacheProbe
{
    bool Loaded = false;
    int MapBegins = 0;
};

// Writes a dev-cache record (no engine_abi marker) naming a garbage DLL, then
// asks the manager to load it. `dllStem` keeps each case on its own path so the
// manager's already-mapped early-return can never mask a result.
DevCacheProbe ProbeDevCacheLoad(const fs::path& root, const std::string& dllStem,
                                const std::string& recordEngineBuildId)
{
    std::error_code ec;
    const fs::path buildDir = root / "NativeScripts" / "build"; // package-module dev layout
    fs::create_directories(buildDir, ec);
    const fs::path dll = buildDir / (dllStem + ".dll");
    WriteFileBytes(dll, "not-a-real-dll");
    // The editor's live cache records an ABSOLUTE path; mirror that exactly.
    EXPECT_TRUE(ns::WriteBuildCacheRecord(
        buildDir, ns::BuildCacheRecord{"dev", dll.generic_string(), "abi", recordEngineBuildId}));
    EXPECT_TRUE(ns::ReadEngineAbiMarker(buildDir).empty()) << "fixture must exercise the DEV branch";

    DevCacheProbe probe;
    ns::NativeScriptManager mgr;
    EXPECT_TRUE(mgr.Initialize(nullptr));
    ns::NativeScriptManager::ModuleImageObserver observer;
    observer.ImageMapBegin = [&probe] { ++probe.MapBegins; };
    observer.ImageMapEnd = [](std::uint64_t, std::uint64_t) {};
    mgr.SetModuleImageObserver(std::move(observer));
    probe.Loaded = mgr.LoadPrebuiltUserModule(root, dllStem);
    mgr.Shutdown();
    return probe;
}
} // namespace

TEST(NativeScriptManager, DevCacheRecordFromAnotherEngineBuildIsSkippedBeforeTheImageIsMapped)
{
    const fs::path root = fs::temp_directory_path() / "ge_nsm_devcache_foreign";
    std::error_code ec;
    fs::remove_all(root, ec);

    const DevCacheProbe probe = ProbeDevCacheLoad(root, "ForeignEngine", "not-this-engines-build-id");
    EXPECT_FALSE(probe.Loaded);
    EXPECT_EQ(probe.MapBegins, 0) << "the stale DLL was mapped anyway — the crash this gate exists to stop";

    fs::remove_all(root, ec);
}

TEST(NativeScriptManager, DevCacheRecordWithoutAnEngineIdentityIsSkippedBeforeTheImageIsMapped)
{
    const fs::path root = fs::temp_directory_path() / "ge_nsm_devcache_unstamped";
    std::error_code ec;
    fs::remove_all(root, ec);

    // A record written before the identity line existed. Unknown is NOT trusted:
    // the slot is machine-global, so "unknown" is far likelier to be another
    // engine's build than this one's.
    const DevCacheProbe probe = ProbeDevCacheLoad(root, "UnstampedEngine", "");
    EXPECT_FALSE(probe.Loaded);
    EXPECT_EQ(probe.MapBegins, 0);

    fs::remove_all(root, ec);
}

TEST(NativeScriptManager, DevCacheRecordFromThisEngineBuildStillReachesTheLoad)
{
    const fs::path root = fs::temp_directory_path() / "ge_nsm_devcache_matching";
    std::error_code ec;
    fs::remove_all(root, ec);

    ASSERT_FALSE(ns::EngineBuildIdentity().empty())
        << "this platform must be able to identify its own engine image, or the gate is inert here";

    const DevCacheProbe probe = ProbeDevCacheLoad(root, "MatchingEngine", ns::EngineBuildIdentity());
    EXPECT_EQ(probe.MapBegins, 1) << "a module built by THIS engine must still be loaded";
    EXPECT_FALSE(probe.Loaded) << "the map is reached; it fails only because the fixture DLL is garbage";

    fs::remove_all(root, ec);
}

TEST(EngineBuildIdentity, IsStableWithinTheProcess)
{
    // Read off the running image, so every call must agree — the value is compared
    // against records written by earlier runs of the same binary.
    EXPECT_EQ(ns::EngineBuildIdentity(), ns::EngineBuildIdentity());
    EXPECT_FALSE(ns::EngineBuildIdentity().empty()) << "Windows always has the PE fallback inputs";
}

// ---------------------------------------------------------------------------
// C3: toolchain fingerprint handshake. LoadModule refuses a user DLL whose
// exported fingerprint differs from the host's in ANY field, with a diagnostic
// naming the field. The comparison is pure logic — tested here directly; the
// accept path is proven end-to-end by UserModuleBuildLoadTest (a really-built
// DLL exporting real values loads against this host).
// ---------------------------------------------------------------------------
TEST(ToolchainFingerprint, HostFingerprintIsPopulated)
{
    const GE_ToolchainFingerprint host = ns::HostToolchainFingerprint();
    EXPECT_NE(host.CompilerVendor, GE_COMPILER_UNKNOWN);
    EXPECT_NE(host.CompilerVersionMajor, 0u);
    EXPECT_NE(host.StdLibAbiTag, 0u) << "STL identity must be captured (see <version> include in the ABI header)";
}

TEST(ToolchainFingerprint, CaptureIsConsistentAcrossTranslationUnits)
{
    // This test TU and the engine's ToolchainFingerprint.cpp are different TUs of
    // the same build; the capture helper must produce identical values in both
    // (guards the "STL macros only exist after an STL include" pitfall).
    GE_ToolchainFingerprint local{};
    GE_FillToolchainFingerprint(&local);
    EXPECT_EQ(ns::DescribeToolchainFingerprintMismatch(ns::HostToolchainFingerprint(), local), "");
}

TEST(ToolchainFingerprint, MatchingFingerprintsProduceNoDiagnostic)
{
    const GE_ToolchainFingerprint host = ns::HostToolchainFingerprint();
    EXPECT_EQ(ns::DescribeToolchainFingerprintMismatch(host, host), "");
}

TEST(ToolchainFingerprint, EachMismatchingFieldIsNamedInTheDiagnostic)
{
    const GE_ToolchainFingerprint host = ns::HostToolchainFingerprint();
    const struct
    {
        const char* Name;
        uint32_t GE_ToolchainFingerprint::*Field;
    } kFields[] = {
        {"CompilerVendor", &GE_ToolchainFingerprint::CompilerVendor},
        {"CompilerVersionMajor", &GE_ToolchainFingerprint::CompilerVersionMajor},
        {"CompilerVersionMinor", &GE_ToolchainFingerprint::CompilerVersionMinor},
        {"CrtId", &GE_ToolchainFingerprint::CrtId},
        {"IteratorDebugLevel", &GE_ToolchainFingerprint::IteratorDebugLevel},
        {"ZcFlags", &GE_ToolchainFingerprint::ZcFlags},
        {"StdLibAbiTag", &GE_ToolchainFingerprint::StdLibAbiTag},
    };
    for (const auto& field : kFields)
    {
        GE_ToolchainFingerprint doctored = host;
        doctored.*(field.Field) += 1u;
        const std::string diag = ns::DescribeToolchainFingerprintMismatch(host, doctored);
        EXPECT_NE(diag.find(field.Name), std::string::npos)
            << "diagnostic must name the mismatching field '" << field.Name << "', got: " << diag;
    }
}

TEST(ToolchainFingerprint, PaddingIsNotCompared)
{
    GE_ToolchainFingerprint doctored = ns::HostToolchainFingerprint();
    doctored._Padding += 1u;
    EXPECT_EQ(ns::DescribeToolchainFingerprintMismatch(ns::HostToolchainFingerprint(), doctored), "");
}

TEST(ToolchainFingerprint, ZeroedLegacyModuleFingerprintIsRefused)
{
    // A DLL built by a pre-C3 SDK exports all zeroes; that must read as a mismatch
    // (rebuild with the current SDK fixes it), never as "compatible".
    const GE_ToolchainFingerprint zeroed{};
    EXPECT_NE(ns::DescribeToolchainFingerprintMismatch(ns::HostToolchainFingerprint(), zeroed), "");
}

// ---------------------------------------------------------------------------
// Crash isolation (D3): a hot-reloaded user system that hard-faults in a hook is
// caught by the SEH guard, logged, and disabled for the session — the host
// survives and later systems still run. The faulting path is Windows-only (SEH);
// off Windows the guard is a passthrough and the fault would kill the process, so
// that scenario is gated out.
// ---------------------------------------------------------------------------
namespace
{
// Minimal user systems for the registry tests. They ignore the World (the registry
// just forwards lifecycle calls); a counter proves the hook actually ran.
struct CountingUserSystem : ns::IUserSystem
{
    int updates = 0;
    void OnStart(GameEngine::ECS::World&) override {}
    void OnUpdate(GameEngine::ECS::World&, float) override { ++updates; }
    void OnDestroy(GameEngine::ECS::World&) override {}
    const char* Name() const override { return "CountingUserSystem"; }
};

#ifdef _WIN32
// Read through a volatile so the deliberate null write is neither constant-folded nor
// flagged as an obvious null deref by the compiler.
volatile std::uintptr_t g_faultAddress = 0;

struct FaultingUserSystem : ns::IUserSystem
{
    int attempts = 0;
    void OnStart(GameEngine::ECS::World&) override {}
    void OnUpdate(GameEngine::ECS::World&, float) override
    {
        ++attempts; // observed AFTER the fault (we live in the test's frame, not OnUpdate's)
        *reinterpret_cast<volatile int*>(g_faultAddress) = 0x0BADC0DE; // access violation
    }
    void OnDestroy(GameEngine::ECS::World&) override {}
    const char* Name() const override { return "FaultingUserSystem"; }
};
#endif
} // namespace

#ifdef _WIN32
TEST(NativeScriptCrashIsolation, HardFaultDisablesSystemAndHostSurvives)
{
    ns::ClearUserSystems();

    FaultingUserSystem faulty;
    CountingUserSystem healthy;
    ns::RegisterUserSystem(&faulty);  // index 0 — faults in OnUpdate
    ns::RegisterUserSystem(&healthy); // index 1 — must still run despite the fault before it

    GameEngine::ECS::World world;

    ns::TickUserSystems(world, 0.016f); // faulty hard-faults -> caught + disabled; healthy runs

    EXPECT_EQ(faulty.attempts, 1);
    EXPECT_EQ(healthy.updates, 1) << "a system after a faulting one must still tick";
    ASSERT_EQ(ns::GetUserSystems().size(), 2u);
    EXPECT_EQ(ns::GetUserSystems()[0].System, nullptr) << "faulting system must be disabled (slot nulled)";
    EXPECT_EQ(ns::GetUserSystems()[1].System, &healthy);

    ns::TickUserSystems(world, 0.016f); // faulty now skipped; healthy ticks again

    EXPECT_EQ(faulty.attempts, 1) << "disabled system must not be invoked again";
    EXPECT_EQ(healthy.updates, 2);

    ns::ClearUserSystems();
}
#endif

// All platforms: a healthy system ticks normally through the guard (no-fault path on
// Windows, plain passthrough elsewhere) across the full lifecycle.
TEST(NativeScriptCrashIsolation, HealthySystemTicksThroughGuard)
{
    ns::ClearUserSystems();

    CountingUserSystem healthy;
    ns::RegisterUserSystem(&healthy);

    GameEngine::ECS::World world;
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, 0.016f);
    ns::TickUserSystems(world, 0.016f);
    ns::StopUserSystems(world);

    EXPECT_EQ(healthy.updates, 2);

    ns::ClearUserSystems();
}

// ---------------------------------------------------------------------------
// Per-module registration (P1 packages): registrations carry the module id the
// host set while the module's registrars ran, and the per-module clear drops
// ONLY that module's systems — reloading one package can't wipe the project
// module (or another package).
// ---------------------------------------------------------------------------
TEST(UserSystemRegistry, PerModuleClearDropsOnlyThatModulesSystems)
{
    ns::ClearUserSystems();

    CountingUserSystem projectSystem;
    CountingUserSystem packageSystem;

    ns::SetActiveRegistrationModule("UserScripts");
    ns::RegisterUserSystem(&projectSystem);
    ns::SetActiveRegistrationModule("OceanPack");
    ns::RegisterUserSystem(&packageSystem);
    ns::SetActiveRegistrationModule({});

    ASSERT_EQ(ns::GetUserSystems().size(), 2u);
    EXPECT_EQ(ns::GetUserSystems()[0].ModuleId, "UserScripts");
    EXPECT_EQ(ns::GetUserSystems()[1].ModuleId, "OceanPack");

    // Package reload: clear + re-register the package module only.
    ns::ClearUserSystems("OceanPack");
    ASSERT_EQ(ns::GetUserSystems().size(), 1u);
    EXPECT_EQ(ns::GetUserSystems()[0].System, &projectSystem) << "project module systems must survive";

    CountingUserSystem packageSystemV2;
    ns::SetActiveRegistrationModule("OceanPack");
    ns::RegisterUserSystem(&packageSystemV2);
    ns::SetActiveRegistrationModule({});

    GameEngine::ECS::World world;
    ns::TickUserSystems(world, 0.016f);
    EXPECT_EQ(projectSystem.updates, 1);
    EXPECT_EQ(packageSystemV2.updates, 1);
    EXPECT_EQ(packageSystem.updates, 0) << "the cleared registration must not tick";

    // And the symmetric direction: project reload leaves the package alone.
    ns::ClearUserSystems("UserScripts");
    ASSERT_EQ(ns::GetUserSystems().size(), 1u);
    EXPECT_EQ(ns::GetUserSystems()[0].System, &packageSystemV2);

    ns::ClearUserSystems();
}

// Registrations made without a bracket (no active module) land on the empty
// module id and are cleared by the empty-id per-module clear.
TEST(UserSystemRegistry, UnbracketedRegistrationsLandOnEmptyModuleId)
{
    ns::ClearUserSystems();

    CountingUserSystem loose;
    ns::RegisterUserSystem(&loose);
    ASSERT_EQ(ns::GetUserSystems().size(), 1u);
    EXPECT_TRUE(ns::GetUserSystems()[0].ModuleId.empty());

    ns::ClearUserSystems("");
    EXPECT_TRUE(ns::GetUserSystems().empty());
}

// ---------------------------------------------------------------------------
// P3 prebuilt package binaries: fingerprint-keyed platform dirs + lookup.
// ---------------------------------------------------------------------------

namespace
{
std::filesystem::path MakePrebuiltTempDir()
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("ns_prebuilt_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteFileAt(const std::filesystem::path& p, const std::string& contents)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}
} // namespace

TEST(PrebuiltModuleBinaries, ShortHashIsStableAndFingerprintSensitive)
{
    const GE_ToolchainFingerprint host = ns::HostToolchainFingerprint();
    const std::string hash = ns::ToolchainFingerprintShortHash(host);
    EXPECT_EQ(hash.size(), 8u);
    EXPECT_EQ(hash, ns::ToolchainFingerprintShortHash(host)); // deterministic

    GE_ToolchainFingerprint other = host;
    other.CrtId ^= 0x1u; // debug<->release CRT flip must change the dir name
    EXPECT_NE(ns::ToolchainFingerprintShortHash(other), hash);

    GE_ToolchainFingerprint padded = host;
    padded._Padding = 0xFFFFFFFFu; // padding is never part of the identity
    EXPECT_EQ(ns::ToolchainFingerprintShortHash(padded), hash);
}

TEST(PrebuiltModuleBinaries, PlatformDirNameCarriesPlatformArchAndHash)
{
    const GE_ToolchainFingerprint host = ns::HostToolchainFingerprint();
    const std::string dir = ns::PrebuiltPlatformDirName(host);
#if defined(_WIN32)
    EXPECT_EQ(dir.rfind("windows-", 0), 0u) << dir;
#endif
    EXPECT_NE(dir.find(ns::ToolchainFingerprintShortHash(host)), std::string::npos) << dir;
}

TEST(PrebuiltModuleBinaries, ModuleFileNameIsTheModuleNameWithTheHostExtensionAndNoPrefix)
{
#if defined(_WIN32)
    EXPECT_EQ(ns::PrebuiltModuleFileName("Eztree.Editor"), "Eztree.Editor.dll");
#elif defined(__APPLE__)
    EXPECT_EQ(ns::PrebuiltModuleFileName("Eztree.Editor"), "Eztree.Editor.dylib");
#else
    EXPECT_EQ(ns::PrebuiltModuleFileName("Eztree.Editor"), "Eztree.Editor.so");
#endif
}

// A host with no engine-ABI digest (a dev Player probes with a config that carries none)
// still loads a marker-less prebuilt, whose own exported handshake is the gate, but cannot
// verify a marker and refuses the prebuilt that ships one.
TEST(PrebuiltModuleBinaries, LookupWithoutHostDigestRefusesOnlyMarkedPrebuilts)
{
    const GE_ToolchainFingerprint host = ns::HostToolchainFingerprint();
    const auto root = MakePrebuiltTempDir();
    const auto prebuilt = root / "Binaries";
    const auto platformDir = prebuilt / ns::PrebuiltPlatformDirName(host);
    const std::string dllName = ns::PrebuiltModuleFileName("WaterPack");
    WriteFileAt(platformDir / dllName, "fake-dll-bytes");

    auto lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "");
    EXPECT_EQ(lookup.Dll, platformDir / dllName) << lookup.Reason;

    ASSERT_TRUE(ns::WriteEngineAbiMarker(platformDir, "abi123"));
    lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "");
    EXPECT_TRUE(lookup.Dll.empty());
    EXPECT_NE(lookup.Reason.find("unavailable"), std::string::npos) << lookup.Reason;

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(PrebuiltModuleBinaries, LookupFindsMatchingDllAndHonorsMarker)
{
    const GE_ToolchainFingerprint host = ns::HostToolchainFingerprint();
    const auto root = MakePrebuiltTempDir();
    const auto prebuilt = root / "Binaries";
    const auto platformDir = prebuilt / ns::PrebuiltPlatformDirName(host);

#if defined(_WIN32)
    const char* dllName = "WaterPack.dll";
#elif defined(__APPLE__)
    const char* dllName = "WaterPack.dylib";
#else
    const char* dllName = "WaterPack.so";
#endif

    // Missing prebuilt root.
    auto lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "abi123");
    EXPECT_TRUE(lookup.Dll.empty());
    EXPECT_NE(lookup.Reason.find("does not exist"), std::string::npos) << lookup.Reason;

    // Root exists but only a foreign toolchain dir: reason names what ships.
    WriteFileAt(prebuilt / "windows-x64-00000000" / "placeholder.txt", "");
    lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "abi123");
    EXPECT_TRUE(lookup.Dll.empty());
    EXPECT_NE(lookup.Reason.find("windows-x64-00000000"), std::string::npos) << lookup.Reason;

    // Matching platform dir but no module DLL: CMake's own MODULE library name
    // (lib-prefixed .so on macOS and Linux) is not the probed name.
    WriteFileAt(platformDir / "unrelated.txt", "");
    WriteFileAt(platformDir / "libWaterPack.so", "");
    lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "abi123");
    EXPECT_TRUE(lookup.Dll.empty());
    EXPECT_NE(lookup.Reason.find("'" + std::string(dllName) + "' missing"), std::string::npos)
        << lookup.Reason;

    // DLL present, no marker: usable (the DLL's own handshake is the final gate).
    WriteFileAt(platformDir / dllName, "fake-dll-bytes");
    lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "abi123");
    EXPECT_EQ(lookup.Dll, platformDir / dllName);
    EXPECT_TRUE(lookup.Reason.empty()) << lookup.Reason;

    // Marker mismatch: refused with both digests named.
    ASSERT_TRUE(ns::WriteEngineAbiMarker(platformDir, "abi-other"));
    lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "abi123");
    EXPECT_TRUE(lookup.Dll.empty());
    EXPECT_NE(lookup.Reason.find("abi-other"), std::string::npos) << lookup.Reason;
    EXPECT_NE(lookup.Reason.find("abi123"), std::string::npos) << lookup.Reason;

    // Marker match: usable.
    ASSERT_TRUE(ns::WriteEngineAbiMarker(platformDir, "abi123"));
    lookup = ns::FindPrebuiltModule(prebuilt, host, "WaterPack", "abi123");
    EXPECT_EQ(lookup.Dll, platformDir / dllName);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// Play mode waits on builds of modules that ship with the game (PlayModeEntryGate), not on a batch
// of editor-only modules; a shipped module whose build failed is named, with its first error line.
TEST(NativeScriptManager, PlayWaitsOnShippedModuleBuildsAndReadsTheirFailure)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("ge_nsm_shipped_pending_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    // Each block runs a real cmake configure and a failing build after up to 20 s of toolchain
    // detection; the two together took 143 s on a loaded machine. Each gets its own bound well
    // above that, which only stops a build that never completes.
    constexpr auto kBuildDeadline = std::chrono::seconds(240);
    auto deadline = std::chrono::steady_clock::now() + kBuildDeadline;
    {
        ns::Testing::InFlightModuleBuild editorOnly(root / "editor" / "src", root / "editor" / "cache",
                                                    root / "editor" / "scratch", "Probe.Editor",
                                                    ns::Testing::ModuleKind::EditorOnly, nullptr);
        EXPECT_FALSE(editorOnly.Manager().AreShippedModuleBuildsPending())
            << "an editor-only batch held play mode";
        editorOnly.TickUntilCompleted(deadline);
        EXPECT_TRUE(editorOnly.Manager().FailedShippedModules().empty());
    }
    deadline = std::chrono::steady_clock::now() + kBuildDeadline;
    {
        ns::Testing::InFlightModuleBuild shipped(root / "shipped" / "src", root / "shipped" / "cache",
                                                 root / "shipped" / "scratch", "Probe", ns::Testing::ModuleKind::Shipped,
                                                 nullptr);
        EXPECT_TRUE(shipped.Manager().AreShippedModuleBuildsPending());
        shipped.TickUntilCompleted(deadline);
        ASSERT_EQ(shipped.Completions(), 1);
        EXPECT_FALSE(shipped.Manager().AreShippedModuleBuildsPending());
        EXPECT_EQ(shipped.Manager().FailedShippedModules(), std::vector<std::string>{"Probe"});
        EXPECT_FALSE(shipped.Manager().FirstBuildErrorLine("Probe").empty());
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// Play mode loads the project module and the runtime package modules, never an editor-only
// package module. A pass with all three kinds stale builds the first two as one batch and holds
// the editor-only module for a second: the play gate waits for the first batch alone (counting only
// its modules), while the module-pending signal and the first package pass wait for both.
TEST(NativeScriptManager, EditorOnlyModulesBuildAfterTheModulesPlayModeLoads)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("ge_nsm_build_priority_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "scratch");
    std::ofstream(root / "scratch" / "not-a-directory") << "blocks every generated project directory";

    // Each build fails fast at project generation: its project directory sits under a file.
    auto makeConfig = [&root](const std::string& name) {
        fs::create_directories(root / name / "src");
        std::ofstream(root / name / "src" / (name + ".cpp")) << "int GeBuildPriorityProbe() { return 1; }\n";
        ns::NativeBuildConfig config;
        config.SourceDir = root / name / "src";
        config.ProjectDir = root / "scratch" / "not-a-directory" / name;
        config.BuildDir = root / name / "cache" / "build";
        config.ActiveDir = root / name / "cache" / "active";
        config.ModuleName = name;
        return config;
    };
    ns::NativeBuildConfig runtimePackage = makeConfig("RuntimePackage");
    ns::NativeBuildConfig editorPackage = makeConfig("ToolsPackage.Editor");
    editorPackage.EditorImportLib = root / "EditorSDK.lib"; // absent file: only the kind matters
    ns::NativeBuildConfig project = makeConfig("UserScripts");

    JobSystem::WorkStealingThreadPool pool(2);
    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize(&pool));
    std::vector<std::string> completedModules;
    manager.SetBuildCompletedCallback([&completedModules](const ns::NativeBuildResult& result) {
        EXPECT_FALSE(result.Success());
        completedModules.push_back(result.ModuleId);
    });
    manager.SetPackageModuleConfigs({runtimePackage, editorPackage});
    manager.SetBuildConfig(project);
    manager.RequestRebuild();
    manager.Tick(); // dispatches the first batch

    EXPECT_TRUE(manager.AreShippedModuleBuildsPending());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (completedModules.size() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        manager.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // The first batch is the two modules play mode loads, applied in load order (packages first).
    ASSERT_EQ(completedModules.size(), 2u) << "the editor-only module was built in the same batch";
    EXPECT_EQ(completedModules, (std::vector<std::string>{"RuntimePackage", "UserScripts"}));
    EXPECT_EQ(manager.FailedShippedModules(), (std::vector<std::string>{"RuntimePackage", "UserScripts"}));
    EXPECT_FALSE(manager.AreShippedModuleBuildsPending()) << "play waited for an editor-only module";
    EXPECT_TRUE(manager.AreModulesPending()) << "the editor-only module was dropped";
    EXPECT_FALSE(manager.InitialPackageModulePassComplete());

    while (completedModules.size() < 3 && std::chrono::steady_clock::now() < deadline)
    {
        manager.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(completedModules.size(), 3u);
    EXPECT_EQ(completedModules, (std::vector<std::string>{"RuntimePackage", "UserScripts", "ToolsPackage.Editor"}));
    EXPECT_FALSE(manager.AreModulesPending());
    EXPECT_TRUE(manager.InitialPackageModulePassComplete());

    manager.Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
}

// A rebuild requested while the modules play mode loads are building (a source edit) goes ahead of
// the held editor-only batch: otherwise the play gate, which counts the request, would wait out
// every editor-only compile.
TEST(NativeScriptManager, ARebuildRequestedDuringTheFirstBatchGoesAheadOfEditorOnlyModules)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("ge_nsm_rebuild_priority_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "scratch");
    std::ofstream(root / "scratch" / "not-a-directory") << "blocks every generated project directory";

    // Each build fails fast at project generation, so the shipped modules stay stale and rebuild.
    auto makeConfig = [&root](const std::string& name) {
        fs::create_directories(root / name / "src");
        std::ofstream(root / name / "src" / (name + ".cpp")) << "int GeRebuildPriorityProbe() { return 1; }\n";
        ns::NativeBuildConfig config;
        config.SourceDir = root / name / "src";
        config.ProjectDir = root / "scratch" / "not-a-directory" / name;
        config.BuildDir = root / name / "cache" / "build";
        config.ActiveDir = root / name / "cache" / "active";
        config.ModuleName = name;
        return config;
    };
    ns::NativeBuildConfig editorPackage = makeConfig("ToolsPackage.Editor");
    editorPackage.EditorImportLib = root / "EditorSDK.lib"; // absent file: only the kind matters

    JobSystem::WorkStealingThreadPool pool(2);
    ns::NativeScriptManager manager;
    ASSERT_TRUE(manager.Initialize(&pool));
    int completions = 0;
    manager.SetBuildCompletedCallback([&completions](const ns::NativeBuildResult&) { ++completions; });
    manager.SetPackageModuleConfigs({makeConfig("RuntimePackage"), editorPackage});
    manager.SetBuildConfig(makeConfig("UserScripts"));
    manager.RequestRebuild();
    manager.Tick();           // dispatches the first batch: UserScripts and RuntimePackage
    manager.RequestRebuild(); // a source edit while it builds

    // Completion counts as each batch is applied: 2 (first batch), then the rebuilt pair (4), and
    // only then the editor-only module (5). A 3 means the editor-only module went first.
    std::vector<int> appliedCounts;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (completions < 5 && std::chrono::steady_clock::now() < deadline)
    {
        const int before = completions;
        manager.Tick();
        if (completions != before)
            appliedCounts.push_back(completions);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(appliedCounts, (std::vector<int>{2, 4, 5})) << "the editor-only module built before the rebuild";

    manager.Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
}

// A scanner the SDK manifest names but the disk lacks is reported by its path and the fix,
// before a shell runs it and answers with its own "not recognized" line.
TEST(CMakeInvoker, MissingComponentScannerIsNamedWithTheFix)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nsm_missing_scanner"));
    const fs::path root = scratch.Path();
    ns::NativeBuildConfig config;
    config.SourceDir = root / "Assets";
    config.BuildDir = root / "build";
    config.ComponentScannerExe = root / "nativescripting" / "ComponentScanner.exe";
    std::string output;
    std::string error;
    ns::ComponentScanStats stats;

    EXPECT_TRUE(ns::CMakeInvoker::RunComponentScanner(config, output, error, stats).empty());
    EXPECT_NE(std::string::npos, error.find("component scanner is missing at")) << error;
    EXPECT_NE(std::string::npos, error.find("nativescripting/ComponentScanner.exe")) << error;

    error.clear();
    EXPECT_TRUE(ns::CMakeInvoker::RunSystemScanner(config, output, error).empty());
    EXPECT_NE(std::string::npos, error.find("component scanner is missing at")) << error;
}

// The scan command carries the project's paths as UTF-8, the encoding the shell process converts a
// command from: a project under a folder the Windows ANSI code page cannot spell still scans.
TEST(CMakeInvoker, ScansAProjectWhosePathIsNotAscii)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_nsm_utf8_scan"));
    const fs::path root = scratch.Path() / fs::path(u8"\u30D7\u30ED\u30B8\u30A7\u30AF\u30C8");
    ns::NativeBuildConfig config;
    config.SourceDir = root / "Assets";
    config.BuildDir = root / "build";
    config.ComponentScannerExe = GE_COMPONENT_SCANNER_EXE;
    config.DetectBase = "ComponentBase";
    fs::create_directories(config.SourceDir);
    std::ofstream(config.SourceDir / "Speed.h", std::ios::binary) << "struct Speed : ECS::ComponentBase { float Value; };\n";
    std::string output;
    std::string error;
    ns::ComponentScanStats stats;

    const fs::path generated = ns::CMakeInvoker::RunComponentScanner(config, output, error, stats);

    EXPECT_EQ("", error) << output;
    EXPECT_EQ(1, stats.Reflected) << output;
    EXPECT_TRUE(fs::exists(generated)) << output;
}
