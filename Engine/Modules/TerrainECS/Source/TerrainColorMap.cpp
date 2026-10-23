#include "TerrainECS/TerrainColorMap.h"

#include "TerrainColorMapCompose.h"

#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainZonePayload.h"
#include "Terrain/TerrainGrassCoverage.h"
#include "Types/Color.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace GameEngine::TerrainECS
{
namespace
{

// Linear values per step of the encode table: fine enough that every sRGB byte boundary falls
// between two entries, including the darkest steps (byte 1 decodes to 0.0003 linear).
constexpr uint32 kSrgbEncodeSteps = 65535u;

const std::array<uint8, kSrgbEncodeSteps + 1>& SrgbEncodeTable()
{
    static const std::array<uint8, kSrgbEncodeSteps + 1> kTable = []
    {
        std::array<uint8, kSrgbEncodeSteps + 1> table{};
        for (uint32 i = 0; i <= kSrgbEncodeSteps; ++i)
        {
            const float32 linear = static_cast<float32>(i) / static_cast<float32>(kSrgbEncodeSteps);
            table[i] = static_cast<uint8>(std::lround(Detail::LinearChannelToSRGB(linear) * 255.0f));
        }
        return table;
    }();
    return kTable;
}

// One colour zone, hoisted out of the per-texel loop: the inverse rotation and extents that map a
// world sample into the payload's UV, and its premultiplied texels.
struct ZoneSampler
{
    const ResolvedModifier* Zone = nullptr;
    float32 Cos = 1.0f, Sin = 0.0f;
    float32 InvExtX = 0.0f, InvExtZ = 0.0f;
};

// Bilinear read of byte channel `channel` of a `channels`-wide lattice at UV (u, v), texel centres
// on the UV corners. Exact at a lattice point.
float32 SampleBytesBilinear(const uint8* texels, uint32 width, uint32 height, uint32 channels, uint32 channel,
                            float32 u, float32 v)
{
    const float32 fx = std::clamp(u, 0.0f, 1.0f) * static_cast<float32>(width - 1);
    const float32 fz = std::clamp(v, 0.0f, 1.0f) * static_cast<float32>(height - 1);
    const uint32 x0 = std::min(static_cast<uint32>(fx), width - 1);
    const uint32 z0 = std::min(static_cast<uint32>(fz), height - 1);
    const uint32 x1 = std::min(x0 + 1, width - 1);
    const uint32 z1 = std::min(z0 + 1, height - 1);
    const float32 tx = fx - static_cast<float32>(x0);
    const float32 tz = fz - static_cast<float32>(z0);
    const auto at = [&](uint32 x, uint32 z)
    { return static_cast<float32>(texels[(static_cast<size_t>(z) * width + x) * channels + channel]); };
    const float32 a = at(x0, z0) + (at(x1, z0) - at(x0, z0)) * tx;
    const float32 b = at(x0, z1) + (at(x1, z1) - at(x0, z1)) * tx;
    return a + (b - a) * tz;
}

// The four channel weights of the splat at lattice sample (x, z), normalized as the ground
// normalizes them: an all-zero texel reads as channel 0.
void ReadSplatWeights(const TerrainColorComposeInputs& in, uint32 x, uint32 z, float32 (&weights)[4])
{
    const TerrainColorSplatSource& splat = in.Splat;
    weights[0] = weights[1] = weights[2] = weights[3] = 0.0f;
    if (splat.Texels && splat.Width == in.Width && splat.Height == in.Height)
    {
        const uint8* texel = splat.Texels + (static_cast<size_t>(z) * in.Width + x) * 4u;
        for (uint32 c = 0; c < 4u; ++c)
            weights[c] = static_cast<float32>(texel[c]) / 255.0f;
    }
    else if (splat.Texels && splat.Width >= 2 && splat.Height >= 2)
    {
        const float32 u = static_cast<float32>(x) / static_cast<float32>(in.Width - 1);
        const float32 v = static_cast<float32>(z) / static_cast<float32>(in.Height - 1);
        for (uint32 c = 0; c < 4u; ++c)
            weights[c] = SampleBytesBilinear(splat.Texels, splat.Width, splat.Height, 4u, c, u, v) / 255.0f;
    }
    const float32 sum = weights[0] + weights[1] + weights[2] + weights[3];
    if (sum > 0.0f)
        for (float32& w : weights)
            w /= sum;
    else
        weights[0] = 1.0f;
}

// The grass control scales at a world sample: height, density and Darken Ground, 1 where the
// field holds nothing.
void ReadGrassScales(const TerrainColorGrassSource& grass, float32 worldX, float32 worldZ, float32& heightScale,
                     float32& densityScale, float32& darkeningScale)
{
    heightScale = densityScale = darkeningScale = 1.0f;
    const TerrainGrassField* field = grass.Field;
    if (!field || field->Width < 2 || field->Height < 2 || grass.SizeX <= 0.0f || grass.SizeZ <= 0.0f)
        return;
    const float32 u = (worldX - grass.OriginX) / grass.SizeX;
    const float32 v = (worldZ - grass.OriginZ) / grass.SizeZ;
    if (field->IsActive())
    {
        heightScale = SampleBytesBilinear(field->Pixels.data(), field->Width, field->Height,
                                          kTerrainGrassFieldChannels, 0, u, v) / 255.0f;
        densityScale = SampleBytesBilinear(field->Pixels.data(), field->Width, field->Height,
                                           kTerrainGrassFieldChannels, 1, u, v) / 255.0f;
    }
    if (!field->GroundDarkening.empty())
        darkeningScale =
            SampleBytesBilinear(field->GroundDarkening.data(), field->Width, field->Height, 1u, 0, u, v) / 255.0f;
}

// A volume writer carries its Color effect as its only effect (the gather's split).
bool IsColorEffectWriter(const ResolvedModifier& zone)
{
    return zone.ModType == ResolvedModifier::Type::Volume && zone.Effects.size() == 1
        && zone.Effects[0].EffectKind == ResolvedEffect::Kind::Color;
}

bool ZoneReaches(const ResolvedModifier& zone, const TerrainColorComposeInputs& in)
{
    const bool hasContent = IsColorEffectWriter(zone)
        ? zone.Effects[0].Color.Strength > 0.0f
        : zone.ModType == ResolvedModifier::Type::ColorZone && zone.ColorZone.Color != nullptr
              && zone.ColorZone.Width > 0 && zone.ColorZone.Height > 0;
    return zone.Enabled && hasContent && zone.BoundsMaxX >= in.OriginX && zone.BoundsMinX <= in.OriginX + in.SizeX
        && zone.BoundsMaxZ >= in.OriginZ && zone.BoundsMinZ <= in.OriginZ + in.SizeZ;
}

// The zone's premultiplied colour (rgb x weight) and weight at payload UV (u, v), bilinear with
// texel centres on the UV corners, as the sculpt and paint zones sample. Premultiplied before the
// filter, so a written texel beside an unwritten one filters to the half mix.
void SampleZonePremultiplied(const ModifierColorZoneParams& zone, float32 u, float32 v, float32 (&out)[4])
{
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float32 fx = u * static_cast<float32>(zone.Width - 1);
    const float32 fz = v * static_cast<float32>(zone.Height - 1);
    const uint32 x0 = static_cast<uint32>(fx);
    const uint32 z0 = static_cast<uint32>(fz);
    const uint32 x1 = std::min(x0 + 1, zone.Width - 1);
    const uint32 z1 = std::min(z0 + 1, zone.Height - 1);
    const float32 tx = fx - static_cast<float32>(x0);
    const float32 tz = fz - static_cast<float32>(z0);
    const float32 corners[4] = {(1.0f - tx) * (1.0f - tz), tx * (1.0f - tz), (1.0f - tx) * tz, tx * tz};
    const uint32 xs[4] = {x0, x1, x0, x1};
    const uint32 zs[4] = {z0, z0, z1, z1};
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    for (uint32 c = 0; c < 4; ++c)
    {
        const float32* texel =
            zone.Color + (static_cast<size_t>(zs[c]) * zone.Width + xs[c]) * kZonePayloadColorChannels;
        const float32 weight = std::clamp(texel[3], 0.0f, 1.0f);
        const float32 k = corners[c] * weight;
        out[0] += std::clamp(texel[0], 0.0f, 1.0f) * k;
        out[1] += std::clamp(texel[1], 0.0f, 1.0f) * k;
        out[2] += std::clamp(texel[2], 0.0f, 1.0f) * k;
        out[3] += k;
    }
}

void ComposeZone(const ZoneSampler& sampler, float32 worldX, float32 worldZ, float32 (&texel)[4])
{
    const ResolvedModifier& zone = *sampler.Zone;
    if (worldX < zone.BoundsMinX || worldX > zone.BoundsMaxX || worldZ < zone.BoundsMinZ
        || worldZ > zone.BoundsMaxZ)
        return;
    const float32 rawFalloff = ComputeWeight(zone, worldX, worldZ);
    const float32 falloff = std::isfinite(rawFalloff) ? std::clamp(rawFalloff, 0.0f, 1.0f) : 0.0f;
    if (falloff <= 0.0f)
        return;
    if (IsColorEffectWriter(zone))
    {
        const ResolvedEffect::ColorParams& effect = zone.Effects[0].Color;
        const float32 weight = effect.Strength * falloff;
        const float32 color[3] = {effect.Linear[0] * weight, effect.Linear[1] * weight, effect.Linear[2] * weight};
        CompositePremultipliedOver(texel, color, weight);
        return;
    }
    const float32 dx = worldX - zone.Position.x;
    const float32 dz = worldZ - zone.Position.z;
    const float32 lx = sampler.Cos * dx - sampler.Sin * dz;
    const float32 lz = sampler.Sin * dx + sampler.Cos * dz;
    float32 sample[4];
    SampleZonePremultiplied(zone.ColorZone, 0.5f + 0.5f * lx * sampler.InvExtX, 0.5f + 0.5f * lz * sampler.InvExtZ,
                            sample);
    const float32 color[3] = {sample[0] * falloff, sample[1] * falloff, sample[2] * falloff};
    CompositePremultipliedOver(texel, color, sample[3] * falloff);
}

// The ground write's weight at one sample: Amount x the Darken Ground scale x the field coverage
// of the normalized splat `weights` under the grass control scales.
float32 TerrainColorGroundWriteWeight(const TerrainColorGroundWrite& write, const float32 (&weights)[4],
                                      bool hasSplatmap, float32 heightScale, float32 densityScale,
                                      float32 darkeningScale)
{
    if (!write.Enabled)
        return 0.0f;
    const float32 coverage = Terrain::GrassFieldCoverage(
        Terrain::GrassSplatCoverage(weights, write.GrassLayer, write.MaskThreshold, hasSplatmap), heightScale,
        densityScale);
    return std::clamp(write.Amount, 0.0f, 1.0f) * std::clamp(darkeningScale, 0.0f, 1.0f) * coverage;
}

// True when a writer reaches the lattice, so the map has content to hold.
bool TerrainColorHasWriters(const TerrainColorComposeInputs& inputs)
{
    if (inputs.Write.Enabled && inputs.Write.Amount > 0.0f)
        return true;
    return std::any_of(inputs.Zones.begin(), inputs.Zones.end(),
                       [&](const ResolvedModifier& zone) { return ZoneReaches(zone, inputs); });
}

} // namespace

uint8 EncodeLinearToSrgbByte(float32 linear)
{
    const float32 clamped = std::isfinite(linear) ? std::clamp(linear, 0.0f, 1.0f) : 0.0f;
    return SrgbEncodeTable()[static_cast<uint32>(clamped * static_cast<float32>(kSrgbEncodeSteps) + 0.5f)];
}

void CompositePremultipliedOver(float32 (&texel)[4], const float32 (&premultipliedColor)[3], float32 weight)
{
    const float32 keep = 1.0f - std::clamp(weight, 0.0f, 1.0f);
    texel[0] = premultipliedColor[0] + texel[0] * keep;
    texel[1] = premultipliedColor[1] + texel[1] * keep;
    texel[2] = premultipliedColor[2] + texel[2] * keep;
    texel[3] = std::clamp(weight, 0.0f, 1.0f) + texel[3] * keep;
}

void ComposeTerrainColorMap(TerrainColorMap& map, const TerrainColorComposeInputs& inputs, int32 minX, int32 minZ,
                            int32 maxX, int32 maxZ)
{
    const uint32 width = inputs.Width;
    const uint32 height = inputs.Height;
    if (width < 2 || height < 2 || !std::isfinite(inputs.SizeX) || !std::isfinite(inputs.SizeZ)
        || inputs.SizeX <= 0.0f || inputs.SizeZ <= 0.0f)
        return;

    const bool resized = map.Width != width || map.Height != height;
    const auto markWhole = [&]
    {
        ++map.Version;
        map.Dirty = true;
        map.DirtyMinX = map.DirtyMinZ = 0;
        map.DirtyMaxX = width;
        map.DirtyMaxZ = height;
    };
    if (!TerrainColorHasWriters(inputs))
    {
        const bool wasActive = map.IsActive();
        std::vector<uint8>{}.swap(map.Pixels);
        map.Width = width;
        map.Height = height;
        map.Initialized = true;
        if (wasActive)
            markWhole();
        return;
    }

    bool whole = false;
    if (resized || !map.IsActive())
    {
        map.Width = width;
        map.Height = height;
        map.Pixels.assign(static_cast<size_t>(width) * height * kTerrainColorMapChannels, 0);
        minX = minZ = 0;
        maxX = maxZ = -1;
        whole = true;
    }
    minX = std::clamp(minX, 0, static_cast<int32>(width - 1));
    minZ = std::clamp(minZ, 0, static_cast<int32>(height - 1));
    maxX = maxX < 0 ? static_cast<int32>(width - 1) : std::min(maxX, static_cast<int32>(width - 1));
    maxZ = maxZ < 0 ? static_cast<int32>(height - 1) : std::min(maxZ, static_cast<int32>(height - 1));
    if (minX > maxX || minZ > maxZ)
        return;

    std::vector<ZoneSampler> zones;
    zones.reserve(inputs.Zones.size());
    for (const ResolvedModifier& zone : inputs.Zones)
    {
        if (!ZoneReaches(zone, inputs))
            continue;
        ZoneSampler& sampler = zones.emplace_back();
        sampler.Zone = &zone;
        sampler.Cos = std::cos(-zone.YawRadians);
        sampler.Sin = std::sin(-zone.YawRadians);
        sampler.InvExtX = zone.RectHalfX > 0.0f ? 1.0f / zone.RectHalfX : 0.0f;
        sampler.InvExtZ = zone.RectHalfZ > 0.0f ? 1.0f / zone.RectHalfZ : 0.0f;
    }

    const bool writeOn = inputs.Write.Enabled && inputs.Write.Amount > 0.0f;
    bool changed = false;
    const float32 stepX = inputs.SizeX / static_cast<float32>(width - 1);
    const float32 stepZ = inputs.SizeZ / static_cast<float32>(height - 1);
    for (int32 z = minZ; z <= maxZ; ++z)
    {
        const float32 worldZ = inputs.OriginZ + stepZ * static_cast<float32>(z);
        for (int32 x = minX; x <= maxX; ++x)
        {
            const float32 worldX = inputs.OriginX + stepX * static_cast<float32>(x);
            float32 texel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (writeOn)
            {
                float32 weights[4];
                ReadSplatWeights(inputs, static_cast<uint32>(x), static_cast<uint32>(z), weights);
                float32 heightScale = 1.0f, densityScale = 1.0f, darkeningScale = 1.0f;
                ReadGrassScales(inputs.Grass, worldX, worldZ, heightScale, densityScale, darkeningScale);
                const float32 weight = TerrainColorGroundWriteWeight(inputs.Write, weights, inputs.Splat.HasSplatmap,
                                                                     heightScale, densityScale, darkeningScale);
                if (weight > 0.0f)
                {
                    const float32 soil[3] = {inputs.Write.SoilLinear[0] * weight, inputs.Write.SoilLinear[1] * weight,
                                             inputs.Write.SoilLinear[2] * weight};
                    CompositePremultipliedOver(texel, soil, weight);
                }
            }
            for (const ZoneSampler& zone : zones)
                ComposeZone(zone, worldX, worldZ, texel);
            const uint8 encoded[4] = {EncodeLinearToSrgbByte(texel[0]), EncodeLinearToSrgbByte(texel[1]),
                                      EncodeLinearToSrgbByte(texel[2]),
                                      static_cast<uint8>(std::lround(std::clamp(texel[3], 0.0f, 1.0f) * 255.0f))};
            uint8* stored = map.Pixels.data() + (static_cast<size_t>(z) * width + x) * kTerrainColorMapChannels;
            if (std::equal(encoded, encoded + 4, stored))
                continue;
            std::copy(encoded, encoded + 4, stored);
            changed = true;
        }
    }

    map.Initialized = true;
    if (whole)
    {
        markWhole();
        return;
    }
    if (!changed)
        return;
    ++map.Version;
    const uint32 x0 = static_cast<uint32>(minX), z0 = static_cast<uint32>(minZ);
    const uint32 x1 = static_cast<uint32>(maxX + 1), z1 = static_cast<uint32>(maxZ + 1);
    if (!map.Dirty)
    {
        map.DirtyMinX = x0;
        map.DirtyMinZ = z0;
        map.DirtyMaxX = x1;
        map.DirtyMaxZ = z1;
    }
    else
    {
        map.DirtyMinX = std::min(map.DirtyMinX, x0);
        map.DirtyMinZ = std::min(map.DirtyMinZ, z0);
        map.DirtyMaxX = std::max(map.DirtyMaxX, x1);
        map.DirtyMaxZ = std::max(map.DirtyMaxZ, z1);
    }
    map.Dirty = true;
}

} // namespace GameEngine::TerrainECS
