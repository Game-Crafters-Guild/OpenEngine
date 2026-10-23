#include "TerrainECS/TerrainZonePayload.h"

#include "Logger/Logger.h"

#include <cmath>
#include <cstring>

namespace GameEngine::TerrainECS
{
namespace
{

// 'T','Z','O','N' little-endian. Bumped only on an incompatible layout change.
constexpr uint32 kZonePayloadMagic = 0x4E4F5A54u;
constexpr uint32 kZonePayloadVersion = 1u;

// Header: magic, version, format, width, height (5 x uint32).
constexpr std::size_t kHeaderBytes = 5 * sizeof(uint32);

void AppendU32(std::vector<uint8>& out, uint32 value)
{
    const std::size_t offset = out.size();
    out.resize(offset + sizeof(uint32));
    std::memcpy(out.data() + offset, &value, sizeof(uint32));
}

uint32 ReadU32(const uint8* data, std::size_t offset)
{
    uint32 value = 0;
    std::memcpy(&value, data + offset, sizeof(uint32));
    return value;
}

} // namespace

std::vector<uint8> EncodeZonePayload(const TerrainZonePayload& payload)
{
    const std::size_t dataBytes = payload.TexelCount() * ZonePayloadBytesPerTexel(payload.Format);

    std::vector<uint8> out;
    out.reserve(kHeaderBytes + dataBytes);
    AppendU32(out, kZonePayloadMagic);
    AppendU32(out, kZonePayloadVersion);
    AppendU32(out, static_cast<uint32>(payload.Format));
    AppendU32(out, payload.Width);
    AppendU32(out, payload.Height);

    const std::size_t offset = out.size();
    out.resize(offset + dataBytes);
    if (dataBytes > 0)
        std::memcpy(out.data() + offset, payload.TexelBytes(), dataBytes);
    return out;
}

bool DecodeZonePayload(const uint8* data, std::size_t size, TerrainZonePayload& out)
{
    if (data == nullptr || size < kHeaderBytes)
        return false;
    if (ReadU32(data, 0) != kZonePayloadMagic || ReadU32(data, sizeof(uint32)) != kZonePayloadVersion)
        return false;

    const uint32 formatRaw = ReadU32(data, 2 * sizeof(uint32));
    const uint32 width = ReadU32(data, 3 * sizeof(uint32));
    const uint32 height = ReadU32(data, 4 * sizeof(uint32));
    if (formatRaw > static_cast<uint32>(kLastZonePayloadFormat))
        return false;
    // Cap dimensions before any width*height math so a crafted header can't
    // overflow the texel count or force a huge allocation.
    if (width > kMaxZoneDimension || height > kMaxZoneDimension)
        return false;

    const auto format = static_cast<ZonePayloadFormat>(formatRaw);
    const std::size_t dataBytes = static_cast<std::size_t>(width) * height * ZonePayloadBytesPerTexel(format);
    if (size - kHeaderBytes < dataBytes)
        return false;

    TerrainZonePayload decoded;
    decoded.Allocate(format, width, height);
    if (dataBytes > 0)
        std::memcpy(decoded.MutableTexelBytes(), data + kHeaderBytes, dataBytes);
    // A colour value or weight that is NaN or infinite would paint a wrong colour or erase what the
    // map holds under the zone, so the payload is refused whole.
    for (std::size_t i = 0; i < decoded.Color.size(); ++i)
    {
        if (!std::isfinite(decoded.Color[i]))
        {
            Logger::Log::Warning("Terrain zone payload: texel {} of a color zone holds NaN or infinity. Delete the "
                                 "zone's .tzone file so the zone loads empty.",
                                 i / kZonePayloadColorChannels);
            return false;
        }
    }
    out = std::move(decoded);
    return true;
}

} // namespace GameEngine::TerrainECS
