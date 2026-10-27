#include "Prefabs/CreatePrefabCommand.h"

#include "EditorChangeNotifications.h"
#include "SceneView/SceneViewNoticeOverlay.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/PrefabAuthoring.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

// The number of instances of `prefab` in `world`.
std::size_t CountInstancesOf(ECS::World& world, const GUID& prefab)
{
    std::size_t count = 0;
    world.Query<ECS::Read<Components::PrefabInstance>>().IncludeDisabled().Each(
        [&count, &prefab](ECS::EntityHandle, const Components::PrefabInstance& instance)
        { count += instance.Source == prefab ? 1u : 0u; });
    return count;
}

// A scene file written at or after `since` that names `prefab`, or nothing. Only the scenes written
// since then are read.
std::optional<std::filesystem::path> FindSceneWrittenSinceThatUses(const GUID& prefab,
                                                                   std::filesystem::file_time_type since)
{
    AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    for (const GUID& scene : registry.GetAssetsByType(AssetType::Scene))
    {
        AssetMetadata metadata;
        if (!registry.TryGetAssetMetadata(scene, metadata))
            continue;
        std::error_code ec;
        const std::filesystem::file_time_type written = std::filesystem::last_write_time(metadata.Path, ec);
        if (ec || written < since)
            continue;
        const auto uses = registry.RefreshDependencies(scene);
        if (std::find(uses.begin(), uses.end(), prefab) != uses.end())
            return metadata.Path;
    }
    return std::nullopt;
}

} // namespace

std::string CreatePrefabCommand::DescribeReplaceRefusal(ECS::World& world, const std::filesystem::path& prefabFile)
{
    // A Replace changes every instance of the old prefab at the next save, so it waits for Apply.
    const GUID replaced = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(prefabFile);
    const std::size_t instances = replaced.IsNull() ? 0 : CountInstancesOf(world, replaced);
    if (instances == 0)
        return {};
    return std::format("{} cannot be replaced while {} {} in this scene {} it. Choose another name.",
                       prefabFile.filename().string(), instances, instances == 1 ? "instance" : "instances",
                       instances == 1 ? "uses" : "use");
}

CreatePrefabCommand::CreatePrefabCommand(ConstructionKey, ECS::World& world, EditorChangeNotifications* notifications,
                                         ECS::EntityHandle root, std::filesystem::path prefabFile)
    : m_World(world)
    , m_Notifications(notifications)
    , m_Root(root)
    , m_PrefabFile(std::move(prefabFile))
{
    const auto* name = world.GetComponent<Components::Name>(root);
    m_Name = name && !name->View().empty() ? std::format("Create Prefab {}", name->View()) : "Create Prefab";
}

std::unique_ptr<CreatePrefabCommand> CreatePrefabCommand::Create(ECS::World& world,
                                                                 EditorChangeNotifications* notifications,
                                                                 ECS::EntityHandle root,
                                                                 const std::filesystem::path& prefabFile, bool replace,
                                                                 std::string& outError)
{
    if (!world.IsValid(root))
    {
        outError = "Cannot create a prefab: the entity no longer exists. Select an entity in the scene.";
        return nullptr;
    }
    auto command = std::make_unique<CreatePrefabCommand>(ConstructionKey{}, world, notifications, root, prefabFile);
    std::error_code ec;
    if (replace && std::filesystem::exists(prefabFile, ec))
    {
        if (std::string refusal = DescribeReplaceRefusal(world, prefabFile); !refusal.empty())
        {
            outError = std::move(refusal);
            return nullptr;
        }
        if (!command->SetReplacedFileAside(outError))
            return nullptr;
    }
    Scene::CreatedPrefab created = Scene::CreatePrefab(world, root, prefabFile, outError);
    if (created.Prefab.IsNull())
    {
        command->WriteReplacedFileBack();
        return nullptr;
    }
    command->m_Prefab = created.Prefab;
    command->m_MembersBefore = std::move(created.Members);
    command->m_WrittenAt = std::filesystem::last_write_time(prefabFile, ec);
    NotifyWorldStructure(notifications, &world);
    return command;
}

bool CreatePrefabCommand::SetReplacedFileAside(std::string& outError)
{
    std::ifstream in(m_PrefabFile, std::ios::binary);
    if (!in)
    {
        outError = std::format("Cannot replace {}: the file cannot be read. Close the program that holds it.",
                               m_PrefabFile.string());
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    if (!std::filesystem::remove(m_PrefabFile, ec))
    {
        outError = std::format("Cannot replace {}: the file cannot be removed ({}). Close the program that holds it.",
                               m_PrefabFile.string(), ec.message());
        return false;
    }
    m_ReplacedBytes = std::move(bytes);
    return true;
}

void CreatePrefabCommand::WriteReplacedFileBack() const
{
    if (!m_ReplacedBytes)
        return;
    std::ofstream out(m_PrefabFile, std::ios::binary);
    out << *m_ReplacedBytes;
    if (!out)
        Logger::Log::Error("Prefabs: cannot write back {}, the file the new prefab replaced. Its contents are lost.",
                           m_PrefabFile.string());
}

void CreatePrefabCommand::RemoveFileUnlessASceneUsesIt()
{
    if (const std::optional<std::filesystem::path> scene = FindSceneWrittenSinceThatUses(m_Prefab, m_WrittenAt))
    {
        m_FileKept = true;
        Logger::Log::Info("Prefabs: the undo of {} keeps {}, because the saved scene {} uses it.", m_Name,
                          m_PrefabFile.string(), scene->string());
        ShowSceneViewNotice(std::format("{} stays in the project, because the saved scene {} uses it.",
                                        m_PrefabFile.filename().string(), scene->filename().string()));
        return;
    }
    std::error_code ec;
    std::filesystem::remove(m_PrefabFile, ec);
}

void CreatePrefabCommand::Undo()
{
    Scene::RestorePrefabIdentities(m_World, m_MembersBefore);
    if (m_ReplacedBytes)
    {
        std::error_code ec;
        std::filesystem::remove(m_PrefabFile, ec);
        WriteReplacedFileBack();
        Scene::RebuildPrefab(m_Prefab);
    }
    else
        RemoveFileUnlessASceneUsesIt();
    NotifyWorldStructure(m_Notifications, &m_World);
}

void CreatePrefabCommand::Redo()
{
    std::error_code ec;
    if (m_ReplacedBytes || m_FileKept)
        std::filesystem::remove(m_PrefabFile, ec);
    m_FileKept = false;
    std::string error;
    if (Scene::CreatePrefab(m_World, m_Root, m_PrefabFile, error).Prefab.IsNull())
    {
        Logger::Log::Error("Prefabs: the redo of {} failed: {}", m_Name, error);
        WriteReplacedFileBack();
    }
    else
        m_WrittenAt = std::filesystem::last_write_time(m_PrefabFile, ec);
    NotifyWorldStructure(m_Notifications, &m_World);
}

} // namespace GameEngine::Editor
