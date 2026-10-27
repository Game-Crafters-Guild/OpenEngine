#pragma once

#include "Logger/LogChannel.h"
#include "Logger/LogLevel.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace Logger
{

/// The registered channels: names, settings and effective levels; slot 0 is Core. The effective level is the
/// setting, else the stricter of the global and the default level, and Off while the global level is Off.
/// Writes take the table's own lock; Enabled, GetName and GetCount read without a lock.
class LogChannelTable
{
  public:
    /// Longest channel name; a longer name asserts and is cut.
    static constexpr std::size_t kMaxNameLength = 63;

    LogChannelTable();

    /// The slot of the channel with this name, made when the name is new.
    std::uint16_t Register(std::string_view name, LogLevel defaultLevel);

    bool Enabled(std::uint16_t index, LogLevel level) const
    {
        return static_cast<std::uint8_t>(level) >= m_Effective[index].load(std::memory_order_relaxed);
    }

    void SetGlobalLevel(LogLevel level);
    LogLevel GetGlobalLevel() const { return m_Global.load(std::memory_order_relaxed); }

    /// Sets or, with nullopt, clears the setting of one channel.
    void SetSetting(std::uint16_t index, std::optional<LogLevel> level);
    std::optional<LogLevel> GetSetting(std::uint16_t index) const;

    std::uint16_t GetCount() const { return m_Count.load(std::memory_order_acquire); }
    /// Empty for an index that is not registered.
    std::string_view GetName(std::uint16_t index) const;
    LogLevel GetDefaultLevel(std::uint16_t index) const;
    LogLevel GetEffectiveLevel(std::uint16_t index) const;
    /// The effective level the channel has with no setting.
    LogLevel GetLevelWithoutSetting(std::uint16_t index) const;

  private:
    static constexpr std::uint8_t kNoSetting = 0xFE;

    std::uint16_t AddSlot(std::string_view name, LogLevel defaultLevel);
    void StoreEffectiveLevel(std::uint16_t index);

    struct Name
    {
        std::array<char, kMaxNameLength + 1> Text{};
        std::uint8_t Length = 0;
    };

    /// The bytes Enabled reads: one byte for each channel, 256 bytes in four cache lines.
    std::array<std::atomic<std::uint8_t>, kMaxLogChannels> m_Effective{};
    std::array<std::atomic<std::uint8_t>, kMaxLogChannels> m_Settings{};
    std::array<LogLevel, kMaxLogChannels> m_Defaults{};
    /// Written once per slot before m_Count publishes the slot.
    std::array<Name, kMaxLogChannels> m_Names{};
    std::atomic<std::uint16_t> m_Count{0};
    std::atomic<LogLevel> m_Global{LogLevel::Off};
    mutable std::mutex m_Mutex;
};

} // namespace Logger
