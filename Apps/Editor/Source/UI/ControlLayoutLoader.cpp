#include "UI/ControlLayoutLoader.h"

#include "AssetCore/AssetEvents.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Editor/EditorPaths.h"
#include "Logger/Logger.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIElement.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

/// The layout files that changed on disk since the last load. The asset event callback fills it on
/// the thread that raised the event; the next load empties it on the UI thread.
struct ControlLayoutLoader::ChangedFiles
{
    std::mutex Mutex;
    std::vector<std::filesystem::path> Paths;
};

namespace
{

/// True when both paths name one file. The watcher can spell the asset root another way, so the
/// file system decides when the spellings differ.
bool IsSameFile(const std::filesystem::path& first, const std::filesystem::path& second)
{
    if (first.lexically_normal() == second.lexically_normal())
        return true;
    std::error_code error;
    return std::filesystem::equivalent(first, second, error);
}

/// True for the events that a save or a delete of the file sends. A save by delete and create sends Created.
bool ChangesTheFile(AssetEventType eventType)
{
    return eventType == AssetEventType::AssetModified || eventType == AssetEventType::AssetCreated ||
           eventType == AssetEventType::AssetDestroyed;
}

} // namespace

ControlLayoutLoader& ControlLayoutLoader::Instance()
{
    static ControlLayoutLoader s_Loader;
    return s_Loader;
}

std::string ControlLayoutLoader::ReadText(const char* layoutAssetPath)
{
    const std::filesystem::path path = GetEditorGlobalPaths().installAssetsRoot / layoutAssetPath;
    std::string text;
    if (!ReadFileTextShared(path, text))
        Logger::Log::Error("{} did not read, so the control it lays out is empty. Check that it is staged under "
                           "the editor asset mount.",
                           path.generic_string());
    return text;
}

/// Adds the callback to the engine's asset events when no dispatcher holds it: at the first load and
/// after the dispatcher that held it is destroyed or cleared. Each new callback drops every text, because
/// no callback saw the edits before it.
void ControlLayoutLoader::ListenToLayoutEdits()
{
    if (!m_ChangedFiles.expired())
        return;
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;
    auto changed = std::make_shared<ChangedFiles>();
    m_ChangedFiles = changed;
    m_Texts.Clear();
    engine.GetAssetManager().GetEventDispatcher().AddCallback(
        [changed = std::move(changed)](const AssetEvent& event)
        {
            if (event.Type != AssetType::UILayout || !ChangesTheFile(event.EventType))
                return;
            std::lock_guard<std::mutex> lock(changed->Mutex);
            changed->Paths.emplace_back(event.AssetPath);
        });
}

void ControlLayoutLoader::DropChangedTexts()
{
    const std::shared_ptr<ChangedFiles> changed = m_ChangedFiles.lock();
    if (!changed)
        return;
    std::vector<std::filesystem::path> paths;
    {
        std::lock_guard<std::mutex> lock(changed->Mutex);
        paths.swap(changed->Paths);
    }
    if (paths.empty())
        return;
    const std::filesystem::path root = GetEditorGlobalPaths().installAssetsRoot;
    for (const std::filesystem::path& path : paths)
        m_Texts.EraseIf([&root, &path](const std::string& layoutAssetPath, const auto&)
                        { return IsSameFile(root / layoutAssetPath, path); });
}

std::unique_ptr<UIElement> ControlLayoutLoader::Load(const char* layoutAssetPath, std::span<const std::string_view> requiredIds)
{
    ControlLayoutLoader& loader = Instance();
    loader.ListenToLayoutEdits();
    loader.DropChangedTexts();
    const std::shared_ptr<const std::string>* cached = loader.m_Texts.Find(layoutAssetPath);
    std::shared_ptr<const std::string> text = cached ? *cached : nullptr;
    if (!text)
    {
        std::string read = ReadText(layoutAssetPath);
        if (read.empty())
            return nullptr;
        text = std::make_shared<const std::string>(std::move(read));
        loader.m_Texts.InsertOrAssign(layoutAssetPath, text);
    }
    std::unique_ptr<UIElement> root;
    if (!UIParsing::XMLParser::ParseLayoutFromString(*text, root, layoutAssetPath) || !root)
    {
        Logger::Log::Error("{} did not parse, so the control it lays out cannot open.", layoutAssetPath);
        return nullptr;
    }
    for (const std::string_view id : requiredIds)
    {
        if (!root->FindById(HashStringId(id)))
        {
            Logger::Log::Error("{} has no element with the id '{}', so the control it lays out cannot open. "
                               "Restore the id.",
                               layoutAssetPath, id);
            return nullptr;
        }
    }
    return root;
}

} // namespace GameEngine::Editor
