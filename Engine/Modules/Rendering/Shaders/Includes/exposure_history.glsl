// The view's ExposureHistory state (ExposureReadbackFeature::State on the CPU, 16 bytes): the adapted
// exposure the auto-exposure resolve writes and the pre-exposure pair the PreExposureSnapshot pass
// writes once per frame. Scene colour is stored at the frame's pre-exposure P: a pass writing radiance
// into it multiplies by GE_PreExposure(), a pass comparing it with an absolute value divides by
// it, and a temporal history stored last frame is rescaled by GE_PreExposureHistoryScale().
//
// Includers define GE_EXPOSURE_HISTORY_BINDING (set 0). GE_EXPOSURE_HISTORY_NAME is the instance name
// the pass binds the buffer under (default uExposure); GE_EXPOSURE_HISTORY_WRITABLE is defined only by
// the two passes that write the state (auto_exposure_resolve.comp, pre_exposure_snapshot.comp).
#ifndef GE_EXPOSURE_HISTORY_GLSL
#define GE_EXPOSURE_HISTORY_GLSL

#include "exposed_brightness.glsl"

#ifndef GE_EXPOSURE_HISTORY_BINDING
#error "define GE_EXPOSURE_HISTORY_BINDING before including exposure_history.glsl"
#endif
#ifndef GE_EXPOSURE_HISTORY_NAME
#define GE_EXPOSURE_HISTORY_NAME uExposure
#endif

#ifdef GE_EXPOSURE_HISTORY_WRITABLE
layout(set = 0, binding = GE_EXPOSURE_HISTORY_BINDING, std430) restrict buffer ExposureHistoryBlock
#else
layout(set = 0, binding = GE_EXPOSURE_HISTORY_BINDING, std430) readonly buffer ExposureHistoryBlock
#endif
{
    float exposureScale;       // the resolve's adapted LINEAR exposure scale
    uint valid;                // != 1: the resolve has not written yet
    float preExposure;         // P this frame's scene colour is stored at; 0 = no snapshot
    float previousPreExposure; // P of the previous frame
} GE_EXPOSURE_HISTORY_NAME;

// P this frame's scene colour is stored at (a field no snapshot wrote reads as 1).
float GE_PreExposure()
{
    return GE_ResolvePreExposure(GE_EXPOSURE_HISTORY_NAME.preExposure);
}

// P / previousPreExposure: brings a history stored last frame to this frame's pre-exposure. 1 on the
// first frame (the snapshot sets previous = current) and where no snapshot ran. Only a history the
// view's previous frame wrote may be rescaled by it: a pass that renders a frame of its view without
// writing its history drops that history (TemporalAANode, ScreenSpaceReflectionsNode).
float GE_PreExposureHistoryScale()
{
    return GE_PreExposure() / GE_ResolvePreExposure(GE_EXPOSURE_HISTORY_NAME.previousPreExposure);
}

#endif // GE_EXPOSURE_HISTORY_GLSL
