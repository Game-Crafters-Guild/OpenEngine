#include "Assets/CubeLutAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Rendering/Utils/CubeLutFileParser.h"

#include <algorithm>
#include <string_view>

namespace GameEngine
{

bool CubeLutAsset::Load()
{
    std::string text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("CubeLutAsset: failed to open '{}'", GetPath().string());
        return false;
    }
    return ParseText(text);
}

bool CubeLutAsset::LoadFromData(const Vector<uint8>& data)
{
    return ParseText(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

void CubeLutAsset::Unload()
{
    m_Header = {};
    m_Proposal = {};
}

bool CubeLutAsset::ParseText(std::string_view text)
{
    Rendering::CubeLutParseResult parsed{};
    if (!Rendering::ParseCubeLutFromText(text, parsed) || !parsed.Ok)
    {
        Logger::Log::Error("CubeLutAsset: '{}' is not a valid .cube file: {}", GetPath().string(), parsed.Error);
        return false;
    }
    m_Header.Title = parsed.Title;
    m_Header.Size3D = parsed.Has3D ? parsed.Size3D : 0u;
    m_Header.Size1D = parsed.Has1D ? parsed.Size1D : 0u;
    // A shaper covers the input range when there is one; the 3D table's range is then the
    // shaper's output.
    m_Header.InputMin = parsed.Has1D ? parsed.In1DMin : parsed.In3DMin;
    m_Header.InputMax = parsed.Has1D ? parsed.In1DMax : parsed.In3DMax;
    m_Proposal = Rendering::ProposeCubeLutColor(parsed);
    return true;
}

} // namespace GameEngine
