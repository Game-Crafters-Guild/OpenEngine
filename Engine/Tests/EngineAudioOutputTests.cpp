// An engine started with the default ApplicationConfig mixes on the Silent output
// and opens no audio device. Only an application that sets AudioOutput::Device
// opens the user's default playback device.

#include "EngineLogCapture.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr std::string_view kSilentOutputNotice = "Audio: output Silent";

// One EngineCore::Initialize with the default ApplicationConfig, with every engine
// log line down to Info captured across it. The workspace is a throwaway directory.
class EngineAudioOutputTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Capture.emplace(&m_Lines, Logger::LogLevel::Info);

        // Positive control on the instrument: a zero read out of a sink that
        // receives nothing would pass a missing notice.
        Logger::Log::Info("EngineAudioOutput log-capture self test");
        Logger::Log::Flush();
        ASSERT_EQ(TestLog::CountLinesContaining(m_Lines, "log-capture self test"), 1u)
            << "the log capture is not receiving Info-level engine lines";
        m_Lines.clear();

        m_OriginalWorkingDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("EngineAudioOutputTests");

        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));
    }

    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::current_path(m_OriginalWorkingDirectory, ec);
        std::filesystem::remove_all(m_Workspace, ec);
    }

    // Declared before the capture so the sink is torn down first.
    std::vector<std::string> m_Lines;
    std::optional<TestLog::ScopedEngineLogCapture> m_Capture;
    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
};

TEST_F(EngineAudioOutputTests, TheDefaultConfigMixesOnTheSilentOutput)
{
    // GetAudioSystem joins the asynchronous audio init.
    ASSERT_NE(EngineCore::GetInstance().GetAudioSystem(), nullptr);
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(m_Lines, kSilentOutputNotice), 1u)
        << "the default ApplicationConfig opened an audio device";
}

} // namespace
} // namespace GameEngine
