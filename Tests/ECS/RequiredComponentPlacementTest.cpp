// RequiredComponentPlacementTest.cpp - an entity lands in the archetype that holds
// every component its components require, in one placement, on every structural
// add and creation path; each column it did not supply starts at its type's
// default value, and a required component it already holds keeps its value.
#include <gtest/gtest.h>
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
// Defaults differ from zero so a column left unwritten cannot pass for one
// written with its default.
struct PlacementLeaf
{
    float Value = 7.5f;
};
struct PlacementMiddle
{
    int Value = 42;
};
struct NeedsChain
{
    int Value = 3;
};
struct Unrelated
{
    int Value = 1;
};
} // namespace

namespace GameEngine::ECS::Detail
{
template <>
struct RequiredComponents<::PlacementMiddle>
{
    using type = TypeList<::PlacementLeaf>;
};
template <>
struct RequiredComponents<::NeedsChain>
{
    using type = TypeList<::PlacementMiddle>;
};
} // namespace GameEngine::ECS::Detail

namespace
{
void ExpectChainDefaults(World& world, EntityHandle entity)
{
    const auto* middle = world.GetComponent<PlacementMiddle>(entity);
    const auto* leaf = world.GetComponent<PlacementLeaf>(entity);
    ASSERT_NE(middle, nullptr);
    ASSERT_NE(leaf, nullptr);
    EXPECT_EQ(middle->Value, 42);
    EXPECT_EQ(leaf->Value, 7.5f);
}
} // namespace

TEST(RequiredComponentPlacement, AddOfARequiringTypeIsOnePlacement)
{
    World world;
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, Unrelated{});
    const std::size_t archetypesBefore = world.GetArchetypeCount();

    world.AddComponentImmediate(entity, NeedsChain{9});

    EXPECT_EQ(world.GetArchetypeCount(), archetypesBefore + 1)
        << "the add passed through an archetype per requirement instead of landing in the closure";
    ASSERT_NE(world.GetComponent<NeedsChain>(entity), nullptr);
    EXPECT_EQ(world.GetComponent<NeedsChain>(entity)->Value, 9);
    ExpectChainDefaults(world, entity);
}

TEST(RequiredComponentPlacement, TypeErasedAddResolvesRequirements)
{
    World world;
    world.AddComponentImmediate(world.CreateEntity(), NeedsChain{}); // registers the types
    const EntityHandle entity = world.CreateEntity();

    const NeedsChain value{11};
    ASSERT_TRUE(world.SetComponentBytesImmediate(entity, GetComponentTypeId<NeedsChain>(), &value,
                                                 sizeof(value)));

    EXPECT_EQ(world.GetComponent<NeedsChain>(entity)->Value, 11);
    ExpectChainDefaults(world, entity);
}

TEST(RequiredComponentPlacement, CreationPathsPlaceRequiredColumns)
{
    World world;
    const EntityHandle typed = world.CreateHandle(NeedsChain{5});
    ExpectChainDefaults(world, typed);

    const std::vector<EntityHandle> batch = world.CreateBatchHandle(64, NeedsChain{6});
    for (const EntityHandle entity : batch)
        ExpectChainDefaults(world, entity);

    const std::vector<EntityHandle> initialized = world.CreateBatchWithInit<NeedsChain>(
        8, [](std::size_t index, NeedsChain& chain) { chain.Value = static_cast<int>(index); });
    for (const EntityHandle entity : initialized)
        ExpectChainDefaults(world, entity);

    ComponentBundle bundle;
    bundle.Add(NeedsChain{8});
    ExpectChainDefaults(world, world.CreateFromBundleHandle(bundle));
    for (const EntityHandle entity : world.CreateBatchFromBundle(4, bundle))
        ExpectChainDefaults(world, entity);
}

TEST(RequiredComponentPlacement, RequiredValueTheEntityHoldsIsKept)
{
    World world;
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, PlacementLeaf{-1.0f});

    world.AddComponentImmediate(entity, NeedsChain{});

    EXPECT_EQ(world.GetComponent<PlacementLeaf>(entity)->Value, -1.0f);
    EXPECT_EQ(world.GetComponent<PlacementMiddle>(entity)->Value, 42);
}

TEST(RequiredComponentPlacement, AddedEventFiresOnceForEachRequiredType)
{
    World world;
    world.EnableLifecycleEvents<PlacementLeaf>();
    world.EnableLifecycleEvents<PlacementMiddle>();
    const EntityHandle entity = world.CreateEntity();

    world.AddComponentImmediate(entity, NeedsChain{});
    world.SwapLifecycleEvents();

    ASSERT_EQ(world.GetAdded<PlacementLeaf>().size(), 1u);
    ASSERT_EQ(world.GetAdded<PlacementMiddle>().size(), 1u);
    EXPECT_EQ(world.GetAdded<PlacementLeaf>()[0], entity);
}

// A requirement holds while its requirer does: removing it is refused,
// immediate or deferred; removing the requirer first frees it.
TEST(RequiredComponentPlacement, RemovingARequirementWhileItsRequirerStaysIsRefused)
{
    World world;
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, NeedsChain{});

    world.RemoveComponentImmediate<PlacementMiddle>(entity);
    EXPECT_NE(world.GetComponent<PlacementMiddle>(entity), nullptr);
    world.RemoveComponent<PlacementLeaf>(entity);
    world.ProcessCommands();
    EXPECT_NE(world.GetComponent<PlacementLeaf>(entity), nullptr) << "PlacementMiddle requires it";
    EXPECT_FALSE(world.RemoveComponentByTypeIdImmediate(entity, GetComponentTypeId<PlacementMiddle>()));

    world.RemoveComponentImmediate<NeedsChain>(entity);
    world.RemoveComponentImmediate<PlacementMiddle>(entity);
    world.RemoveComponentImmediate<PlacementLeaf>(entity);
    EXPECT_EQ(world.GetComponent<PlacementMiddle>(entity), nullptr);
    EXPECT_EQ(world.GetComponent<PlacementLeaf>(entity), nullptr);
}

// P3 parity: the closure placement leaves the same bytes in every column as
// adding each required component with its default, then the requiring one.
TEST(RequiredComponentPlacement, ClosurePlacementEqualsSequentialAdds)
{
    World world;
    const EntityHandle closure = world.CreateEntity();
    world.AddComponentImmediate(closure, NeedsChain{17});

    const EntityHandle sequential = world.CreateEntity();
    world.AddComponentImmediate(sequential, PlacementLeaf{});
    world.AddComponentImmediate(sequential, PlacementMiddle{});
    world.AddComponentImmediate(sequential, NeedsChain{17});

    EXPECT_EQ(std::memcmp(world.GetComponent<PlacementLeaf>(closure),
                          world.GetComponent<PlacementLeaf>(sequential), sizeof(PlacementLeaf)), 0);
    EXPECT_EQ(std::memcmp(world.GetComponent<PlacementMiddle>(closure),
                          world.GetComponent<PlacementMiddle>(sequential), sizeof(PlacementMiddle)), 0);
    EXPECT_EQ(std::memcmp(world.GetComponent<NeedsChain>(closure),
                          world.GetComponent<NeedsChain>(sequential), sizeof(NeedsChain)), 0);
}
