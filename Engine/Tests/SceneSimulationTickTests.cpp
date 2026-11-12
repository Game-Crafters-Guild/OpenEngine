// A save with SceneWriteOptions::writeSimulationTick writes the world's simulation tick in the
// [scene] header, and a load that clears the world restores it. An additive load keeps the tick
// of the world it loads into, and a save without the option writes no tick.

#include "ECS/Entity.h"
#include "ECS/SimulationClock.h"
#include "Scene/SceneIO.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace GameEngine;

namespace
{

constexpr uint64 kSavedTick = 123456789012ull;

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Runs one fixed step on the world's clock.
void StepOnce(ECS::World& world)
{
    ECS::SimulationClock& clock = world.GetSimulationClock();
    clock.Advance(clock.GetFixedDeltaSeconds());
    ASSERT_TRUE(clock.BeginStep());
    EXPECT_FALSE(clock.BeginStep());
}

class SceneSimulationTick : public testing::Test
{
protected:
    TestUtils::ScopedTempDir m_Directory{TestUtils::MakeUniqueTempDirectory("SceneSimulationTick")};

    std::filesystem::path SaveWorldAt(uint64 tick, bool writeTick)
    {
        ECS::World source;
        source.CreateEntity();
        source.GetSimulationClock().Restore(tick);
        const std::filesystem::path path = m_Directory.Path() / "saved.scene";
        Scene::SceneWriteOptions options;
        options.writeSimulationTick = writeTick;
        EXPECT_TRUE(Scene::WriteSceneFile(source, path, options));
        return path;
    }
};

} // namespace

TEST_F(SceneSimulationTick, LoadIntoAFreshWorldContinuesFromTheSavedTick)
{
    const std::filesystem::path path = SaveWorldAt(kSavedTick, true);
    EXPECT_NE(ReadText(path).find("simulation_tick=" + std::to_string(kSavedTick)), std::string::npos);

    ECS::World loaded;
    const Scene::SceneInstantiateResult result = Scene::ReadSceneInto(loaded, path);
    ASSERT_TRUE(result) << result.Error->message;
    EXPECT_EQ(loaded.GetSimulationClock().GetTick(), kSavedTick);
    StepOnce(loaded);
    EXPECT_EQ(loaded.GetSimulationClock().GetTick(), kSavedTick + 1);
}

TEST_F(SceneSimulationTick, AdditiveLoadKeepsTheTickOfTheWorld)
{
    const std::filesystem::path path = SaveWorldAt(kSavedTick, true);

    ECS::World target;
    StepOnce(target);
    Scene::SceneInstantiateOptions additive;
    additive.ClearWorldFirst = false;
    const Scene::SceneInstantiateResult result = Scene::ReadSceneInto(target, path, {}, additive);
    ASSERT_TRUE(result) << result.Error->message;
    EXPECT_EQ(target.GetSimulationClock().GetTick(), 1u);
}

TEST_F(SceneSimulationTick, SaveWithoutTheOptionWritesNoTick)
{
    const std::filesystem::path path = SaveWorldAt(kSavedTick, false);
    EXPECT_EQ(ReadText(path).find("simulation_tick"), std::string::npos);
}

TEST_F(SceneSimulationTick, LoadRefusesATickThatIsNotAWholeNumber)
{
    const std::filesystem::path path = m_Directory.Path() / "malformed.scene";
    std::ofstream(path, std::ios::binary) << "[scene name=\"malformed\" version=1 simulation_tick=-1]\n";

    ECS::World loaded;
    const Scene::SceneInstantiateResult result = Scene::ReadSceneInto(loaded, path);
    ASSERT_FALSE(result);
    EXPECT_NE(result.Error->message.find("simulation_tick"), std::string::npos);
}
