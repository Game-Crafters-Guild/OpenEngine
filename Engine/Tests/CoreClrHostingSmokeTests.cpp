// Minimal cross-platform CoreCLR hosting smoke test.
// Verifies that CoreCLRHost can boot a runtime from a runtimeconfig and
// invoke CoreBridge.SanityPing via the load_assembly_and_get_function_pointer
// delegate when hosting is available.

#include <gtest/gtest.h>

#include "Core/ApplicationPaths.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/PathResolver.h"
#include "ScopedCurrentPath.h"
#include "TestTempDir.h"

#include <filesystem>

using namespace GameEngine;

TEST(CoreClrHostingSmoke, InitializeAndSanityPing)
{
#if !defined(NETHOST_AVAILABLE)
    GTEST_SKIP() << "NETHOST_AVAILABLE not defined; CoreCLR hosting is disabled in this build.";
#endif

    std::filesystem::path runtimeConfig = ScriptingPaths::ResolveScriptsRuntimeConfig();
    if (runtimeConfig.empty() || !std::filesystem::exists(runtimeConfig))
    {
        GTEST_SKIP() << "Scripts runtimeconfig.json not found; ensure CoreBridge/HotReload are staged.";
    }

    std::filesystem::path exeDir = ApplicationPaths::GetExecutableDirectory();
    std::filesystem::path preferred = exeDir.parent_path() / "GameEngine.CoreBridge.dll";
    std::filesystem::path coreBridgeDll = ScriptingPaths::ResolveCoreBridgeDll(preferred);
    if (coreBridgeDll.empty() || !std::filesystem::exists(coreBridgeDll))
    {
        GTEST_SKIP() << "GameEngine.CoreBridge.dll not found near test binary; hosting cannot be validated.";
    }

    // The host runs from an empty working directory and must write nothing into it.
    const TestUtils::ScopedTempDir workingDirectory(TestUtils::MakeUniqueTempDirectory("gameengine-coreclr-cwd"));
    {
        const TestUtils::ScopedCurrentPath workingDirectoryGuard(workingDirectory.Path());
        ASSERT_TRUE(workingDirectoryGuard.IsActive());

        CoreCLRHost host;
        ASSERT_FALSE(host.IsInitialized());
        ASSERT_TRUE(host.Initialize(runtimeConfig));

        void* fnPtr = host.GetManagedFunction(
            String(coreBridgeDll.string().c_str()),
            String("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge"),
            String("SanityPing"));

        ASSERT_NE(fnPtr, nullptr);

        using SanityPingFn = int32(CORECLR_DELEGATE_CALLTYPE*)();
        auto sanityPing = reinterpret_cast<SanityPingFn>(fnPtr);
        int32 value = sanityPing();
        EXPECT_EQ(value, 42);
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(workingDirectory.Path()))
        ADD_FAILURE() << "The host wrote into the working directory: " << entry.path().string();
}

TEST(CoreClrHostingSmoke, FailsWhenRuntimeConfigMissing)
{
#if !defined(NETHOST_AVAILABLE)
    GTEST_SKIP() << "NETHOST_AVAILABLE not defined; CoreCLR hosting is disabled in this build.";
#endif

    std::filesystem::path missingConfig = std::filesystem::path("./this_does_not_exist.runtimeconfig.json");
    CoreCLRHost host;
    EXPECT_FALSE(host.Initialize(missingConfig));
}
