#pragma once

namespace GameEngine::Editor
{
/// Observers may refresh during replay, but must not author additional changes.
class HistoryReplayScope
{
public:
    HistoryReplayScope() { ++s_Depth; }
    ~HistoryReplayScope() { --s_Depth; }
    HistoryReplayScope(const HistoryReplayScope&) = delete;
    HistoryReplayScope& operator=(const HistoryReplayScope&) = delete;
    static bool IsActive() { return s_Depth != 0; }

private:
    inline static thread_local unsigned int s_Depth = 0;
};
} // namespace GameEngine::Editor
