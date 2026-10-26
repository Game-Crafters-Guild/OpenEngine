#include "SplineGeometry/AreaBoolean.h"

#include "Mathematics/Geometry.h"

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;

namespace
{

// Grid steps per meter of the plain boolean: a millimeter.
constexpr double kBooleanScale = 1.0e3;
// Grid steps per meter of the touching boolean: a hundredth of a millimeter, so an offset of half
// the touching tolerance spans many steps.
constexpr double kTouchingScale = 1.0e5;
// Each closing (out, then in) and opening (in, then out) offsets out and back by the same distance.
// The first offset squares off a corner sharper than its miter limit of 2 (in multiples of the
// offset), and the second offset brings that corner back to its exact point. The second offset's
// limit of 1,000 only has to pass the sharpest corner (about 0.1 degree), so it squares none.
constexpr double kFirstOffsetMiterLimit = 2.0;
constexpr double kSecondOffsetMiterLimit = 1.0e3;

// `ring` on a grid of `scale` steps per meter, wound counter-clockwise, so the nonzero fill adds
// every operand whichever way it was authored.
Clipper2Lib::Path64 CounterClockwisePath(std::span<const Vector2> ring, double scale)
{
    Clipper2Lib::Path64 path;
    path.reserve(ring.size());
    for (const Vector2& point : ring)
        path.emplace_back(std::llround(point.x * scale), std::llround(point.y * scale));
    if (Mathematics::PolygonDoubledSignedArea(ring) < 0.0f)
        std::reverse(path.begin(), path.end());
    return path;
}

Clipper2Lib::Paths64 CounterClockwisePaths(std::span<const AreaRing> rings, double scale)
{
    Clipper2Lib::Paths64 paths;
    paths.reserve(rings.size());
    for (const AreaRing& ring : rings)
        paths.push_back(CounterClockwisePath(ring, scale));
    return paths;
}

// `path` back in meters as a ring wound counter-clockwise (`outer`) or clockwise.
AreaRing RingOf(const Clipper2Lib::Path64& path, double scale, bool outer)
{
    AreaRing ring;
    ring.reserve(path.size());
    for (const Clipper2Lib::Point64& point : path)
        ring.emplace_back(static_cast<float32>(static_cast<double>(point.x) / scale),
                          static_cast<float32>(static_cast<double>(point.y) / scale));
    if ((Mathematics::PolygonDoubledSignedArea(ring) > 0.0f) != outer)
        std::reverse(ring.begin(), ring.end());
    return ring;
}

// The outer rings under `node` as pieces with their holes, and the islands inside those holes.
void CollectPieces(const Clipper2Lib::PolyPath64& node, double scale, std::vector<AreaPiece>& pieces)
{
    for (const auto& outer : node)
    {
        AreaPiece& piece = pieces.emplace_back();
        piece.Outer = RingOf(outer->Polygon(), scale, true);
        for (const auto& hole : *outer)
            piece.Holes.push_back(RingOf(hole->Polygon(), scale, false));
        for (const auto& hole : *outer)
            CollectPieces(*hole, scale, pieces);
    }
}

Clipper2Lib::Paths64 Offset(const Clipper2Lib::Paths64& paths, double delta, double miterLimit)
{
    return Clipper2Lib::InflatePaths(paths, delta, Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon,
                                     miterLimit);
}

// Twice the area of `path` over its perimeter, in grid steps: its width on average.
double MeanWidth(const Clipper2Lib::Path64& path)
{
    double perimeter = 0.0;
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        const Clipper2Lib::Point64& a = path[i];
        const Clipper2Lib::Point64& b = path[(i + 1) % path.size()];
        perimeter += std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y));
    }
    return perimeter > 0.0 ? 2.0 * std::abs(Clipper2Lib::Area(path)) / perimeter : 0.0;
}

// `paths` (outer rings counter-clockwise, holes clockwise) without the holes narrower than
// `minWidth` grid steps on average, so those are filled.
Clipper2Lib::Paths64 WithoutSliverHoles(const Clipper2Lib::Paths64& paths, double minWidth)
{
    Clipper2Lib::Paths64 kept;
    kept.reserve(paths.size());
    for (const Clipper2Lib::Path64& path : paths)
    {
        if (Clipper2Lib::Area(path) < 0.0 && MeanWidth(path) < minWidth)
            continue;
        kept.push_back(path);
    }
    return kept;
}

} // namespace

std::vector<AreaPiece> CombineAreas(std::span<const AreaRing> unions, std::span<const AreaRing> cuts)
{
    Clipper2Lib::Clipper64 clipper;
    clipper.AddSubject(CounterClockwisePaths(unions, kBooleanScale));
    clipper.AddClip(CounterClockwisePaths(cuts, kBooleanScale));
    Clipper2Lib::PolyTree64 tree;
    std::vector<AreaPiece> pieces;
    if (!clipper.Execute(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero, tree))
        return pieces;
    CollectPieces(tree, kBooleanScale, pieces);
    return pieces;
}

std::vector<AreaPiece> CombineTouchingAreas(std::span<const AreaRing> unions, std::span<const AreaRing> cuts,
                                            float32 touchMeters, float32 sliverMeters)
{
    // A gap narrower than twice the offset closes and a strip narrower than twice it opens, so
    // half the tolerance welds exactly what touches.
    const double offset = 0.5 * static_cast<double>(touchMeters) * kTouchingScale;
    const Clipper2Lib::Paths64 joined =
        Clipper2Lib::Union(CounterClockwisePaths(unions, kTouchingScale), Clipper2Lib::FillRule::NonZero);
    const Clipper2Lib::Paths64 closed =
        WithoutSliverHoles(Offset(Offset(joined, offset, kFirstOffsetMiterLimit), -offset, kSecondOffsetMiterLimit),
                           static_cast<double>(sliverMeters) * kTouchingScale);
    const Clipper2Lib::Paths64 paved =
        Clipper2Lib::Difference(closed, CounterClockwisePaths(cuts, kTouchingScale), Clipper2Lib::FillRule::NonZero);
    Clipper2Lib::ClipperOffset opening(kSecondOffsetMiterLimit);
    opening.AddPaths(Offset(paved, -offset, kFirstOffsetMiterLimit), Clipper2Lib::JoinType::Miter,
                     Clipper2Lib::EndType::Polygon);
    Clipper2Lib::PolyTree64 tree;
    opening.Execute(offset, tree);
    std::vector<AreaPiece> pieces;
    CollectPieces(tree, kTouchingScale, pieces);
    return pieces;
}

} // namespace GameEngine::SplineGeometry
