// The view's exposure compensation state (Pipeline::kViewExposureCompensationBytes on the CPU, 16
// bytes): the meter's reading and the stops auto_exposure_resolve.comp took from the compensation
// curve at it, this frame. A pass that must not take the compensation (the star pass: the stars keep
// the brightness the metered exposure gives them while the curve darkens the night) multiplies by
// GE_ExposureCompensationUndo(); the night tint gates on the reading (GE_MeteredNits). Zero-filled
// when a view's buffer is created; valid is 1 once the resolve has written it. A fullscreen pass that
// declares uCompensation is bound the view's state without a pipeline entry (FullscreenShaderNode).
//
// Includers define GE_EXPOSURE_COMPENSATION_BINDING (set 0). GE_EXPOSURE_COMPENSATION_WRITABLE is
// defined only by the resolve, the one pass that writes it (read-write: the web cook refuses writeonly
// storage).
#ifndef GE_EXPOSURE_COMPENSATION_GLSL
#define GE_EXPOSURE_COMPENSATION_GLSL

#ifndef GE_EXPOSURE_COMPENSATION_BINDING
#error "define GE_EXPOSURE_COMPENSATION_BINDING before including exposure_compensation.glsl"
#endif

#ifdef GE_EXPOSURE_COMPENSATION_WRITABLE
layout(set = 0, binding = GE_EXPOSURE_COMPENSATION_BINDING, std430) restrict buffer ExposureCompensationBlock
#else
layout(set = 0, binding = GE_EXPOSURE_COMPENSATION_BINDING, std430) readonly buffer ExposureCompensationBlock
#endif
{
    float compensationStops; // the curve's stops at the reading (+ brightens)
    uint compensationValid;  // 1 once the resolve has written
    float meteredLogLuminance; // the meter's reading, scene-linear log2 (1.0 = 203 cd/m2)
    float compensationPad1;
} uCompensation;

// The stops the compensation curve may be undone by: it never brightens past the metered exposure
// and never by more than 8 stops (a stale or garbage word reads as no undo).
const float kExposureCompensationUndoMaxStops = 8.0;

#ifndef GE_EXPOSURE_COMPENSATION_WRITABLE
// The meter's reading as a luminance in cd/m2 (the scene's metered average, before the clamps and the
// curve); negative when the resolve has not written it.
float GE_MeteredNits()
{
    if (uCompensation.compensationValid != 1u)
        return -1.0;
    float logLum = uCompensation.meteredLogLuminance;
#if defined(GE_COMPAT_PROFILE)
    bool finite = logLum == logLum && abs(logLum) < 3.4e38;
#else
    bool finite = !isnan(logLum) && !isinf(logLum);
#endif
    return finite ? exp2(clamp(logLum, -40.0, 40.0)) * 203.0 : -1.0;
}

// The factor that cancels a darkening compensation: 2^-stops for a negative compensation, 1 when the
// resolve has not written or the curve brightens.
float GE_ExposureCompensationUndo()
{
    if (uCompensation.compensationValid != 1u)
        return 1.0;
    float stops = uCompensation.compensationStops;
#if defined(GE_COMPAT_PROFILE)
    bool finite = stops == stops && abs(stops) < 3.4e38;
#else
    bool finite = !isnan(stops) && !isinf(stops);
#endif
    if (!finite)
        return 1.0;
    return exp2(clamp(-stops, 0.0, kExposureCompensationUndoMaxStops));
}
#endif

#endif // GE_EXPOSURE_COMPENSATION_GLSL
