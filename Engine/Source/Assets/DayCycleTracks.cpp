#include "Assets/DayCycleTracks.h"

#include <cstddef>

namespace GameEngine
{

const DayCycleTrackDesc& GetDayCycleTrackDesc(DayCycleTrack track)
{
    return kDayCycleTracks[static_cast<std::size_t>(track)];
}

} // namespace GameEngine
