#pragma once

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Scripting/ScriptsConfig.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace GameEngine::Testing
{

// The engine started on an empty temporary workspace, with the CLR off. A
// derived fixture calls SetUp first and TearDown last.
class TemporaryWorkspaceEngineFixture : public testing::Test
{
  protected:
    void SetUp() override
    {
        namespace fs = std::filesystem;
        m_Root = fs::temp_directory_path() / ("engine-test-workspace-" + GUID::Generate().ToString());
        fs::create_directories(m_Root / "Assets");
        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        EngineCore& engine = EngineCore::GetInstance();
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "TemporaryWorkspaceEngineTest";
        config.WorkspaceDirectory = m_Root.string();
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(engine.Initialize(config));
        m_Initialized = true;
    }

    void TearDown() override
    {
        if (m_Initialized)
            EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    std::filesystem::path m_Root;
    bool m_Initialized = false;
};

} // namespace GameEngine::Testing
