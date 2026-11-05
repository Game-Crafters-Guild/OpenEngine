#include "UndoRedo/History/HistoryComponentTargets.h"

#include "HistoryValueFormatter.h"

#include <format>
#include <span>

namespace GameEngine::Editor
{
namespace
{
bool ReadSize(std::span<const std::uint8_t>& bytes, std::uint32_t& size)
{
    if (bytes.size() < sizeof(size))
        return false;
    std::memcpy(&size, bytes.data(), sizeof(size));
    bytes = bytes.subspan(sizeof(size));
    return true;
}
} // namespace

std::string HistoryComponentTargets::Describe(const Snapshot& before, const Snapshot& after) const
{
    std::span<const std::uint8_t> oldBytes(before);
    std::span<const std::uint8_t> newBytes(after);
    std::uint32_t oldCount = 0;
    std::uint32_t newCount = 0;
    if (!ReadSize(oldBytes, oldCount) || !ReadSize(newBytes, newCount) ||
        oldCount != m_Entities.size() || newCount != oldCount)
        return {};
    std::string result;
    for (const auto entity : m_Entities)
    {
        std::uint32_t oldSize = 0;
        std::uint32_t newSize = 0;
        if (!ReadSize(oldBytes, oldSize) || !ReadSize(newBytes, newSize) ||
            oldSize > oldBytes.size() || newSize > newBytes.size())
            return {};
        const std::string description = HistoryValueFormatter::DescribeComponent(
            m_TypeId, oldBytes.first(oldSize), newBytes.first(newSize));
        oldBytes = oldBytes.subspan(oldSize);
        newBytes = newBytes.subspan(newSize);
        if (description.empty())
            continue;
        if (!result.empty())
            result += '\n';
        result += m_Entities.size() > 1 ? std::format("Entity #{}: {}", entity.id, description) : description;
    }
    return oldBytes.empty() && newBytes.empty() ? result : std::string{};
}
} // namespace GameEngine::Editor
