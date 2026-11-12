#include "Terrain/Planet/PlanetHeightLayerSaver.h"

#include "EditorChangeNotifications.h"
#include "Scene/SceneBackupRecovery.h"

#include "Assets/AssetRegistry.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "PageStreaming/Height/HeightLayerStore.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "TerrainECS/TerrainService.h"
#include "Types/PathUtils.h"

#include <chrono>
#include <string>
#include <system_error>

namespace GameEngine::Editor
{

namespace
{

constexpr const char* kHeightLayerExtension = ".gepage";

void SetHeightLayerReference(ECS::World& world, ECS::EntityHandle entity, const GUID& asset,
                             EditorChangeNotifications* notifications)
{
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    if (!terrain || terrain->HeightLayerGuid.ToGuid() == asset)
        return;
    Components::Terrain updated = *terrain;
    if (asset.IsNull())
        updated.HeightLayerGuid.Clear();
    else
        updated.HeightLayerGuid.Set(asset);
    world.AddComponentImmediate(entity, updated);
    // A save sets the reference and writes the scene, so the rows rebuild and the scene stays clean.
    if (notifications)
        notifications->NotifyComponentChange<Components::Terrain>(
            &world, entity, EditorChangeNotifications::ChangeKind::InspectorRebuild);
}

} // namespace

uint32_t FlushPlanetHeightLayer(ECS::World& world, const std::filesystem::path& scenePath, AssetRegistry& registry,
                                TerrainECS::TerrainService& service, ZonePayloadFlushMode mode,
                                ZonePayloadStagingLedger* stagingLedger, EditorChangeNotifications* notifications)
{
    const bool staging = mode == ZonePayloadFlushMode::BackupExistingOnly;
    if (staging && !stagingLedger)
        return 0;

    // The sculpt belongs to the active spherical planet, the terrain the brush and the renderer use.
    Components::Terrain terrain{};
    const ECS::EntityHandle entity = CBTTerrainECS::FindActiveTerrainEntity(world);
    if (!entity.IsValid() || !CBTTerrainECS::FindActiveTerrain(world, terrain) ||
        terrain.Domain != Components::TerrainDomain::Spherical)
        return 0;
    const uint32 planet = entity.id;
    if (!service.PlanetHeightLayerNeedsSave(planet))
        return 0;
    const TerrainECS::TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet);
    const uint64 version = overlay->SculptVersion;
    const bool empty = !overlay->Layer || overlay->Layer->Bytes() == 0u;

    // The file the reference names is the write anchor, so the GUID stays the same across saves.
    const GUID guid = terrain.HeightLayerGuid.ToGuid();
    std::filesystem::path liveTarget;
    AssetMetadata meta{};
    if (!guid.IsNull() && registry.TryGetAssetMetadata(guid, meta) && !meta.Path.empty() &&
        LowerCaseExtension(meta.Path) == kHeightLayerExtension)
        liveTarget = meta.Path;
    const bool hasExisting = !liveTarget.empty();

    if (staging)
    {
        // A backup never mints a file or changes the scene; an emptied sculpt waits for the next Save.
        if (!hasExisting || empty || stagingLedger->StagedHeightLayerVersion == version)
            return 0;
        if (const std::string reason = PageStreaming::SaveHeightLayer(*overlay->Layer, StagedZonePayloadPathFor(liveTarget));
            !reason.empty())
        {
            Logger::Log::Warning("The planet's height layer backup was not written: {}", reason);
            return 0;
        }
        stagingLedger->StagedHeightLayerVersion = version;
        return 1;
    }

    if (stagingLedger)
        stagingLedger->StagedHeightLayerVersion = 0;
    std::error_code ec;
    if (empty)
    {
        if (hasExisting)
        {
            std::filesystem::remove(liveTarget, ec);
            std::filesystem::remove(StagedZonePayloadPathFor(liveTarget), ec);
        }
        SetHeightLayerReference(world, entity, GUID::Null(), notifications);
        service.MarkPlanetHeightLayerSaved(planet, GUID::Null(), version);
        return hasExisting ? 1u : 0u;
    }

    if (!hasExisting)
    {
        const std::filesystem::path zonesDir = SceneZonesFolderFor(scenePath);
        std::filesystem::create_directories(zonesDir, ec);
        liveTarget = zonesDir / (GUID::Generate().ToString() + kHeightLayerExtension);
    }
    const auto saveStart = std::chrono::steady_clock::now();
    if (const std::string reason = PageStreaming::SaveHeightLayer(*overlay->Layer, liveTarget); !reason.empty())
    {
        Logger::Log::Warning("The planet's height layer was not saved: {}", reason);
        return 0;
    }
    const auto saveEnd = std::chrono::steady_clock::now();
    // The live file supersedes a staged backup: a stale one would be offered after a later crash.
    std::filesystem::remove(StagedZonePayloadPathFor(liveTarget), ec);

    // The project resolves a file by its path-derived GUID: the reference takes that GUID.
    const auto registered = registry.RegisterAssetByPath(liveTarget);
    if (!registered.IsOk())
        Logger::Log::Warning("The planet's height layer {} was saved, but the project did not register it: the "
                             "Terrain's Height Layer keeps its earlier value and does not load this file",
                             liveTarget.string());
    const GUID fileGuid = registered.IsOk() ? registered.Value() : guid;
    SetHeightLayerReference(world, entity, fileGuid, notifications);
    service.MarkPlanetHeightLayerSaved(planet, fileGuid, version);
    Logger::Log::Info("Planet height layer: wrote {} ({} KiB) in {:.1f} ms", liveTarget.filename().string(),
                      std::filesystem::file_size(liveTarget, ec) / 1024u,
                      std::chrono::duration<float, std::milli>(saveEnd - saveStart).count());
    return 1;
}

} // namespace GameEngine::Editor
