#pragma once

#include "Types/Types.h"

#include <chrono>

namespace GameEngine::Ocean
{
// The frame a per-view temporal ocean pass last declared in. Frames are the
// render graph's monotonic RGFrame::FrameIndex(); the device frame slot
// (IDevice::GetFrameIndex) wraps every frames-in-flight and is no identity.
struct OceanFrameStamp
{
    static constexpr uint64 kNever = ~0ull;
    uint64 Frame = kNever;

    bool IsFrame(uint64 frame) const
    {
        return Frame == frame;
    }
    // True when the stamp is the frame immediately before `frame`: the stored
    // history is exactly one frame old and may be accumulated into.
    bool PrecedesFrame(uint64 frame) const
    {
        return Frame != kNever && Frame + 1u == frame;
    }
};

// A view that goes this long without declaring no longer exists or no longer
// shows the ocean, and its per-view history is released. Wall-clock, because
// views in different windows advance separate render-graph frame counters. The
// planar reflection measures the time with IsOceanViewIdle.
inline constexpr std::chrono::seconds kOceanViewHistoryIdleTimeout{5};

// Whether a view's state is idle, judged while another view declares.
// lastDeclared is when the state's view last declared; declarerPrevious is when
// the declaring view declared before (a default time point on its first
// declare, which judges no state idle). The state is idle when its view had
// already gone longer than the timeout without declaring at the declaring view's
// previous declare, so a stopped view is released one declare after the
// timeout. The time of the current declare is not used: one frame longer than
// the timeout (a movie recorder waiting for its encoder, a long shader compile)
// would make every view that declares later in that frame look idle.
inline bool IsOceanViewIdle(std::chrono::steady_clock::time_point lastDeclared,
                            std::chrono::steady_clock::time_point declarerPrevious)
{
    return declarerPrevious - lastDeclared > kOceanViewHistoryIdleTimeout;
}
} // namespace GameEngine::Ocean
