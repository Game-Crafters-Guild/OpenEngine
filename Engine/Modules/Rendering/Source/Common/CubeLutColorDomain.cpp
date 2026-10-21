#include "Rendering/Utils/CubeLutColorDomain.h"

#include "Rendering/Utils/CubeLutFileParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <vector>

namespace GameEngine::Rendering
{
namespace
{
struct EncodingName
{
    CubeLutInputEncoding Encoding;
    std::string_view Key;
    std::string_view Label;
};

constexpr std::array<EncodingName, kCubeLutInputEncodingCount> kEncodingNames{{
    {CubeLutInputEncoding::Linear, "linear", "Linear"},
    {CubeLutInputEncoding::Rec709SRGB, "srgb", "Rec.709 / sRGB"},
    {CubeLutInputEncoding::ArriLogC3, "logc3", "ARRI LogC3"},
    {CubeLutInputEncoding::DaVinciIntermediate, "davinci", "DaVinci Intermediate"},
    {CubeLutInputEncoding::ACEScct, "acescct", "ACEScct"},
    {CubeLutInputEncoding::Cineon, "cineon", "Cineon"},
}};

// An encoding a title can name: the words that name it (lower case, consecutive words of the title)
// and, for one the engine does not have, its name in the reason.
struct TitleName
{
    std::array<std::string_view, 3> Words;
    CubeLutInputEncoding Encoding;
    std::string_view Unsupported; // empty for an encoding the engine has
};

constexpr std::array<TitleName, 22> kTitleNames{{
    {{"logc"}, CubeLutInputEncoding::ArriLogC3, {}},
    {{"logc3"}, CubeLutInputEncoding::ArriLogC3, {}},
    {{"acescct"}, CubeLutInputEncoding::ACEScct, {}},
    {{"aces", "cct"}, CubeLutInputEncoding::ACEScct, {}},
    {{"davinci", "intermediate"}, CubeLutInputEncoding::DaVinciIntermediate, {}},
    {{"davinci", "wide", "gamut"}, CubeLutInputEncoding::DaVinciIntermediate, {}},
    {{"dwg"}, CubeLutInputEncoding::DaVinciIntermediate, {}},
    {{"cineon"}, CubeLutInputEncoding::Cineon, {}},
    {{"rec709"}, CubeLutInputEncoding::Rec709SRGB, {}},
    {{"rec", "709"}, CubeLutInputEncoding::Rec709SRGB, {}},
    {{"709"}, CubeLutInputEncoding::Rec709SRGB, {}},
    {{"srgb"}, CubeLutInputEncoding::Rec709SRGB, {}},
    {{"linear"}, CubeLutInputEncoding::Linear, {}},
    {{"logc4"}, CubeLutInputEncoding::Rec709SRGB, "LogC4"},
    {{"slog3"}, CubeLutInputEncoding::Rec709SRGB, "S-Log3"},
    {{"s", "log3"}, CubeLutInputEncoding::Rec709SRGB, "S-Log3"},
    {{"acescc"}, CubeLutInputEncoding::Rec709SRGB, "ACEScc"},
    {{"vlog"}, CubeLutInputEncoding::Rec709SRGB, "V-Log"},
    {{"v", "log"}, CubeLutInputEncoding::Rec709SRGB, "V-Log"},
    {{"log3g10"}, CubeLutInputEncoding::Rec709SRGB, "Log3G10"},
    {{"flog"}, CubeLutInputEncoding::Rec709SRGB, "F-Log"},
    {{"f", "log"}, CubeLutInputEncoding::Rec709SRGB, "F-Log"},
}};

// The title's words: runs of letters and digits, in lower case.
std::vector<std::string> Words(std::string_view title)
{
    std::vector<std::string> words;
    std::string word;
    for (const char c : title)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            continue;
        }
        if (!word.empty())
            words.push_back(std::move(word));
        word.clear();
    }
    if (!word.empty())
        words.push_back(std::move(word));
    return words;
}

bool NamesAt(const std::vector<std::string>& words, size_t at, const TitleName& name)
{
    for (size_t i = 0; i < name.Words.size() && !name.Words[i].empty(); ++i)
        if (at + i >= words.size() || words[at + i] != name.Words[i])
            return false;
    return true;
}

// The first encoding named among the words before the first "to" (a title "X to Y" converts from X).
const TitleName* FirstNamed(const std::vector<std::string>& words)
{
    const auto to = std::find(words.begin(), words.end(), "to");
    const size_t end = static_cast<size_t>(to - words.begin());
    for (size_t at = 0; at < end; ++at)
        for (const TitleName& name : kTitleNames)
            if (NamesAt(words, at, name))
                return &name;
    return nullptr;
}

std::string FormatRangeMax(float value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
    return text;
}
} // namespace

std::string_view CubeLutInputEncodingKey(CubeLutInputEncoding encoding)
{
    for (const EncodingName& name : kEncodingNames)
        if (name.Encoding == encoding)
            return name.Key;
    return kEncodingNames[0].Key;
}

std::string_view CubeLutInputEncodingLabel(CubeLutInputEncoding encoding)
{
    for (const EncodingName& name : kEncodingNames)
        if (name.Encoding == encoding)
            return name.Label;
    return kEncodingNames[0].Label;
}

std::optional<CubeLutInputEncoding> ParseCubeLutInputEncodingKey(std::string_view key)
{
    for (const EncodingName& name : kEncodingNames)
        if (name.Key == key)
            return name.Encoding;
    return std::nullopt;
}

bool CubeLutExpectsDisplayInput(CubeLutInputEncoding encoding, float inputMax)
{
    return encoding == CubeLutInputEncoding::Rec709SRGB || (encoding == CubeLutInputEncoding::Linear && inputMax <= 1.0f);
}

CubeLutColorProposal ProposeCubeLutColor(const CubeLutParseResult& parsed)
{
    CubeLutColorProposal proposal{};
    if (const TitleName* named = FirstNamed(Words(parsed.Title)))
    {
        proposal.InputEncoding = named->Encoding;
        proposal.Reason = named->Unsupported.empty()
                              ? "the title names " + std::string(CubeLutInputEncodingLabel(named->Encoding))
                              : "the title names " + std::string(named->Unsupported) +
                                    ", which is not supported, so it is treated as Rec.709 / sRGB";
        return proposal;
    }
    const float rangeMax = std::max(parsed.Has1D ? parsed.In1DMax : 0.0f, parsed.Has3D ? parsed.In3DMax : 0.0f);
    if (rangeMax > 1.0f)
    {
        proposal.InputEncoding = CubeLutInputEncoding::Linear;
        proposal.Reason = "the input range reaches " + FormatRangeMax(rangeMax);
        return proposal;
    }
    proposal.InputEncoding = CubeLutInputEncoding::Rec709SRGB;
    proposal.Reason = "a 0 to 1 input range and no encoding named in the title";
    return proposal;
}

} // namespace GameEngine::Rendering
