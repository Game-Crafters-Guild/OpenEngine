#include "Engine/Build/CancellableShellProcess.h"
#include "Platform/Process.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace
{

namespace fs = std::filesystem;

struct ExitScenario
{
    const char* Mode;
    int ExitCode;
};

class PlayerManagedExit : public ::testing::TestWithParam<ExitScenario>
{
};

TEST_P(PlayerManagedExit, RequestStopsPlayerAfterTheCurrentFrame)
{
    const auto& scenario = GetParam();
    const GameEngine::TestUtils::ScopedTempDir scratch(
        GameEngine::TestUtils::MakeUniqueTempDirectory("PlayerManagedExit"));
    const fs::path& project = scratch.Path();
    const fs::path fixture = GameEngine::TestPaths::ExecutableDirectory() / "PlayerExitFixture";
    ASSERT_TRUE(fs::is_regular_file(fixture / "ScriptAssemblies/GameEngine.Scripts.dll"));
    fs::copy(fixture, project, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::create_directories(project / "Profile");

    const nlohmann::json config{
        {"gameName", "PlayerManagedExitTests"},
        {"startupScene", "Main.scene"},
        {"scriptAssemblyPath", (project / "ScriptAssemblies/GameEngine.Scripts.dll").generic_string()}};
    std::ofstream(project / "game.config") << config.dump();

    GameEngine::ProcessLaunch launch;
    launch.Executable = GE_TEST_PLAYER_EXECUTABLE;
    launch.WorkingDirectory = project;
    // The guard ends a broken run; the assertions below require the script to end it at frame 3.
    launch.Arguments = {"--headless", "--fixed-fps", "60", "--quit-after", "120",
                        "--config", (project / "game.config").string(),
                        "--asset-root", (project / "Assets").string()};
    launch.Environment = {{"EXIT_PROBE_MODE", scenario.Mode}, {"APPDATA", (project / "Profile").string()}};

    constexpr std::chrono::minutes kShutdownTimeout{2};
    const auto deadline = std::chrono::steady_clock::now() + kShutdownTimeout;
    std::string output;
    int updateCalls = 0;
    GameEngine::CancellableShellProcess process;
    const auto result = process.Run(launch,
        [deadline] { return std::chrono::steady_clock::now() >= deadline; },
        [&output, &updateCalls](const std::string& line)
        {
            output += line + '\n';
            if (line.find("EXIT_PROBE_UPDATE ") != std::string::npos)
                ++updateCalls;
        });

    SCOPED_TRACE(output + result.stderrTail);
    ASSERT_FALSE(result.cancelled);
    EXPECT_EQ(result.exitCode, scenario.ExitCode);
    EXPECT_EQ(updateCalls, 3);
    EXPECT_NE(output.find("EXIT_PROBE_AFTER_REQUEST"), std::string::npos);
    EXPECT_NE(output.find("EXIT_PROBE_DESTROY 3"), std::string::npos);
    EXPECT_EQ(output.find("EXIT_PROBE_UNEXPECTED_ACCEPT"), std::string::npos);
    if (std::string(scenario.Mode) == "Refused")
        EXPECT_NE(output.find("EXIT_PROBE_REFUSED"), std::string::npos);
}

std::string ScenarioName(const ::testing::TestParamInfo<ExitScenario>& scenario)
{
    return scenario.param.Mode;
}

INSTANTIATE_TEST_SUITE_P(CSharp, PlayerManagedExit,
    ::testing::Values(ExitScenario{"Direct", 23}, ExitScenario{"Default", 0},
                      ExitScenario{"Posted", 23}, ExitScenario{"Refused", 23}),
    ScenarioName);

} // namespace
