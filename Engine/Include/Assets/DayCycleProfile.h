#pragma once

#include "AssetCore/Asset.h"
#include "Assets/DayCycleTracks.h"
#include "Mathematics/Angle.h"
#include "Mathematics/Curve.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace Rendering
{
struct SkySunState;
}

/// The keys of one track a profile holds. Key times are the sun's elevation in degrees, -90 to 90,
/// ascending; values are in the track's unit (kDayCycleTracks). Beyond its first and last key a
/// track holds the end value.
struct DayCycleTrackKeys
{
    DayCycleTrack Track = DayCycleTrack::ExposureOffset;
    /// The rising half's keys, and the whole day's while SeparateDusk is off.
    Math::Curve Keys{};
    /// On: the setting half of the day reads DuskKeys instead of Keys.
    bool SeparateDusk = false;
    Math::Curve DuskKeys{};
};

/// What a Day Cycle profile file holds: a name, a description and the tracks it keys. A track the
/// profile leaves out is not keyed by it.
struct DayCycleProfileData
{
    std::string Name;
    std::string Description;
    /// At most one entry per track, in table order.
    std::vector<DayCycleTrackKeys> Tracks;

    /// The keys of `track`, or null when the profile leaves it out.
    const DayCycleTrackKeys* Find(DayCycleTrack track) const;
    DayCycleTrackKeys* Find(DayCycleTrack track);
};

/// Reads a profile file's text. The format has one version and no version field. Refused, with the
/// reason and the fix in `outError`: text that is not a JSON object, a key written twice in one
/// object, a field the format does not have, a field of the wrong type, a track the table does not
/// declare (named, with the tracks it does), a track with no keys or more keys than a curve holds, a
/// key field that is not a finite number, an elevation outside -90 to 90 degrees, an interpolation or
/// tangent mode outside its list, keys that are not in ascending elevation, and a dusk list without
/// separateDusk or separateDusk without one.
bool ParseDayCycleProfile(std::string_view text, DayCycleProfileData& out, std::string& outError);

/// The file text for `data`, which ParseDayCycleProfile reads back to the same data.
std::string WriteDayCycleProfile(const DayCycleProfileData& data);

/// The value of `keys` with the sun `elevation` above the horizon, on the setting half of the day when
/// `duskHalf` (which reads DuskKeys under SeparateDusk).
float EvaluateDayCycleTrack(const DayCycleTrackKeys& keys, Mathematics::Radians elevation, bool duskHalf);

/// Whether the sun stands on the setting half of its day: its bearing west of the meridian, azimuth in
/// (pi, 2 pi). The same in both hemispheres and under a sun set by hand (the azimuth is computed from
/// its direction); a polar day that never sets splits at local noon and midnight, its afternoon the
/// setting half.
bool IsDuskHalf(const Rendering::SkySunState& sunState);

/// A Day Cycle profile asset (<name>.daycycle.json), created by the asset type registry's factory
/// (CoreAssetRegistrations) and loaded through Load. The loaded profile is an immutable object held
/// through a shared pointer: a reload parses a new one and publishes it, and a reader that took the
/// old one keeps it as long as it holds it. A file that does not parse leaves the last good profile
/// published. GetData is safe from any thread.
class DayCycleProfile final : public Asset
{
  public:
    DayCycleProfile(const GUID& guid, const std::filesystem::path& path);

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    /// The published profile; null until loaded.
    std::shared_ptr<const DayCycleProfileData> GetData() const;

  protected:
    bool ReloadFromData(const Vector<uint8>& data) override;

  private:
    // The parsed text, or null with the reason logged.
    std::shared_ptr<const DayCycleProfileData> ParseBytes(const Vector<uint8>& data) const;
    void Publish(std::shared_ptr<const DayCycleProfileData> data);

    // Guards m_Data: each critical section is one shared_ptr copy or swap.
    mutable std::mutex m_DataMutex;
    std::shared_ptr<const DayCycleProfileData> m_Data;

    DISALLOW_COPY_AND_ASSIGN(DayCycleProfile);
};

} // namespace GameEngine
