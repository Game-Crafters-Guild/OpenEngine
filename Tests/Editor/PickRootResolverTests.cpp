#include "Picking/PickRootResolver.h"

#include "AssetCore/GUID.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Scene/SceneRaycast.h"

#include <gtest/gtest.h>

namespace GameEngine::Editor::Picking
{
namespace
{
struct GeneratedShapeRoot
{
    bool Active = true;
};

class PickRootResolverTest : public testing::Test
{
protected:
    void SetUp() override
    {
        EditorComponentTraits traits;
        traits.IsPickClimbRoot = true;
        EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<GeneratedShapeRoot>(), traits);
        Owner = World.CreateEntity();
        World.AddComponentImmediate(Owner, GeneratedShapeRoot{});
    }

    void TearDown() override
    {
        EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<GeneratedShapeRoot>(), {});
    }

    ECS::EntityHandle CreatePiece(bool generated)
    {
        const auto entity = World.CreateEntity();
        World.AddComponentImmediate(entity, Components::Parent{Owner});
        World.AddComponentImmediate(entity, Components::MeshRenderer{});
        if (generated)
            World.AddComponentImmediate(entity, Components::RuntimeOnlyEntity{});
        return entity;
    }

    ECS::World World;
    ECS::EntityHandle Owner;
};
} // namespace

TEST_F(PickRootResolverTest, RepeatClicksNeverDrillIntoGeneratedFillOrEdges)
{
    const auto fill = CreatePiece(true);
    const auto edge = CreatePiece(true);
    std::vector<Scene::PickHit> hits(2);
    hits[0].Entity = edge;
    hits[1].Entity = fill;
    std::vector<ECS::EntityHandle> candidates;
    BuildPickCandidates(World, hits, false, candidates);
    EXPECT_EQ(candidates, (std::vector<ECS::EntityHandle>{Owner}));
    BuildPickCandidates(World, hits, true, candidates);
    EXPECT_EQ(candidates, (std::vector<ECS::EntityHandle>{edge, fill}));
}

TEST_F(PickRootResolverTest, AuthoredChildrenAndUnownedRuntimeMeshesRemainIndependent)
{
    const auto authored = CreatePiece(false);
    std::vector<Scene::PickHit> hits(1);
    hits[0].Entity = authored;
    std::vector<ECS::EntityHandle> candidates;
    BuildPickCandidates(World, hits, false, candidates);
    EXPECT_EQ(candidates, (std::vector<ECS::EntityHandle>{Owner, authored}));
    const auto generated = CreatePiece(true);
    World.RemoveComponentImmediate<GeneratedShapeRoot>(Owner);
    EXPECT_EQ(ResolvePickRoot(World, generated), generated);
    EXPECT_EQ(ResolvePickRoot(World, ECS::EntityHandle{}), ECS::EntityHandle{});
}

TEST_F(PickRootResolverTest, NestedGeneratedPiecesCollapseToTheOutermostAuthoringRoot)
{
    const auto inner = CreatePiece(true);
    World.AddComponentImmediate(inner, GeneratedShapeRoot{});
    const auto piece = CreatePiece(true);
    World.GetComponentForWrite<Components::Parent>(piece)->Entity = inner;
    std::vector<Scene::PickHit> hits(2);
    hits[0].Entity = piece;
    hits[1].Entity = inner;
    std::vector<ECS::EntityHandle> candidates;
    BuildPickCandidates(World, hits, false, candidates);
    EXPECT_EQ(candidates, (std::vector<ECS::EntityHandle>{Owner}));
}

TEST_F(PickRootResolverTest, RuntimeModelPiecesOutsideAClimbRootKeepTheirOwnCandidate)
{
    // The model-instance climb lifts the piece to its group; the click-through list still holds the piece.
    const auto group = World.CreateEntity();
    const auto piece = World.CreateEntity();
    World.AddComponentImmediate(piece, Components::Parent{group});
    Components::MeshRenderer renderer;
    renderer.modelAssetGuid = Components::ModelRef(GUID::Generate());
    World.AddComponentImmediate(piece, renderer);
    World.AddComponentImmediate(piece, Components::RuntimeOnlyEntity{});
    std::vector<Scene::PickHit> hits(1);
    hits[0].Entity = piece;
    std::vector<ECS::EntityHandle> candidates;
    BuildPickCandidates(World, hits, false, candidates);
    EXPECT_EQ(candidates, (std::vector<ECS::EntityHandle>{group, piece}));
}

} // namespace GameEngine::Editor::Picking
