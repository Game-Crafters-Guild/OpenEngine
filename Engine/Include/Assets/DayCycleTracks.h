#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace GameEngine
{

/// The tracks a Day Cycle profile may key, one per field the engine shapes over the sun's elevation.
/// The order is the table's order in kDayCycleTracks and the order the editor lists them in.
enum class DayCycleTrack : std::uint8_t
{
    /// A picture-wide exposure offset in stops, added after the camera's metering.
    ExposureOffset,
};

/// One row of the track table: what a profile file calls the track, how the editor names and draws
/// it, and the value that leaves the frame as it is.
struct DayCycleTrackDesc
{
    DayCycleTrack Track;
    /// The track's key in a profile file's "tracks" object.
    std::string_view FileKey;
    /// The track's name in the editor.
    std::string_view Label;
    /// The unit after a value in the editor, empty for a plain multiplier.
    std::string_view Unit;
    /// The value that changes nothing: what a new track starts flat at.
    float Identity;
    /// The value range the editor's graph shows.
    float DisplayMin;
    float DisplayMax;
};

/// Every track a profile may hold. A profile file naming a track that is not here is refused; adding
/// a keyable field is one row here and one combine line in the field's consumer.
inline constexpr std::array<DayCycleTrackDesc, 1> kDayCycleTracks = {{
    {DayCycleTrack::ExposureOffset, "exposureOffset", "Exposure offset", "stops", 0.0f, -3.0f, 1.0f},
}};

/// The row for `track`.
const DayCycleTrackDesc& GetDayCycleTrackDesc(DayCycleTrack track);

} // namespace GameEngine
