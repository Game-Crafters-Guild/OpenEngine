// The level rule of a planet's height pages as pure oracles: the horizon against rays from the
// camera, and the requester's counts against the model of design page-streaming-planets 3.8.

#include "TerrainECS/Planet/PlanetPageRequester.h"

#include "CBTTerrain/CBTPlanetShading.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/TerrainSurface/TerrainSurfacePages.h"

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
// The surface slots design 3.8 counts at: 2,048 pin L11 up at P600 and L15 up at Earth.
constexpr uint32 kSurfaceSlots = 2048u;

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
    lattice.HeightRow = [relief](std::span<const Mathematics::Vector3> directions, float32 minResolvedArc,
                                 std::span<float32> outHeights) {
        CBTTerrain::PlanetReliefRow(directions, relief.Amplitude, relief.Frequency, relief.Octaves, minResolvedArc,
                                    outHeights);
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

// A camera that looks every way: the geometry's request.
PageRequestCamera DiscCamera(const Mathematics::Vector3& position)
{
    return PageRequestCamera{position, {}, 0.0f, false};
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

// The editor's scene view in the live readback: 3,067 x 893 pixels.
constexpr float32 kEditorViewAspect = 3067.0f / 893.0f;

// The driver's surface view: 1,080 rows, a 60 degree vertical field, one texel a pixel.
PageLevelView SurfaceView()
{
    return MakePageLevelView(kPageLevelReferenceRows, std::numbers::pi_v<float32> / 3.0f, kSurfacePageTargetPixels);
}

// The P-600 scene's planet over a surface field of `slots` slots.
PlanetPageRequestTerrain P600(uint32 slots, const Components::TerrainPlanetRelief& relief)
{
    PlanetPageRequestTerrain planet = Planet(600000.0, 0.5f, relief);
    planet.CacheSlots = slots;
    return planet;
}

// The P-600 scene's landing eye: 1.7 m over the relief at the +Y pole, looking along +Z, in a view of
// `aspect` columns a row.
PageRequestCamera P600LandingEye(float32 aspect, const Components::TerrainPlanetRelief& relief)
{
    const Mathematics::Vector3 up(0, 1, 0);
    const float32 halfFov = std::atan(aspect * std::tan(std::numbers::pi_v<float32> / 6.0f));
    return PageRequestCamera{At(up, 600000.0 + ReliefAt(up, relief) + 1.7), Mathematics::Vector3(0, 0, 1), halfFov,
                             true};
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

TEST(PlanetPageRequester, TheLargestPlanetRadiusIsTheLastWholeMeterThePageStoreHolds)
{
    const uint64 largest = LargestPlanetRadius(PageStreaming::kPageStoreMaxSamplesPerAxis, 0.5f);
    ASSERT_GT(largest, 600000u);
    EXPECT_NE(PlanetFaceSamples(static_cast<float64>(largest), 0.5f), 0u);
    EXPECT_EQ(PlanetFaceSamples(static_cast<float64>(largest + 1u), 0.5f), 0u) << "one meter past the largest";
}

TEST(PlanetPageRequester, TheHorizonNeverDropsAPageARayFromTheCameraReaches)
{
    // P-2's relief on a 2 km planet, a fine view, so the pages past the floor sphere's horizon need
    // levels finer than the pinned ones. A ray reaches a point of the full relief.
    const Components::TerrainPlanetRelief relief{50.0f, 6.0f, 4u};
    const PlanetPageRequestTerrain planet = Planet(2000.0, 0.5f, relief);
    PlanetPageRequester requester;
    requester.Configure(planet, PageRequestArea::Disc);
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
            const PageRequestCamera camera =
                DiscCamera(At(direction, planet.ReferenceRadius + ReliefAt(direction, relief) + altitude));
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
                        if (!RayClearsFloor(camera.Position, point, floor))
                            continue;
                        const float32 distance = (point - camera.Position).Length();
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
        requester.Configure(planet, PageRequestArea::Disc);
        const PageRequestCamera camera =
            DiscCamera(At(pose.Direction, pose.Radius + ReliefAt(pose.Direction, relief) + pose.Altitude));
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

TEST(PlanetPageRequester, TheShadingWedgeRequestsWithinAQuarterOfTheModelAtTheEyes)
{
    // The shading wedge at the note's view (900 rows, a 60 degree vertical field, 3,067 columns, one
    // level-0 texel a pixel), the pinned pages apart; the driver ships 1080 rows at kSurfacePageTargetPixels.
    // Model, per unpinned level (2,048 slots pin L11 up at P600, L15 up at Earth): a page is wanted within
    // r = 2^(L+1) x focal x texel of the eye (the level rule), r capped at the horizon over the floor
    // sphere (radius - 1.5 x amplitude) from the eye and from the envelope's top. In page sides p
    // (128 x 2^L texels, the equal-angle cell's area at the eye), the wedge of angle w (the horizontal
    // field plus twice the margin) holds w/2 (r/p)^2 + (2 + w)(2/pi)(r/p) + 1 pages, and the two-page
    // foot outside it 4 pi (1 - w/(2 pi)). Design is 3.8's figure, kept as a record.
    struct Pose
    {
        const char* Name;
        float64 Radius;
        Mathematics::Vector3 Direction;
        uint32 Model;
        uint32 Design;
    };
    const Pose poses[] = {
        {"P600_eye_center", 600000.0, Unit(1, 0, 0), 2338u, 1883u},
        {"P600_eye_edge", 600000.0, Unit(1, 1, 0), 3180u, 2445u},
        {"P600_eye_corner", 600000.0, Unit(1, 1, 1), 2657u, 2133u},
        {"Earth_eye_center", 6371000.0, Unit(1, 0, 0), 2776u, 2332u},
    };
    constexpr float32 kNoteFocalPixels = 450.0f / 0.57735027f; // 450 / tan(30 degrees)
    const float32 halfFov = std::atan(1533.5f / kNoteFocalPixels);
    const PageLevelView view = MakePageLevelView(900, std::numbers::pi_v<float32> / 3.0f, 1080.0f / 900.0f);
    const Components::TerrainPlanetRelief relief{4000.0f, 6.0f, 4u};
    for (const Pose& pose : poses)
    {
        PlanetPageRequestTerrain planet = Planet(pose.Radius, 0.5f, relief);
        planet.CacheSlots = kSurfaceSlots;
        PlanetPageRequester requester;
        requester.Configure(planet, PageRequestArea::Wedge);
        const Mathematics::Vector3 across = Mathematics::Vector3::Cross(pose.Direction, Mathematics::Vector3(0, 1, 0));
        const PageRequestCamera camera{At(pose.Direction, pose.Radius + ReliefAt(pose.Direction, relief) + 1.7),
                                       across.Normalize(), halfFov, true};
        std::vector<PageStreaming::PageWant> wants;
        requester.Build(std::span(&camera, 1), view, wants);
        const uint32 count = Unpinned(wants);
        RecordProperty(std::string(pose.Name) + "_requested", static_cast<int>(count));
        RecordProperty(std::string(pose.Name) + "_model", static_cast<int>(pose.Model));
        RecordProperty(std::string(pose.Name) + "_design38", static_cast<int>(pose.Design));
        EXPECT_LE(std::abs(static_cast<float64>(count) - pose.Model), 0.25 * pose.Model)
            << pose.Name << ": requested " << count << ", model " << pose.Model;
    }
}

TEST(PlanetPageRequester, AWedgePastTheSlotsIsRequestedALevelCoarserEverywhereUntilItFits)
{
    // The P-600 landing wedge in the editor's view passes the Steam Deck's slots: the fewest passes
    // that fit, each doubling the pixel error.
    const Components::TerrainPlanetRelief relief{4000.0f, 6.0f, 4u};
    const uint32 slots = kSteamDeckSurfacePages.Slots;
    PlanetPageRequester requester;
    requester.Configure(P600(slots, relief), PageRequestArea::Wedge);
    const PageRequestCamera camera = P600LandingEye(kEditorViewAspect, relief);
    std::vector<PageStreaming::PageWant> wants;
    const PageSlotFit fit = requester.BuildWithinSlots(std::span(&camera, 1), SurfaceView(), slots, wants);
    const uint32 passes = fit.Passes;
    RecordProperty("passes", static_cast<int>(passes));
    RecordProperty("requested", static_cast<int>(wants.size()));
    ASSERT_GE(passes, 1u);
    EXPECT_LE(wants.size(), slots);
    std::vector<PageStreaming::PageWant> fullDetail;
    requester.Build(std::span(&camera, 1), SurfaceView(), fullDetail);
    EXPECT_EQ(fit.FullDetailPages, fullDetail.size()) << "the pages the view wants before the fit";
    PageLevelView fewer = SurfaceView();
    fewer.TargetPixels *= static_cast<float32>(1u << (passes - 1u));
    requester.Build(std::span(&camera, 1), fewer, wants);
    EXPECT_GT(wants.size(), slots) << "one pass fewer fits too";
}

TEST(PlanetPageRequester, TheDesktopSurfaceSlotsHoldTheP600LandingWedge)
{
    // A 16:9 view at one texel a pixel fits whole; the editor's wider view takes at most one pass.
    const Components::TerrainPlanetRelief relief{4000.0f, 6.0f, 4u};
    const uint32 slots = DesktopSurfacePageProfile().PlanetSlots;
    PlanetPageRequester requester;
    requester.Configure(P600(slots, relief), PageRequestArea::Wedge);
    const PageRequestCamera wide = P600LandingEye(16.0f / 9.0f, relief);
    std::vector<PageStreaming::PageWant> wants;
    requester.Build(std::span(&wide, 1), SurfaceView(), wants);
    RecordProperty("slots", static_cast<int>(slots));
    RecordProperty("requested_16x9", static_cast<int>(wants.size()));
    EXPECT_LE(wants.size(), slots);
    const PageRequestCamera editor = P600LandingEye(kEditorViewAspect, relief);
    const uint32 passes = requester.BuildWithinSlots(std::span(&editor, 1), SurfaceView(), slots, wants).Passes;
    RecordProperty("requested_editor", static_cast<int>(wants.size()));
    RecordProperty("passes_editor", static_cast<int>(passes));
    EXPECT_LE(passes, 1u);
}

TEST(PlanetPageRequester, ARequesterThatWalkedElsewhereRequestsWhatAFreshOneRequests)
{
    // The page shapes a requester keeps from earlier frames give the request a fresh one gives.
    const Components::TerrainPlanetRelief relief{4000.0f, 6.0f, 4u};
    const PlanetPageRequestTerrain planet = Planet(600000.0, 0.5f, relief);
    PlanetPageRequester walked;
    walked.Configure(planet, PageRequestArea::Disc);
    std::vector<PageStreaming::PageWant> wants;
    for (const Mathematics::Vector3& direction : {Unit(1, 0, 0), Unit(1, 1, 0), Unit(1, 1, 1), Unit(1, 0, 0)})
    {
        const PageRequestCamera camera =
            DiscCamera(At(direction, planet.ReferenceRadius + ReliefAt(direction, relief) + 1.7));
        walked.Build(std::span(&camera, 1), ModelView(), wants);
        PlanetPageRequester fresh;
        fresh.Configure(planet, PageRequestArea::Disc);
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
    const PageRequestCamera camera =
        DiscCamera(At(Unit(1, 1, 0), planet.ReferenceRadius + ReliefAt(Unit(1, 1, 0), relief) + 1.7));
    PlanetPageRequester reconfigured;
    reconfigured.Configure(Planet(600000.0, 0.5f, Components::TerrainPlanetRelief{1000.0f, 6.0f, 4u}), PageRequestArea::Disc);
    std::vector<PageStreaming::PageWant> wants;
    reconfigured.Build(std::span(&camera, 1), ModelView(), wants);
    reconfigured.Configure(planet, PageRequestArea::Disc);
    reconfigured.Build(std::span(&camera, 1), ModelView(), wants);
    PlanetPageRequester fresh;
    fresh.Configure(planet, PageRequestArea::Disc);
    std::vector<PageStreaming::PageWant> expected;
    fresh.Build(std::span(&camera, 1), ModelView(), expected);
    ASSERT_EQ(wants.size(), expected.size());
    for (std::size_t i = 0; i < wants.size(); ++i)
    {
        EXPECT_EQ(wants[i].Address, expected[i].Address);
        EXPECT_EQ(wants[i].Distance, expected[i].Distance);
    }
}
