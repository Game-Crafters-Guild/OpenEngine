#include "Scene/RenderLayerMaskText.h"

#include "Scene/SceneValue.h"

namespace GameEngine::Scene
{
namespace
{

// The refused text is quoted up to this many bytes, so a long value cannot flood the load report.
constexpr std::size_t kQuotedTextBytes = 64;

bool Refuse(std::string* outError, std::string_view key, std::string_view text)
{
    if (outError)
    {
        const bool cut = text.size() > kQuotedTextBytes;
        *outError = std::string(key) +
                    " takes layer numbers 0 to 31 in brackets, in ascending order and each once, or all, or none, not '" +
                    std::string(text.substr(0, kQuotedTextBytes)) + (cut ? "...'" : "'");
    }
    return false;
}

} // namespace

std::string FormatRenderLayerMask(Rendering::RenderLayerMask mask)
{
    if (mask == Rendering::RenderLayerMask::All())
        return "all";
    if (mask == Rendering::RenderLayerMask::None())
        return "none";
    std::string text = "[";
    for (uint32 layer = 0; layer < Rendering::RenderLayerMask::kCount; ++layer)
    {
        if (!mask.Contains(layer))
            continue;
        if (text.size() > 1)
            text += ", ";
        text += std::to_string(layer);
    }
    text += ']';
    return text;
}

bool ParseRenderLayerMask(std::string_view text, std::string_view key, Rendering::RenderLayerMask& out,
                          std::string* outError)
{
    SceneValue value;
    if (!ParseValue(text, value, nullptr))
        return Refuse(outError, key, text);
    if (value.Kind == SceneValueKind::Identifier)
    {
        if (value.StringValue == "all")
        {
            out = Rendering::RenderLayerMask::All();
            return true;
        }
        if (value.StringValue == "none")
        {
            out = Rendering::RenderLayerMask::None();
            return true;
        }
        return Refuse(outError, key, text);
    }
    if (value.Kind != SceneValueKind::Array || value.Items.empty())
        return Refuse(outError, key, text);

    Rendering::RenderLayerMask parsed = Rendering::RenderLayerMask::None();
    long long previous = -1;
    for (const SceneValue& item : value.Items)
    {
        if (item.Kind != SceneValueKind::Int || item.IntValue <= previous ||
            item.IntValue >= static_cast<long long>(Rendering::RenderLayerMask::kCount))
            return Refuse(outError, key, text);
        parsed = parsed | Rendering::RenderLayerMask::FromLayer(static_cast<uint32>(item.IntValue));
        previous = item.IntValue;
    }
    out = parsed;
    return true;
}

} // namespace GameEngine::Scene
