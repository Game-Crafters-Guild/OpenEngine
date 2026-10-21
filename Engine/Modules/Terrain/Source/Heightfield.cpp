#include "Terrain/Heightfield.h"

#include "Image/Decode.h"
#include "Image/ImageHeader.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace GameEngine::Terrain
{

HeightfieldData::HeightfieldData(uint32 width, uint32 height, float32 defaultValue)
    : m_Width(width)
    , m_Height(height)
    , m_Samples(static_cast<std::size_t>(width) * height, defaultValue)
{
}

float32 HeightfieldData::GetSample(uint32 x, uint32 z) const
{
    assert(x < m_Width && z < m_Height);
    return m_Samples[static_cast<std::size_t>(z) * m_Width + x];
}

void HeightfieldData::SetSample(uint32 x, uint32 z, float32 value)
{
    assert(x < m_Width && z < m_Height);
    m_Samples[static_cast<std::size_t>(z) * m_Width + x] = value;
}

float32 HeightfieldData::SampleBilinear(float32 u, float32 v) const
{
    if (m_Samples.empty())
        return 0.0f;

    const float32 fx = u * static_cast<float32>(m_Width - 1);
    const float32 fz = v * static_cast<float32>(m_Height - 1);

    const int32 x0 = static_cast<int32>(std::floor(fx));
    const int32 z0 = static_cast<int32>(std::floor(fz));
    const int32 x1 = std::min(x0 + 1, static_cast<int32>(m_Width - 1));
    const int32 z1 = std::min(z0 + 1, static_cast<int32>(m_Height - 1));

    const float32 fracX = fx - static_cast<float32>(x0);
    const float32 fracZ = fz - static_cast<float32>(z0);

    const uint32 ux0 = static_cast<uint32>(std::max(x0, 0));
    const uint32 uz0 = static_cast<uint32>(std::max(z0, 0));
    const uint32 ux1 = static_cast<uint32>(x1);
    const uint32 uz1 = static_cast<uint32>(z1);

    const float32 h00 = GetSample(ux0, uz0);
    const float32 h10 = GetSample(ux1, uz0);
    const float32 h01 = GetSample(ux0, uz1);
    const float32 h11 = GetSample(ux1, uz1);

    const float32 h0 = h00 + fracX * (h10 - h00);
    const float32 h1 = h01 + fracX * (h11 - h01);

    return h0 + fracZ * (h1 - h0);
}

Mathematics::Vector3 HeightfieldData::ComputeNormal(int32 x, int32 z,
                                                     float32 spacingX, float32 spacingZ) const
{
    const int32 w = static_cast<int32>(m_Width);
    const int32 h = static_cast<int32>(m_Height);

    const int32 xm = std::max(x - 1, 0);
    const int32 xp = std::min(x + 1, w - 1);
    const int32 zm = std::max(z - 1, 0);
    const int32 zp = std::min(z + 1, h - 1);

    const float32 hL = GetSample(static_cast<uint32>(xm), static_cast<uint32>(z));
    const float32 hR = GetSample(static_cast<uint32>(xp), static_cast<uint32>(z));
    const float32 hD = GetSample(static_cast<uint32>(x), static_cast<uint32>(zm));
    const float32 hU = GetSample(static_cast<uint32>(x), static_cast<uint32>(zp));

    // Central differences
    const float32 dx = (hR - hL) / (spacingX * static_cast<float32>(xp - xm));
    const float32 dz = (hU - hD) / (spacingZ * static_cast<float32>(zp - zm));

    // Normal = normalize(-dh/dx, 1, -dh/dz) -- Y-up convention
    Mathematics::Vector3 normal(-dx, 1.0f, -dz);
    const float32 len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len > 0.0f)
    {
        normal.x /= len;
        normal.y /= len;
        normal.z /= len;
    }
    return normal;
}

Mathematics::Vector3 HeightfieldData::ComputeNormalWithNeighbor(
    int32 x, int32 z, float32 spacingX, float32 spacingZ,
    const HeightfieldData* neighborLeft,
    const HeightfieldData* neighborRight,
    const HeightfieldData* neighborUp,
    const HeightfieldData* neighborDown) const
{
    const int32 w = static_cast<int32>(m_Width);
    const int32 h = static_cast<int32>(m_Height);

    // Left sample (X-1)
    float32 hL;
    if (x > 0)
        hL = GetSample(static_cast<uint32>(x - 1), static_cast<uint32>(z));
    else if (neighborLeft && z < static_cast<int32>(neighborLeft->GetHeight()))
        hL = neighborLeft->GetSample(neighborLeft->GetWidth() - 2, static_cast<uint32>(z));
    else
        hL = GetSample(0, static_cast<uint32>(z));

    // Right sample (X+1)
    float32 hR;
    if (x < w - 1)
        hR = GetSample(static_cast<uint32>(x + 1), static_cast<uint32>(z));
    else if (neighborRight && z < static_cast<int32>(neighborRight->GetHeight()))
        hR = neighborRight->GetSample(1, static_cast<uint32>(z));
    else
        hR = GetSample(static_cast<uint32>(w - 1), static_cast<uint32>(z));

    // Down sample (Z-1)
    float32 hD;
    if (z > 0)
        hD = GetSample(static_cast<uint32>(x), static_cast<uint32>(z - 1));
    else if (neighborUp && x < static_cast<int32>(neighborUp->GetWidth()))
        hD = neighborUp->GetSample(static_cast<uint32>(x), neighborUp->GetHeight() - 2);
    else
        hD = GetSample(static_cast<uint32>(x), 0);

    // Up sample (Z+1)
    float32 hU;
    if (z < h - 1)
        hU = GetSample(static_cast<uint32>(x), static_cast<uint32>(z + 1));
    else if (neighborDown && x < static_cast<int32>(neighborDown->GetWidth()))
        hU = neighborDown->GetSample(static_cast<uint32>(x), 1);
    else
        hU = GetSample(static_cast<uint32>(x), static_cast<uint32>(h - 1));

    // Central differences (always 2 samples apart when neighbors are available)
    const float32 dx = (hR - hL) / (spacingX * 2.0f);
    const float32 dz = (hU - hD) / (spacingZ * 2.0f);

    Mathematics::Vector3 normal(-dx, 1.0f, -dz);
    const float32 len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len > 0.0f)
    {
        normal.x /= len;
        normal.y /= len;
        normal.z /= len;
    }
    return normal;
}

void HeightfieldData::GetMinMax(int32 x0, int32 z0, int32 sizeX, int32 sizeZ,
                                 float32& outMin, float32& outMax) const
{
    outMin = std::numeric_limits<float32>::max();
    outMax = std::numeric_limits<float32>::lowest();

    const int32 endX = std::min(x0 + sizeX, static_cast<int32>(m_Width));
    const int32 endZ = std::min(z0 + sizeZ, static_cast<int32>(m_Height));
    const int32 startX = std::max(x0, 0);
    const int32 startZ = std::max(z0, 0);

    for (int32 z = startZ; z < endZ; ++z)
    {
        for (int32 x = startX; x < endX; ++x)
        {
            const float32 val = GetSample(static_cast<uint32>(x), static_cast<uint32>(z));
            outMin = std::min(outMin, val);
            outMax = std::max(outMax, val);
        }
    }

    if (outMin > outMax)
    {
        outMin = 0.0f;
        outMax = 0.0f;
    }
}

namespace
{

// Reads a whole file in one sized read. False when it cannot be opened or read.
bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& outBytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const std::streamoff size = file.tellg();
    if (size < 0)
        return false;
    outBytes.resize(static_cast<size_t>(size));
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(size)));
}

} // namespace

std::string ResolvePngHeightmapSize(const std::filesystem::path& file, PngHeightmapSize& out)
{
    const Image::ImageHeader header = Image::ReadImageHeader(file);
    if (!header.Valid)
        return "the PNG " + file.filename().string() + " could not be read";
    // LoadFromPNG16 decodes to one 16-bit sample per pixel, held in one int-sized block, whatever
    // the file stores: an 8-bit PNG the decoder reads can still be too large for that result.
    const uint64 heightBytes = static_cast<uint64>(header.Width) * header.Height * sizeof(uint16);
    if (!header.WithinDecoderLimits || heightBytes > INT_MAX)
        return "the PNG " + file.filename().string() + " is " + std::to_string(header.Width) + " x " +
               std::to_string(header.Height) +
               " samples, more than a PNG heightmap can hold (a PNG decodes whole to 16 bits: up to 32768 x "
               "32766 samples for a 16-bit PNG, 32768 x 32767 for an 8-bit one); export the heightmap as .r32, "
               "which has no size limit";
    out = PngHeightmapSize{header.Width, header.Height};
    return {};
}

bool HeightfieldData::LoadFromPNG16(const std::filesystem::path& path)
{
    std::vector<uint8_t> fileBytes;
    if (!ReadWholeFile(path, fileBytes))
        return false;
    const Image::Image16 decoded = Image::Decode16(fileBytes, path.string(), Image::DecodeChannels::Gray);
    if (!decoded.Valid)
        return false;

    m_Width = decoded.Width;
    m_Height = decoded.Height;
    m_Samples.resize(static_cast<std::size_t>(m_Width) * m_Height);

    // Normalize 16-bit [0, 65535] to [0, 1]
    constexpr float32 kInv65535 = 1.0f / 65535.0f;
    const auto totalSamples = static_cast<std::size_t>(m_Width) * m_Height;
    for (std::size_t i = 0; i < totalSamples; ++i)
    {
        m_Samples[i] = static_cast<float32>(decoded.Pixels[i]) * kInv65535;
    }
    return true;
}

bool HeightfieldData::LoadFromRawFloat(const std::filesystem::path& path, uint32 width, uint32 height)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    const auto expectedSize = static_cast<std::size_t>(width) * height * sizeof(float32);
    m_Width = width;
    m_Height = height;
    m_Samples.resize(static_cast<std::size_t>(width) * height);

    file.read(reinterpret_cast<char*>(m_Samples.data()), static_cast<std::streamsize>(expectedSize));
    if (!file.good())
    {
        m_Samples.clear();
        m_Width = 0;
        m_Height = 0;
        return false;
    }

    return true;
}

bool HeightfieldData::LoadFromRawUInt16(const std::filesystem::path& path, uint32 width, uint32 height)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    const auto sampleCount = static_cast<std::size_t>(width) * height;
    std::vector<uint16> raw(sampleCount);
    file.read(reinterpret_cast<char*>(raw.data()),
              static_cast<std::streamsize>(sampleCount * sizeof(uint16)));
    if (!file.good())
        return false;

    m_Width = width;
    m_Height = height;
    m_Samples.resize(sampleCount);

    constexpr float32 kInv65535 = 1.0f / 65535.0f;
    for (std::size_t i = 0; i < sampleCount; ++i)
        m_Samples[i] = static_cast<float32>(raw[i]) * kInv65535;

    return true;
}

void HeightfieldData::Fill(float32 value)
{
    std::fill(m_Samples.begin(), m_Samples.end(), value);
}

// Simple hash-based value noise with smoothstep interpolation (no external dependency)
namespace
{
static float32 NoiseHash(int32 x, int32 z, uint32 seed)
{
    uint32 h = static_cast<uint32>(x) * 374761393u +
               static_cast<uint32>(z) * 668265263u +
               seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float32>(h & 0x7FFFFFFF) / static_cast<float32>(0x7FFFFFFF);
}

static float32 SmoothNoise(float32 x, float32 z, uint32 seed)
{
    const int32 ix = static_cast<int32>(std::floor(x));
    const int32 iz = static_cast<int32>(std::floor(z));
    const float32 fx = x - static_cast<float32>(ix);
    const float32 fz = z - static_cast<float32>(iz);

    // Smoothstep interpolation
    const float32 sx = fx * fx * (3.0f - 2.0f * fx);
    const float32 sz = fz * fz * (3.0f - 2.0f * fz);

    const float32 n00 = NoiseHash(ix, iz, seed);
    const float32 n10 = NoiseHash(ix + 1, iz, seed);
    const float32 n01 = NoiseHash(ix, iz + 1, seed);
    const float32 n11 = NoiseHash(ix + 1, iz + 1, seed);

    const float32 nx0 = n00 + sx * (n10 - n00);
    const float32 nx1 = n01 + sx * (n11 - n01);
    return nx0 + sz * (nx1 - nx0);
}
} // namespace

void HeightfieldData::FillWithNoise(float32 frequency, float32 amplitude,
                                     uint32 octaves, uint32 seed)
{
    FillRegionWithNoise(frequency, amplitude,
                        0, 0,
                        static_cast<int32>(m_Width) - 1, static_cast<int32>(m_Height) - 1,
                        octaves, seed);
}

void HeightfieldData::FillRegionWithNoise(float32 frequency, float32 amplitude,
                                           int32 minX, int32 minZ, int32 maxX, int32 maxZ,
                                           uint32 octaves, uint32 seed)
{
    if (m_Samples.empty())
        return;

    const uint32 x0 = static_cast<uint32>(std::max(minX, 0));
    const uint32 z0 = static_cast<uint32>(std::max(minZ, 0));
    const uint32 x1 = static_cast<uint32>(std::min(maxX, static_cast<int32>(m_Width) - 1));
    const uint32 z1 = static_cast<uint32>(std::min(maxZ, static_cast<int32>(m_Height) - 1));
    if (x0 > x1 || z0 > z1)
        return;

    for (uint32 z = z0; z <= z1; ++z)
    {
        for (uint32 x = x0; x <= x1; ++x)
        {
            float32 value = 0.0f;
            float32 freq = frequency;
            float32 amp = 1.0f;
            float32 totalAmp = 0.0f;

            for (uint32 o = 0; o < octaves; ++o)
            {
                const float32 nx = static_cast<float32>(x) / static_cast<float32>(m_Width) * freq;
                const float32 nz = static_cast<float32>(z) / static_cast<float32>(m_Height) * freq;
                value += SmoothNoise(nx, nz, seed + o) * amp;
                totalAmp += amp;
                freq *= 2.0f;
                amp *= 0.5f;
            }

            value = (value / totalAmp) * amplitude;
            SetSample(x, z, value);
        }
    }
}

void HeightfieldData::FillWithNoiseWorldSpace(float32 frequency, float32 amplitude,
                                                float32 worldOriginX, float32 worldOriginZ,
                                                float32 worldSizeX, float32 worldSizeZ,
                                                uint32 octaves, uint32 seed)
{
    FillRegionWithNoiseWorldSpace(frequency, amplitude, worldOriginX, worldOriginZ,
                                  worldSizeX, worldSizeZ,
                                  0, 0,
                                  static_cast<int32>(m_Width) - 1, static_cast<int32>(m_Height) - 1,
                                  octaves, seed);
}

void HeightfieldData::FillRegionWithNoiseWorldSpace(float32 frequency, float32 amplitude,
                                                     float32 worldOriginX, float32 worldOriginZ,
                                                     float32 worldSizeX, float32 worldSizeZ,
                                                     int32 minX, int32 minZ, int32 maxX, int32 maxZ,
                                                     uint32 octaves, uint32 seed)
{
    if (m_Samples.empty())
        return;

    const uint32 x0 = static_cast<uint32>(std::max(minX, 0));
    const uint32 z0 = static_cast<uint32>(std::max(minZ, 0));
    const uint32 x1 = static_cast<uint32>(std::min(maxX, static_cast<int32>(m_Width) - 1));
    const uint32 z1 = static_cast<uint32>(std::min(maxZ, static_cast<int32>(m_Height) - 1));
    if (x0 > x1 || z0 > z1)
        return;

    const float32 spacingX = worldSizeX / static_cast<float32>(m_Width - 1);
    const float32 spacingZ = worldSizeZ / static_cast<float32>(m_Height - 1);

    for (uint32 z = z0; z <= z1; ++z)
    {
        for (uint32 x = x0; x <= x1; ++x)
        {
            // World-space position of this sample.
            const float32 wx = worldOriginX + static_cast<float32>(x) * spacingX;
            const float32 wz = worldOriginZ + static_cast<float32>(z) * spacingZ;
            SetSample(x, z, SampleWorldSpaceNoise(wx, wz, frequency, amplitude, octaves, seed));
        }
    }
}

float32 SampleWorldSpaceNoise(float32 worldX, float32 worldZ, float32 frequency, float32 amplitude, uint32 octaves,
                              uint32 seed, float32 minResolvedCellMeters)
{
    constexpr float32 kValueNoiseMean = 0.5f;
    float32 value = 0.0f;
    float32 freq = frequency;
    float32 amp = 1.0f;
    float32 totalAmp = 0.0f;

    for (uint32 o = 0; o < octaves; ++o)
    {
        if (freq * minResolvedCellMeters > 1.0f)
        {
            value += kValueNoiseMean * amp;
        }
        else
        {
            const float32 nx = worldX * freq;
            const float32 nz = worldZ * freq;
            value += SmoothNoise(nx, nz, seed + o) * amp;
        }
        totalAmp += amp;
        freq *= 2.0f;
        amp *= 0.5f;
    }

    return (value / totalAmp) * amplitude;
}

void HeightfieldData::Resize(uint32 width, uint32 height, float32 defaultValue)
{
    m_Width = width;
    m_Height = height;
    m_Samples.assign(static_cast<std::size_t>(width) * height, defaultValue);
}

} // namespace GameEngine::Terrain
