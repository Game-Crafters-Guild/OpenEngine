#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>

namespace GameEngine::Rendering
{

/// A Resolve .cube LUT as a shader samples it: the uploaded tables (a 3D table, a 1D shaper strip,
/// or both), the domain each covers and the encoding the asset says its input is authored in. The
/// names match the shaders' push members (Includes/cube_lut.glsl). An unbound LUT is the identity
/// tables with intensity left at zero.
struct CubeLutGpuBindingState
{
    TextureHandle Lut3D{};
    TextureHandle Lut1DStrip{};
    int32_t Size3D = 0;
    int32_t Size1D = 0;
    int32_t Flags = 0; // bit 0 = has a 3D table, bit 1 = has a 1D shaper
    float In1DMin = 0.0f;
    float In1DMax = 1.0f;
    float In3DMin = 0.0f;
    float In3DMax = 1.0f;
    int32_t InputEncoding = 0; // CubeLutInputEncoding (Rendering/Utils/CubeLutColorDomain.h)
};

} // namespace GameEngine::Rendering
