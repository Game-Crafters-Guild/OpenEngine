#include "SpatialGridECS/SpatialGridView.h"
#include "SpatialGridECS/Systems/SpatialGridBuildSystem.h"
#include "Components/SpatialGridMover.h"
#include "Components/SpatialGridSettings.h"
#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/Component/ComponentFactory.h"
#include "ECS/Component/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "../Source/SpatialGridState.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::SpatialGridECS
{
namespace
{

// (g) A position written in step T is found at the old place in step T and at the new place in step T + 1.
TEST(SpatialGridSystemTests, APositionWrittenInAStepIsFoundInTheNextStep)
{
    ECS::World world;
    SpatialGridBuildSystem build;
    const ECS::EntityHandle mover = world.Create(Components::SpatialGridMover{}).GetHandle();
    world.GetComponentForWrite<Components::Transform>(mover)->Position = {10.0, 0.0, 10.0};
    const Mathematics::WorldPosition oldPlace{10.0, 0.0, 10.0};
    const Mathematics::WorldPosition newPlace{90.0, 0.0, 10.0};
    std::array<SpatialGridHit, 4> out{};

    build.Update(world, 0.0f);
    world.GetComponentForWrite<Components::Transform>(mover)->Position = newPlace;
    const SpatialGridView stepT(world);
    ASSERT_EQ(stepT.Sphere(oldPlace, 1.0, SpatialGridFilter{}, out), 1u);
    EXPECT_EQ(out[0].Entity, mover);
    EXPECT_EQ(stepT.Sphere(newPlace, 1.0, SpatialGridFilter{}, out), 0u);

    build.Update(world, 0.0f);
    const SpatialGridView stepNext(world);
    EXPECT_EQ(stepNext.Sphere(oldPlace, 1.0, SpatialGridFilter{}, out), 0u);
    ASSERT_EQ(stepNext.Sphere(newPlace, 1.0, SpatialGridFilter{}, out), 1u);
    EXPECT_EQ(out[0].Entity, mover);
    EXPECT_EQ(stepNext.Sphere(newPlace, 1.0, SpatialGridFilter{~0u, mover}, out), 0u);
}

constexpr Mathematics::WorldPosition kStepPlace{90.0, 0.0, 10.0};

// Each simulation step, counts the movers at kStepPlace, then writes the mover's local Transform there.
class StepProbeSystem : public ECS::ISystem
{
public:
    static constexpr ECS::SystemCadence Cadence = ECS::SystemCadence::Simulation;

    StepProbeSystem(ECS::EntityHandle mover, std::vector<uint32>* found) : m_Mover(mover), m_Found(found) {}
    const char* GetName() const override { return "StepProbe"; }
    void Update(ECS::World& world, float32) override
    {
        std::array<SpatialGridHit, 4> out{};
        m_Found->push_back(SpatialGridView(world).Sphere(kStepPlace, 1.0, SpatialGridFilter{}, out));
        world.GetComponentForWrite<Components::Transform>(m_Mover)->Position = kStepPlace;
    }

private:
    ECS::EntityHandle m_Mover;
    std::vector<uint32>* m_Found;
};

// Runs one frame of two simulation steps: the build, then the probe that moves `mover` in the first step.
std::vector<uint32> FoundInTwoStepsOfOneFrame(ECS::World& world, ECS::EntityHandle mover)
{
    std::vector<uint32> found;
    ECS::SystemScheduleBuilder builder;
    AddSpatialGridSystemsToSchedule(builder);
    builder.Add<StepProbeSystem>("StepProbe", ECS::SystemPhase::Early, 20, {"SpatialGridBuildSystem"}, mover, &found);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);
    manager.Update(world, static_cast<float32>(2.5 * world.GetSimulationClock().GetFixedDeltaSeconds()));
    return found;
}

// A root mover that a step writes is found by the next step of the same frame.
TEST(SpatialGridSystemTests, ARootMoverWrittenInAStepIsFoundInTheNextStepOfTheFrame)
{
    ECS::World world;
    const ECS::EntityHandle mover = world.Create(Components::SpatialGridMover{}).GetHandle();
    EXPECT_EQ(FoundInTwoStepsOfOneFrame(world, mover), (std::vector<uint32>{0u, 1u}));
}

// A child mover keeps the place of the last hierarchy update for the rest of the frame.
TEST(SpatialGridSystemTests, AChildMoverKeepsItsPlaceUntilTheHierarchyUpdate)
{
    ECS::World world;
    const ECS::EntityHandle parent = world.Create(Components::Transform{}).GetHandle();
    const ECS::EntityHandle mover = world.Create(Components::SpatialGridMover{}, Components::Parent{parent}).GetHandle();
    EXPECT_EQ(FoundInTwoStepsOfOneFrame(world, mover), (std::vector<uint32>{0u, 0u}));
}

// A mover whose Parent holds an empty handle is a root: a write in one step is found in the next step.
TEST(SpatialGridSystemTests, AMoverWithAnEmptyParentIsFoundInTheNextStepOfTheFrame)
{
    ECS::World world;
    const ECS::EntityHandle mover = world.Create(Components::SpatialGridMover{}, Components::Parent{}).GetHandle();
    EXPECT_EQ(FoundInTwoStepsOfOneFrame(world, mover), (std::vector<uint32>{0u, 1u}));
}

// The place of the named system in the manager's execution plan: its wave, then its slot in the wave.
// A system that is not in the plan has no place.
std::optional<std::pair<size_t, size_t>> FindPlaceOf(const ECS::SystemManager& manager, std::string_view name)
{
    const auto& plan = manager.GetExecutionPlan();
    for (size_t wave = 0; wave < plan.Waves.size(); ++wave)
    {
        const std::vector<size_t>& indices = plan.Waves[wave].SystemIndices;
        for (size_t slot = 0; slot < indices.size(); ++slot)
        {
            const char* systemName = manager.GetSequentialSystemName(indices[slot]);
            if (systemName != nullptr && name == systemName)
                return std::pair{wave, slot};
        }
    }
    return std::nullopt;
}

// The build runs before a system at the AI systems' place (Early 10) that declares no dependency on it.
// The build is exclusive: in a shared wave, the systems before it finish first and the systems after it
// start after it.
TEST(SpatialGridSystemTests, TheBuildRunsBeforeTheAiSystems)
{
    std::vector<uint32> found;
    ECS::SystemScheduleBuilder builder;
    AddSpatialGridSystemsToSchedule(builder);
    builder.Add<StepProbeSystem>("StepProbe", ECS::SystemPhase::Early, 10, {}, ECS::EntityHandle{}, &found);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const auto build = FindPlaceOf(manager, "SpatialGridBuildSystem");
    const auto probe = FindPlaceOf(manager, "StepProbe");
    ASSERT_TRUE(build.has_value());
    ASSERT_TRUE(probe.has_value());
    EXPECT_LT(*build, *probe);
}

// A view of a world whose build system is gone has no movers.
TEST(SpatialGridSystemTests, AViewHasNoMoversAfterTheBuildSystemIsDestroyed)
{
    ECS::World world;
    auto build = std::make_unique<SpatialGridBuildSystem>();
    world.Create(Components::SpatialGridMover{});
    std::array<SpatialGridHit, 4> out{};
    build->Update(world, 0.0f);
    ASSERT_EQ(SpatialGridView(world).Sphere({}, 1.0, SpatialGridFilter{}, out), 1u);

    build.reset();
    const Components::SpatialGridState* state = world.GetSingleton<Components::SpatialGridState>();
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->Grid, nullptr);
    const SpatialGridView view(world);
    EXPECT_EQ(view.Sphere({}, 1.0, SpatialGridFilter{}, out), 0u);
    EXPECT_EQ(view.Nearest({}, 1.0, SpatialGridFilter{}, out), 0u);
    EXPECT_EQ(view.Count({}, 1.0, SpatialGridFilter{}), 0u);
}

// The Inspector's Add Component lists SpatialGridMover with its radius and mask, and not the world's settings.
TEST(SpatialGridSystemTests, AMoverIsAddableWithItsRadiusAndMaskAndTheSettingsAreNot)
{
    const ECS::ComponentTypeId id = ECS::ComponentFieldRegistry::FindByName("GameEngine::Components::SpatialGridMover");
    ASSERT_EQ(id, ECS::GetComponentTypeId<Components::SpatialGridMover>());
    const std::vector<ECS::ComponentTypeId> addable = ECS::ComponentFactory::RegisteredTypes();
    EXPECT_NE(std::ranges::find(addable, id), addable.end());
    std::vector<std::string_view> fields;
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(id))
        fields.push_back(field.Name);
    EXPECT_EQ(fields, (std::vector<std::string_view>{"Radius", "Mask"}));

    const ECS::ComponentTypeId settings = ECS::ComponentFieldRegistry::FindByName("GameEngine::Components::SpatialGridSettings");
    ASSERT_EQ(settings, ECS::GetComponentTypeId<Components::SpatialGridSettings>());
    EXPECT_EQ(std::ranges::find(addable, settings), addable.end());
}

// A mover with a NaN, infinite or negative radius is not indexed, and the state counts it.
TEST(SpatialGridSystemTests, AMoverWithARadiusOutsideItsDomainIsCountedAndNotIndexed)
{
    ECS::World world;
    SpatialGridBuildSystem build;
    for (const float32 radius : {std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::infinity(), -1.0f})
        world.Create(Components::SpatialGridMover{.Radius = radius});
    world.Create(Components::SpatialGridMover{});
    build.Update(world, 0.0f);

    std::array<SpatialGridHit, 4> out{};
    EXPECT_EQ(SpatialGridView(world).Sphere({0.0, 0.0, 0.0}, 2.0, SpatialGridFilter{}, out), 1u);
    const Components::SpatialGridState* state = world.GetSingleton<Components::SpatialGridState>();
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->RefusedMovers, 3u);
}

class SpatialGridWarningTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Logger::Log::Initialize(Logger::Log::Config{});
        auto sink = std::make_unique<Logger::CallbackSink>();
        sink->RegisterCallback([warnings = m_Warnings](const Logger::LogMessage& message)
                               {
                                   if (message.Level == Logger::LogLevel::Warning)
                                       warnings->push_back(std::string(message.Message));
                               });
        Logger::Log::AddSink(std::move(sink));
    }

    void TearDown() override { Logger::Log::Shutdown(); }

    std::shared_ptr<std::vector<std::string>> m_Warnings = std::make_shared<std::vector<std::string>>();
};

// A build logs one warning when the refused count changes, and none while it stays the same.
TEST_F(SpatialGridWarningTests, ABuildWarnsOnceWhenTheRefusedCountChanges)
{
    ECS::World world;
    SpatialGridBuildSystem build;
    world.Create(Components::SpatialGridMover{.Radius = -1.0f});
    build.Update(world, 0.0f);
    build.Update(world, 0.0f);
    world.Create(Components::SpatialGridMover{.Radius = -1.0f});
    build.Update(world, 0.0f);
    Logger::Log::Flush();

    ASSERT_EQ(m_Warnings->size(), 2u);
    EXPECT_NE(m_Warnings->back().find("movers not in the grid: 2."), std::string::npos);
    EXPECT_NE(m_Warnings->back().find("Radius must be 0 or more and finite"), std::string::npos);
}

// A cell size under 1 or not finite builds a grid and logs one warning that states the fix. A huge cell size is valid.
TEST_F(SpatialGridWarningTests, ACellSizeUnder1OrNotFiniteWarnsAndTheGridStillAnswers)
{
    constexpr std::array kInvalid{0.0f, -8.0f, 0.5f, std::numeric_limits<float32>::quiet_NaN(),
                                  -std::numeric_limits<float32>::infinity()};
    std::vector<float32> cellSizes(kInvalid.begin(), kInvalid.end());
    cellSizes.push_back(std::numeric_limits<float32>::max());
    for (const float32 cellSize : cellSizes)
    {
        ECS::World world;
        SpatialGridBuildSystem build;
        world.SetSingleton(Components::SpatialGridSettings{.CellSize = cellSize});
        world.Create(Components::SpatialGridMover{});
        build.Update(world, 0.0f);
        build.Update(world, 0.0f);
        std::array<SpatialGridHit, 1> out{};
        EXPECT_EQ(SpatialGridView(world).Sphere({0.0, 0.0, 0.0}, 2.0, SpatialGridFilter{}, out), 1u) << cellSize;
    }
    Logger::Log::Flush();

    ASSERT_EQ(m_Warnings->size(), kInvalid.size());
    for (const std::string& warning : *m_Warnings)
        EXPECT_NE(warning.find("Set CellSize to a finite value of 1 or more."), std::string::npos) << warning;
}

// A change from the default cell size to NaN keeps the grid's cell size and still logs one warning.
TEST_F(SpatialGridWarningTests, ACellSizeThatBecomesNaNWarnsOnce)
{
    ECS::World world;
    SpatialGridBuildSystem build;
    build.Update(world, 0.0f);
    world.SetSingleton(Components::SpatialGridSettings{.CellSize = std::numeric_limits<float32>::quiet_NaN()});
    build.Update(world, 0.0f);
    build.Update(world, 0.0f);
    Logger::Log::Flush();

    EXPECT_EQ(m_Warnings->size(), 1u);
}

} // namespace
} // namespace GameEngine::SpatialGridECS
