#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "Mathematics/Vector3.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

using namespace GameEngine;
using Mathematics::Vector3;

namespace {

Vector3 PositionOf(const Vertex& vertex) {
    return {vertex.Position[0], vertex.Position[1], vertex.Position[2]};
}

Vertex MakeVertex(const Vector3& position, const Vector3& normal) {
    Vertex vertex{};
    for (int axis = 0; axis < 3; ++axis) {
        vertex.Position[axis] = position[static_cast<size_t>(axis)];
        vertex.Normal[axis] = normal[static_cast<size_t>(axis)];
    }
    return vertex;
}

// One flat-shaded triangle with its own three wedges. The winding is chosen so
// the geometric normal points to the same side as `outward`.
void AddFlatTriangle(Mesh& mesh, Vector3 a, Vector3 b, Vector3 c, const Vector3& outward) {
    Vector3 normal = Vector3::Cross(b - a, c - a).Normalize();
    if (Vector3::Dot(normal, outward) < 0.0f) {
        std::swap(b, c);
        normal = normal * -1.0f;
    }
    const uint32 base = static_cast<uint32>(mesh.Vertices.size());
    mesh.Vertices.push_back(MakeVertex(a, normal));
    mesh.Vertices.push_back(MakeVertex(b, normal));
    mesh.Vertices.push_back(MakeVertex(c, normal));
    mesh.Indices.insert(mesh.Indices.end(), {base, base + 1u, base + 2u});
}

void AddFlatQuad(Mesh& mesh, const Vector3& a, const Vector3& b, const Vector3& c,
                 const Vector3& d, const Vector3& outward) {
    AddFlatTriangle(mesh, a, b, c, outward);
    AddFlatTriangle(mesh, a, c, d, outward);
}

// A row of tapered tubes (flat-shaded staves, wider at the foot than at the
// top rim, a floor disc on the foot ring), each closed by a separate
// single-sided cap disc set just inside the rim: the shape of a lidded barrel.
// The cap shares no vertex with the wall, so its rim is an open border off
// every AABB face plane, and the floor and the wider foot cover the cap in
// every axis view, so losing the cap changes no axis-view outline.
struct CappedTubeRow {
    static constexpr uint32 kTubeCount = 4;
    static constexpr uint32 kSides = 8;
    static constexpr float kSpacing = 0.74f;
    static constexpr float kFootRadius = 0.3f;
    static constexpr float kRimRadius = 0.27f;
    static constexpr float kHeight = 0.92f;
    static constexpr float kCapHeight = 0.875f;
    static constexpr float kCapRadius = 0.26f;

    static float CenterX(uint32 tube) {
        return (static_cast<float>(tube) - 0.5f * static_cast<float>(kTubeCount - 1)) * kSpacing;
    }
};

Vector3 RingPoint(float centerX, float radius, float height, uint32 side) {
    const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(side) /
                        static_cast<float>(CappedTubeRow::kSides);
    return {centerX + radius * std::cos(angle), height, radius * std::sin(angle)};
}

// Single-sided up-facing fan disc on one ring of the row.
void AddDisc(Mesh& mesh, float centerX, float radius, float height) {
    const Vector3 up(0.0f, 1.0f, 0.0f);
    const Vector3 first = RingPoint(centerX, radius, height, 0u);
    for (uint32 side = 1; side + 1 < CappedTubeRow::kSides; ++side)
        AddFlatTriangle(mesh, first, RingPoint(centerX, radius, height, side),
                        RingPoint(centerX, radius, height, side + 1u), up);
}

Mesh MakeCappedTubeRow() {
    using Row = CappedTubeRow;
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    for (uint32 tube = 0; tube < Row::kTubeCount; ++tube) {
        const float centerX = Row::CenterX(tube);
        for (uint32 side = 0; side < Row::kSides; ++side) {
            const uint32 next = (side + 1u) % Row::kSides;
            const Vector3 foot0 = RingPoint(centerX, Row::kFootRadius, 0.0f, side);
            const Vector3 foot1 = RingPoint(centerX, Row::kFootRadius, 0.0f, next);
            const Vector3 middle = (foot0 + foot1) * 0.5f;
            AddFlatQuad(mesh, foot0, foot1, RingPoint(centerX, Row::kRimRadius, Row::kHeight, next),
                        RingPoint(centerX, Row::kRimRadius, Row::kHeight, side),
                        Vector3(middle.x - centerX, 0.0f, middle.z));
        }
        AddDisc(mesh, centerX, Row::kFootRadius, 0.0f);
        AddDisc(mesh, centerX, Row::kCapRadius, Row::kCapHeight);
    }
    return mesh;
}

float TriangleArea(const Vector3& a, const Vector3& b, const Vector3& c) {
    return 0.5f * Vector3::Cross(b - a, c - a).Length();
}

// Up-facing area of `indices` inside one tube's cap footprint, at cap height.
float CapArea(const Mesh& mesh, const Vector<uint32>& indices, uint32 tube) {
    using Row = CappedTubeRow;
    float area = 0.0f;
    for (size_t first = 0; first + 2 < indices.size(); first += 3) {
        const Vector3 a = PositionOf(mesh.Vertices[indices[first]]);
        const Vector3 b = PositionOf(mesh.Vertices[indices[first + 1]]);
        const Vector3 c = PositionOf(mesh.Vertices[indices[first + 2]]);
        const Vector3 normal = Vector3::Cross(b - a, c - a).NormalizeOrZero();
        const Vector3 centroid = (a + b + c) / 3.0f;
        const float dx = centroid.x - Row::CenterX(tube);
        if (normal.y < 0.9f || std::abs(centroid.y - Row::kCapHeight) > 0.01f ||
            dx * dx + centroid.z * centroid.z > Row::kCapRadius * Row::kCapRadius)
            continue;
        area += TriangleArea(a, b, c);
    }
    return area;
}

float MaxExtent(const Mesh& mesh) {
    Vector3 low = PositionOf(mesh.Vertices[0]);
    Vector3 high = low;
    for (const Vertex& vertex : mesh.Vertices) {
        for (size_t axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], vertex.Position[axis]);
            high[axis] = std::max(high[axis], vertex.Position[axis]);
        }
    }
    return std::max({high.x - low.x, high.y - low.y, high.z - low.z});
}

// Vertices AddClosedBox appends: six flat faces of two triangles, three wedges
// each.
constexpr uint32 kClosedBoxVertexCount = 36;

// A closed flat-shaded axis-aligned box: its faces meet at welded positions,
// so it has no open border.
void AddClosedBox(Mesh& mesh, const Vector3& low, const Vector3& high) {
    const Vector3 center = (low + high) * 0.5f;
    std::array<Vector3, 8> corners;
    for (uint32 corner = 0; corner < 8u; ++corner) {
        corners[corner] = Vector3((corner & 1u) != 0u ? high.x : low.x,
                                  (corner & 2u) != 0u ? high.y : low.y,
                                  (corner & 4u) != 0u ? high.z : low.z);
    }
    constexpr std::array<std::array<uint32, 4>, 6> kBoxFaces = {{
        {0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}}};
    for (const std::array<uint32, 4>& face : kBoxFaces) {
        const Vector3 faceCenter =
            (corners[face[0]] + corners[face[1]] + corners[face[2]] + corners[face[3]]) * 0.25f;
        AddFlatQuad(mesh, corners[face[0]], corners[face[1]], corners[face[2]], corners[face[3]],
                    faceCenter - center);
    }
}

// A stack of separate flat cards, spaced by a gap that is a large fraction of
// a card (foliage cards in a bush), inside a closed box that stands for the
// rest of the mesh and keeps the cards off every AABB face plane. Each card is
// a strip of collinear cells, so a level can merge its cells at no position
// error, and every vertex of a card is on its open border.
struct CardStack {
    static constexpr uint32 kCardCount = 10;
    static constexpr uint32 kCells = 4;
    static constexpr float kCardLength = 0.4f;
    static constexpr float kCardWidth = 0.1f;
    static constexpr float kGap = 0.025f; // 12.5% of the card's size, the root of its area
    static constexpr float kBoxHalfSize = 1.5f;
};

Mesh MakeCardStack() {
    using Stack = CardStack;
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const Vector3 up(0.0f, 1.0f, 0.0f);
    for (uint32 card = 0; card < Stack::kCardCount; ++card) {
        const float height = static_cast<float>(card) * Stack::kGap;
        const uint32 base = static_cast<uint32>(mesh.Vertices.size());
        for (uint32 column = 0; column <= Stack::kCells; ++column) {
            const float x = Stack::kCardLength *
                            (static_cast<float>(column) / static_cast<float>(Stack::kCells) - 0.5f);
            mesh.Vertices.push_back(MakeVertex({x, height, -0.5f * Stack::kCardWidth}, up));
            mesh.Vertices.push_back(MakeVertex({x, height, 0.5f * Stack::kCardWidth}, up));
        }
        for (uint32 cell = 0; cell < Stack::kCells; ++cell) {
            const uint32 near0 = base + 2u * cell;
            mesh.Indices.insert(mesh.Indices.end(),
                                {near0, near0 + 1u, near0 + 3u, near0, near0 + 3u, near0 + 2u});
        }
    }
    const Vector3 half(Stack::kBoxHalfSize, Stack::kBoxHalfSize, Stack::kBoxHalfSize);
    AddClosedBox(mesh, half * -1.0f, half);
    return mesh;
}

// A deck of planks resting across five beams, inside a closed box that keeps
// every part off the AABB face planes. Each plank is a flat strip whose border
// vertices all sit on the first, third and fifth beams; its border edges cross
// the second and fourth beams at their midpoints and span open air between.
struct PlankDeck {
    static constexpr uint32 kPlankCount = 12;
    static constexpr uint32 kBeamCount = 5;
    static constexpr float kBeamSpacing = 1.0f;
    static constexpr float kBeamHalfWidth = 0.05f;
    static constexpr float kBeamTop = 0.0f;
    static constexpr float kBeamDepth = 0.2f;
    static constexpr float kBeamOverhang = 0.1f;
    static constexpr float kPlankWidth = 0.2f;
    static constexpr float kPlankSpacing = 0.25f;
    static constexpr float kBoxHalfSize = 4.0f;
};

Mesh MakePlankDeck() {
    using Deck = PlankDeck;
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const float deckDepth = static_cast<float>(Deck::kPlankCount) * Deck::kPlankSpacing;
    for (uint32 beam = 0; beam < Deck::kBeamCount; ++beam) {
        const float x = static_cast<float>(beam) * Deck::kBeamSpacing;
        AddClosedBox(mesh,
                     {x - Deck::kBeamHalfWidth, Deck::kBeamTop - Deck::kBeamDepth,
                      -Deck::kBeamOverhang},
                     {x + Deck::kBeamHalfWidth, Deck::kBeamTop, deckDepth + Deck::kBeamOverhang});
    }
    const Vector3 up(0.0f, 1.0f, 0.0f);
    for (uint32 plank = 0; plank < Deck::kPlankCount; ++plank) {
        const float nearZ = static_cast<float>(plank) * Deck::kPlankSpacing;
        const uint32 base = static_cast<uint32>(mesh.Vertices.size());
        for (uint32 column = 0; column < 3u; ++column) {
            const float x = static_cast<float>(2u * column) * Deck::kBeamSpacing;
            mesh.Vertices.push_back(MakeVertex({x, Deck::kBeamTop, nearZ}, up));
            mesh.Vertices.push_back(MakeVertex({x, Deck::kBeamTop, nearZ + Deck::kPlankWidth}, up));
        }
        for (uint32 cell = 0; cell < 2u; ++cell) {
            const uint32 near0 = base + 2u * cell;
            mesh.Indices.insert(mesh.Indices.end(),
                                {near0, near0 + 1u, near0 + 3u, near0, near0 + 3u, near0 + 2u});
        }
    }
    const Vector3 boxCenter(2.0f, 0.0f, 0.5f * deckDepth);
    const Vector3 half(Deck::kBoxHalfSize, Deck::kBoxHalfSize, Deck::kBoxHalfSize);
    AddClosedBox(mesh, boxCenter - half, boxCenter + half);
    return mesh;
}

// Triangles of `indices` whose corners are all plank vertices: the beams come
// first in the deck's vertex list, one closed box each, then six per plank.
uint32 PlankTriangleCount(const Vector<uint32>& indices) {
    constexpr uint32 kFirstPlankVertex = PlankDeck::kBeamCount * kClosedBoxVertexCount;
    constexpr uint32 kEndPlankVertex = kFirstPlankVertex + PlankDeck::kPlankCount * 6u;
    uint32 count = 0;
    for (size_t first = 0; first + 2 < indices.size(); first += 3) {
        bool onPlank = true;
        for (size_t corner = 0; corner < 3; ++corner)
            onPlank = onPlank && indices[first + corner] >= kFirstPlankVertex &&
                      indices[first + corner] < kEndPlankVertex;
        if (onPlank)
            ++count;
    }
    return count;
}

} // namespace

// A generated level selects where its projected error fits the pixel budget,
// so a level that opens a cap may only carry an error at least the cap's own
// size: then the cap is under the budget wherever the open level draws.
TEST(MeshLODVisibleError, CapInsetInATubeOpensOnlyAtALevelPricedAtTheCapSize) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh mesh = MakeCappedTubeRow();
    const MeshLODs lods = GenerateMeshLODs(mesh, {});
    ASSERT_GT(lods.LodCount(), 1u) << "the tube row must emit a quality level";
    const float maxExtent = MaxExtent(mesh);
    const float capDiameter = 2.0f * CappedTubeRow::kCapRadius;
    for (uint32 level = 1; level < lods.LodCount(); ++level) {
        if (lods.LodSloppy[level] != 0u)
            continue;
        uint32 openCaps = 0;
        for (uint32 tube = 0; tube < CappedTubeRow::kTubeCount; ++tube) {
            const float lod0Area = CapArea(mesh, lods.LodIndices[0], tube);
            if (CapArea(mesh, lods.LodIndices[level], tube) < 0.95f * lod0Area)
                ++openCaps;
        }
        if (openCaps == 0u)
            continue;
        EXPECT_GE(lods.LodErrors[level] * maxExtent, capDiameter)
            << "level " << level << " opens " << openCaps << " of " << CappedTubeRow::kTubeCount
            << " caps at an error of " << lods.LodErrors[level] * maxExtent << " m";
    }
}

// Cards that only lie near one another are not closed against each other, so
// their borders stay free and the mesh keeps its quality levels.
TEST(MeshLODVisibleError, CardsNearOneAnotherKeepTheirLevels) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh mesh = MakeCardStack();
    const MeshLODs lods = GenerateMeshLODs(mesh, {});
    ASSERT_GT(lods.LodCount(), 1u)
        << "cards " << CardStack::kGap << " m apart must still simplify";
    EXPECT_EQ(lods.LodSloppy[1], 0u);
    EXPECT_LE(lods.TriangleCount(1), lods.TriangleCount(0) / 2u);
}

// A plank resting across beams touches them only where it crosses them, so its
// border stays free and its collinear border vertices merge.
TEST(MeshLODVisibleError, PlankRestingAcrossBeamsKeepsItsBorderFree) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh mesh = MakePlankDeck();
    const MeshLODs lods = GenerateMeshLODs(mesh, {});
    ASSERT_GT(lods.LodCount(), 1u) << "the deck must emit a quality level";
    EXPECT_LT(PlankTriangleCount(lods.LodIndices[1]), PlankTriangleCount(lods.LodIndices[0]));
}
