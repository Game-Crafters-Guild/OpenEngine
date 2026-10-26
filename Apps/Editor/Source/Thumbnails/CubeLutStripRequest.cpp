#include "Thumbnails/CubeLutStripRequest.h"

#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <vector>

namespace GameEngine
{
namespace
{
using Rendering::Passes::CubeLutPreviewSource;

constexpr std::string_view kEnginePrefix = "engine:";
constexpr std::string_view kKeyPrefix = "cubelut-strip-";
constexpr size_t kKeyFieldCount = 8; // guid, owner, look, intensity, stage, source, extent, content
constexpr float kIntensityUnitsPerOne = 1000.0f;
constexpr uint32_t kStageCount = 2;

constexpr std::string_view ContentName(CubeLutStripContent content)
{
    switch (content)
    {
    case CubeLutStripContent::LookAlone:
        return "look";
    case CubeLutStripContent::WithLut:
        return "lut";
    case CubeLutStripContent::Sheet:
        return "sheet";
    }
    return "lut";
}

std::optional<CubeLutStripContent> ContentFromName(std::string_view name)
{
    for (const CubeLutStripContent content :
         {CubeLutStripContent::LookAlone, CubeLutStripContent::WithLut, CubeLutStripContent::Sheet})
    {
        if (ContentName(content) == name)
            return content;
    }
    return std::nullopt;
}

constexpr std::string_view SourceName(CubeLutPreviewSource source)
{
    return source == CubeLutPreviewSource::Probes ? "probes" : "ramp";
}

std::optional<CubeLutPreviewSource> SourceFromName(std::string_view name)
{
    if (name == SourceName(CubeLutPreviewSource::Ramp))
        return CubeLutPreviewSource::Ramp;
    if (name == SourceName(CubeLutPreviewSource::Probes))
        return CubeLutPreviewSource::Probes;
    return std::nullopt;
}

template <typename T>
bool ParseNumber(std::string_view text, T& out)
{
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, out);
    return result.ec == std::errc() && result.ptr == end;
}

// A field "<tag><number>" (o12, l1, i800, s0).
template <typename T>
bool ParseTagged(std::string_view field, char tag, T& out)
{
    return !field.empty() && field.front() == tag && ParseNumber(field.substr(1), out);
}
} // namespace

std::string MakeCubeLutStripKey(const CubeLutStripRequest& request)
{
    if (request.Lut.IsNull())
        return {};
    // Intensity in thousandths: finer than a slider step, so equal settings share a texture. The
    // look-alone band does not depend on the LUT's intensity or stage, so they are not named.
    const bool usesLut = request.Content != CubeLutStripContent::LookAlone;
    const int32_t intensity =
        usesLut ? static_cast<int32_t>(std::lround(Math::Clamp01(request.Intensity) * kIntensityUnitsPerOne))
                : 0;
    const uint32_t stage = usesLut ? std::min(request.Stage, kStageCount - 1u) : 0u;
    // The sheet holds both sources; its name carries the ramp's.
    const CubeLutPreviewSource source =
        request.Content == CubeLutStripContent::Sheet ? CubeLutPreviewSource::Ramp : request.Source;
    std::string key(kKeyPrefix);
    key += request.Lut.ToCompactString();
    key += "-o" + std::to_string(request.Owner);
    key += "-l" + std::to_string(request.Look);
    key += "-i" + std::to_string(intensity);
    key += "-s" + std::to_string(stage);
    key += "-";
    key += SourceName(source);
    key += "-" + std::to_string(request.Width) + "x" + std::to_string(request.Height);
    key += "-";
    key += ContentName(request.Content);
    return key;
}

std::optional<CubeLutStripRequest> ParseCubeLutStripKey(std::string_view key)
{
    if (key.substr(0, kEnginePrefix.size()) == kEnginePrefix)
        key.remove_prefix(kEnginePrefix.size());
    if (key.substr(0, kKeyPrefix.size()) != kKeyPrefix)
        return std::nullopt;
    key.remove_prefix(kKeyPrefix.size());

    std::vector<std::string_view> fields;
    while (!key.empty() && fields.size() <= kKeyFieldCount)
    {
        const size_t dash = key.find('-');
        fields.push_back(key.substr(0, dash));
        key = dash == std::string_view::npos ? std::string_view{} : key.substr(dash + 1);
    }
    if (fields.size() != kKeyFieldCount)
        return std::nullopt;

    CubeLutStripRequest request{};
    request.Lut = GUID(std::string(fields[0]));
    int32_t intensity = 0;
    const size_t x = fields[6].find('x');
    const auto source = SourceFromName(fields[5]);
    const auto content = ContentFromName(fields[7]);
    if (request.Lut.IsNull() || !ParseTagged(fields[1], 'o', request.Owner) ||
        !ParseTagged(fields[2], 'l', request.Look) || !ParseTagged(fields[3], 'i', intensity) ||
        !ParseTagged(fields[4], 's', request.Stage) || request.Stage >= kStageCount || !source ||
        x == std::string_view::npos || !ParseNumber(fields[6].substr(0, x), request.Width) ||
        !ParseNumber(fields[6].substr(x + 1), request.Height) || !content)
        return std::nullopt;
    request.Intensity = static_cast<float>(intensity) / kIntensityUnitsPerOne;
    request.Source = *source;
    request.Content = *content;
    return request;
}

} // namespace GameEngine
