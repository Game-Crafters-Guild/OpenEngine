// World::CreateBatchFromWorld: the cross-world batch clone the prefab library spawns through.

#include <gtest/gtest.h>

#include "ECS/ClonePlan.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/EntityRemap.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "TestComponents.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace
{
// The world's parent relation; not reflected, so the clone finds it through the relation.
struct CloneParentLink
{
    uint32 Depth = 0;
    EntityHandle Parent{};
};

// Reflected entity fields: one handle and an array of two.
struct CloneTargets
{
    float Weight = 0.0f;
    EntityHandle Target{};
    EntityHandle Others[2]{};
};

constexpr std::size_t kTemplateSize = 20;

void RegisterCloneComponents(World& world)
{
    AutoComponentRegistrar<CloneParentLink>::EnsureRegistered();
    AutoComponentRegistrar<CloneTargets>::EnsureRegistered();
    world.SetParentRelation(GetComponentTypeId<CloneParentLink>(),
                            static_cast<uint32>(offsetof(CloneParentLink, Parent)));
    static const std::array<FieldInfo, 3> kFields = {
        FieldInfo{"Weight", static_cast<std::uint32_t>(offsetof(CloneTargets, Weight)), sizeof(float),
                  FieldTypeId::Float},
        FieldInfo{"Target", static_cast<std::uint32_t>(offsetof(CloneTargets, Target)), sizeof(EntityHandle),
                  FieldTypeId::EntityHandle},
        FieldInfo{"Others", static_cast<std::uint32_t>(offsetof(CloneTargets, Others)), 2 * sizeof(EntityHandle),
                  FieldTypeId::EntityHandle},
    };
    ComponentFieldRegistry::Register(GetComponentTypeId<CloneTargets>(), kFields, "CloneTargets");
}

// A 20-entity tree in three archetypes: a root, then children under the root or the
// previous child. Every fifth child names the root, its previous sibling and `outside`,
// an entity outside the tree.
std::vector<EntityHandle> BuildTemplate(World& world, EntityHandle outside)
{
    std::vector<EntityHandle> entities;
    const EntityHandle root = world.CreateEntity();
    world.AddComponentImmediate(root, Position{1.0f, 2.0f, 3.0f});
    entities.push_back(root);
    for (std::size_t i = 1; i < kTemplateSize; ++i)
    {
        const EntityHandle entity = world.CreateEntity();
        world.AddComponentImmediate(entity, Position{static_cast<float32>(i), 0.5f, -static_cast<float32>(i)});
        const EntityHandle parent = (i % 3 == 0) ? entities[i - 1] : root;
        world.AddComponentImmediate(entity, CloneParentLink{static_cast<uint32>(i), parent});
        if (i % 2 == 0)
            world.AddComponentImmediate(entity, Velocity{static_cast<float32>(i) * 2.0f, 0.0f, 1.0f});
        if (i % 5 == 0)
        {
            CloneTargets targets;
            targets.Weight = static_cast<float>(i) * 0.25f;
            targets.Target = root;
            targets.Others[0] = entities[i - 1];
            targets.Others[1] = outside;
            world.AddComponentImmediate(entity, targets);
        }
        entities.push_back(entity);
    }
    return entities;
}

std::size_t IndexOf(std::span<const EntityHandle> entities, EntityHandle entity)
{
    return static_cast<std::size_t>(std::find(entities.begin(), entities.end(), entity) - entities.begin());
}

std::size_t CountOf(std::span<const EntityHandle> entities, EntityHandle entity)
{
    return static_cast<std::size_t>(std::count(entities.begin(), entities.end(), entity));
}

// Every component of targets[i] equals sources[i]'s, its entity fields re-pointed into `targets`.
void ExpectCopyOf(const World& source, std::span<const EntityHandle> sources, const World& target,
                  std::span<const EntityHandle> targets)
{
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        const Position* sourcePosition = source.GetComponent<Position>(sources[i]);
        const Position* targetPosition = target.GetComponent<Position>(targets[i]);
        ASSERT_NE(targetPosition, nullptr) << "entity " << i;
        EXPECT_EQ(targetPosition->x, sourcePosition->x);
        EXPECT_EQ(targetPosition->y, sourcePosition->y);
        EXPECT_EQ(targetPosition->z, sourcePosition->z);
        EXPECT_EQ(target.HasComponent<Velocity>(targets[i]), source.HasComponent<Velocity>(sources[i]));
        if (const Velocity* velocity = source.GetComponent<Velocity>(sources[i]))
            EXPECT_EQ(target.GetComponent<Velocity>(targets[i])->x, velocity->x);

        if (const CloneParentLink* link = source.GetComponent<CloneParentLink>(sources[i]))
        {
            const CloneParentLink* cloned = target.GetComponent<CloneParentLink>(targets[i]);
            ASSERT_NE(cloned, nullptr);
            EXPECT_EQ(cloned->Depth, link->Depth);
            EXPECT_EQ(cloned->Parent, targets[IndexOf(sources, link->Parent)]) << "entity " << i;
        }
        if (const CloneTargets* fields = source.GetComponent<CloneTargets>(sources[i]))
        {
            const CloneTargets* cloned = target.GetComponent<CloneTargets>(targets[i]);
            ASSERT_NE(cloned, nullptr);
            EXPECT_EQ(cloned->Weight, fields->Weight);
            EXPECT_EQ(cloned->Target, targets[IndexOf(sources, fields->Target)]);
            EXPECT_EQ(cloned->Others[0], targets[IndexOf(sources, fields->Others[0])]);
            EXPECT_FALSE(target.IsValid(cloned->Others[1])) << "a handle outside the span becomes invalid";
        }
    }
}
// `actual` holds a T exactly when `expected` does, with the same bytes.
template <typename T>
void ExpectSameBytes(const World& expectedWorld, EntityHandle expected, const World& actualWorld, EntityHandle actual,
                     std::size_t entry)
{
    const T* want = expectedWorld.GetComponent<T>(expected);
    const T* got = actualWorld.GetComponent<T>(actual);
    ASSERT_EQ(got != nullptr, want != nullptr) << "entry " << entry;
    if (want)
        EXPECT_EQ(std::memcmp(got, want, sizeof(T)), 0) << "entry " << entry;
}
} // namespace

TEST(CreateBatchFromWorld, CopiesEveryComponentAndRepointsEntityFields)
{
    World source;
    World target;
    RegisterCloneComponents(source);
    RegisterCloneComponents(target);
    // Offset the target's allocator so a copied, unpatched handle would not alias its copy.
    for (int i = 0; i < 7; ++i)
        target.AddComponentImmediate(target.CreateEntity(), Velocity{});
    const EntityHandle outside = source.CreateEntity();
    const std::vector<EntityHandle> sources = BuildTemplate(source, outside);

    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone(sources, plan));
    EntityRemap remap;
    ASSERT_TRUE(target.CreateBatchFromWorld(source, plan, remap));

    const std::span<const EntityHandle> targets = remap.GetTargets();
    ASSERT_EQ(targets.size(), kTemplateSize);
    EXPECT_EQ(target.GetEntityCount(), 7u + kTemplateSize);
    for (const EntityHandle entity : targets)
        EXPECT_TRUE(target.IsValid(entity));
    ExpectCopyOf(source, sources, target, targets);
}

TEST(CreateBatchFromWorld, EachCopyPointsAtItsOwnEntities)
{
    World source;
    World target;
    RegisterCloneComponents(source);
    RegisterCloneComponents(target);
    const std::vector<EntityHandle> tree = BuildTemplate(source, source.CreateEntity());
    std::vector<EntityHandle> span = tree;
    span.insert(span.end(), tree.begin(), tree.end());
    span.insert(span.end(), tree.begin(), tree.end());

    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone(span, plan));
    EntityRemap remap;
    ASSERT_TRUE(target.CreateBatchFromWorld(source, plan, remap));

    ASSERT_EQ(remap.GetTargets().size(), 3 * kTemplateSize);
    for (std::size_t copy = 0; copy < 3; ++copy)
        ExpectCopyOf(source, tree, target, remap.GetTargets().subspan(copy * kTemplateSize, kTemplateSize));
}

TEST(CreateBatchFromWorld, AFieldThatNamesItsOwnEntityNamesTheCopyAndANullFieldStaysNull)
{
    World source;
    World target;
    RegisterCloneComponents(source);
    RegisterCloneComponents(target);
    // Entities in the target first, so a copy's handle differs from its source's handle.
    for (int i = 0; i < 5; ++i)
        target.AddComponentImmediate(target.CreateEntity(), Velocity{});
    const EntityHandle self = source.CreateEntity();
    CloneTargets fields;
    fields.Target = self;
    fields.Others[1] = self;
    source.AddComponentImmediate(self, fields);

    const std::vector<EntityHandle> twice{self, self};
    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone(twice, plan));
    EntityRemap remap;
    ASSERT_TRUE(target.CreateBatchFromWorld(source, plan, remap));

    ASSERT_EQ(remap.GetTargets().size(), 2u);
    for (const EntityHandle clone : remap.GetTargets())
    {
        const CloneTargets* cloned = target.GetComponent<CloneTargets>(clone);
        ASSERT_NE(cloned, nullptr);
        EXPECT_NE(clone, self);
        EXPECT_EQ(cloned->Target, clone);
        EXPECT_EQ(cloned->Others[1], clone);
        EXPECT_EQ(cloned->Others[0], EntityHandle{});
    }
}

TEST(CreateBatchFromWorld, RecordsOneAddedPerClonedEntity)
{
    World source;
    World target;
    RegisterCloneComponents(source);
    RegisterCloneComponents(target);
    target.EnableLifecycleEvents<Position>();
    target.EnableLifecycleEvents<CloneParentLink>();
    const std::vector<EntityHandle> sources = BuildTemplate(source, source.CreateEntity());

    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone(sources, plan));
    EntityRemap remap;
    ASSERT_TRUE(target.CreateBatchFromWorld(source, plan, remap));
    target.SwapLifecycleEvents();

    const auto addedPositions = target.GetAdded<Position>();
    const auto addedLinks = target.GetAdded<CloneParentLink>();
    EXPECT_EQ(addedPositions.size(), kTemplateSize);
    EXPECT_EQ(addedLinks.size(), kTemplateSize - 1);
    for (const EntityHandle entity : remap.GetTargets())
        EXPECT_EQ(CountOf(addedPositions, entity), 1u);
}

TEST(CreateBatchFromWorld, EmptyPlanCreatesNothing)
{
    World source;
    World target;
    const std::size_t versionBefore = target.GetStructuralChangeVersion();

    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone({}, plan));
    EntityRemap remap;
    EXPECT_TRUE(target.CreateBatchFromWorld(source, plan, remap));

    EXPECT_TRUE(remap.GetTargets().empty());
    EXPECT_EQ(target.GetEntityCount(), 0u);
    EXPECT_EQ(target.GetStructuralChangeVersion(), versionBefore);
}

TEST(CreateBatchFromWorld, RefusesAPlannedEntityNoLongerAliveInTheSource)
{
    World source;
    World target;
    RegisterCloneComponents(source);
    RegisterCloneComponents(target);
    std::vector<EntityHandle> sources = BuildTemplate(source, source.CreateEntity());
    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone(sources, plan));
    source.DestroyEntityImmediate(sources[7]);

    EntityRemap remap;
    EXPECT_FALSE(target.CreateBatchFromWorld(source, plan, remap));

    EXPECT_TRUE(remap.GetTargets().empty());
    EXPECT_EQ(target.GetEntityCount(), 0u);
    ClonePlan replanned;
    EXPECT_FALSE(source.PlanClone(sources, replanned));
}

TEST(CreateBatchFromWorld, CopiesPreservedUnknownComponentsAndFields)
{
    World source;
    World target;
    const EntityHandle entity = source.CreateEntity();
    source.AddComponentImmediate(entity, Position{4.0f, 5.0f, 6.0f});
    UnresolvedComponentStore& store = source.GetUnresolvedComponents();
    store.Add(entity, PreservedComponent{"Jumpable", {{"height", "2.5"}}});
    PreservedField field;
    field.Component = "SplineFence";
    field.Field = "grade";
    field.RawText = "steep";
    store.AddField(entity, field);
    store.NoteUnresolvedReference("0123456789abcdef", "Meshes/Fence.mesh");

    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone(std::span<const EntityHandle>(&entity, 1), plan));
    EntityRemap remap;
    ASSERT_TRUE(target.CreateBatchFromWorld(source, plan, remap));

    const UnresolvedComponentStore* cloned = target.TryGetUnresolvedComponents();
    ASSERT_NE(cloned, nullptr);
    const EntityHandle clone = remap.GetTargets()[0];
    const auto components = cloned->Map().find(clone);
    ASSERT_NE(components, cloned->Map().end());
    ASSERT_EQ(components->second.size(), 1u);
    EXPECT_EQ(components->second[0].Name, "Jumpable");
    EXPECT_EQ(components->second[0].Props[0].second, "2.5");
    const std::vector<PreservedField>* fields = cloned->FieldsFor(clone);
    ASSERT_NE(fields, nullptr);
    ASSERT_EQ(fields->size(), 1u);
    EXPECT_EQ((*fields)[0].RawText, "steep");
    const std::string* path = cloned->FindUnresolvedReference("0123456789abcdef");
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(*path, "Meshes/Fence.mesh");
}

TEST(CreateBatchFromWorld, CopiesEachRowAsAPerEntityCloneDoes)
{
    World source;
    World target;
    // The Position rows start in a target chunk with five free rows and continue past the next chunk.
    const EntityHandle first = target.CreateEntity();
    target.AddComponentImmediate(first, Position{});
    const Archetype* positionArchetype = target.GetEntityArchetype(first);
    const uint32_t capacity = positionArchetype->GetTable().GetLayout().Capacity;
    for (uint32_t i = 1; i + 5 < capacity; ++i)
        target.AddComponentImmediate(target.CreateEntity(), Position{});
    // Two archetypes over several source chunks; every seventh entity is destroyed after the
    // others are listed, so the listed rows of each archetype are not one consecutive run.
    std::vector<EntityHandle> sources;
    std::vector<EntityHandle> spacers;
    for (uint32_t i = 0; i < 3 * capacity; ++i)
    {
        const EntityHandle entity = source.CreateEntity();
        source.AddComponentImmediate(entity, Position{static_cast<float32>(i), 0.5f, -static_cast<float32>(i)});
        if (i % 3 == 0)
            source.AddComponentImmediate(entity, Velocity{1.0f, static_cast<float32>(i), 2.0f});
        (i % 7 == 3 ? spacers : sources).push_back(entity);
    }
    for (const EntityHandle spacer : spacers)
        source.DestroyEntityImmediate(spacer);

    ClonePlan plan;
    ASSERT_TRUE(source.PlanClone(sources, plan));
    EntityRemap remap;
    ASSERT_TRUE(target.CreateBatchFromWorld(source, plan, remap));

    const std::span<const EntityHandle> targets = remap.GetTargets();
    ASSERT_EQ(targets.size(), sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        const EntityHandle clone = source.CloneEntity(sources[i]);
        ExpectSameBytes<Position>(source, clone, target, targets[i], i);
        ExpectSameBytes<Velocity>(source, clone, target, targets[i], i);
    }
}
