// The sRGB transfer curve, both directions, per channel and per colour (IEC 61966-2-1). The GLSL twin
// of ColorSRGB::ToLinear and ColorLinear::ToSRGB (Types/Color.h): the same thresholds and exponents,
// and a negative input is held at 0. An input above 1 is not clamped, so an HDR value encodes past 1.
//
// The literals carry the `f` suffix because the block is lifted verbatim into the host tests
// (GlslShim.h), where an unsuffixed literal is a double, and the components are named x, y, z
// because the shim carries no swizzles.
#ifndef GE_SRGB_TRANSFER_GLSL
#define GE_SRGB_TRANSFER_GLSL

// GE_SHARED_SRGB_TRANSFER_BEGIN
float LinearToSRGB1(float x)
{
    x = max(x, 0.0f);
    return (x <= 0.0031308f) ? (12.92f * x) : (1.055f * pow(x, 1.0f / 2.4f) - 0.055f);
}

float SRGBToLinear1(float x)
{
    x = max(x, 0.0f);
    return (x <= 0.04045f) ? (x / 12.92f) : pow((x + 0.055f) / 1.055f, 2.4f);
}

vec3 LinearToSRGB(vec3 c)
{
    return vec3(LinearToSRGB1(c.x), LinearToSRGB1(c.y), LinearToSRGB1(c.z));
}

vec3 SRGBToLinear(vec3 c)
{
    return vec3(SRGBToLinear1(c.x), SRGBToLinear1(c.y), SRGBToLinear1(c.z));
}
// GE_SHARED_SRGB_TRANSFER_END

#endif // GE_SRGB_TRANSFER_GLSL
