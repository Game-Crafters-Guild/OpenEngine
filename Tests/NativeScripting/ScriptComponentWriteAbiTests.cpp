// A script's write of an engine component (ECS ABI 2.4): GE_ECSABI_SetComponentBytesFromScript, the
// entry ComponentType<T>.Set of a C# mirror calls, keeps the engine's bytes of every field a script
// may not write, refuses a component the engine owns with the system that writes it, and merges a
// queued write at the flush. GE_ECSABI_GetComponentDefaults hands out the default bytes the mirrors'
// constructors copy. These drive the exports with no CLR, against the engine's own registrations.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/ComponentRegistration.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scripting/ECSABI.h"
#include "Scripting/ScriptingABI.h"

namespace ScriptComponentWriteAbiTestTypes
{
// A component with no fields (a tag), registered as the scanner registers one marked // @ge-tag.
struct PickedUp
{
};

// A typed component with no reflection and so no registered defaults.
struct Unreflected
{
    std::uint32_t Value = 3;
};
} // namespace ScriptComponentWriteAbiTestTypes

GE_REGISTER_TAG_COMPONENT(ScriptComponentWriteAbiTestTypes::PickedUp)

namespace
{
using GameEngine::Components::MeshRenderer;
using GameEngine::Components::ShadowCasting;
using GameEngine::ECS::World;

GE_Handle HandleOf(World* world)
{
    return static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
}

std::string LastScriptError()
{
    char text[512] = {};
    int32_t length = 0;
    if (GE_GetLastScriptError(text, static_cast<int32_t>(sizeof(text)), &length) != GE_Result_Ok)
        return {};
    return std::string(text, static_cast<std::size_t>(length));
}

// The script's copy of a MeshRenderer it read earlier: another GPU handle, and Shadows Only.
MeshRenderer StaleScriptCopy(const MeshRenderer& engine)
{
    MeshRenderer script = engine;
    script.meshGpuHandleId = engine.meshGpuHandleId + 100;
    script.shadowCasting = ShadowCasting::ShadowsOnly;
    return script;
}

GE_Result SetFromScript(World& world, GameEngine::ECS::EntityHandle entity, const MeshRenderer& value)
{
    return GE_ECSABI_SetComponentBytesFromScript(HandleOf(&world), entity.id, GameEngine::ECS::GetComponentTypeId<MeshRenderer>(),
                                                 reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}
} // namespace

TEST(ScriptComponentWriteAbi, KeepsTheEngineBytesOfAHiddenField)
{
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    MeshRenderer engine{};
    engine.meshGpuHandleId = 7;
    world.AddComponentImmediate(entity, engine);

    ASSERT_EQ(SetFromScript(world, entity, StaleScriptCopy(engine)), GE_Result_Ok);

    const MeshRenderer* written = world.GetComponent<MeshRenderer>(entity);
    ASSERT_NE(written, nullptr);
    EXPECT_EQ(written->shadowCasting, ShadowCasting::ShadowsOnly) << "a field a script may write takes the script's value";
    EXPECT_EQ(written->meshGpuHandleId, 7u) << "the hidden GPU handle keeps the engine's value";
}

TEST(ScriptComponentWriteAbi, KeepsThePlaybackStateTheAnimationSystemWritesOnARetargeter)
{
    using GameEngine::Components::HumanoidRetargeterComponent;
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    HumanoidRetargeterComponent engine{};
    engine.SourceClipIndex = 4;
    engine.SourceSkeletonId = 9;
    engine.ClipTimeSeconds = 1.5f;
    world.AddComponentImmediate(entity, engine);

    HumanoidRetargeterComponent script = engine;
    script.SourceClipIndex = 5;
    script.SourceSkeletonId = 12;
    script.ClipTimeSeconds = 3.0f;
    script.IKEnabled = false;
    ASSERT_EQ(GE_ECSABI_SetComponentBytesFromScript(HandleOf(&world), entity.id,
                                                    GameEngine::ECS::GetComponentTypeId<HumanoidRetargeterComponent>(),
                                                    reinterpret_cast<const uint8_t*>(&script), sizeof(script)),
              GE_Result_Ok);

    const HumanoidRetargeterComponent* written = world.GetComponent<HumanoidRetargeterComponent>(entity);
    ASSERT_NE(written, nullptr);
    EXPECT_FALSE(written->IKEnabled) << "a field a script may write takes the script's value";
    EXPECT_EQ(written->SourceClipIndex, 4u) << "the clip index keeps the animation system's value";
    EXPECT_EQ(written->SourceSkeletonId, 9u) << "the source skeleton keeps the animation system's value";
    EXPECT_EQ(written->ClipTimeSeconds, 1.5f) << "the clip time keeps the animation system's value";
}

TEST(ScriptComponentWriteAbi, AQueuedWriteMergesWithTheValueAtTheFlush)
{
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    MeshRenderer engine{};
    engine.meshGpuHandleId = 7;
    world.AddComponentImmediate(entity, engine);

    ASSERT_EQ(GE_ECSABI_SetDeferStructuralChanges(HandleOf(&world), 1), GE_Result_Ok);
    ASSERT_EQ(SetFromScript(world, entity, StaleScriptCopy(engine)), GE_Result_Ok);
    world.GetComponentForWrite<MeshRenderer>(entity)->meshGpuHandleId = 9; // render binding writes outside the system phase
    ASSERT_EQ(GE_ECSABI_FlushDeferredCommands(HandleOf(&world)), GE_Result_Ok);

    const MeshRenderer* written = world.GetComponent<MeshRenderer>(entity);
    EXPECT_EQ(written->shadowCasting, ShadowCasting::ShadowsOnly);
    EXPECT_EQ(written->meshGpuHandleId, 9u) << "the merge runs against the value at the flush";
}

TEST(ScriptComponentWriteAbi, AnAddKeepsTheDefaultsOfTheEngineFields)
{
    GameEngine::ECS::AutoComponentRegistrar<MeshRenderer>::EnsureRegistered();
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    MeshRenderer script{};
    script.meshGpuHandleId = 42;
    script.receiveShadows = false;

    ASSERT_EQ(SetFromScript(world, entity, script), GE_Result_Ok);

    const MeshRenderer* added = world.GetComponent<MeshRenderer>(entity);
    ASSERT_NE(added, nullptr);
    EXPECT_FALSE(added->receiveShadows);
    EXPECT_EQ(added->meshGpuHandleId, MeshRenderer{}.meshGpuHandleId);
}

TEST(ScriptComponentWriteAbi, RefusesAComponentTheEngineOwnsAndNamesItsWriter)
{
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    const GameEngine::Components::WorldTransform world0{};
    const GameEngine::Components::MeshGPUData slot{};

    EXPECT_EQ(GE_ECSABI_SetComponentBytesFromScript(HandleOf(&world), entity.id,
                                                    GameEngine::ECS::GetComponentTypeId<GameEngine::Components::WorldTransform>(),
                                                    reinterpret_cast<const uint8_t*>(&world0), sizeof(world0)),
              GE_Result_InvalidArg);
    EXPECT_EQ(LastScriptError(), "A script cannot write WorldTransform: it is owned by the engine and written by "
                                 "TransformHierarchySystem. Read it with ComponentType<WorldTransform>.Get");

    EXPECT_EQ(GE_ECSABI_SetComponentBytesFromScript(HandleOf(&world), entity.id,
                                                    GameEngine::ECS::GetComponentTypeId<GameEngine::Components::MeshGPUData>(),
                                                    reinterpret_cast<const uint8_t*>(&slot), sizeof(slot)),
              GE_Result_InvalidArg);
    EXPECT_NE(LastScriptError().find("cannot write MeshGPUData: it is engine runtime state written by render extraction"),
              std::string::npos)
        << LastScriptError();
    EXPECT_FALSE(world.HasComponent<GameEngine::Components::WorldTransform>(entity));
    EXPECT_FALSE(world.HasComponent<GameEngine::Components::MeshGPUData>(entity));
}

// A script that skips the mirror and writes raw bytes (WorldHandle.SetComponentBytes) meets the same refusal, so
// no path lets a script write a component the engine owns.
TEST(ScriptComponentWriteAbi, TheRawByteWriteRefusesAComponentTheEngineOwnsAsTheMirrorDoes)
{
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    const GameEngine::Components::WorldTransform placed{};
    const GameEngine::Components::MeshGPUData slot{};

    EXPECT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(&world), entity.id,
                                          GameEngine::ECS::GetComponentTypeId<GameEngine::Components::WorldTransform>(),
                                          reinterpret_cast<const uint8_t*>(&placed), sizeof(placed)),
              GE_Result_InvalidArg);
    EXPECT_EQ(LastScriptError(), "A script cannot write WorldTransform: it is owned by the engine and written by "
                                 "TransformHierarchySystem. Read it with ComponentType<WorldTransform>.Get");

    EXPECT_EQ(GE_ECSABI_SetComponentBytes(HandleOf(&world), entity.id,
                                          GameEngine::ECS::GetComponentTypeId<GameEngine::Components::MeshGPUData>(),
                                          reinterpret_cast<const uint8_t*>(&slot), sizeof(slot)),
              GE_Result_InvalidArg);
    EXPECT_NE(LastScriptError().find("cannot write MeshGPUData: it is engine runtime state written by render extraction"),
              std::string::npos)
        << LastScriptError();
    EXPECT_FALSE(world.HasComponent<GameEngine::Components::WorldTransform>(entity));
    EXPECT_FALSE(world.HasComponent<GameEngine::Components::MeshGPUData>(entity));
}

// EntityCommands.AddComponent and SetComponent of a mirror queue kind 5: the queued stale copy keeps the engine's
// GPU handle at the flush, and a component the engine owns is refused when it is queued, not at the flush.
TEST(ScriptComponentWriteAbi, AQueuedScriptWriteKeepsTheEngineFieldsAndIsRefusedWhenQueued)
{
    constexpr uint32_t kDeferScriptWrite = 5;
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    MeshRenderer engine{};
    engine.meshGpuHandleId = 7;
    world.AddComponentImmediate(entity, engine);
    const MeshRenderer stale = StaleScriptCopy(engine);
    const GE_Handle handle = HandleOf(&world);

    ASSERT_EQ(GE_ECSABI_DeferCommand(handle, kDeferScriptWrite, entity.id, GameEngine::ECS::GetComponentTypeId<MeshRenderer>(),
                                     &stale, sizeof(stale)),
              GE_Result_Ok);
    const GameEngine::Components::WorldTransform placed{};
    EXPECT_EQ(GE_ECSABI_DeferCommand(handle, kDeferScriptWrite, entity.id,
                                     GameEngine::ECS::GetComponentTypeId<GameEngine::Components::WorldTransform>(), &placed,
                                     sizeof(placed)),
              GE_Result_InvalidArg);
    EXPECT_NE(LastScriptError().find("cannot write WorldTransform"), std::string::npos) << LastScriptError();
    ASSERT_EQ(GE_ECSABI_FlushDeferredCommands(handle), GE_Result_Ok);

    const MeshRenderer* written = world.GetComponent<MeshRenderer>(entity);
    EXPECT_EQ(written->shadowCasting, ShadowCasting::ShadowsOnly);
    EXPECT_EQ(written->meshGpuHandleId, 7u);
}

// The plain queued add and set (DeferCommand kinds 2 and 4) refuse a component the engine owns when it is queued,
// with the same text, so a raw queued write cannot reach it at the flush either.
TEST(ScriptComponentWriteAbi, APlainQueuedWriteRefusesAComponentTheEngineOwnsWhenQueued)
{
    constexpr uint32_t kDeferAdd = 2;
    constexpr uint32_t kDeferSet = 4;
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    const GE_Handle handle = HandleOf(&world);
    const GameEngine::Components::WorldTransform placed{};

    for (const uint32_t kind : {kDeferAdd, kDeferSet})
    {
        EXPECT_EQ(GE_ECSABI_DeferCommand(handle, kind, entity.id,
                                         GameEngine::ECS::GetComponentTypeId<GameEngine::Components::WorldTransform>(), &placed,
                                         sizeof(placed)),
                  GE_Result_InvalidArg);
        EXPECT_EQ(LastScriptError(), "A script cannot write WorldTransform: it is owned by the engine and written by "
                                     "TransformHierarchySystem. Read it with ComponentType<WorldTransform>.Get");
    }
    ASSERT_EQ(GE_ECSABI_FlushDeferredCommands(handle), GE_Result_Ok);
    EXPECT_FALSE(world.HasComponent<GameEngine::Components::WorldTransform>(entity));
}

// An add of a component with no registered defaults has no engine bytes to keep, so an add that keeps some
// bytes is refused rather than filled from the script; an add whose one run covers every byte keeps nothing.
TEST(ScriptComponentWriteAbi, AnAddWithNoDefaultsIsRefused)
{
    using ScriptComponentWriteAbiTestTypes::Unreflected;
    GameEngine::ECS::AutoComponentRegistrar<Unreflected>::EnsureRegistered();
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    const Unreflected script{};

    // Runs that keep some bytes (here bytes 2 to 4): the add has no engine bytes to keep them from.
    const GameEngine::ECS::ScriptWritableRange partial[] = {{0, 2}};
    EXPECT_FALSE(world.SetComponentBytesFromScriptImmediate(entity, GameEngine::ECS::GetComponentTypeId<Unreflected>(), &script,
                                                            sizeof(script), partial));
    EXPECT_FALSE(world.HasComponent<Unreflected>(entity));

    // One run over every byte keeps nothing, so the add takes the script's bytes.
    const GameEngine::ECS::ScriptWritableRange whole[] = {{0, sizeof(Unreflected)}};
    EXPECT_TRUE(world.SetComponentBytesFromScriptImmediate(entity, GameEngine::ECS::GetComponentTypeId<Unreflected>(), &script,
                                                           sizeof(script), whole));
    EXPECT_EQ(world.GetComponent<Unreflected>(entity)->Value, 3u);
}

// A tag registers with an empty field table and a one-byte default; a script's Set adds it by presence.
TEST(ScriptComponentWriteAbi, ATagRegistersWithNoFieldsAndASetAddsIt)
{
    using ScriptComponentWriteAbiTestTypes::PickedUp;
    const GameEngine::ECS::ComponentTypeId tag = GameEngine::ECS::GetComponentTypeId<PickedUp>();
    EXPECT_TRUE(GameEngine::ECS::ComponentFieldRegistry::Has(tag));
    EXPECT_TRUE(GameEngine::ECS::ComponentFieldRegistry::Get(tag).empty());
    std::vector<uint8_t> defaults;
    ASSERT_TRUE(GameEngine::ECS::ComponentFactory::GetDefaultBytes(tag, defaults));
    EXPECT_EQ(defaults.size(), 1u);

    GameEngine::ECS::AutoComponentRegistrar<PickedUp>::EnsureRegistered();
    World world(nullptr);
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    const uint8_t presence = 0;
    ASSERT_EQ(GE_ECSABI_SetComponentBytesFromScript(HandleOf(&world), entity.id, tag, &presence, 1), GE_Result_Ok);
    EXPECT_TRUE(world.HasComponent<PickedUp>(entity));
}

TEST(ScriptComponentWriteAbi, TheDefaultsAreTheBytesOfTheDefaultConstructedStruct)
{
    uint32_t length = 0;
    ASSERT_EQ(GE_ECSABI_GetComponentDefaults(nullptr, 0, &length), GE_Result_Ok);
    ASSERT_GT(length, 0u);
    std::vector<uint8_t> records(length);
    uint32_t tooSmall = 0;
    EXPECT_EQ(GE_ECSABI_GetComponentDefaults(records.data(), length - 1, &tooSmall), GE_Result_BufferFull);
    EXPECT_EQ(tooSmall, length);
    ASSERT_EQ(GE_ECSABI_GetComponentDefaults(records.data(), length, &length), GE_Result_Ok);

    const uint64_t wanted = GameEngine::ECS::GetComponentTypeId<MeshRenderer>();
    bool found = false;
    constexpr std::size_t kRecordHeader = sizeof(uint64_t) + sizeof(uint32_t); // {typeId, size} before each default
    for (std::size_t at = 0; at + kRecordHeader <= records.size();)
    {
        uint64_t typeId = 0;
        uint32_t size = 0;
        std::memcpy(&typeId, records.data() + at, sizeof(typeId));
        std::memcpy(&size, records.data() + at + sizeof(typeId), sizeof(size));
        if (typeId == wanted)
        {
            ASSERT_EQ(size, sizeof(MeshRenderer));
            MeshRenderer defaults;
            std::memcpy(&defaults, records.data() + at + kRecordHeader, sizeof(defaults));
            EXPECT_EQ(defaults.shadowCasting, ShadowCasting::On);
            EXPECT_TRUE(defaults.receiveShadows);
            EXPECT_TRUE(defaults.motionVectors);
            EXPECT_EQ(defaults.RenderLayers, MeshRenderer{}.RenderLayers);
            found = true;
        }
        at += kRecordHeader + size;
    }
    EXPECT_TRUE(found) << "every reflected engine component has a default";
}
