#pragma once

#include "ECS/ECS.h"
#include "SplineRecipes/SplineRecipesScriptApi.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ECS { class World; }

namespace GameEngine::SplineRecipes
{

// The reasons the recipe controllers give for a Held or Failed recipe, and for a Built recipe
// that made nothing.
inline constexpr std::string_view kHeldForTerrainReason =
    "A terrain near it is still loading. It builds when the terrain loads.";
inline constexpr std::string_view kHeldForPickingDataReason =
    "A mesh under it is still being prepared. It builds when the mesh is ready.";
inline constexpr std::string_view kNoRenderDeviceReason =
    "This run has no render device, so the recipe cannot build its mesh.";
inline constexpr std::string_view kSplineTooShortReason =
    "The spline has fewer than 2 points. Add points to build it.";
inline constexpr std::string_view kNoFencePieceReason =
    "The recipe has no piece to place. Add a mesh to Post 1 or Span 1.";
inline constexpr std::string_view kNoTileReason =
    "The recipe has no tile to place. Add a mesh to Straight 1.";
inline constexpr std::string_view kBuiltNothingReason =
    "It made no surface: the spline and the recipe's shape give nothing to build.";
inline constexpr std::string_view kNoTerrainToFillReason =
    "It made no water: no terrain is under the run to fill against. A planet terrain cannot hold it.";
inline constexpr std::string_view kNoWaterToFillReason =
    "It made no water: the run found no water its banks can hold. Carve the bed deeper or give it banks.";
inline constexpr std::string_view kFillWindowTooLargeReason =
    "It made no water: the run is too long for one fill. Split it into shorter splines.";
inline constexpr std::string_view kFillOverBudgetReason =
    "It made no water: the water spread past the fill's limit. Reduce Max Half Width or raise the bed.";

// The build state of every spline recipe in one world, as the recipe controllers set it on
// their last update. A recipe the controllers did not visit on that update reads Pending.
class RecipeBuildStates
{
public:
    // Starts an update of `world`. Each controller then calls Set once for each recipe it visits.
    void BeginUpdate(const ECS::World& world);
    void Set(ECS::EntityHandle recipe, BuildState state, std::string_view reason = {});
    // Sets every recipe of `world` Failed with `reason`, for an update in which no controller can build.
    void FailEveryRecipe(ECS::World& world, std::string_view reason);
    // Drops the recipes no controller visited, collects the failures and records whether the
    // scene is settled.
    void EndUpdate(ECS::World& world);

    [[nodiscard]] RecipeStatus StateOf(ECS::World& world, ECS::EntityHandle recipe) const;
    [[nodiscard]] std::string_view ReasonOf(ECS::World& world, ECS::EntityHandle recipe) const;
    // Reads the recipe components of `world`, so a recipe that no update has seen keeps the
    // scene unsettled.
    [[nodiscard]] bool IsSceneSettled(ECS::World& world) const;
    [[nodiscard]] std::span<const RecipeStatus> Failures(ECS::World& world) const;
    // IsSceneSettled as EndUpdate found it; false when no update of `world` ended since its reset.
    [[nodiscard]] bool IsSceneSettledAtLastUpdate(const ECS::World& world) const;

private:
    struct Entry
    {
        BuildState State = BuildState::Pending;
        std::string Reason;
        uint64 VisitStamp = 0;
    };

    const Entry* Find(const ECS::World& world, ECS::EntityHandle recipe) const;

    // Keyed by the packed entity id, so a recycled index with a new version is a new key.
    FlatMap<uint32, Entry> m_Entries;
    std::vector<RecipeStatus> m_Failures;
    const ECS::World* m_World = nullptr;
    uint64 m_WorldResetGeneration = 0;
    uint64 m_VisitStamp = 0;
    bool m_SettledAtLastUpdate = false;
};

} // namespace GameEngine::SplineRecipes
