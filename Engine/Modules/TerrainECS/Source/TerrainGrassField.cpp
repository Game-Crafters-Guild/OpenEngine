#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainRowBands.h"

#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace GameEngine::TerrainECS
{
namespace
{
// Ease the volume's weight across the falloff before a grass target is blended with it.
//
// The shared weight ramps linearly from 0 at the outer edge to 1 at the inner boundary, which is
// right for a height offset: the ground it produces is that ramp. Grass is not that ramp. Measured
// on a 60 m region with a 25 m falloff, visible coverage is an S-curve under either blend, steepest
// in the middle of the band and far shallower at both boundaries than mid-band - the linear blend
// still arrives at the outer edge at roughly 40% of its peak slope, which is the corner this
// softens. That shape is consistent with visible coverage saturating in a dense field: it holds
// near full until height x density has fallen well below 1.
//
// What this changes is the CORNERS. Smoothstep has zero slope at both ends of the weight, so the
// slope jump where a region meets the untouched field is 1.5-2.3x smaller (0.0156 -> 0.0107 per
// metre at the outer edge, 0.0100 -> 0.0044 at the inner one, on a 50%/50% region), and the change
// it takes off the corners reappears mid-band, which is about 1.4x steeper. A corner is what reads
// as a seam; the two blends are otherwise hard to tell apart.
//
// Grass only: the shared ComputeWeight is what every other effect ramps with and is untouched, so
// no terrain without a grass region changes by a texel.
float32 GrassRegionEase(float32 weight)
{
    return weight * weight * (3.0f - 2.0f * weight);
}

// What composing a band of rows did to the field: the change in its counts of nonneutral texels and
// whether any texel changed.
struct ComposedRows
{
    int64 NonNeutralDelta = 0;
    int64 NonNeutralDarkeningDelta = 0;
    bool Changed = false;
};

// What the bands of one composition did in all; the bands add theirs concurrently.
struct ComposedTotals
{
    std::atomic<int64> NonNeutralDelta{0};
    std::atomic<int64> NonNeutralDarkeningDelta{0};
    std::atomic<bool> Changed{false};

    void Add(const ComposedRows& band)
    {
        NonNeutralDelta.fetch_add(band.NonNeutralDelta, std::memory_order_relaxed);
        NonNeutralDarkeningDelta.fetch_add(band.NonNeutralDarkeningDelta, std::memory_order_relaxed);
        if (band.Changed)
            Changed.store(true, std::memory_order_relaxed);
    }
};

// True when a region of the stack can darken the ground. Without one every texel's Darken Ground
// scale stays 1, so a field without a darkening channel needs none.
bool AnyRegionDarkensGround(std::span<const ResolvedModifier> modifiers)
{
    for (const ResolvedModifier& mod : modifiers)
        for (const auto& fx : mod.Effects)
            if (fx.EffectKind == ResolvedEffect::Kind::Grass
                && Components::ClampTerrainGrassEffectScale(fx.Grass.GroundDarkeningScale) < 1)
                return true;
    return false;
}

// Compose rows z0..z1 (inclusive) of columns minX..maxX into the field's allocated pixels, and into its
// darkening channel when that is allocated. Every texel is a pure function of its world position and
// the ordered stack, and a band writes only its own rows, so bands may run concurrently and in any
// order with byte-identical results.
ComposedRows ComposeGrassRows(TerrainGrassField& field, uint32 width, uint32 height, float32 sizeX,
                              float32 sizeZ, float32 originX, float32 originZ,
                              std::span<const ResolvedModifier> modifiers, int32 minX, int32 maxX, int32 z0,
                              int32 z1)
{
    ComposedRows result;
    const bool darkening = !field.GroundDarkening.empty();
    // The modifiers whose Z extent holds the current row, in stack order. A scene can hold hundreds
    // of regions while a row crosses a few, so each texel tests only these for its X extent.
    std::vector<const ResolvedModifier*> rowModifiers;
    rowModifiers.reserve(modifiers.size());
    for (int32 z = z0; z <= z1; ++z)
    {
        const float32 worldZ = originZ + sizeZ * (static_cast<float32>(z) / (height - 1));
        rowModifiers.clear();
        for (const auto& mod : modifiers)
            if (mod.Enabled && !(worldZ < mod.BoundsMinZ || worldZ > mod.BoundsMaxZ)) rowModifiers.push_back(&mod);
        for (int32 x = minX; x <= maxX; ++x)
        {
            const float32 worldX = originX + sizeX * (static_cast<float32>(x) / (width - 1));
            float32 heightScale = 1.0f, densityScale = 1.0f, darkeningScale = 1.0f;
            for (const ResolvedModifier* row : rowModifiers)
            {
                const ResolvedModifier& mod = *row;
                if (worldX < mod.BoundsMinX || worldX > mod.BoundsMaxX) continue;
                const float32 rawWeight = ComputeWeight(mod, worldX, worldZ);
                const float32 linearWeight = std::isfinite(rawWeight) ? Math::Clamp01(rawWeight) : 0.0f;
                if (linearWeight <= 0) continue;
                const float32 weight = GrassRegionEase(linearWeight);
                for (const auto& fx : mod.Effects)
                {
                    if (fx.EffectKind != ResolvedEffect::Kind::Grass) continue;
                    heightScale += (Components::ClampTerrainGrassEffectScale(fx.Grass.HeightScale) - heightScale) * weight;
                    densityScale += (Components::ClampTerrainGrassEffectScale(fx.Grass.DensityScale) - densityScale) * weight;
                    darkeningScale += (Components::ClampTerrainGrassEffectScale(fx.Grass.GroundDarkeningScale)
                                       - darkeningScale) * weight;
                }
            }
            const uint8 h = static_cast<uint8>(std::lround(Math::Clamp01(heightScale) * 255.0f));
            const uint8 d = static_cast<uint8>(std::lround(Math::Clamp01(densityScale) * 255.0f));
            if (darkening)
            {
                const uint8 g = static_cast<uint8>(std::lround(Math::Clamp01(darkeningScale) * 255.0f));
                uint8& stored = field.GroundDarkening[static_cast<size_t>(z) * width + x];
                if (stored != g)
                {
                    if (stored != 255) --result.NonNeutralDarkeningDelta;
                    if (g != 255) ++result.NonNeutralDarkeningDelta;
                    stored = g;
                    result.Changed = true;
                }
            }
            const size_t offset = (static_cast<size_t>(z) * width + x) * kTerrainGrassFieldChannels;
            if (field.Pixels[offset] == h && field.Pixels[offset + 1] == d) continue;
            if (field.Pixels[offset] != 255 || field.Pixels[offset + 1] != 255) --result.NonNeutralDelta;
            if (h != 255 || d != 255) ++result.NonNeutralDelta;
            field.Pixels[offset] = h; field.Pixels[offset + 1] = d;
            result.Changed = true;
        }
    }
    return result;
}
}

void ComposeTerrainGrassField(TerrainGrassField& field, uint32 width, uint32 height,
                              float32 sizeX, float32 sizeZ, float32 originX, float32 originZ,
                              std::span<const ResolvedModifier> modifiers,
                              int32 minX, int32 minZ, int32 maxX, int32 maxZ,
                              JobSystem::WorkStealingThreadPool* jobs)
{
    if (width < 2 || height < 2 || !std::isfinite(sizeX) || !std::isfinite(sizeZ)
        || !std::isfinite(originX) || !std::isfinite(originZ)
        || sizeX <= 0 || sizeZ <= 0) return;
    const bool firstComposition = !field.Initialized;
    const bool resized = field.Width != width || field.Height != height;
    bool changed = resized && field.IsActive();
    if (resized)
    {
        changed = changed || !field.GroundDarkening.empty();
        field.Pixels.clear();
        field.GroundDarkening.clear();
        field.NonNeutralTexels = 0;
        field.NonNeutralDarkeningTexels = 0;
        field.Width = width; field.Height = height;
        minX = minZ = 0; maxX = maxZ = -1;
    }
    minX = std::clamp(minX, 0, static_cast<int32>(width - 1));
    minZ = std::clamp(minZ, 0, static_cast<int32>(height - 1));
    maxX = maxX < 0 ? width - 1 : std::min(maxX, static_cast<int32>(width - 1));
    maxZ = maxZ < 0 ? height - 1 : std::min(maxZ, static_cast<int32>(height - 1));
    // A fresh field is implicit white. Only targets below one can make it
    // nonneutral, so avoid scanning an entire large terrain for a small region.
    // Neutral restorers are still evaluated inside this union in stack order.
    if (field.Pixels.empty() && field.GroundDarkening.empty() && !modifiers.empty())
    {
        int32 loX = width, loZ = height, hiX = -1, hiZ = -1;
        for (const auto& mod : modifiers)
        {
            if (!mod.Enabled || mod.Weight <= 0
                || std::isnan(mod.BoundsMinX) || std::isnan(mod.BoundsMaxX)
                || std::isnan(mod.BoundsMinZ) || std::isnan(mod.BoundsMaxZ)
                || mod.BoundsMaxX < originX || mod.BoundsMinX > originX + sizeX
                || mod.BoundsMaxZ < originZ || mod.BoundsMinZ > originZ + sizeZ) continue;
            const bool lowers = std::any_of(mod.Effects.begin(), mod.Effects.end(), [](const auto& fx) {
                return fx.EffectKind == ResolvedEffect::Kind::Grass
                    && (Components::ClampTerrainGrassEffectScale(fx.Grass.HeightScale) < 1
                        || Components::ClampTerrainGrassEffectScale(fx.Grass.DensityScale) < 1
                        || Components::ClampTerrainGrassEffectScale(fx.Grass.GroundDarkeningScale) < 1);
            });
            if (!lowers) continue;
            loX = std::min(loX, AxisSample(mod.BoundsMinX, originX, sizeX, width, false));
            loZ = std::min(loZ, AxisSample(mod.BoundsMinZ, originZ, sizeZ, height, false));
            hiX = std::max(hiX, AxisSample(mod.BoundsMaxX, originX, sizeX, width, true));
            hiZ = std::max(hiZ, AxisSample(mod.BoundsMaxZ, originZ, sizeZ, height, true));
        }
        minX = std::max(minX, loX); minZ = std::max(minZ, loZ);
        maxX = std::min(maxX, hiX); maxZ = std::min(maxZ, hiZ);
        if (minX > maxX || minZ > maxZ)
        {
            std::vector<uint8>{}.swap(field.Pixels);
            field.Initialized = true;
            // A resized former active field still has to retire its old image.
            if (changed) { ++field.Version; field.Dirty = true; }
            return;
        }
    }
    if (modifiers.empty())
    {
        changed |= field.IsActive() || !field.GroundDarkening.empty();
        std::vector<uint8>{}.swap(field.Pixels);
        std::vector<uint8>{}.swap(field.GroundDarkening);
        field.NonNeutralTexels = 0;
        field.NonNeutralDarkeningTexels = 0;
        minX = minZ = 0; maxX = width - 1; maxZ = height - 1;
    }
    else
    {
        // A neutral field is allocated white for the bands to write into, and released again below
        // when nothing in it is nonneutral. The darkening channel likewise, when the field holds one
        // or a region darkens the ground.
        if (field.Pixels.empty())
            field.Pixels.assign(static_cast<size_t>(width) * height * kTerrainGrassFieldChannels, 255);
        if (field.GroundDarkening.empty() && AnyRegionDarkensGround(modifiers))
            field.GroundDarkening.assign(static_cast<size_t>(width) * height, 255);
        // Row bands across the pool for a large rectangle (RunRowBands); a band split is the serial
        // walk in another order. Each band adds its own totals once.
        ComposedTotals totals;
        const BakeBandRegion rect{minX, minZ, maxX, maxZ};
        RunRowBands(jobs, {&rect, 1},
                    [&field, &totals, width, height, sizeX, sizeZ, originX, originZ, modifiers](
                        std::size_t, int32 x0, int32 x1, int32 z0, int32 z1) {
                        totals.Add(ComposeGrassRows(field, width, height, sizeX, sizeZ, originX, originZ, modifiers,
                                                    x0, x1, z0, z1));
                    });
        field.NonNeutralTexels =
            static_cast<uint64>(static_cast<int64>(field.NonNeutralTexels) + totals.NonNeutralDelta.load());
        field.NonNeutralDarkeningTexels = static_cast<uint64>(static_cast<int64>(field.NonNeutralDarkeningTexels)
                                                              + totals.NonNeutralDarkeningDelta.load());
        changed |= totals.Changed.load();
    }
    if (!field.NonNeutralTexels) std::vector<uint8>{}.swap(field.Pixels);
    if (!field.NonNeutralDarkeningTexels) std::vector<uint8>{}.swap(field.GroundDarkening);
    field.Initialized = true;
    if (changed)
    {
        ++field.Version;
        if (!field.Dirty || resized)
        {
            // A new/replaced CPU field must replace the whole destination,
            // even if its new local Version coincides with an old tile's one.
            field.DirtyMinX = (resized || firstComposition) ? 0 : minX;
            field.DirtyMinZ = (resized || firstComposition) ? 0 : minZ;
            field.DirtyMaxX = (resized || firstComposition) ? width : maxX + 1;
            field.DirtyMaxZ = (resized || firstComposition) ? height : maxZ + 1;
        }
        else
        {
            field.DirtyMinX = std::min(field.DirtyMinX, static_cast<uint32>(minX));
            field.DirtyMinZ = std::min(field.DirtyMinZ, static_cast<uint32>(minZ));
            field.DirtyMaxX = std::max(field.DirtyMaxX, static_cast<uint32>(maxX + 1));
            field.DirtyMaxZ = std::max(field.DirtyMaxZ, static_cast<uint32>(maxZ + 1));
        }
        field.Dirty = true;
    }
}

} // namespace GameEngine::TerrainECS
