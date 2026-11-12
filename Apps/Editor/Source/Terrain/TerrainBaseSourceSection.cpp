#include "Terrain/TerrainBaseSourceSection.h"

#include "InspectorRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Terrain/Terrain.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "EditorChangeNotifications.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "Types/FormatNumber.h"
#include "Types/StringUtils.h"

#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponent/InspectorComponentSection.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Terrain/TerrainEntityProvisioning.h"
#include "Terrain/TerrainModifier/GeneratedNoiseNoticeHost.h"
#include "Terrain/TerrainModifier/TerrainModifierInspectors.h"
#include "UI/AssetField.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/SeedField.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

using InspectorDrag::CommitComponentWithUndo;

// The significant digits of the Weight on the relief line: a dragged value reads 0.637, not 0.63741.
constexpr int kReliefWeightDigits = 3;

// Option values are the TerrainBaseSource enumerators, so the scene file, set_component and this
// dropdown all name a source by the same number.
const std::vector<Dropdown::Option>& BaseSourceOptions()
{
    static const std::vector<Dropdown::Option> kOptions = {
        {"1", "Heightmap"},
        {"2", "Flat"},
        {"3", "Function"},
    };
    return kOptions;
}

void AddBaseSourceRow(UIElement* content, const InspectorContext& ctx, Components::TerrainBaseSource source)
{
    Dropdown* dropdown = InspectorUI::AddDropdownRow(
        content, "Base Source", BaseSourceOptions(),
        InspectorUI::OptionIndexOfValue(BaseSourceOptions(), std::to_string(static_cast<uint32>(source))),
        "Where the terrain's heights come from before its modifiers apply. Flat is level at the "
        "terrain's position; a Global volume's Noise effect in Generated mode adds hills to it. "
        "Heightmap reads an imported heightmap asset (.r16, .r32 or a 16-bit PNG) spread over "
        "Size X and Size Z. Function reads heights from a function that a game module registers. Pick it in "
        "the Height Function row and a seed in the Seed row.");
    dropdown->SetId("terrain-base-source");
    dropdown->SetOnValueChanged(
        [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
         requestRefresh = ctx.RequestInspectorRefresh](const std::string& value)
        {
            const auto picked = static_cast<Components::TerrainBaseSource>(std::stoi(value));
            CommitComponentWithUndo<Components::Terrain>(w, e, n, undo, "Change Terrain Base Source",
                                                         [picked](Components::Terrain& u) { u.BaseSource = picked; });
            // The heightmap row belongs to the Heightmap source only.
            if (requestRefresh)
                requestRefresh();
        });
}

void AddHeightmapRow(UIElement* content, const InspectorContext& ctx, const GUID& heightmap)
{
    AssetField* field = InspectorUI::AddAssetFieldRow(
        content, "Heightmap", heightmap, {AssetType::TerrainHeightmap, AssetType::Texture},
        &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
         requestRefresh = ctx.RequestInspectorRefresh](const GUID& guid)
        {
            CommitComponentWithUndo<Components::Terrain>(w, e, n, undo, "Change Terrain Heightmap",
                                                         [guid](Components::Terrain& u) {
                                                             if (guid.IsNull())
                                                                 u.TerrainAssetGuid.Clear();
                                                             else
                                                                 u.TerrainAssetGuid.Set(guid);
                                                         });
            if (requestRefresh)
                requestRefresh();
        },
        ctx.Thumbnails,
        "The heightmap the terrain's base is read from. A raw .r16 or .r32 file needs its Samples X (or "
        "Samples Z) set in its own inspector unless it is square.");
    if (field)
        field->SetId("terrain-heightmap");
}

// One option for each registered name, the value and the label both the name. A stored name that no
// module registers stays as the last option, marked, so the panel never replaces it silently.
std::vector<Dropdown::Option> HeightFunctionOptions(std::string_view stored, bool storedRegistered)
{
    std::vector<Dropdown::Option> options;
    for (std::string& name : TerrainECS::TerrainService::GetHeightFunctionNames())
        options.push_back({name, name});
    if (!stored.empty() && !storedRegistered)
        options.push_back({std::string(stored), std::string(stored) + " (not registered)"});
    return options;
}

// Stores the picked function name with an undo step, then builds the section again: the notice shows
// only for a name that no module registers.
void CommitHeightFunctionName(const InspectorContext& ctx, const std::string& name)
{
    if (name.empty() || name.size() >= Components::kTerrainHeightFunctionNameCapacity)
        return;
    CommitComponentWithUndo<Components::Terrain>(ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
                                                 "Change Terrain Height Function",
                                                 [&name](Components::Terrain& terrain) {
                                                     std::ranges::fill(terrain.HeightFunction, '\0');
                                                     name.copy(terrain.HeightFunction, name.size());
                                                 });
    if (ctx.RequestInspectorRefresh)
        ctx.RequestInspectorRefresh();
}

void AddHeightFunctionRow(UIElement* content, const InspectorContext& ctx, std::string_view stored,
                          bool storedRegistered)
{
    const std::vector<Dropdown::Option> options = HeightFunctionOptions(stored, storedRegistered);
    const auto picked = std::ranges::find(options, stored, &Dropdown::Option::value);
    const int selected = stored.empty() || picked == options.end() ? -1 : static_cast<int>(picked - options.begin());
    Dropdown* dropdown = InspectorUI::AddDropdownRow(
        content, "Height Function", options, selected,
        "The height function the terrain's base is read from. A game module registers it by name.");
    dropdown->SetId("terrain-height-function");
    // SetOptions selects the first option for an index out of range; the silent call clears it.
    if (selected < 0)
    {
        dropdown->SetSelectedIndexWithoutNotify(-1);
        dropdown->SetNoSelectionText("None");
    }
    dropdown->SetOnValueChanged([ctx](const std::string& name) { CommitHeightFunctionName(ctx, name); });
}

void CommitSeed(const InspectorContext& ctx, uint64 seed)
{
    CommitComponentWithUndo<Components::Terrain>(ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
                                                 "Change Terrain Seed",
                                                 [seed](Components::Terrain& terrain) { terrain.BaseSeed = seed; });
}

// The Seed row: the seed's field and the dice button. A typed seed and a dice click are each one undo step.
void AddSeedRow(UIElement* content, const InspectorContext& ctx, uint64 seed)
{
    constexpr const char* kTooltip =
        "The seed the function generates its world from, 0 to 2,147,483,647. Each seed gives a different world.";
    UIElement* row = InspectorUI::AddRow(content);
    InspectorUI::AddLabel(row, "Seed", kTooltip);
    auto* field = InspectorUI::AddControl(InspectorUI::AddFieldContainer(row), std::make_unique<EditorUI::SeedField>());
    field->SetId("terrain-base-seed");
    field->SetTooltip(kTooltip);
    field->SetSeed(seed);
    field->SetOnSeedCommitted([ctx](uint64 picked) { CommitSeed(ctx, picked); });
}

// "'Islands' for seed 42..." while the terrain's height function generates the world for its name and
// seed; empty when the world is ready, when no module registers the name, or for another base source.
std::string GeneratingText(TerrainECS::TerrainService& service, const Components::Terrain& terrain)
{
    if (terrain.BaseSource != Components::TerrainBaseSource::Function)
        return {};
    const std::string_view name = FixedStringView(terrain.HeightFunction);
    if (!TerrainECS::TerrainService::IsHeightFunctionRegistered(name) || service.IsTiledTerrainBaseReady(terrain))
        return {};
    return "'" + std::string(name) + "' for seed " + std::to_string(terrain.BaseSeed) + "...";
}

// The Base header's Generating badge and the text its tooltip shows, read again each frame. The section
// is built again once when a module load or unload changes whether the stored name is registered.
struct GeneratingFollower
{
    Foldout* Section = nullptr;
    TerrainECS::TerrainService* Service = nullptr;
    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    std::function<void()> RequestRefresh;
    std::string Shown;
    bool Registered = false;
    bool RefreshRequested = false;
};

// Asks for one rebuild when the stored name's registration differs from the one the section shows.
void FollowRegistration(GeneratingFollower& follower, const Components::Terrain& terrain)
{
    if (follower.RefreshRequested || !follower.RequestRefresh)
        return;
    const std::string_view name = FixedStringView(terrain.HeightFunction);
    if (TerrainECS::TerrainService::IsHeightFunctionRegistered(name) == follower.Registered)
        return;
    follower.RefreshRequested = true;
    follower.RequestRefresh();
}

// Shows the badge while the function generates the world, and hides it when the world is ready.
void ShowGenerating(Foldout& section, const std::string& text)
{
    section.SetHeaderBadge(text.empty() ? std::string() : "Generating", text);
}

// Updates the badge only when the text differs from the one shown.
void FollowGenerating(GeneratingFollower& follower)
{
    const auto* terrain = follower.World->GetComponent<Components::Terrain>(follower.Entity);
    if (terrain)
        FollowRegistration(follower, *terrain);
    const std::string text = terrain ? GeneratingText(*follower.Service, *terrain) : std::string();
    if (text == follower.Shown)
        return;
    follower.Shown = text;
    ShowGenerating(*follower.Section, text);
}

// The Base header's badge says the function generates the world for a new name or seed. The old
// shape stays drawn until then, and no row moves.
void FollowGeneratingInHeader(Foldout& section, const InspectorContext& ctx, const Components::Terrain& terrain,
                              bool registered)
{
    auto* service = TerrainECS::TerrainService::TryGet();
    if (!service)
        return;
    GeneratingFollower follower{&section, service, ctx.World, ctx.Entity, ctx.RequestInspectorRefresh,
                                GeneratingText(*service, terrain), registered};
    ShowGenerating(section, follower.Shown);
    if (ctx.FrameRefreshCallbacks)
        ctx.FrameRefreshCallbacks->push_back([follower]() mutable { FollowGenerating(follower); });
}

// The rows of a Function base (the function and the seed), the notice while the function is missing,
// and the header badge while it generates the world.
void AddFunctionRows(Foldout& section, const InspectorContext& ctx, const Components::Terrain& terrain)
{
    UIElement* content = section.GetContentContainer();
    const std::string_view stored = FixedStringView(terrain.HeightFunction);
    const bool registered = !stored.empty() && TerrainECS::TerrainService::IsHeightFunctionRegistered(stored);
    AddHeightFunctionRow(content, ctx, stored, registered);
    AddSeedRow(content, ctx, terrain.BaseSeed);
    if (stored.empty())
        InspectorUI::AddNotice(content,
                               "No function is picked, so the base is flat. Pick one in Height Function above.",
                               EditorUI::InspectorNotice::Kind::Warning);
    else if (!registered)
        InspectorUI::AddNotice(content,
                               "No loaded module registers the height function '" + std::string(stored) +
                                   "'. The terrain is flat until one does. Save the game module's source in "
                                   "the project's Assets folder so the editor builds it, or pick another "
                                   "function.",
                               EditorUI::InspectorNotice::Kind::Warning);
    FollowGeneratingInHeader(section, ctx, terrain, registered);
}

// Ratios that differ by less than this read as the same shape: a DEM's sample grid is the
// terrain's footprint to within a sample's rounding, never to within 1%.
constexpr double kShapeTolerance = 0.01;

// "4:1" or "1:4": the longer side first as a whole number where it is one.
std::string FormatRatio(double ratio)
{
    char text[32];
    if (ratio >= 1.0)
        std::snprintf(text, sizeof(text), "%.3g:1", ratio);
    else
        std::snprintf(text, sizeof(text), "1:%.3g", 1.0 / ratio);
    return text;
}

void AddSelectHeightmapAction(EditorUI::InspectorNotice& notice, const InspectorContext& ctx, const GUID& heightmap,
                              const char* tooltip)
{
    AssetMetadata meta{};
    if (ctx.PingAsset &&
        EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(heightmap, meta) &&
        !meta.Path.empty())
    {
        notice.SetAction("Select Heightmap", tooltip, [ping = ctx.PingAsset, path = meta.Path] { ping(path); });
    }
}

// A heightmap whose sample grid does not have the terrain's shape is stretched to fit it. When the
// transposed grid would fit, Samples X and Samples Z were most likely typed the wrong way round.
void AddShapeNotice(UIElement* content, const InspectorContext& ctx, const GUID& heightmap,
                    const Terrain::HeightfieldData& decoded, const Components::Terrain& terrain)
{
    if (decoded.GetWidth() < 2 || decoded.GetHeight() < 2 || terrain.SizeX <= 0.0f || terrain.SizeZ <= 0.0f)
        return;
    const double grid = static_cast<double>(decoded.GetWidth() - 1) / static_cast<double>(decoded.GetHeight() - 1);
    const double footprint = static_cast<double>(terrain.SizeX) / static_cast<double>(terrain.SizeZ);
    if (std::abs(grid / footprint - 1.0) <= kShapeTolerance)
        return;

    std::string text = "The heightmap is " + FormatGroupedInteger(decoded.GetWidth()) + " x " +
                       FormatGroupedInteger(decoded.GetHeight()) + " samples (" + FormatRatio(grid) +
                       ") but the terrain is " + FormatGroupedInteger(static_cast<uint64>(terrain.SizeX)) + " x " +
                       FormatGroupedInteger(static_cast<uint64>(terrain.SizeZ)) + " m (" + FormatRatio(footprint) +
                       "), so its relief is stretched to the terrain's shape.";
    if (std::abs((1.0 / grid) / footprint - 1.0) <= kShapeTolerance)
        text += " Samples X and Samples Z look swapped: the other way round, the heightmap fits.";
    else
        text += " Set Size X and Size Z to the area the heightmap covers.";
    EditorUI::InspectorNotice* notice = InspectorUI::AddNotice(content, text, EditorUI::InspectorNotice::Kind::Warning);
    AddSelectHeightmapAction(*notice, ctx, heightmap, "Selects the heightmap so its Samples X and Samples Z can be checked.");
}

// Samples Per Meter above the heightmap's own density adds samples the heightmap has no data for, at
// the memory price of real ones. Match Heightmap sets the terrain to the heightmap's density along X.
void AddDensityNotice(UIElement* content, const InspectorContext& ctx, const Terrain::HeightfieldData& decoded,
                      const Components::Terrain& terrain)
{
    if (decoded.GetWidth() < 2 || terrain.SizeX <= 0.0f)
        return;
    const float32 heightmapDensity = static_cast<float32>(decoded.GetWidth() - 1) / terrain.SizeX;
    if (terrain.SamplesPerMeter <= heightmapDensity * (1.0f + static_cast<float32>(kShapeTolerance)))
        return;

    const uint64 asSet =
        TerrainECS::DeriveTerrainSizingPlan(terrain.SizeX, terrain.SizeZ, terrain.SamplesPerMeter).PredictedVramBytes;
    const uint64 matched =
        TerrainECS::DeriveTerrainSizingPlan(terrain.SizeX, terrain.SizeZ, heightmapDensity).PredictedVramBytes;
    char numbers[96];
    std::snprintf(numbers, sizeof(numbers), "%.1fx finer than the heightmap (%.3g per meter along X)",
                  terrain.SamplesPerMeter / heightmapDensity, heightmapDensity);
    auto notice = std::make_unique<EditorUI::InspectorNotice>(
        std::string("Samples Per Meter is ") + numbers + ": " + FormatMebibytes(asSet) +
            " of terrain maps where the heightmap's own density needs " + FormatMebibytes(matched) + ".",
        EditorUI::InspectorNotice::Kind::Information);
    notice->SetAction("Match Heightmap", "Sets Samples Per Meter to the heightmap's density.",
                      [w = ctx.World, e = ctx.Entity, n = ctx.ChangeNotifications, undo = ctx.Undo,
                       requestRefresh = ctx.RequestInspectorRefresh, heightmapDensity]
                      {
                          CommitComponentWithUndo<Components::Terrain>(
                              w, e, n, undo, "Match Terrain To Heightmap",
                              [heightmapDensity](Components::Terrain& u) { u.SamplesPerMeter = heightmapDensity; });
                          if (requestRefresh)
                              requestRefresh();
                      });
    content->AddChild(std::move(notice));
}

// What the bound heightmap gives the terrain: nothing (no asset picked, or the decode's own reason,
// with the way to the fix), or a grid whose shape or density does not suit the terrain.
void AddHeightmapNotices(UIElement* content, const InspectorContext& ctx, const GUID& heightmap,
                         const Components::Terrain& terrain)
{
    if (heightmap.IsNull())
    {
        content->AddChild(std::make_unique<EditorUI::InspectorNotice>("No heightmap is picked, so the base is flat."));
        return;
    }
    // Resolving here decodes the heightmap now rather than on the terrain's next frame, so the panel
    // can say at once why a heightmap does not decode. The decode is cached either way.
    auto* service = TerrainECS::TerrainService::TryGet();
    if (!service)
        return;
    if (const std::shared_ptr<const Terrain::HeightfieldData> decoded = service->ResolveHeightmapAsset(heightmap))
    {
        AddShapeNotice(content, ctx, heightmap, *decoded, terrain);
        AddDensityNotice(content, ctx, *decoded, terrain);
        return;
    }
    auto notice = std::make_unique<EditorUI::InspectorNotice>("The heightmap does not decode: " +
                                                              service->GetHeightmapDecodeError(heightmap) +
                                                              ". The base is flat until it does.");
    AddSelectHeightmapAction(*notice, ctx, heightmap, "Selects the heightmap so its Samples X and Samples Z can be set.");
    content->AddChild(std::move(notice));
}

// What the relief line's actions use, copied from the section's InspectorContext.
struct ReliefLineContext
{
    ECS::World* World = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    Editor::EditorChangeNotifications* Notifications = nullptr;
    std::function<void(ECS::EntityHandle)> SelectEntity;
};

// Makes the "Terrain Noise" volume as one undo step and selects it.
void AddRelief(const ReliefLineContext& relief)
{
    const ECS::EntityHandle volume = Editor::CreateGeneratedNoiseVolume(*relief.World, relief.Undo, relief.Notifications);
    Editor::NotifyWorldStructure(relief.Notifications, relief.World);
    if (volume.IsValid() && relief.SelectEntity)
        relief.SelectEntity(volume);
}

// A Global volume with a Generated noise that FindGeneratedNoiseVolume skips, and why it supplies no relief.
struct SkippedReliefVolume
{
    ECS::EntityHandle Entity;
    const char* Reason = "";
};

// The part of a skipped volume that is off, from the entity inward. With all parts on, the Weight is 0.
const char* SkippedReliefReason(ECS::World& world, ECS::EntityHandle entity, const Components::TerrainNoiseEffect& noise)
{
    if (!ECS::Entity(&world, entity).IsEnabledInHierarchy())
        return "its entity is disabled";
    if (!world.IsComponentEnabled(entity, ECS::GetComponentTypeId<Components::TerrainModifierVolume>()))
        return "its volume component is disabled";
    if (!noise.Enabled || !world.IsComponentEnabled(entity, ECS::GetComponentTypeId<Components::TerrainNoiseEffect>()))
        return "its Noise effect is disabled";
    return "its Weight is 0";
}

// The skipped Global Generated volume with the lowest entity index. Invalid Entity when there is none.
SkippedReliefVolume FindSkippedReliefVolume(ECS::World& world)
{
    SkippedReliefVolume skipped;
    world.Query<ECS::Read<Components::TerrainModifierVolume>, ECS::Read<Components::TerrainNoiseEffect>>()
        .IncludeDisabled()
        .Each([&world, &skipped](ECS::EntityHandle entity, const Components::TerrainModifierVolume& volume,
                                  const Components::TerrainNoiseEffect& noise) {
            if (volume.Shape != Components::TerrainVolumeShape::Global ||
                noise.Mode != Components::TerrainNoiseMode::Generated)
                return;
            if (skipped.Entity.IsValid() && skipped.Entity.index < entity.index)
                return;
            skipped = {entity, SkippedReliefReason(world, entity, noise)};
        });
    return skipped;
}

// Adds the relief line's Select action for `volume`.
void AddSelectVolumeAction(EditorUI::InspectorNotice& notice, const ReliefLineContext& relief, ECS::EntityHandle volume,
                           const std::string& tooltip)
{
    if (relief.SelectEntity)
        notice.SetAction("Select", tooltip, [select = relief.SelectEntity, volume] { select(volume); });
}

// A Flat planar terrain takes the scene's Generated noise as its relief. The line names the volume that
// supplies it and its Weight when it is not 1, or the skipped volume and the reason, or offers to add one.
// It is an information notice: it states a fact and carries an action.
void AddReliefSourceLine(UIElement& host, const ReliefLineContext& relief)
{
    ECS::World& world = *relief.World;
    const ECS::EntityHandle source = TerrainECS::FindGeneratedNoiseVolume(world);
    if (source.IsValid())
    {
        const auto* noise = world.GetComponent<Components::TerrainNoiseEffect>(source);
        const float32 weight = world.GetComponent<Components::TerrainModifierVolume>(source)->Weight;
        const std::string name = "\"" + Editor::EntityDisplayName(world, source) + "\"";
        const std::string atWeight = weight == 1.0f ? "" : " at Weight " + FormatFloat(weight, kReliefWeightDigits);
        EditorUI::InspectorNotice* notice = InspectorUI::AddControl(&host, std::make_unique<EditorUI::InspectorNotice>(
            "The relief is the " + TerrainNoisePresetLabel(noise->Preset) + " noise of " + name + atWeight + ".",
            EditorUI::InspectorNotice::Kind::Information));
        AddSelectVolumeAction(*notice, relief, source, "Select " + name + " to change its preset.");
        return;
    }
    const SkippedReliefVolume skipped = FindSkippedReliefVolume(world);
    if (skipped.Entity.IsValid())
    {
        const std::string name = "\"" + Editor::EntityDisplayName(world, skipped.Entity) + "\"";
        EditorUI::InspectorNotice* notice = InspectorUI::AddControl(&host, std::make_unique<EditorUI::InspectorNotice>(
            name + " supplies no relief: " + skipped.Reason + ".", EditorUI::InspectorNotice::Kind::Information));
        AddSelectVolumeAction(*notice, relief, skipped.Entity, "Select " + name + " to turn its relief on.");
        return;
    }
    EditorUI::InspectorNotice* notice = InspectorUI::AddControl(&host, std::make_unique<EditorUI::InspectorNotice>(
        "No volume supplies relief. Add a Noise effect volume in Generated mode.",
        EditorUI::InspectorNotice::Kind::Information));
    notice->SetAction("Add Relief", "Add a \"Terrain Noise\" volume with the Rolling preset and select it.",
                      [relief] { AddRelief(relief); });
}

} // namespace

bool IsFlatFunctionBase(const Components::Terrain& terrain)
{
    return terrain.BaseSource == Components::TerrainBaseSource::Function &&
           !TerrainECS::TerrainService::IsHeightFunctionRegistered(FixedStringView(terrain.HeightFunction));
}

void AddTerrainBaseSourceSection(const InspectorContext& ctx)
{
    const auto* terrain = ctx.World ? ctx.World->GetComponent<Components::Terrain>(ctx.Entity) : nullptr;
    if (!terrain)
        return;
    Foldout* section = InspectorUI::AddComponentSection(ctx.Parent, "Terrain/Base", "Base");
    UIElement* content = section ? section->GetContentContainer() : nullptr;
    if (!content)
        return;

    AddBaseSourceRow(content, ctx, terrain->BaseSource);
    if (terrain->Domain == Components::TerrainDomain::Planar &&
        terrain->BaseSource == Components::TerrainBaseSource::Flat)
        InspectorUI::AddControl(content, std::make_unique<Editor::GeneratedNoiseNoticeHost>(
            ctx.World, ctx.ChangeNotifications,
            [relief = ReliefLineContext{ctx.World, ctx.Undo, ctx.ChangeNotifications, ctx.SelectEntity}](
                UIElement& host) { AddReliefSourceLine(host, relief); }));
    if (terrain->BaseSource == Components::TerrainBaseSource::Function)
        AddFunctionRows(*section, ctx, *terrain);
    if (terrain->BaseSource != Components::TerrainBaseSource::HeightmapAsset)
        return;
    const GUID heightmap = terrain->TerrainAssetGuid.ToGuid();
    AddHeightmapRow(content, ctx, heightmap);
    AddHeightmapNotices(content, ctx, heightmap, *terrain);
}

} // namespace GameEngine
