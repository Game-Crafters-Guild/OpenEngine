// The terrain height function registry and its built instances (design planar-height-function-source
// 3.3): registration by name, the newest registration wins, an unknown name answers absent, one built
// instance per key, built on the job system, and a revocation that waits for the call in flight before
// it destroys the function.

#include <gtest/gtest.h>

#include "Components/Terrain/Terrain.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"
#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainHeightFunction/TerrainHeightFunctionInstances.h"
#include "TerrainHeightFunction/TerrainHeightFunctionRegistry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

// The seed as a constant height, so a test can tell instances apart.
class SeedHeight final : public ITerrainHeightFunction
{
public:
    explicit SeedHeight(const TerrainHeightFunctionContext& context)
        : m_Height(static_cast<float32>(context.Seed))
    {
    }
    void HeightRow(float32, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        for (std::size_t i = 0; i < x.size(); ++i)
            outMeters[i] = m_Height;
    }
    TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {m_Height, m_Height};
    }

private:
    float32 m_Height = 0.0f;
};

// Another factory, so a replacement is observable.
class Zero final : public ITerrainHeightFunction
{
public:
    explicit Zero(const TerrainHeightFunctionContext&) {}
    void HeightRow(float32, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        for (std::size_t i = 0; i < x.size(); ++i)
            outMeters[i] = 0.0f;
    }
    TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {};
    }
};

// A registration the module load `moduleId` / `generation` made, as Register records it.
TerrainHeightFunctionRegistration RegistrationOf(TerrainHeightFunctionFactory factory, std::string moduleId = "Game",
                                                 uint64 generation = 1u)
{
    return {factory, 1u, {std::move(moduleId), generation}};
}

TEST(TerrainHeightFunctionRegistry, FindsARegisteredNameAndNotAnUnknownOne)
{
    TerrainHeightFunctionRegistry& registry = TerrainHeightFunctionRegistry::Get();
    ASSERT_TRUE(registry.Register("RegistryTests.Seed", &MakeTerrainHeightFunction<SeedHeight>));
    const auto found = registry.Find("RegistryTests.Seed"_sid);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->Factory, &MakeTerrainHeightFunction<SeedHeight>);
    EXPECT_NE(found->Generation, 0u);
    EXPECT_FALSE(registry.Find("RegistryTests.NoModuleRegistersThis"_sid).has_value());
}

TEST(TerrainHeightFunctionRegistry, ASecondRegistrationOfTheNameWinsWithANewGeneration)
{
    TerrainHeightFunctionRegistry& registry = TerrainHeightFunctionRegistry::Get();
    ASSERT_TRUE(registry.Register("RegistryTests.Reloaded", &MakeTerrainHeightFunction<SeedHeight>));
    const uint64 first = registry.Find("RegistryTests.Reloaded"_sid)->Generation;
    ASSERT_TRUE(registry.Register("RegistryTests.Reloaded", &MakeTerrainHeightFunction<Zero>));
    const auto second = registry.Find("RegistryTests.Reloaded"_sid);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->Factory, &MakeTerrainHeightFunction<Zero>);
    EXPECT_GT(second->Generation, first);
}

TEST(TerrainHeightFunctionRegistry, RefusesAnEmptyOrTooLongName)
{
    TerrainHeightFunctionRegistry& registry = TerrainHeightFunctionRegistry::Get();
    EXPECT_FALSE(registry.Register("", &MakeTerrainHeightFunction<Zero>));
    const std::string tooLong(Components::kTerrainHeightFunctionNameCapacity, 'x');
    EXPECT_FALSE(registry.Register(tooLong, &MakeTerrainHeightFunction<Zero>));
    EXPECT_FALSE(registry.Find(HashStringId(tooLong)).has_value());
}

TEST(TerrainHeightFunctionRegistry, AModuleUnloadRemovesOnlyTheNamesThatLoadRegistered)
{
    TerrainHeightFunctionRegistry& registry = TerrainHeightFunctionRegistry::Get();
    ECS::SetActiveRegistrationModule("RegistryTests.Islands", 3u);
    ASSERT_TRUE(registry.Register("RegistryTests.Islands", &MakeTerrainHeightFunction<SeedHeight>));
    ECS::SetActiveRegistrationModule("RegistryTests.Canyons", 3u);
    ASSERT_TRUE(registry.Register("RegistryTests.Canyons", &MakeTerrainHeightFunction<SeedHeight>));
    ECS::ClearActiveRegistrationModule();
    EXPECT_EQ(registry.Find("RegistryTests.Islands"_sid)->Owner.Generation, 3u);

    EXPECT_EQ(registry.RemoveModuleGenerations("RegistryTests.Islands", 0u, 3u), 0u) << "an older load only";
    EXPECT_EQ(registry.RemoveModuleGenerations("RegistryTests.Islands", 3u, 4u), 1u);
    EXPECT_FALSE(registry.Find("RegistryTests.Islands"_sid).has_value());
    EXPECT_TRUE(registry.Find("RegistryTests.Canyons"_sid).has_value());
    EXPECT_EQ(registry.RemoveModuleGenerations("RegistryTests.Canyons", 3u, 4u), 1u);
}

TEST(TerrainHeightFunctionRegistry, ListsTheRegisteredNamesSortedUntilTheirModuleUnloads)
{
    TerrainHeightFunctionRegistry& registry = TerrainHeightFunctionRegistry::Get();
    ECS::SetActiveRegistrationModule("RegistryTests.Listed", 5u);
    ASSERT_TRUE(registry.Register("RegistryTests.Listed.Zeta", &MakeTerrainHeightFunction<Zero>));
    ASSERT_TRUE(registry.Register("RegistryTests.Listed.Alpha", &MakeTerrainHeightFunction<SeedHeight>));
    ECS::ClearActiveRegistrationModule();

    const std::vector<std::string> names = TerrainService::GetHeightFunctionNames();
    EXPECT_TRUE(std::ranges::is_sorted(names));
    const auto alpha = std::ranges::find(names, "RegistryTests.Listed.Alpha");
    const auto zeta = std::ranges::find(names, "RegistryTests.Listed.Zeta");
    ASSERT_NE(alpha, names.end());
    ASSERT_NE(zeta, names.end());
    EXPECT_LT(alpha, zeta);

    ASSERT_EQ(registry.RemoveModuleGenerations("RegistryTests.Listed", 5u, 6u), 2u);
    const std::vector<std::string> after = TerrainService::GetHeightFunctionNames();
    EXPECT_TRUE(std::ranges::is_sorted(after));
    EXPECT_EQ(std::ranges::find(after, "RegistryTests.Listed.Alpha"), after.end());
    EXPECT_EQ(std::ranges::find(after, "RegistryTests.Listed.Zeta"), after.end());
}

TEST(TerrainHeightFunctionRegistry, AnswersWhetherANameIsRegistered)
{
    ASSERT_TRUE(TerrainHeightFunctionRegistry::Get().Register("RegistryTests.Queried",
                                                              &MakeTerrainHeightFunction<SeedHeight>));
    EXPECT_TRUE(TerrainService::IsHeightFunctionRegistered("RegistryTests.Queried"));
    EXPECT_FALSE(TerrainService::IsHeightFunctionRegistered("RegistryTests.NoModuleRegistersThis"));
}

TerrainHeightFunctionKey SeedKey(uint64 seed)
{
    return {"Seed"_sid, 1u, {seed, 512.0f, 512.0f}};
}

float32 HeightOf(const RevocableTerrainHeightFunction& function)
{
    const float32 x[1] = {0.0f};
    float32 out[1] = {-1.0f};
    EXPECT_TRUE(function.HeightRow(0.0f, x, 0.0f, out));
    return out[0];
}

TEST(TerrainHeightFunctionInstances, BuildsOneInstancePerKey)
{
    TerrainHeightFunctionInstances instances(nullptr);
    const TerrainHeightFunctionRegistration seed = RegistrationOf(&MakeTerrainHeightFunction<SeedHeight>);
    const auto first = instances.Request(SeedKey(7u), seed);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(HeightOf(*first), 7.0f);
    EXPECT_EQ(instances.Request(SeedKey(7u), seed), first);
    const auto other = instances.Request(SeedKey(9u), seed);
    ASSERT_NE(other, nullptr);
    EXPECT_NE(other, first);
    EXPECT_EQ(HeightOf(*other), 9.0f);
}

TEST(TerrainHeightFunctionInstances, ReleasesAnInstanceNoOneHoldsOrRequested)
{
    TerrainHeightFunctionInstances instances(nullptr);
    const TerrainHeightFunctionRegistration seed = RegistrationOf(&MakeTerrainHeightFunction<SeedHeight>);
    std::weak_ptr<const RevocableTerrainHeightFunction> released = instances.Request(SeedKey(3u), seed);
    const auto held = instances.Request(SeedKey(4u), seed);
    instances.ReleaseUnused(); // both were requested since the last call
    EXPECT_FALSE(released.expired());
    instances.ReleaseUnused();
    EXPECT_TRUE(released.expired());
    EXPECT_EQ(instances.Request(SeedKey(4u), seed), held);
}

// Blocks in its constructor until released, and counts its destruction.
std::atomic<bool> g_BuildRelease{false};
std::atomic<int32> g_BuildDestroyed{0};

class BlockingBuild final : public ITerrainHeightFunction
{
public:
    explicit BlockingBuild(const TerrainHeightFunctionContext&)
    {
        while (!g_BuildRelease.load())
            std::this_thread::yield();
    }
    ~BlockingBuild() override { g_BuildDestroyed.fetch_add(1); }
    void HeightRow(float32, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        for (std::size_t i = 0; i < x.size(); ++i)
            outMeters[i] = 2.0f;
    }
    TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {};
    }
};

constexpr std::chrono::seconds kBuildWaitLimit{5};

TEST(TerrainHeightFunctionInstances, ARequestAnswersNullUntilTheBuildOnTheJobSystemEnds)
{
    g_BuildRelease.store(false);
    JobSystem::WorkStealingThreadPool jobs(2);
    TerrainHeightFunctionInstances instances(&jobs);
    const TerrainHeightFunctionRegistration registration = RegistrationOf(&MakeTerrainHeightFunction<BlockingBuild>);
    EXPECT_EQ(instances.Request(SeedKey(1u), registration), nullptr);
    EXPECT_EQ(instances.Request(SeedKey(1u), registration), nullptr) << "the constructor still runs";

    g_BuildRelease.store(true);
    std::shared_ptr<const RevocableTerrainHeightFunction> built;
    const auto deadline = std::chrono::steady_clock::now() + kBuildWaitLimit;
    while (built == nullptr && std::chrono::steady_clock::now() < deadline)
        built = instances.Request(SeedKey(1u), registration);
    ASSERT_NE(built, nullptr);
    EXPECT_EQ(HeightOf(*built), 2.0f);
}

TEST(TerrainHeightFunctionInstances, AKeyAbandonedDuringItsBuildReleasesTheFunctionWhenTheBuildEnds)
{
    g_BuildRelease.store(false);
    g_BuildDestroyed.store(0);
    JobSystem::WorkStealingThreadPool jobs(2);
    TerrainHeightFunctionInstances instances(&jobs);
    EXPECT_EQ(instances.Request(SeedKey(2u), RegistrationOf(&MakeTerrainHeightFunction<BlockingBuild>)), nullptr);
    instances.ReleaseUnused(); // the key was requested since the last call

    g_BuildRelease.store(true);
    const auto deadline = std::chrono::steady_clock::now() + kBuildWaitLimit;
    while (g_BuildDestroyed.load() == 0 && std::chrono::steady_clock::now() < deadline)
        instances.ReleaseUnused();
    EXPECT_EQ(g_BuildDestroyed.load(), 1) << "the built function stays alive until a module reload";
}

// Blocks in HeightRow until released, counts its destruction and records a destruction under a row.
std::atomic<bool> g_BlockingEntered{false};
std::atomic<bool> g_BlockingRelease{false};
std::atomic<int32> g_BlockingDestroyed{0};
std::atomic<int32> g_BlockingRowsInside{0};
std::atomic<bool> g_BlockingDestroyedUnderRow{false};

class Blocking final : public ITerrainHeightFunction
{
public:
    explicit Blocking(const TerrainHeightFunctionContext&) {}
    ~Blocking() override
    {
        if (g_BlockingRowsInside.load() != 0)
            g_BlockingDestroyedUnderRow.store(true);
        g_BlockingDestroyed.fetch_add(1);
    }
    void HeightRow(float32, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        g_BlockingRowsInside.fetch_add(1);
        g_BlockingEntered.store(true);
        while (!g_BlockingRelease.load())
            std::this_thread::yield();
        for (std::size_t i = 0; i < x.size(); ++i)
            outMeters[i] = 1.0f;
        g_BlockingRowsInside.fetch_sub(1);
    }
    TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {};
    }
};

TEST(TerrainHeightFunctionInstances, ARevocationWaitsForTheRowInFlightThenDestroysTheFunction)
{
    TerrainHeightFunctionInstances instances(nullptr);
    const auto function =
        instances.Request({"Blocking"_sid, 1u, {}}, RegistrationOf(&MakeTerrainHeightFunction<Blocking>, "Blocking", 2u));
    ASSERT_NE(function, nullptr);

    bool rowFilled = false;
    std::thread fill([&] {
        const float32 x[2] = {0.0f, 1.0f};
        float32 out[2] = {};
        rowFilled = function->HeightRow(0.0f, x, 0.0f, out) && out[0] == 1.0f && out[1] == 1.0f;
    });
    while (!g_BlockingEntered.load())
        std::this_thread::yield();

    std::thread revoke([&] { instances.RevokeModuleGenerations("Blocking", 2u, 3u); });
    // A new call answers "revoked" once the flag is set, while the first row is still in flight.
    while (function->HeightRange(0.0f, 0.0f, 1.0f, 1.0f, 0.0f).has_value())
        std::this_thread::yield();
    EXPECT_EQ(g_BlockingDestroyed.load(), 0) << "the function was destroyed under a row in flight";

    g_BlockingRelease.store(true);
    fill.join();
    revoke.join();
    EXPECT_TRUE(rowFilled);
    EXPECT_EQ(g_BlockingDestroyed.load(), 1);
    EXPECT_FALSE(g_BlockingDestroyedUnderRow.load()) << "the function was destroyed while a row ran in it";
    const float32 x[1] = {0.0f};
    float32 out[1] = {};
    EXPECT_FALSE(function->HeightRow(0.0f, x, 0.0f, out));
}

} // namespace
