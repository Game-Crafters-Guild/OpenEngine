// The level rule of a planet's height pages as pure oracles: the horizon against rays from the
// camera, and the requester's counts against the model of design page-streaming-planets 3.8.

#include "TerrainECS/PlanetPageRequester.h"

#include "CBTTerrain/CBTPlanetShading.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::PageStreaming::PageAddress;

namespace
{

constexpr uint32 kDesktopSlots = 512u;

// The model's view: 1080 rows, 60 degree vertical field, the default TargetPixelError of 11.
PageLevelView ModelView()
{
    return MakePageLevelView(1080, std::numbers::pi_v<float32> / 3.0f, 11.0f);
}

// The full relief at a unit direction.
float32 ReliefAt(const Mathematics::Vector3& direction, const Components::TerrainPlanetRelief& relief)
{
    return CBTTerrain::PlanetRelief(direction.x, direction.y, direction.z, relief.Amplitude, relief.Frequency,
                                    relief.Octaves);
}

// A planet of radius `radius` meters at `texel` level-0 meters, generated with `relief`.
PlanetPageRequestTerrain Planet(float64 radius, float32 texel, const Components::TerrainPlanetRelief& relief)
{
    PageStreaming::GeneratedPlanetLattice lattice;
    lattice.ReferenceRadius = radius;
    lattice.FaceSamples = PlanetFaceSamples(radius, texel);
    lattice.Height = [relief](const Mathematics::Vector3& direction, float32 minResolvedArc) {
        return CBTTerrain::PlanetRelief(direction.x, direction.y, direction.z, relief.Amplitude, relief.Frequency,
                                        relief.Octaves, minResolvedArc);
    };
    lattice.HeightRange = [relief](const Mathematics::Vector3& direction, float32 chord, float32 minResolvedArc) {
        const CBTTerrain::PlanetReliefRange range = CBTTerrain::PlanetReliefRangeNear(
            direction.x, direction.y, direction.z, chord, relief.Amplitude, relief.Frequency, relief.Octaves, minResolvedArc);
        return PageStreaming::PageHeightRange{range.Min, range.Max};
    };
    PlanetPageRequestTerrain planet;
    planet.ReferenceRadius = radius;
    planet.FaceSamples = lattice.FaceSamples;
    planet.CacheSlots = kDesktopSlots;
    planet.Heights = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(std::move(lattice));
    return planet;
}

Mathematics::Vector3 Unit(float32 x, float32 y, float32 z)
{
    const float32 length = std::sqrt(x * x + y * y + z * z);
    return Mathematics::Vector3(x / length, y / length, z / length);
}

Mathematics::Vector3 At(const Mathematics::Vector3& direction, float64 radius)
{
    const float32 r = static_cast<float32>(radius);
    return Mathematics::Vector3(direction.x * r, direction.y * r, direction.z * r);
}

std::set<std::tuple<uint8, uint8, uint32, uint32>> Keys(const std::vector<PageStreaming::PageWant>& wants)
{
    std::set<std::tuple<uint8, uint8, uint32, uint32>> keys;
    for (const PageStreaming::PageWant& want : wants)
        keys.emplace(want.Address.Face, want.Address.Level, want.Address.X, want.Address.Z);
    return keys;
}

// The pages a request wants that are not pinned.
uint32 Unpinned(const std::vector<PageStreaming::PageWant>& wants)
{
    return static_cast<uint32>(std::count_if(wants.begin(), wants.end(), [](const auto& want) { return !want.Pinned; }));
}

// The smallest angle between two neighbor level-0 samples of a face: along its edge row, where
// the equal-angle map's spacing is smallest, and along its center row.
float64 SmallestSampleAngle(uint32 samples)
{
    float64 smallest = std::numbers::pi;
    for (uint32 row : {0u, samples / 2u})
        for (uint32 x = 0; x + 1u < samples; ++x)
        {
            const Mathematics::Vector3 a = PageStreaming::EqualAngleSampleDirection(0, x, row, samples);
            const Mathematics::Vector3 b = PageStreaming::EqualAngleSampleDirection(0, x + 1u, row, samples);
            const Mathematics::Vector3 across = Mathematics::Vector3::Cross(a, b);
            smallest = std::min(smallest, std::atan2(static_cast<float64>(across.Length()), static_cast<float64>(Mathematics::Vector3::Dot(a, b))));
        }
    return smallest;
}

// True when the segment from `camera` to `point` stays outside the sphere of radius `floor`.
bool RayClearsFloor(const Mathematics::Vector3& camera, const Mathematics::Vector3& point, float64 floor)
{
    const float64 dx = point.x - camera.x;
    const float64 dy = point.y - camera.y;
    const float64 dz = point.z - camera.z;
    const float64 along = -(camera.x * dx + camera.y * dy + camera.z * dz) / (dx * dx + dy * dy + dz * dz);
    const float64 t = std::clamp(along, 0.0, 1.0);
    const float64 x = camera.x + t * dx;
    const float64 y = camera.y + t * dy;
    const float64 z = camera.z + t * dz;
    return std::sqrt(x * x + y * y + z * z) > floor;
}

} // namespace

TEST(PlanetPageRequester, FaceSamplesAreTheFewestThatHalveToOnePageAtTheTexel)
{
    EXPECT_EQ(PlanetFaceSamples(50000.0, 0.5f), 77u * 2048u + 1u);
    EXPECT_EQ(PlanetFaceSamples(600000.0, 0.5f), 116u * 16384u + 1u);
    EXPECT_EQ(PlanetFaceSamples(2000.0, 0.5f), 99u * 64u + 1u);
    EXPECT_EQ(PlanetFaceSamples(1.0, 0.5f), 5u);
    EXPECT_EQ(PlanetFaceSamples(0.0, 0.5f), 0u);
    EXPECT_EQ(PlanetFaceSamples(1.0e12, 0.5f), 0u) << "past the store's samples per axis";
}

TEST(PlanetPageRequester, TheHorizonNeverDropsAPageARayFromTheCameraReaches)
{
    // P-2's relief on a 2 km planet, a fine view, so the pages past the floor sphere's horizon need
    // levels finer than the pinned ones. A ray reaches a point of the full relief.
    const Components::TerrainPlanetRelief relief{50.0f, 6.0f, 4u};
    const PlanetPageRequestTerrain planet = Planet(2000.0, 0.5f, relief);
    PlanetPageRequester requester;
    requester.Configure(planet);
    const PageLevelView view = MakePageLevelView(1080, std::numbers::pi_v<float32> / 3.0f, 1.0f);
    const uint32 samples = planet.FaceSamples;
    const float64 floor = planet.ReferenceRadius - CBTTerrain::kReliefEnvelope * relief.Amplitude;
    const float32 smallestTexel = static_cast<float32>(SmallestSampleAngle(samples) * floor * 0.999);
    const uint32 pinned = requester.FirstPinnedLevel();
    ASSERT_GT(pinned, 3u);

    uint32 checked = 0;
    for (const Mathematics::Vector3& direction : {Unit(1, 0, 0), Unit(1, 1, 0), Unit(1, 1, 1), Unit(0.3f, -1, 0.6f)})
        for (float64 altitude : {2.0, 80.0, 500.0, 3000.0})
        {
            const Mathematics::Vector3 camera =
                At(direction, planet.ReferenceRadius + ReliefAt(direction, relief) + altitude);
            std::vector<PageStreaming::PageWant> wants;
            requester.Build(std::span(&camera, 1), view, wants);
            const auto keys = Keys(wants);
            const uint32 stride = (samples - 1u) / 64u;
            for (uint8 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
                for (uint32 z = 0; z < samples; z += stride)
                    for (uint32 x = 0; x < samples; x += stride)
                    {
                        const Mathematics::Vector3 unit = PageStreaming::EqualAngleSampleDirection(face, x, z, samples);
                        const Mathematics::Vector3 point = At(unit, planet.ReferenceRadius + ReliefAt(unit, relief));
                        if (!RayClearsFloor(camera, point, floor))
                            continue;
                        const float32 distance = (point - camera).Length();
                        const float32 needed = GeometryPageLevel(distance, smallestTexel, view);
                        const uint32 coarsest = static_cast<uint32>(std::max(std::floor(needed), 0.0f));
                        if (coarsest >= pinned)
                            continue; // a pinned page holds it
                        bool held = false;
                        for (uint32 level = 0; level <= coarsest && !held; ++level)
                        {
                            const PageStreaming::PageStoreLevel& onLevel = requester.Levels()[level];
                            const uint32 span = PageStreaming::kPageOwnedSamples << level;
                            held = keys.count({face, static_cast<uint8>(level), std::min(x / span, onLevel.PagesX - 1u),
                                               std::min(z / span, onLevel.PagesZ - 1u)}) != 0;
                        }
                        ++checked;
                        ASSERT_TRUE(held) << "face " << int(face) << " sample " << x << "," << z << " at "
                                          << distance << " m needs level " << coarsest << " or finer; camera "
                                          << altitude << " m above the ground";
                    }
        }
    EXPECT_GT(checked, 1000u);
}

TEST(PlanetPageRequester, CountsAreTheDesktopRowsAndWithinATenthAboveTheModelAtThePlanetPoses)
{
    // Design 3.8's model on the store's layout: the last-sample pages, and level 0 wherever the rule
    // wants it or finer. The rest of the gap is the per-page height shells. Requested is the desktop
    // count (512 slots); the camera is at its altitude above the ground on a face's center, edge or corner.
    struct Pose
    {
        const char* Name;
        float64 Radius;
        Mathematics::Vector3 Direction;
        float64 Altitude;
        uint32 Requested;
        uint32 Model;
        uint32 Design;
    };
    const Mathematics::Vector3 center = Unit(1, 0, 0);
    const Mathematics::Vector3 edge = Unit(1, 1, 0);
    const Mathematics::Vector3 corner = Unit(1, 1, 1);
    const Pose poses[] = {
        {"P50_eye_center", 50000.0, center, 1.7, 101u, 101u, 100u},
        {"P50_eye_edge", 50000.0, edge, 1.7, 200u, 190u, 154u},
        {"P600_eye_center", 600000.0, center, 1.7, 139u, 136u, 141u},
        {"P600_eye_edge", 600000.0, edge, 1.7, 284u, 272u, 191u},
        {"P600_eye_corner", 600000.0, corner, 1.7, 321u, 306u, 177u},
        {"P600_500m", 600000.0, center, 500.0, 103u, 94u, 108u},
        {"P600_3km", 600000.0, center, 3000.0, 71u, 67u, 74u},
        {"P600_100km", 600000.0, center, 100000.0, 9u, 9u, 22u},
    };
    for (const Pose& pose : poses)
    {
        const Components::TerrainPlanetRelief relief{pose.Radius < 100000.0 ? 1300.0f : 4000.0f, 6.0f, 4u};
        const PlanetPageRequestTerrain planet = Planet(pose.Radius, 0.5f, relief);
        PlanetPageRequester requester;
        requester.Configure(planet);
        const Mathematics::Vector3 camera =
            At(pose.Direction, pose.Radius + ReliefAt(pose.Direction, relief) + pose.Altitude);
        std::vector<PageStreaming::PageWant> wants;
        requester.Build(std::span(&camera, 1), ModelView(), wants);
        ASSERT_EQ(requester.FirstPinnedLevel() + 3u, requester.Levels().size()) << "the model pins three levels";
        const uint32 count = Unpinned(wants);
        RecordProperty(std::string(pose.Name) + "_requested", static_cast<int>(count));
        RecordProperty(std::string(pose.Name) + "_model", static_cast<int>(pose.Model));
        RecordProperty(std::string(pose.Name) + "_design38", static_cast<int>(pose.Design));
        RecordProperty(std::string(pose.Name) + "_pinned", static_cast<int>(wants.size() - count));
        EXPECT_EQ(count, pose.Requested) << pose.Name;
        EXPECT_GE(count, pose.Model) << pose.Name << ": fewer pages than the model needs";
        EXPECT_LE(static_cast<float64>(count), 1.1 * pose.Model) << pose.Name;
    }
}

TEST(PlanetPageRequester, ARequesterThatWalkedElsewhereRequestsWhatAFreshOneRequests)
{
    // The page shapes a requester keeps from earlier frames give the request a fresh one gives.
    const Components::TerrainPlanetRelief relief{4000.0f, 6.0f, 4u};
    const PlanetPageRequestTerrain planet = Planet(600000.0, 0.5f, relief);
    PlanetPageRequester walked;
    walked.Configure(planet);
    std::vector<PageStreaming::PageWant> wants;
    for (const Mathematics::Vector3& direction : {Unit(1, 0, 0), Unit(1, 1, 0), Unit(1, 1, 1), Unit(1, 0, 0)})
    {
        const Mathematics::Vector3 camera = At(direction, planet.ReferenceRadius + ReliefAt(direction, relief) + 1.7);
        walked.Build(std::span(&camera, 1), ModelView(), wants);
        PlanetPageRequester fresh;
        fresh.Configure(planet);
        std::vector<PageStreaming::PageWant> expected;
        fresh.Build(std::span(&camera, 1), ModelView(), expected);
        ASSERT_EQ(wants.size(), expected.size());
        for (std::size_t i = 0; i < wants.size(); ++i)
        {
            EXPECT_EQ(wants[i].Address, expected[i].Address);
            EXPECT_EQ(wants[i].Distance, expected[i].Distance);
            EXPECT_EQ(wants[i].Pinned, expected[i].Pinned);
        }
    }
}

TEST(PlanetPageRequester, AReconfiguredRequesterRequestsWhatAFreshOneRequests)
{
    // Configure drops the page shapes of the earlier relief.
    const Components::TerrainPlanetRelief relief{4000.0f, 6.0f, 4u};
    const PlanetPageRequestTerrain planet = Planet(600000.0, 0.5f, relief);
    const Mathematics::Vector3 camera = At(Unit(1, 1, 0), planet.ReferenceRadius + ReliefAt(Unit(1, 1, 0), relief) + 1.7);
    PlanetPageRequester reconfigured;
    reconfigured.Configure(Planet(600000.0, 0.5f, Components::TerrainPlanetRelief{1000.0f, 6.0f, 4u}));
    std::vector<PageStreaming::PageWant> wants;
    reconfigured.Build(std::span(&camera, 1), ModelView(), wants);
    reconfigured.Configure(planet);
    reconfigured.Build(std::span(&camera, 1), ModelView(), wants);
    PlanetPageRequester fresh;
    fresh.Configure(planet);
    std::vector<PageStreaming::PageWant> expected;
    fresh.Build(std::span(&camera, 1), ModelView(), expected);
    ASSERT_EQ(wants.size(), expected.size());
    for (std::size_t i = 0; i < wants.size(); ++i)
    {
        EXPECT_EQ(wants[i].Address, expected[i].Address);
        EXPECT_EQ(wants[i].Distance, expected[i].Distance);
    }
}
