#pragma once

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Scripting/ScriptsConfig.h"
#include "StagedTestPaths.h"

#include <filesystem>
#include <system_error>

namespace GameEngine::Testing
{
// Starts a scripting-free EngineCore (no .NET runtime in this target) on `workspace`, whose assets are in
// "Assets". Initialize moves the working directory. The destructor stops the engine, restores the working
// directory and removes the workspace. The engine does not start if the working directory is unreadable
// or if the shader packages do not copy.
class ScopedScriptingFreeEngine
{
public:
    ScopedScriptingFreeEngine(const char* name, const std::filesystem::path& workspace)
        : m_Workspace(workspace)
    {
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ApplicationConfig cfg{};
        cfg.Name = name;
        cfg.WorkspaceDirectory = workspace.string();
        cfg.AssetDirectory = "Assets";
        std::error_code ec;
        m_CallerWorkingDirectory = std::filesystem::current_path(ec);
        if (!ec && CopyShaderPackages())
            m_Up = EngineCore::GetInstance().Initialize(cfg);
    }
    ~ScopedScriptingFreeEngine()
    {
        if (m_Up)
            EngineCore::GetInstance().Shutdown();
        std::error_code ignored;
        if (!m_CallerWorkingDirectory.empty())
            std::filesystem::current_path(m_CallerWorkingDirectory, ignored);
        std::filesystem::remove_all(m_Workspace, ignored);
    }
    ScopedScriptingFreeEngine(const ScopedScriptingFreeEngine&) = delete;
    ScopedScriptingFreeEngine& operator=(const ScopedScriptingFreeEngine&) = delete;

    bool Up() const { return m_Up; }

private:
    // The engine resolves "Shaders/<name>.shaderpkg" in the project's Assets, not in the working directory.
    // So the build's shader packages are copied there, or the nodes that load them declare nothing.
    bool CopyShaderPackages() const
    {
        namespace fs = std::filesystem;
        const fs::path target = m_Workspace / "Assets" / "Shaders";
        std::error_code ec;
        fs::create_directories(target, ec);
        if (ec)
            return false;
        fs::directory_iterator it(TestPaths::StagedRoot() / "Shaders", ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            if (it->path().extension() == ".shaderpkg")
                fs::copy_file(it->path(), target / it->path().filename(), fs::copy_options::overwrite_existing, ec);
        }
        return !ec;
    }

    std::filesystem::path m_Workspace;
    std::filesystem::path m_CallerWorkingDirectory;
    bool m_Up = false;
};
} // namespace GameEngine::Testing
