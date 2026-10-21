#include "Thumbnails/CubeLutStripBands.h"

namespace GameEngine
{
namespace
{
// The slot a request fills for its owner: one live texture per content, source and size (the
// inspector strip and its enlarged view are different sizes, so neither retires the other).
std::string SlotId(const CubeLutStripRequest& request)
{
    return std::to_string(request.Owner) + "-" + std::to_string(static_cast<uint32_t>(request.Content)) + "-" +
           std::to_string(static_cast<uint32_t>(request.Source)) + "-" + std::to_string(request.Width) + "x" +
           std::to_string(request.Height);
}
} // namespace

bool CubeLutStripBands::Open(const std::string& key)
{
    const auto request = ParseCubeLutStripKey(key);
    if (!request)
        return false;
    // The texture key without the engine prefix: one entry however the UI spelled the name.
    const std::string uiKey = MakeCubeLutStripKey(*request);
    if (m_Live.contains(uiKey))
        return true;

    if (request->Owner != 0)
    {
        std::string& current = m_KeyBySlot[SlotId(*request)];
        if (const auto previous = m_Live.find(current); previous != m_Live.end())
        {
            m_Retired.emplace_back(previous->first, previous->second);
            m_Live.erase(previous);
        }
        current = uiKey;
    }
    m_Live.emplace(uiKey, Band{*request});
    return true;
}

CubeLutStripBands::Retired CubeLutStripBands::TakeRetired()
{
    Retired retired;
    retired.swap(m_Retired);
    return retired;
}

CubeLutStripBands::Retired CubeLutStripBands::TakeAll()
{
    Retired all = TakeRetired();
    for (auto& [key, band] : m_Live)
        all.emplace_back(key, band);
    m_Live.clear();
    m_KeyBySlot.clear();
    return all;
}

bool CubeLutStripBands::MarkLutChanged(const GUID& lut)
{
    bool any = false;
    for (auto& [key, band] : m_Live)
    {
        if (band.Request.Lut != lut)
            continue;
        band.Rendered = false;
        any = true;
    }
    return any;
}

} // namespace GameEngine
