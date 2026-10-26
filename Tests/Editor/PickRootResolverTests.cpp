#include "Picking/PickRootResolver.h"

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
        traits.IsPickInstanceRoot = true;
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
}

TEST_F(PickRootResolverTest, GeneratedMeshesSelectTheirAuthoringContainerWithoutAModelAsset)
{
    EXPECT_EQ(ResolvePickRoot(World, CreatePiece(true)), Owner);
    EXPECT_EQ(ResolvePickRoot(World, Owner), Owner);
}

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
    EXPECT_EQ(ResolvePickRoot(World, authored), authored);
    const auto generated = CreatePiece(true);
    World.RemoveComponentImmediate<GeneratedShapeRoot>(Owner);
    EXPECT_EQ(ResolvePickRoot(World, generated), generated);
    EXPECT_EQ(ResolvePickRoot(World, ECS::EntityHandle{}), ECS::EntityHandle{});
}
}
