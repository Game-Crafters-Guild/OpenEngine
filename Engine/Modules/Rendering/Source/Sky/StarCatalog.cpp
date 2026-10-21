#include "StarCatalog.h"

#include <algorithm>
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

} // namespace

std::array<float, 3> BlackbodyLinearRec709(float kelvin)
{
    constexpr double kSecondRadiation = 1.4387769e-2; // m K
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    for (double nm = 380.0; nm <= 780.0; nm += 2.0)
    {
        const double metres = nm * 1e-9;
        const double planck = 1.0 / (std::pow(metres, 5.0) * (std::exp(kSecondRadiation / (metres * kelvin)) - 1.0));
        x += planck * (1.056 * CieLobe(nm, 599.8, 37.9, 31.0) + 0.362 * CieLobe(nm, 442.0, 16.0, 26.7) -
                       0.065 * CieLobe(nm, 501.1, 20.4, 26.2));
        y += planck * (0.821 * CieLobe(nm, 568.8, 46.9, 40.5) + 0.286 * CieLobe(nm, 530.9, 16.3, 31.1));
        z += planck * (1.217 * CieLobe(nm, 437.0, 11.8, 36.0) + 0.681 * CieLobe(nm, 459.0, 26.0, 13.8));
    }
    x /= y;
    z /= y;
    y = 1.0;
    std::array<double, 3> rgb = {3.2406 * x - 1.5372 * y - 0.4986 * z, -0.9689 * x + 1.8758 * y + 0.0415 * z,
                                 0.0557 * x - 0.2040 * y + 1.0570 * z};
    for (double& channel : rgb)
        channel = std::max(channel, 0.0);
    const double luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
    return {static_cast<float>(rgb[0] / luminance), static_cast<float>(rgb[1] / luminance),
            static_cast<float>(rgb[2] / luminance)};
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
        const std::array<float, 3> color = BlackbodyLinearRec709(TemperatureOfStar(classPick, withinClass));
        star.colorAndSeed[0] = color[0];
        star.colorAndSeed[1] = color[1];
        star.colorAndSeed[2] = color[2];
        star.colorAndSeed[3] = static_cast<float>(seed);
        stars.push_back(star);
    }
    return stars;
}

} // namespace GameEngine::Rendering
