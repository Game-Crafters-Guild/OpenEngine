#include "Assets/TextureCookActivity.h"

#include <algorithm>

namespace GameEngine {

TextureCookActivity::Scope::~Scope()
{
    if (m_Activity)
        m_Activity->End(m_Texture);
}

TextureCookActivity::Scope TextureCookActivity::Begin(const GUID& texture)
{
    std::lock_guard lock(m_Mutex);
    const auto it = std::find_if(m_Cooking.begin(), m_Cooking.end(),
                                 [&texture](const auto& entry) { return entry.first == texture; });
    if (it != m_Cooking.end())
        ++it->second;
    else
        m_Cooking.emplace_back(texture, 1u);
    return Scope(*this, texture);
}

size_t TextureCookActivity::CookingCount() const
{
    std::lock_guard lock(m_Mutex);
    return m_Cooking.size();
}

bool TextureCookActivity::IsCooking(const GUID& texture) const
{
    std::lock_guard lock(m_Mutex);
    return std::any_of(m_Cooking.begin(), m_Cooking.end(),
                       [&texture](const auto& entry) { return entry.first == texture; });
}

void TextureCookActivity::End(const GUID& texture)
{
    std::lock_guard lock(m_Mutex);
    const auto it = std::find_if(m_Cooking.begin(), m_Cooking.end(),
                                 [&texture](const auto& entry) { return entry.first == texture; });
    if (it == m_Cooking.end())
        return;
    if (--it->second == 0)
        m_Cooking.erase(it);
}

} // namespace GameEngine
