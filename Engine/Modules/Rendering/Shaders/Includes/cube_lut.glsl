// Resolve .cube LUT sampling (a 1D shaper, a 3D table, or both) and the input encodings the
// LUT's domain is authored in. A pass that samples the LUT declares GE_CUBE_LUT_PUSH_MEMBERS in its
// push block and builds its GE_CubeLutParams with GE_CUBE_LUT_PARAMS: the effect's settings write
// lutIntensity and lutStage, and TextureService::TryWriteCubeLutPushMember writes the uploaded
// tables' members (sizes, flags, domains and the asset's input encoding) by name.
#ifndef GE_CUBE_LUT_GLSL
#define GE_CUBE_LUT_GLSL

#include "srgb_transfer.glsl"

// The LUT's members of a push block. lutStage names the pass that samples it (0 = Finish, inside
// the Tonemap pass after the look; 1 = Grade, the grade's last stage before the look).
#define GE_CUBE_LUT_PUSH_MEMBERS \
    float lutIntensity;          \
    int lutStage;                \
    int lutInputEncoding;        \
    int lutSize3d;               \
    int lutSize1d;               \
    int lutFlags;                \
    float lut1dInMin;            \
    float lut1dInMax;            \
    float lut3dInMin;            \
    float lut3dInMax;

const int kCubeLutStageFinish = 0;
const int kCubeLutStageGrade = 1;

struct GE_CubeLutParams
{
    float Intensity;    // 0 = bypass, 1 = the LUT's output
    int InputEncoding;  // Components::CubeLutInputEncoding
    int Size3D;
    int Size1D;
    int Flags;          // bit 0 = has a 3D table, bit 1 = has a 1D shaper
    float In1DMin;
    float In1DMax;
    float In3DMin;
    float In3DMax;
};

vec3 Rec709ToAWG3(vec3 c)
{
    return vec3(
        dot(vec3(0.631320638, 0.270800406, 0.097878818), c),
        dot(vec3(0.036819917, 0.793037090, 0.170143018), c),
        dot(vec3(0.017369856, 0.148789076, 0.833840896), c));
}

vec3 AWG3ToRec709(vec3 c)
{
    return vec3(
        dot(vec3(1.617524436, -0.537286235, -0.080238194), c),
        dot(vec3(-0.070572684, 1.334612859, -0.264040156), c),
        dot(vec3(-0.021102017, -0.226953556, 1.248055903), c));
}

float Log10(float x)
{
    return log(x) / log(10.0);
}

vec3 Rec709ToDWG(vec3 c)
{
    return vec3(
        dot(vec3(0.562767460, 0.323516518, 0.113715973), c),
        dot(vec3(0.077754626, 0.749577398, 0.172668004), c),
        dot(vec3(0.064669185, 0.191998705, 0.743332142), c));
}

vec3 DWGToRec709(vec3 c)
{
    return vec3(
        dot(vec3(1.898614861, -0.792176192, -0.106438765), c),
        dot(vec3(-0.168948765, 1.488975755, -0.320026913), c),
        dot(vec3(-0.121539126, -0.315675802, 1.437214990), c));
}

vec3 Rec709ToAP1(vec3 c)
{
    return vec3(
        dot(vec3(0.613117813, 0.339538016, 0.047416696), c),
        dot(vec3(0.070193722, 0.916353879, 0.013452398), c),
        dot(vec3(0.020615592, 0.109569773, 0.869814635), c));
}

vec3 AP1ToRec709(vec3 c)
{
    return vec3(
        dot(vec3(1.704858676, -0.621716022, -0.083299372), c),
        dot(vec3(-0.130076824, 1.140735775, -0.010559802), c),
        dot(vec3(-0.023964073, -0.128975508, 1.153014019), c));
}

float LinearToLogC3_1(float x)
{
    x = max(x, 0.0);
    return (x > 0.010591) ? (0.247190 * Log10(5.555556 * x + 0.052272) + 0.385537)
                          : (5.367655 * x + 0.092809);
}

float LogC3ToLinear_1(float x)
{
    return (x > 0.149658) ? ((pow(10.0, (x - 0.385537) / 0.247190) - 0.052272) / 5.555556)
                          : ((x - 0.092809) / 5.367655);
}

vec3 LinearToLogC3(vec3 c)
{
    return vec3(LinearToLogC3_1(c.r), LinearToLogC3_1(c.g), LinearToLogC3_1(c.b));
}

vec3 LogC3ToLinear(vec3 c)
{
    return vec3(LogC3ToLinear_1(c.r), LogC3ToLinear_1(c.g), LogC3ToLinear_1(c.b));
}

// DaVinci Intermediate as Blackmagic publishes it: x * M below the linear cut 0.00262409,
// (log2(x + A) + B) * C above it, A = 0.0075, B = 7, C = 0.07329248, M = 10.44426855; the two
// meet at the encoded cut 0.02740668.
float LinearToDaVinciIntermediate_1(float x)
{
    x = max(x, 0.0);
    return (x <= 0.00262409) ? (x * 10.44426855) : ((log2(x + 0.0075) + 7.0) * 0.07329248);
}

float DaVinciIntermediateToLinear_1(float x)
{
    return (x <= 0.02740668) ? (x / 10.44426855) : (exp2(x / 0.07329248 - 7.0) - 0.0075);
}

vec3 LinearToDaVinciIntermediate(vec3 c)
{
    return vec3(LinearToDaVinciIntermediate_1(c.r), LinearToDaVinciIntermediate_1(c.g), LinearToDaVinciIntermediate_1(c.b));
}

vec3 DaVinciIntermediateToLinear(vec3 c)
{
    return vec3(DaVinciIntermediateToLinear_1(c.r), DaVinciIntermediateToLinear_1(c.g), DaVinciIntermediateToLinear_1(c.b));
}

float LinearToACEScct_1(float x)
{
    x = max(x, 0.0);
    return (x <= 0.0078125) ? (10.540237742 * x + 0.072905534)
                            : ((log2(x) + 9.72) / 17.52);
}

float ACEScctToLinear_1(float x)
{
    return (x <= 0.155251142) ? ((x - 0.072905534) / 10.540237742)
                              : exp2(x * 17.52 - 9.72);
}

vec3 LinearToACEScct(vec3 c)
{
    return vec3(LinearToACEScct_1(c.r), LinearToACEScct_1(c.g), LinearToACEScct_1(c.b));
}

vec3 ACEScctToLinear(vec3 c)
{
    return vec3(ACEScctToLinear_1(c.r), ACEScctToLinear_1(c.g), ACEScctToLinear_1(c.b));
}

float LinearToCineon_1(float x)
{
    const float refWhite = 685.0;
    const float refBlack = 95.0;
    const float densityPerCode = 0.002 / 0.6;
    float blackOffset = pow(10.0, (refBlack - refWhite) * densityPerCode);
    float v = max(x, 0.0) * (1.0 - blackOffset) + blackOffset;
    return (Log10(v) / densityPerCode + refWhite) / 1023.0;
}

float CineonToLinear_1(float x)
{
    const float refWhite = 685.0;
    const float refBlack = 95.0;
    const float densityPerCode = 0.002 / 0.6;
    float blackOffset = pow(10.0, (refBlack - refWhite) * densityPerCode);
    float v = pow(10.0, (x * 1023.0 - refWhite) * densityPerCode);
    return (v - blackOffset) / (1.0 - blackOffset);
}

vec3 LinearToCineon(vec3 c)
{
    return vec3(LinearToCineon_1(c.r), LinearToCineon_1(c.g), LinearToCineon_1(c.b));
}

vec3 CineonToLinear(vec3 c)
{
    return vec3(CineonToLinear_1(c.r), CineonToLinear_1(c.g), CineonToLinear_1(c.b));
}

vec3 GE_EncodeCubeLutInput(vec3 linearRec709, int encoding)
{
    if (encoding == 1)
        return LinearToSRGB(linearRec709);
    if (encoding == 2)
        return LinearToLogC3(Rec709ToAWG3(linearRec709));
    if (encoding == 3)
        return LinearToDaVinciIntermediate(Rec709ToDWG(linearRec709));
    if (encoding == 4)
        return LinearToACEScct(Rec709ToAP1(linearRec709));
    if (encoding == 5)
        return LinearToCineon(linearRec709);
    return linearRec709;
}

vec3 GE_DecodeCubeLutOutput(vec3 lutRgb, int encoding)
{
    if (encoding == 1)
        return SRGBToLinear(lutRgb);
    if (encoding == 2)
        return AWG3ToRec709(LogC3ToLinear(lutRgb));
    if (encoding == 3)
        return DWGToRec709(DaVinciIntermediateToLinear(lutRgb));
    if (encoding == 4)
        return AP1ToRec709(ACEScctToLinear(lutRgb));
    if (encoding == 5)
        return CineonToLinear(lutRgb);
    return lutRgb;
}

vec3 GE_CubeLutApply1DStrip(vec3 lutRgb, GE_CubeLutParams lut, sampler2D lut1d)
{
    int n = max(lut.Size1D, 2);
    float invR = 1.0 / max(lut.In1DMax - lut.In1DMin, 1e-6);
    vec3 t = clamp((lutRgb - vec3(lut.In1DMin)) * invR, 0.0, 1.0);
    float nf = float(n);
    vec2 uvR = vec2((t.r * (nf - 1.0) + 0.5) / nf, (0.0 + 0.5) / 3.0);
    vec2 uvG = vec2((t.g * (nf - 1.0) + 0.5) / nf, (1.0 + 0.5) / 3.0);
    vec2 uvB = vec2((t.b * (nf - 1.0) + 0.5) / nf, (2.0 + 0.5) / 3.0);
    float r = texture(lut1d, uvR).r;
    float g = texture(lut1d, uvG).r;
    float b = texture(lut1d, uvB).r;
    return vec3(r, g, b);
}

vec3 GE_CubeLutApply3DTable(vec3 lutRgb, GE_CubeLutParams lut, sampler3D lut3d)
{
    int n = max(lut.Size3D, 2);
    float invR = 1.0 / max(lut.In3DMax - lut.In3DMin, 1e-6);
    vec3 t = clamp((lutRgb - vec3(lut.In3DMin)) * invR, 0.0, 1.0);
    vec3 coord = (t * (float(n) - 1.0) + 0.5) / float(n);
    return texture(lut3d, coord).rgb;
}

// Applies the LUT to `rgb` at the struct's intensity: encode into the LUT's input encoding, the
// optional 1D shaper, the 3D table, decode back, then blend by intensity. Intensity 0 returns
// `rgb` unchanged.
vec3 GE_ApplyCubeLut(vec3 rgb, GE_CubeLutParams lut, sampler3D lut3d, sampler2D lut1d)
{
    float w = clamp(lut.Intensity, 0.0, 1.0);
    if (w <= 1e-5)
        return rgb;

    vec3 c = GE_EncodeCubeLutInput(rgb, lut.InputEncoding);
    bool has1d = (lut.Flags & 2) != 0;
    bool has3d = (lut.Flags & 1) != 0;
    if (has1d)
        c = GE_CubeLutApply1DStrip(c, lut, lut1d);
    if (has3d)
        c = GE_CubeLutApply3DTable(c, lut, lut3d);
    c = GE_DecodeCubeLutOutput(c, lut.InputEncoding);
    return mix(rgb, max(c, vec3(0.0)), w);
}

// A GE_CubeLutParams from a push block that declares GE_CUBE_LUT_PUSH_MEMBERS.
#define GE_CUBE_LUT_PARAMS(block)                                                                 \
    GE_CubeLutParams(block.lutIntensity, block.lutInputEncoding, block.lutSize3d, block.lutSize1d, \
                     block.lutFlags, block.lut1dInMin, block.lut1dInMax, block.lut3dInMin,         \
                     block.lut3dInMax)

#endif // GE_CUBE_LUT_GLSL
