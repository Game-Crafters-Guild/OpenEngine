#include "StarCatalog.h"

#include "Mathematics/Interpolation.h"
#include "Types/Color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>

namespace GameEngine::Rendering
{

namespace
{

// The naked-eye sky's cumulative star count: N(< m) = kReferenceCount x 10^(kCountSlope (m - kReferenceMagnitude)).
constexpr double kReferenceMagnitude = 6.5;
constexpr double kReferenceCount = 9000.0;
constexpr double kCountSlope = 0.5;
// Sirius, the brightest star of the night sky.
constexpr float kBrightestMagnitude = -1.46f;

// The spectral classes of naked-eye stars (the Bright Star Catalogue's mix) with the class edges'
// temperatures in kelvin, hot edge first.
struct SpectralClass
{
    double Share;
    float HotKelvin;
    float CoolKelvin;
};
constexpr std::array<SpectralClass, 6> kSpectralMix = {{
    {0.20, 25000.0f, 10000.0f}, // B
    {0.22, 10000.0f, 7500.0f},  // A
    {0.13, 7500.0f, 6000.0f},   // F
    {0.14, 6000.0f, 5200.0f},   // G
    {0.27, 5200.0f, 3900.0f},   // K
    {0.04, 3900.0f, 2800.0f},   // M
}};

// A uniform value in [0, 1) from the generator's raw 32-bit output. std::mt19937's output sequence is
// fixed by the standard; the library's real distributions are not, so they are not used.
double Uniform01(std::mt19937& rng)
{
    return static_cast<double>(rng() >> 8) * (1.0 / 16777216.0);
}

float MagnitudeOfRank(uint32_t rank)
{
    const double count = static_cast<double>(rank) + 0.5;
    const double magnitude = kReferenceMagnitude + std::log10(count / kReferenceCount) / kCountSlope;
    return std::max(static_cast<float>(magnitude), kBrightestMagnitude);
}

float TemperatureOfStar(double classPick, double withinClass)
{
    double cumulative = 0.0;
    for (const SpectralClass& spectral : kSpectralMix)
    {
        cumulative += spectral.Share;
        if (classPick < cumulative)
            return spectral.CoolKelvin + static_cast<float>(withinClass) * (spectral.HotKelvin - spectral.CoolKelvin);
    }
    return kSpectralMix.back().CoolKelvin;
}

// One lobe of the CIE 1931 colour matching function fit: a Gaussian with a different width on
// each side of its peak.
double CieLobe(double nm, double peak, double widthBelow, double widthAbove)
{
    const double t = (nm - peak) / (nm < peak ? widthBelow : widthAbove);
    return std::exp(-0.5 * t * t);
}

// The temperature-independent terms of the blackbody integral at each sample wavelength.
struct SpectralSample
{
    double Metres;
    double MetresPow5;
    double X;
    double Y;
    double Z;
};
constexpr double kSpectralFirstNm = 380.0;
constexpr double kSpectralLastNm = 780.0;
constexpr double kSpectralStepNm = 2.0;
constexpr size_t kSpectralSampleCount = static_cast<size_t>((kSpectralLastNm - kSpectralFirstNm) / kSpectralStepNm) + 1;

std::array<SpectralSample, kSpectralSampleCount> BuildSpectralSamples()
{
    std::array<SpectralSample, kSpectralSampleCount> samples{};
    double nm = kSpectralFirstNm;
    for (SpectralSample& sample : samples)
    {
        sample.Metres = nm * 1e-9;
        sample.MetresPow5 = std::pow(sample.Metres, 5.0);
        sample.X = 1.056 * CieLobe(nm, 599.8, 37.9, 31.0) + 0.362 * CieLobe(nm, 442.0, 16.0, 26.7) -
                   0.065 * CieLobe(nm, 501.1, 20.4, 26.2);
        sample.Y = 0.821 * CieLobe(nm, 568.8, 46.9, 40.5) + 0.286 * CieLobe(nm, 530.9, 16.3, 31.1);
        sample.Z = 1.217 * CieLobe(nm, 437.0, 11.8, 36.0) + 0.681 * CieLobe(nm, 459.0, 26.0, 13.8);
        nm += kSpectralStepNm;
    }
    return samples;
}

// BuildStarField reads star colours from a table of BlackbodyLinearRec709 over the spectral mix's
// temperature range, at even steps of reciprocal temperature (1/K), where the colour is close to linear.
// With 256 entries the error is about 6e-6 per channel; StarCatalogTests holds every star within 1e-4.
constexpr double kBlackbodyTableHottestKelvin = kSpectralMix.front().HotKelvin;
constexpr double kBlackbodyTableCoolestKelvin = kSpectralMix.back().CoolKelvin;
constexpr size_t kBlackbodyTableEntryCount = 256;
constexpr double kBlackbodyTableFirstReciprocalKelvin = 1.0 / kBlackbodyTableHottestKelvin;
constexpr double kBlackbodyTableReciprocalKelvinStep =
    (1.0 / kBlackbodyTableCoolestKelvin - kBlackbodyTableFirstReciprocalKelvin) / static_cast<double>(kBlackbodyTableEntryCount - 1);

using BlackbodyTable = std::array<ColorLinear, kBlackbodyTableEntryCount>;

BlackbodyTable BuildBlackbodyTable()
{
    BlackbodyTable table{};
    for (size_t index = 0; index < table.size(); ++index)
    {
        const double reciprocalKelvin =
            kBlackbodyTableFirstReciprocalKelvin + static_cast<double>(index) * kBlackbodyTableReciprocalKelvinStep;
        table[index] = BlackbodyLinearRec709(static_cast<float>(1.0 / reciprocalKelvin));
    }
    return table;
}

// Temperatures outside the table's range read its nearest edge.
ColorLinear BlackbodyFromTable(const BlackbodyTable& table, float kelvin)
{
    const double position = (1.0 / static_cast<double>(kelvin) - kBlackbodyTableFirstReciprocalKelvin) /
                            kBlackbodyTableReciprocalKelvinStep;
    const double clamped = std::clamp(position, 0.0, static_cast<double>(kBlackbodyTableEntryCount - 1));
    const size_t index = std::min(static_cast<size_t>(clamped), kBlackbodyTableEntryCount - 2);
    const float t = static_cast<float>(clamped - static_cast<double>(index));
    const ColorLinear& below = table[index];
    const ColorLinear& above = table[index + 1];
    return ColorLinear(Math::Lerp(below.r, above.r, t), Math::Lerp(below.g, above.g, t), Math::Lerp(below.b, above.b, t));
}

} // namespace

ColorLinear BlackbodyLinearRec709(float kelvin)
{
    constexpr double kSecondRadiation = 1.4387769e-2; // m K
    static const std::array<SpectralSample, kSpectralSampleCount> kSamples = BuildSpectralSamples();
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    for (const SpectralSample& sample : kSamples)
    {
        const double planck = 1.0 / (sample.MetresPow5 * (std::exp(kSecondRadiation / (sample.Metres * kelvin)) - 1.0));
        x += planck * sample.X;
        y += planck * sample.Y;
        z += planck * sample.Z;
    }
    x /= y;
    z /= y;
    y = 1.0;
    std::array<double, 3> rgb = {3.2406 * x - 1.5372 * y - 0.4986 * z, -0.9689 * x + 1.8758 * y + 0.0415 * z,
                                 0.0557 * x - 0.2040 * y + 1.0570 * z};
    for (double& channel : rgb)
        channel = std::max(channel, 0.0);
    const double luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
    return ColorLinear(static_cast<float>(rgb[0] / luminance), static_cast<float>(rgb[1] / luminance),
                       static_cast<float>(rgb[2] / luminance));
}

uint32_t StarFieldCountBrighterThan(float magnitude)
{
    const double clamped = std::min(static_cast<double>(magnitude), static_cast<double>(kStarFieldFaintestMagnitude));
    const double count = kReferenceCount * std::pow(10.0, kCountSlope * (clamped - kReferenceMagnitude));
    return static_cast<uint32_t>(std::max(std::floor(count + 0.5), 0.0));
}

std::vector<StarInstanceGPU> BuildStarField()
{
    const uint32_t starCount = StarFieldCountBrighterThan(kStarFieldFaintestMagnitude);
    std::vector<StarInstanceGPU> stars;
    stars.reserve(starCount);

    const BlackbodyTable blackbodyTable = BuildBlackbodyTable();
    std::mt19937 rng(42u);
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    for (uint32_t rank = 0; rank < starCount; ++rank)
    {
        const double z = 2.0 * Uniform01(rng) - 1.0;
        const double azimuth = kTwoPi * Uniform01(rng);
        const double ring = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double classPick = Uniform01(rng);
        const double withinClass = Uniform01(rng);
        const double seed = Uniform01(rng);

        StarInstanceGPU star{};
        star.dirAndMagnitude[0] = static_cast<float>(ring * std::cos(azimuth));
        star.dirAndMagnitude[1] = static_cast<float>(z);
        star.dirAndMagnitude[2] = static_cast<float>(ring * std::sin(azimuth));
        star.dirAndMagnitude[3] = MagnitudeOfRank(rank);
        const ColorLinear color = BlackbodyFromTable(blackbodyTable, TemperatureOfStar(classPick, withinClass));
        star.colorAndSeed[0] = color.r;
        star.colorAndSeed[1] = color.g;
        star.colorAndSeed[2] = color.b;
        star.colorAndSeed[3] = static_cast<float>(seed);
        stars.push_back(star);
    }
    return stars;
}

} // namespace GameEngine::Rendering
