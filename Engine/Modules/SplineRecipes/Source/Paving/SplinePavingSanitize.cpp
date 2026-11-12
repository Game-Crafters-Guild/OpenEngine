#include "SplineRecipes/Paving/SplinePavingSanitize.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineRecipes
{
namespace
{

using Components::SplinePaving;

// The ranges the inspector offers (@ge-range on the fields).
constexpr float32 kMaxBorderMeters = 10.0f;
constexpr float32 kMinBorderHeightMeters = 0.01f;
constexpr float32 kMinMouthRadiusMeters = 0.1f;
constexpr float32 kMaxMouthRadiusMeters = 10.0f;
constexpr float32 kMinSeamBandTiles = 0.5f;
constexpr float32 kMaxSeamBandTiles = 8.0f;
constexpr float32 kMaxSeamCourseMeters = 2.0f;

[[nodiscard]] float32 Sanitized(float32 value, float32 fallback, float32 low, float32 high)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

} // namespace

SplinePaving SanitizePavingRecipe(const SplinePaving& authored)
{
    const SplinePaving defaults{};
    SplinePaving clean = authored;
    clean.MemberCount = std::min(authored.MemberCount, Components::kMaxPavingMembers);
    for (uint32 i = 0; i < Components::kMaxPavingMembers; ++i)
    {
        Components::SplinePavingMember& member = clean.Members[i];
        if (i >= clean.MemberCount)
        {
            member = {};
            continue;
        }
        std::fill(std::begin(member._Pad), std::end(member._Pad), uint8{0});
        if (member.Mode != Components::SplinePavingMemberMode::Exclude &&
            member.Mode != Components::SplinePavingMemberMode::Ignore)
            member.Mode = Components::SplinePavingMemberMode::Exclude;
    }
    clean.BorderWidth = Sanitized(authored.BorderWidth, defaults.BorderWidth, 0.0f, kMaxBorderMeters);
    clean.BorderHeight =
        Sanitized(authored.BorderHeight, defaults.BorderHeight, kMinBorderHeightMeters, kMaxBorderMeters);
    if (authored.BorderCorner != Components::SplineWallCorner::Miter &&
        authored.BorderCorner != Components::SplineWallCorner::Round)
        clean.BorderCorner = defaults.BorderCorner;
    if (authored.Mouth != Components::SplinePavingMouth::Straight &&
        authored.Mouth != Components::SplinePavingMouth::Flared)
        clean.Mouth = defaults.Mouth;
    clean.MouthRadius =
        Sanitized(authored.MouthRadius, defaults.MouthRadius, kMinMouthRadiusMeters, kMaxMouthRadiusMeters);
    if (authored.ConformTarget != Components::SplineConformTarget::Scene &&
        authored.ConformTarget != Components::SplineConformTarget::TerrainOnly)
        clean.ConformTarget = defaults.ConformTarget;
    if (authored.GroundAuthority != Components::SplineGroundAuthority::Auto &&
        authored.GroundAuthority != Components::SplineGroundAuthority::None &&
        authored.GroundAuthority != Components::SplineGroundAuthority::Grades &&
        authored.GroundAuthority != Components::SplineGroundAuthority::Owns)
        clean.GroundAuthority = defaults.GroundAuthority;
    if (authored.Seam != Components::SplinePavingSeam::HardCut && authored.Seam != Components::SplinePavingSeam::Blend &&
        authored.Seam != Components::SplinePavingSeam::JointLine &&
        authored.Seam != Components::SplinePavingSeam::StoneCourse)
        clean.Seam = defaults.Seam;
    clean.SeamBandTiles = Sanitized(authored.SeamBandTiles, defaults.SeamBandTiles, kMinSeamBandTiles, kMaxSeamBandTiles);
    clean.SeamCourseWidth = Sanitized(authored.SeamCourseWidth, defaults.SeamCourseWidth, 0.0f, kMaxSeamCourseMeters);
    return clean;
}

SplinePaving PavingGeometryKey(const SplinePaving& recipe)
{
    const SplinePaving defaults{};
    SplinePaving key = recipe;
    key.Material = defaults.Material;
    key.BorderMaterial = defaults.BorderMaterial;
    key.CastShadows = defaults.CastShadows;
    key.ReceiveShadows = defaults.ReceiveShadows;
    key.GroundAuthority = defaults.GroundAuthority;
    return key;
}

} // namespace GameEngine::SplineRecipes
