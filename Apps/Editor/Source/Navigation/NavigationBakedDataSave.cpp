#include "Navigation/NavigationBakedDataSave.h"

#include "Terrain/TerrainZonePayloadSaver.h"

#include "AssetCore/SharedFileRead.h"
#include "Components/SceneEntityTag.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <unordered_set>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

// The file name a surface's baked data takes beside `scenePath`: the scene's name, then the
// surface's scene tag (16 hex digits of its FNV-1a) once it has one, else a new GUID.
std::filesystem::path BakedDataFileFor(ECS::World& world, ECS::EntityHandle entity, const std::filesystem::path& scenePath)
{
    const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity);
    const std::string identity = tag && !tag->View().empty() ? std::format("{:016x}", Hashing::Fnv1a64(tag->View()))
                                                             : GUID::Generate().ToString();
    return scenePath.parent_path() / (scenePath.stem().string() + "-" + identity + ".navsurface");
}

// `text` in lower case: the registry may report a path case-folded.
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// True when `stem` is a name BakedDataFileFor gives under `prefix`: the prefix, then a tag's 16 hex
// digits or a GUID's 36 characters. Compared without case.
bool IsSceneFileName(const std::string& stem, const std::string& prefix)
{
    if (!Lower(stem).starts_with(Lower(prefix)))
        return false;
    const size_t identity = stem.size() - prefix.size();
    return identity == 16 || identity == 36;
}

// The file a surface's baked data goes to. The file its BakedData names is kept when it is this
// scene's (beside it, named after it) and no surface before it in `claimed` writes it; otherwise
// the surface gets its own: a copied surface or a scene saved under a new name never writes into
// another's file.
std::filesystem::path BakedDataTarget(ECS::World& world, ECS::EntityHandle entity,
                                      const Components::NavigationSurface& surface,
                                      const std::filesystem::path& scenePath, AssetRegistry& registry,
                                      std::unordered_set<std::string>& claimed)
{
    AssetMetadata metadata{};
    const std::string prefix = scenePath.stem().string() + "-";
    if (!surface.BakedData.IsNull() && registry.TryGetAssetMetadata(surface.BakedData.ToGuid(), metadata) &&
        Lower(metadata.Path.extension().string()) == ".navsurface" &&
        Lower(metadata.Path.parent_path().generic_string()) == Lower(scenePath.parent_path().generic_string()) &&
        IsSceneFileName(metadata.Path.stem().string(), prefix) && claimed.insert(Lower(metadata.Path.generic_string())).second)
        return metadata.Path;
    std::filesystem::path target = BakedDataFileFor(world, entity, scenePath);
    while (!claimed.insert(Lower(target.generic_string())).second)
        target = scenePath.parent_path() / (prefix + GUID::Generate().ToString() + ".navsurface");
    return target;
}

bool SameBytes(const std::filesystem::path& file, const std::vector<uint8>& bytes)
{
    std::vector<uint8> existing;
    return ReadFileBytesShared(file, existing) && existing == bytes;
}

} // namespace

uint32_t WriteNavigationBakedDataForSave(ECS::World& world, const std::filesystem::path& scenePath,
                                         AssetManager& assets)
{
    AssetRegistry& registry = assets.GetRegistry();
    std::vector<ECS::EntityHandle> surfaces;
    world.Query<ECS::Read<Components::NavigationSurface>>().Each(
        [&](ECS::EntityHandle entity, const Components::NavigationSurface&) { surfaces.push_back(entity); });

    // Entity order, so the first of two surfaces that name one file keeps it.
    std::sort(surfaces.begin(), surfaces.end(), [](ECS::EntityHandle a, ECS::EntityHandle b) { return a.id < b.id; });
    std::unordered_set<std::string> claimed;
    uint32_t written = 0;
    for (const ECS::EntityHandle entity : surfaces)
    {
        std::vector<uint8> bytes;
        if (!Navigation::WriteBakedSurface(world, entity, bytes))
            continue;
        const auto& surface = *world.GetComponent<Components::NavigationSurface>(entity);
        const std::filesystem::path target = BakedDataTarget(world, entity, surface, scenePath, registry, claimed);
        if (!SameBytes(target, bytes))
        {
            auto write = assets.ExpectWrite(target);
            if (!WriteBlobAtomic(target, bytes))
            {
                Logger::Log::Warning("Navigation surface on entity {}: could not write its baked data to '{}'; the "
                                     "scene bakes it when it opens",
                                     entity.id, target.string());
                continue;
            }
            write.Report(target);
            ++written;
        }
        const auto registered = registry.RegisterAssetByPath(target);
        if (!registered.IsOk() || registered.Value() == surface.BakedData.ToGuid())
            continue;
        if (auto* writable = world.GetComponentForWrite<Components::NavigationSurface>(entity))
            writable->BakedData.Set(registered.Value());
    }
    return written;
}

} // namespace GameEngine::Editor
