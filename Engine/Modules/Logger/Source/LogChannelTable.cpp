#include "LogChannelTable.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace Logger
{

namespace
{
LogLevel LevelWithoutSetting(LogLevel global, LogLevel defaultLevel)
{
    return global == LogLevel::Off ? LogLevel::Off : (std::max)(global, defaultLevel);
}
} // namespace

LogChannelTable::LogChannelTable()
{
    for (auto& effective : m_Effective)
        effective.store(static_cast<std::uint8_t>(LogLevel::Off), std::memory_order_relaxed);
    for (auto& setting : m_Settings)
        setting.store(kNoSetting, std::memory_order_relaxed);
    const LogChannel& core = CoreLog();
    AddSlot(core.Name, core.DefaultLevel);
}

std::uint16_t LogChannelTable::Register(std::string_view name, LogLevel defaultLevel)
{
    assert(!name.empty() && "a log channel needs a name");
    assert(name.size() <= kMaxNameLength && "a log channel name is at most kMaxNameLength characters");
    name = name.substr(0, kMaxNameLength);

    std::lock_guard<std::mutex> lock(m_Mutex);
    const std::uint16_t count = m_Count.load(std::memory_order_relaxed);
    for (std::uint16_t index = 0; index < count; ++index)
    {
        if (GetName(index) == name)
            return index;
    }
    assert(count < kMaxLogChannels && "more log channels than kMaxLogChannels; the record goes to Core");
    if (count >= kMaxLogChannels)
        return CoreLog().Index;
    return AddSlot(name, defaultLevel);
}

std::uint16_t LogChannelTable::AddSlot(std::string_view name, LogLevel defaultLevel)
{
    const std::uint16_t index = m_Count.load(std::memory_order_relaxed);
    Name& slotName = m_Names[index];
    std::memcpy(slotName.Text.data(), name.data(), name.size());
    slotName.Length = static_cast<std::uint8_t>(name.size());
    m_Defaults[index] = defaultLevel;
    StoreEffectiveLevel(index);
    m_Count.store(static_cast<std::uint16_t>(index + 1), std::memory_order_release);
    return index;
}

void LogChannelTable::StoreEffectiveLevel(std::uint16_t index)
{
    const LogLevel global = m_Global.load(std::memory_order_relaxed);
    const std::uint8_t setting = m_Settings[index].load(std::memory_order_relaxed);
    std::uint8_t effective = static_cast<std::uint8_t>(LevelWithoutSetting(global, m_Defaults[index]));
    if (global != LogLevel::Off && setting != kNoSetting)
        effective = setting;
    m_Effective[index].store(effective, std::memory_order_relaxed);
}

void LogChannelTable::SetGlobalLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Global.store(level, std::memory_order_relaxed);
    const std::uint16_t count = m_Count.load(std::memory_order_relaxed);
    for (std::uint16_t index = 0; index < count; ++index)
        StoreEffectiveLevel(index);
}

void LogChannelTable::SetSetting(std::uint16_t index, std::optional<LogLevel> level)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_Count.load(std::memory_order_relaxed))
        return;
    m_Settings[index].store(level ? static_cast<std::uint8_t>(*level) : kNoSetting, std::memory_order_relaxed);
    StoreEffectiveLevel(index);
}

std::optional<LogLevel> LogChannelTable::GetSetting(std::uint16_t index) const
{
    if (index >= GetCount())
        return std::nullopt;
    const std::uint8_t setting = m_Settings[index].load(std::memory_order_relaxed);
    if (setting == kNoSetting)
        return std::nullopt;
    return static_cast<LogLevel>(setting);
}

std::string_view LogChannelTable::GetName(std::uint16_t index) const
{
    if (index >= GetCount())
        return {};
    const Name& name = m_Names[index];
    return {name.Text.data(), name.Length};
}

LogLevel LogChannelTable::GetDefaultLevel(std::uint16_t index) const
{
    return index < GetCount() ? m_Defaults[index] : LogLevel::Off;
}

LogLevel LogChannelTable::GetEffectiveLevel(std::uint16_t index) const
{
    return index < GetCount() ? static_cast<LogLevel>(m_Effective[index].load(std::memory_order_relaxed))
                              : LogLevel::Off;
}

LogLevel LogChannelTable::GetLevelWithoutSetting(std::uint16_t index) const
{
    return index < GetCount() ? LevelWithoutSetting(m_Global.load(std::memory_order_relaxed), m_Defaults[index])
                              : LogLevel::Off;
}

} // namespace Logger
